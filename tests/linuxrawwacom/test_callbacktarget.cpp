#include "../../app/streaming/plankcallbacktarget.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

#define CHECK(x) do { if (!(x)) std::abort(); } while (false)

int main()
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        PlankCallbackTarget<int> gate;
        int target = 0;
        gate.publish(&target);
        std::promise<void> entered, release, clearing;
        auto canReturn = release.get_future();
        auto callback = std::async(std::launch::async, [&] {
            gate.invoke([&](int& value) {
                entered.set_value();
                canReturn.wait();
                ++value;
            });
        });
        entered.get_future().wait();
        auto cleanup = std::async(std::launch::async, [&] {
            clearing.set_value();
            gate.clear();
        });
        clearing.get_future().wait();
        CHECK(cleanup.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout);
        release.set_value();
        callback.get();
        cleanup.get();
        CHECK(target == 1);
        gate.invoke([](int&) { CHECK(false); });
        // Reusing the gate for a replacement target must not lose callbacks.
        gate.publish(&target);
        gate.invoke([](int& value) { ++value; });
        gate.clear();
        CHECK(target == 2);
    }
    std::cout << "callback lifetime: concurrent retirement and late delivery passed\n";
}
