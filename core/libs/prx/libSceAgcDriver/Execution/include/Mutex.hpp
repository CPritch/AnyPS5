#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MUTEX_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_MUTEX_HPP

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace AgcDriver {

inline void WaitWhile(const std::atomic<std::uint32_t>& word, std::uint32_t value) noexcept {
    word.wait(value, std::memory_order_relaxed);
}

inline void WakeOne(std::atomic<std::uint32_t>& word) noexcept {
    word.notify_one();
}

inline void WakeAll(std::atomic<std::uint32_t>& word) noexcept {
    word.notify_all();
}

class Mutex {
public:
    constexpr Mutex() noexcept = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() noexcept {
        if (!tryAcquire()) lockContended();
        ++HeldOnThisThread();
    }

    bool try_lock() noexcept {
        if (!tryAcquire()) return false;
        ++HeldOnThisThread();
        return true;
    }

    void unlock() noexcept {
        --HeldOnThisThread();
        if (state.exchange(Unlocked, std::memory_order_release) == Contended) WakeOne(state);
    }

    static std::int32_t& HeldOnThisThread() noexcept {
        static thread_local std::int32_t held = 0;
        return held;
    }

private:
    bool tryAcquire() noexcept {
        std::uint32_t expected = Unlocked;
        return state.compare_exchange_strong(expected, Locked, std::memory_order_acquire, std::memory_order_relaxed);
    }

    static constexpr std::uint32_t Unlocked = 0;
    static constexpr std::uint32_t Locked = 1;
    static constexpr std::uint32_t Contended = 2;

    static std::uint32_t SpinLimit() noexcept {
        static const std::uint32_t limit = [] {
            const char* text = std::getenv("APS5_MUTEX_SPINS");
            return text == nullptr ? 160u : static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10));
        }();
        return limit;
    }

    [[gnu::noinline]] void lockContended() noexcept {
        for (std::uint32_t spin = SpinLimit(); spin != 0; --spin) {
            if (state.load(std::memory_order_relaxed) == Unlocked && tryAcquire()) return;
#if defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        }
        while (state.exchange(Contended, std::memory_order_acquire) != Unlocked) WaitWhile(state, Contended);
    }

    std::atomic<std::uint32_t> state{Unlocked};
};

inline const void* CurrentThreadToken() noexcept {
#if defined(_WIN32) && defined(__x86_64__) && defined(__GNUC__)
    const void* block;
    __asm__("movq %%gs:0x30, %0" : "=r"(block));
    return block;
#else
    thread_local char token;
    return &token;
#endif
}

}

#endif
