#include "l04_async_queue.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using edgedoc::AsyncWorker;
using edgedoc::FrameSnapshot;
using edgedoc::SubmitResult;
using namespace std::chrono_literals;

static_assert(!std::is_copy_constructible<FrameSnapshot>::value, "snapshots must not copy");
static_assert(std::is_nothrow_move_constructible<FrameSnapshot>::value, "snapshots must move safely");

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

FrameSnapshot make_frame(std::uint64_t id) {
    return FrameSnapshot(id, {2, 2, 2, "GRAY8"}, {1, 2, 3, static_cast<std::uint8_t>(id)});
}

class Gate {
public:
    void enter() {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
    }
    bool wait_until_entered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, 5s, [this] { return entered_; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_ = false, released_ = false;
};

struct ReleaseOnExit {
    Gate& gate;
    ~ReleaseOnExit() { gate.release(); }
};

void test_empty_stop_wakes() {
    edgedoc::BoundedTaskQueue queue;
    std::promise<bool> completed;
    auto result = completed.get_future();
    std::thread consumer([&] { completed.set_value(!queue.wait_pop().has_value()); });
    queue.stop();
    queue.stop();
    const bool ready = result.wait_for(5s) == std::future_status::ready;
    if (!ready) { std::cerr << "FAIL stopped consumer did not return\n"; std::abort(); }
    const bool empty = result.get();
    consumer.join();
    require(empty, "stopped empty queue yielded a task");
    auto frame = make_frame(7);
    const auto* ptr = frame.pixels.data();
    require(queue.try_submit(std::move(frame)) == SubmitResult::Stopped, "accepted after stop");
    require(frame.pixels.data() == ptr && frame.pixels.size() == 4, "stopped submission consumed frame");
}

void test_full_drain_and_lifetime() {
    Gate gate;
    std::vector<std::uint64_t> order;
    std::vector<std::vector<std::uint8_t>> observed;
    const std::uint8_t* first_data = nullptr;
    AsyncWorker worker([&](const FrameSnapshot& frame) {
        if (frame.task_id == 1) { first_data = frame.pixels.data(); gate.enter(); }
        order.push_back(frame.task_id);
        observed.push_back(frame.pixels);
        require(frame.metadata.width == 2 && frame.metadata.height == 2 &&
                frame.metadata.stride_bytes == 2 && frame.metadata.pixel_format == "GRAY8", "metadata changed");
    });
    ReleaseOnExit release{gate};
    auto first = make_frame(1);
    const auto* initial_data = first.pixels.data();
    require(worker.try_submit(std::move(first)) == SubmitResult::Accepted, "first not accepted");
    require(gate.wait_until_entered(), "first task did not begin");
    {
        auto second = make_frame(2);
        require(worker.try_submit(std::move(second)) == SubmitResult::Accepted, "waiting task not accepted");
    } // Moved-from producer and its scope are gone before task 2 is processed.
    auto third = make_frame(3);
    const auto* third_data = third.pixels.data();
    require(worker.try_submit(std::move(third)) == SubmitResult::Busy, "full queue did not report Busy");
    require(third.pixels.data() == third_data && third.pixels == std::vector<std::uint8_t>({1,2,3,3}),
            "Busy submission consumed or altered data");
    worker.stop();
    auto fourth = make_frame(4);
    require(worker.try_submit(std::move(fourth)) == SubmitResult::Stopped, "accepted after stop");
    gate.release();
    worker.stop_and_join();
    worker.stop_and_join();
    auto status = worker.status();
    require(first_data == initial_data, "move unexpectedly copied image storage");
    require(order == std::vector<std::uint64_t>({1,2}), "accepted tasks were lost or reordered");
    require(observed == std::vector<std::vector<std::uint8_t>>({{1,2,3,1},{1,2,3,2}}), "snapshot data corrupted");
    require(status.completed == 2 && status.failed == 0, "incorrect completion counts");
}

void test_handler_exception_drains() {
    Gate gate;
    std::vector<std::uint64_t> order;
    AsyncWorker worker([&](const FrameSnapshot& frame) {
        order.push_back(frame.task_id);
        if (frame.task_id == 1) { gate.enter(); throw std::runtime_error("simulated task failure"); }
    });
    ReleaseOnExit release{gate};
    auto one = make_frame(1);
    require(worker.try_submit(std::move(one)) == SubmitResult::Accepted, "exception task not accepted");
    require(gate.wait_until_entered(), "exception task did not start");
    auto two = make_frame(2);
    require(worker.try_submit(std::move(two)) == SubmitResult::Accepted, "second task not accepted");
    worker.stop();
    gate.release();
    worker.stop_and_join();
    auto status = worker.status();
    require(order == std::vector<std::uint64_t>({1,2}), "worker failed to drain after exception");
    require(status.failed == 1 && status.completed == 1 && status.last_failed_task == 1 && status.last_error,
            "worker did not retain exception status");
    try { std::rethrow_exception(status.last_error); }
    catch (const std::runtime_error& e) {
        require(std::string(e.what()) == "simulated task failure", "wrong error retained");
        return;
    }
    throw std::runtime_error("wrong exception type");
}

void test_competing_producers() {
    constexpr std::size_t count = 8;
    Gate gate;
    std::vector<std::uint64_t> order;
    AsyncWorker worker([&](const FrameSnapshot& frame) {
        if (frame.task_id == 1) gate.enter();
        order.push_back(frame.task_id);
    });
    ReleaseOnExit release{gate};
    auto first = make_frame(1);
    require(worker.try_submit(std::move(first)) == SubmitResult::Accepted, "first task not accepted");
    require(gate.wait_until_entered(), "worker did not enter handler");
    std::promise<void> start;
    auto signal = start.get_future().share();
    std::array<SubmitResult, count> results{};
    std::array<bool, count> retained{};
    std::vector<std::thread> producers;
    producers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        producers.emplace_back([&, i] {
            auto frame = make_frame(10 + i);
            const auto* ptr = frame.pixels.data();
            signal.wait();
            results[i] = worker.try_submit(std::move(frame));
            retained[i] = results[i] != SubmitResult::Busy || (frame.pixels.data() == ptr && frame.pixels.size() == 4);
        });
    }
    start.set_value();
    for (auto& producer : producers) producer.join();
    std::size_t accepted = 0, busy = 0;
    for (std::size_t i = 0; i < count; ++i) {
        accepted += results[i] == SubmitResult::Accepted;
        busy += results[i] == SubmitResult::Busy;
        require(retained[i], "Busy producer lost ownership");
    }
    require(accepted == 1 && busy == count - 1, "waiting capacity exceeded or accepted task overwritten");
    worker.stop();
    gate.release();
    worker.stop_and_join();
    require(order.size() == 2 && order.front() == 1 && worker.status().completed == 2,
            "accepted producer task was not drained");
}

