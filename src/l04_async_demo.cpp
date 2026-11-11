#include "l04_async_queue.hpp"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

int main() {
    using namespace std::chrono_literals;
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false, release = false;
    edgedoc::AsyncWorker worker([&](const edgedoc::FrameSnapshot& frame) {
        if (frame.task_id == 1) {
            std::unique_lock<std::mutex> lock(mutex);
            started = true;
            cv.notify_one();
            cv.wait(lock, [&] { return release; });
        }
        std::this_thread::sleep_for(30ms); // Simulated work, not a synchronization mechanism.
        std::cout << "processed task=" << frame.task_id << " bytes=" << frame.pixels.size() << '\n';
    });
    auto make_frame = [](std::uint64_t id) {
        return edgedoc::FrameSnapshot(id, {2, 2, 2, "GRAY8"}, {1, 2, 3, 4});
    };
    auto first = make_frame(1);
    if (worker.try_submit(std::move(first)) != edgedoc::SubmitResult::Accepted) return 1;
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return started; });
    }
    auto second = make_frame(2), third = make_frame(3);
    auto result2 = worker.try_submit(std::move(second));
    auto result3 = worker.try_submit(std::move(third));
    std::cout << "task2=" << (result2 == edgedoc::SubmitResult::Accepted ? "Accepted" : "unexpected")
              << " task3=" << (result3 == edgedoc::SubmitResult::Busy ? "Busy" : "unexpected") << '\n';
    worker.stop();
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    cv.notify_all();
    worker.stop_and_join();
    auto status = worker.status();
    std::cout << "completed=" << status.completed << " failed=" << status.failed << '\n';
    return result2 == edgedoc::SubmitResult::Accepted && result3 == edgedoc::SubmitResult::Busy &&
                   status.completed == 2 && status.failed == 0 ? 0 : 1;
}
