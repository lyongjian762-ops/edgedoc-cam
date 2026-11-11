#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace edgedoc {

struct ImageMetadata {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride_bytes = 0;
    std::string pixel_format;
};

struct FrameSnapshot {
    std::uint64_t task_id;
    ImageMetadata metadata;
    std::vector<std::uint8_t> pixels;

    FrameSnapshot(std::uint64_t id, ImageMetadata info, std::vector<std::uint8_t> data);
    FrameSnapshot(const FrameSnapshot&) = delete;
    FrameSnapshot& operator=(const FrameSnapshot&) = delete;
    FrameSnapshot(FrameSnapshot&&) noexcept = default;
    FrameSnapshot& operator=(FrameSnapshot&&) noexcept = default;
};

enum class SubmitResult { Accepted, Busy, Stopped };

// Capacity is one waiting snapshot; a snapshot already popped is not counted.
class BoundedTaskQueue {
public:
    SubmitResult try_submit(FrameSnapshot&& snapshot);
    std::optional<FrameSnapshot> wait_pop();
    void stop();

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<FrameSnapshot> pending_;
    bool stopped_ = false;
};

struct WorkerStatus {
    std::size_t completed = 0;
    std::size_t failed = 0;
    std::optional<std::uint64_t> last_failed_task;
    std::exception_ptr last_error;
};

// One worker thread. The handler borrows the snapshot until it returns.
class AsyncWorker {
public:
    using Handler = std::function<void(const FrameSnapshot&)>;
    explicit AsyncWorker(Handler handler);
    ~AsyncWorker();
    AsyncWorker(const AsyncWorker&) = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;

    SubmitResult try_submit(FrameSnapshot&& snapshot);
    void stop(); // Safe to call concurrently, including from the handler.
    void stop_and_join(); // Call from the owner thread after producers finish.
    WorkerStatus status() const;

private:
    void consume();
    BoundedTaskQueue queue_;
    Handler handler_;
    mutable std::mutex status_mutex_;
    WorkerStatus status_;
    std::thread worker_;
};
} // namespace edgedoc
