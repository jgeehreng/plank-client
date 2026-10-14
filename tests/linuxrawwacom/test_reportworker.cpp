#include "../../app/streaming/input/linuxwacomreportworker.h"

#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>

using namespace std::chrono_literals;

// Keep checks active in release/package builds too.
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
    std::abort(); \
} } while (false)

int main()
{
    using Worker = LinuxWacomReportWorker;
    Worker worker;
    std::promise<void> entered, unblock;
    auto gate = unblock.get_future().share();
    CHECK(worker.submit([&] {
        entered.set_value();
        gate.wait();
        return Worker::Result {1, 0, 1, {42}};
    }));
    CHECK(entered.get_future().wait_for(1s) == std::future_status::ready);

    // A blocked ioctl must not hold the result/lifecycle lock, nor create an
    // unbounded request backlog. Outstanding includes unread completions.
    for (unsigned i = 0; i < 31; ++i)
        CHECK(worker.submit([i] { return Worker::Result {1, 0, i + 2, {43}}; }));
    CHECK(!worker.submit([] { return Worker::Result {}; }));
    CHECK(worker.take().empty());
    worker.invalidate();
    CHECK(worker.submit([] { return Worker::Result {2, 0, 99, {44}}; }));
    unblock.set_value();
    std::deque<Worker::Result> result;
    auto deadline = std::chrono::steady_clock::now() + 1s;
    while (result.empty() && std::chrono::steady_clock::now() < deadline) {
        result = worker.take();
        std::this_thread::sleep_for(1ms);
    }
    CHECK(result.size() == 1 && result[0].transaction == 99 && result[0].payload[0] == 44);

    // Destruction is bounded even if the OS ioctl is still in progress; the
    // operation owns its state and cannot touch the destroyed input object.
    auto held = std::make_unique<Worker>();
    std::promise<void> entered2, unblock2, completed2;
    auto gate2 = unblock2.get_future().share();
    auto completed = completed2.get_future();
    CHECK(held->submit([&] {
        entered2.set_value(); gate2.wait(); completed2.set_value();
        return Worker::Result {};
    }));
    CHECK(entered2.get_future().wait_for(1s) == std::future_status::ready);
    auto before = std::chrono::steady_clock::now();
    held.reset();
    CHECK(std::chrono::steady_clock::now() - before < 500ms);
    unblock2.set_value();
    CHECK(completed.wait_for(1s) == std::future_status::ready);
    std::cout << "report worker: bounded queue, unblocked capture/lifecycle, stale reply rejection and shutdown passed\n";
}
