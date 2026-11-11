#include "l04_async_queue.hpp"

#include <stdexcept>
#include <utility>

namespace edgedoc {

FrameSnapshot::FrameSnapshot(std::uint64_t id, ImageMetadata info, std::vector<std::uint8_t> data)
    : task_id(id), metadata(std::move(info)), pixels(std::move(data)) {}

SubmitResult BoundedTaskQueue::try_submit(FrameSnapshot&& snapshot) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return SubmitResult::Stopped;
        if (pending_) return SubmitResult::Busy;
        pending_.emplace(std::move(snapshot));
    }
    ready_.notify_one();
    return SubmitResult::Accepted;
}

std::optional<FrameSnapshot> BoundedTaskQueue::wait_pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return stopped_ || pending_.has_value(); });
    if (!pending_) return std::nullopt;
    auto snapshot = std::move(pending_);
    pending_.reset();
    return snapshot;
}

void BoundedTaskQueue::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    ready_.notify_all();
}

AsyncWorker::AsyncWorker(Handler handler) : handler_(std::move(handler)) {
    if (!handler_) throw std::invalid_argument("worker handler is empty");
    worker_ = std::thread(&AsyncWorker::consume, this);
}

AsyncWorker::~AsyncWorker() { stop_and_join(); }

SubmitResult AsyncWorker::try_submit(FrameSnapshot&& snapshot) {
    return queue_.try_submit(std::move(snapshot));
}

void AsyncWorker::stop() { queue_.stop(); }

void AsyncWorker::stop_and_join() {
    stop();
    if (worker_.joinable()) worker_.join();
}

WorkerStatus AsyncWorker::status() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void AsyncWorker::consume() {
    while (auto snapshot = queue_.wait_pop()) {
        try {
            // wait_pop has already released the queue mutex before the handler runs.
            handler_(*snapshot);
            std::lock_guard<std::mutex> lock(status_mutex_);
            ++status_.completed;
        } catch (...) {
            auto error = std::current_exception();
            std::lock_guard<std::mutex> lock(status_mutex_);
            ++status_.failed;
            status_.last_failed_task = snapshot->task_id;
            status_.last_error = std::move(error);
        }
    }
}
} // namespace edgedoc