void test_stop_submission_race() {
    constexpr std::size_t count = 6;
    for (int round = 0; round < 25; ++round) {
        Gate gate;
        std::vector<std::uint64_t> order;
        AsyncWorker worker([&](const FrameSnapshot& frame) {
            if (frame.task_id == 1) gate.enter();
            order.push_back(frame.task_id);
        });
        ReleaseOnExit release{gate};
        auto first = make_frame(1);
        require(worker.try_submit(std::move(first)) == SubmitResult::Accepted, "first task not accepted");
        require(gate.wait_until_entered(), "worker did not enter handler");
        std::promise<void> start;
        auto signal = start.get_future().share();
        std::array<SubmitResult, count> results{};
        std::array<bool, count> retained{};
        std::vector<std::thread> producers;
        producers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            producers.emplace_back([&, i] {
                auto frame = make_frame(10 + i);
                const auto* ptr = frame.pixels.data();
                signal.wait();
                results[i] = worker.try_submit(std::move(frame));
                retained[i] = results[i] == SubmitResult::Accepted ||
                              (frame.pixels.data() == ptr && frame.pixels.size() == 4);
            });
        }
        std::thread stopper([&] { signal.wait(); worker.stop(); });
        start.set_value();
        for (auto& producer : producers) producer.join();
        stopper.join();
        std::size_t accepted = 0;
        std::uint64_t accepted_id = 0;
        for (std::size_t i = 0; i < count; ++i) {
            require(retained[i], "rejected concurrent submission consumed its frame");
            if (results[i] == SubmitResult::Accepted) { ++accepted; accepted_id = 10 + i; }
        }
        require(accepted <= 1, "more than one waiting task accepted");
        gate.release();
        worker.stop_and_join();
        require(worker.status().completed == 1 + accepted && order.size() == 1 + accepted,
                "accepted task lost during concurrent stop");
        require(accepted == 0 || order[1] == accepted_id, "wrong task drained after stop");
    }
}

void test_destructor_drains() {
    std::uint64_t seen = 0;
    {
        AsyncWorker worker([&](const FrameSnapshot& frame) { seen = frame.task_id; });
        auto frame = make_frame(44);
        require(worker.try_submit(std::move(frame)) == SubmitResult::Accepted, "destructor test not accepted");
    }
    require(seen == 44, "destructor failed to drain and join");
}
} // namespace

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {{"empty stop wakes", test_empty_stop_wakes},
                          {"full queue, drain and lifetime", test_full_drain_and_lifetime},
                          {"exception and drain", test_handler_exception_drains},
                          {"competing producers", test_competing_producers},
                          {"concurrent stop and submit", test_stop_submission_race},
                          {"destructor drains", test_destructor_drains}};
    int failures = 0;
    for (const auto& test : tests) {
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << test.name << ": " << e.what() << '\n'; }
    }
    std::cout << "tests=" << (sizeof tests / sizeof tests[0]) << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
