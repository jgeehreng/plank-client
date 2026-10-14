#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// hidraw feature ioctls may wait for a USB control transfer despite O_NONBLOCK.
// Keep them off both capture and network callback threads. Jobs own duplicated
// descriptors, never a Client pointer; an old lease cannot publish a completion
// into a resumed session. At most one ioctl executes and 32 requests are held.
class LinuxWacomReportWorker
{
public:
    struct Result {
        std::uint16_t type, interfaceId;
        std::uint32_t transaction;
        std::vector<unsigned char> payload;
    };

    LinuxWacomReportWorker() : m_State(std::make_shared<State>()),
        m_Thread([state = m_State] { run(state); }) {}

    ~LinuxWacomReportWorker()
    {
        std::unique_lock<std::mutex> lock(m_State->mutex);
        m_State->stopping = true;
        ++m_State->epoch;
        m_State->jobs.clear();
        m_State->ready.clear();
        m_State->changed.notify_all();
        const bool exited = m_State->changed.wait_for(lock,
            std::chrono::milliseconds(100), [&] { return m_State->exited; });
        lock.unlock();
        if (exited) m_Thread.join();
        else m_Thread.detach(); // Owns only State until the bounded kernel ioctl returns.
    }

    LinuxWacomReportWorker(const LinuxWacomReportWorker&) = delete;
    LinuxWacomReportWorker& operator=(const LinuxWacomReportWorker&) = delete;

    bool submit(std::function<Result()> operation)
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        if (m_State->stopping || m_State->outstanding >= 32) return false;
        m_State->jobs.push_back({m_State->epoch, std::move(operation)});
        ++m_State->outstanding;
        m_State->changed.notify_all();
        return true;
    }

    void invalidate()
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        ++m_State->epoch;
        m_State->jobs.clear();
        m_State->ready.clear();
        m_State->outstanding = 0;
    }

    std::deque<Result> take()
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        std::deque<Result> ready;
        ready.swap(m_State->ready);
        m_State->outstanding -= ready.size();
        return ready;
    }

private:
    struct Job {
        std::uint64_t epoch;
        std::function<Result()> operation;
    };
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Job> jobs;
        std::deque<Result> ready;
        std::uint64_t epoch = 1;
        std::size_t outstanding = 0;
        bool stopping = false, exited = false;
    };

    static void run(const std::shared_ptr<State>& state)
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        for (;;) {
            state->changed.wait(lock, [&] { return state->stopping || !state->jobs.empty(); });
            if (state->stopping) break;
            Job job = std::move(state->jobs.front());
            state->jobs.pop_front();
            lock.unlock();
            Result result = job.operation();
            lock.lock();
            if (!state->stopping && job.epoch == state->epoch) {
                state->ready.push_back(std::move(result));
            }
        }
        state->exited = true;
        state->changed.notify_all();
    }

    std::shared_ptr<State> m_State;
    std::thread m_Thread;
};
