#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_IMPORTWORKERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_IMPORTWORKERS_HPP

#include "prx/libc/include/HostThread.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace AgcDriver {

class ImportWorkers {
public:
    static constexpr std::size_t Limit = 16;

    static ImportWorkers& Get() {
        static auto* const workers = new ImportWorkers;
        return *workers;
    }

    static bool Enabled() {
        static const bool enabled = std::getenv("APS5_NO_IMPORT_WORKERS") == nullptr;
        return enabled;
    }

    bool Post(std::function<void()> job) {
        {
            std::lock_guard lock(mutex);
            if (stopping) return false;
            if (idle <= jobs.size()) {
                if (threads.size() >= Limit) return false;
                try {
                    threads.push_back(Libc::StartHostThread([this] { work(); }));
                } catch (const std::system_error&) {
                    return false;
                }
                ++idle;
            }
            jobs.push_back(std::move(job));
        }
        wake.notify_one();
        return true;
    }

    std::size_t Running() {
        std::lock_guard lock(mutex);
        return threads.size();
    }

    void Stop() {
        std::vector<std::thread> joined;
        {
            std::lock_guard lock(mutex);
            stopping = true;
            joined.swap(threads);
        }
        wake.notify_all();
        for (auto& thread : joined) {
            if (thread.get_id() == std::this_thread::get_id()) thread.detach();
            else thread.join();
        }
    }

private:
    void work() {
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return stopping || !jobs.empty(); });
            if (jobs.empty()) break;
            auto job = std::move(jobs.front());
            jobs.pop_front();
            --idle;
            lock.unlock();
            job();
            job = nullptr;
            lock.lock();
            ++idle;
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> threads;
    std::size_t idle = 0;
    bool stopping = false;
};

}

#endif
