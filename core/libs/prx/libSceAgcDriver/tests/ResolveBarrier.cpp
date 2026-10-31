#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void HeldCountBalances() {
    AgcDriver::Mutex first;
    AgcDriver::Mutex second;
    const auto before = AgcDriver::Mutex::HeldOnThisThread();
    first.lock();
    Require(AgcDriver::Mutex::HeldOnThisThread() == before + 1, "lock counts one held mutex");
    Require(second.try_lock(), "an unlocked mutex is taken by try_lock");
    Require(AgcDriver::Mutex::HeldOnThisThread() == before + 2, "a successful try_lock counts one held mutex");
    bool otherTook = true;
    std::thread other([&] { otherTook = first.try_lock(); });
    other.join();
    Require(!otherTook, "try_lock fails on a mutex another thread holds");
    second.unlock();
    first.unlock();
    Require(AgcDriver::Mutex::HeldOnThisThread() == before, "unlock gives the count back");
    try {
        const std::lock_guard guard(first);
        Require(AgcDriver::Mutex::HeldOnThisThread() == before + 1, "a guard counts its mutex");
        throw std::logic_error("unwind");
    } catch (const std::logic_error&) {
    }
    Require(AgcDriver::Mutex::HeldOnThisThread() == before, "an exception through a guard gives the count back");
}

void BarrierOutsideTraceIsInert() {
    AgcDriver::GuestMemory::ResolveBarrier(AgcDriver::GuestMemory::BarrierKind::Commands, nullptr);
    AgcDriver::GuestMemory::NoteResolveEffect(AgcDriver::GuestMemory::ResolveEffect::Submit);
    AgcDriver::GuestMemory::NoteResolveMemo();
    Require(!AgcDriver::GuestMemory::ResolveTraceActive(), "no trace is active outside Begin and End");
    AgcDriver::GuestMemory::BeginResolveLockTrace();
    const auto empty = AgcDriver::GuestMemory::EndResolveLockTrace();
    Require(empty.barriers == 0 && empty.effects == 0 && empty.memoWrites == 0, "a barrier outside a trace leaves the next trace empty");
}

void FirstBarrierIsKept() {
    int siteA = 0;
    int siteB = 0;
    AgcDriver::Mutex held;
    AgcDriver::GuestMemory::BeginResolveLockTrace();
    AgcDriver::GuestMemory::SetResolvePhase(3);
    held.lock();
    AgcDriver::GuestMemory::ResolveBarrier(AgcDriver::GuestMemory::BarrierKind::PendingSet, &siteA);
    held.unlock();
    AgcDriver::GuestMemory::ResolveBarrier(AgcDriver::GuestMemory::BarrierKind::Commands, &siteB);
    AgcDriver::GuestMemory::NoteResolveMemo();
    const auto trace = AgcDriver::GuestMemory::EndResolveLockTrace();
    Require(trace.barriers == 2, "every barrier is counted");
    Require(trace.firstKind == static_cast<std::uint8_t>(AgcDriver::GuestMemory::BarrierKind::PendingSet), "the first barrier's kind is kept");
    Require(trace.firstSite == &siteA, "the first barrier's site is kept");
    Require(trace.firstPhase == 3, "the first barrier's phase is kept");
    Require(trace.firstMutexes == 1, "the first barrier sees the one driver mutex taken inside the trace");
    Require(trace.memoWrites == 1, "memo writes are counted apart");
}

void GpuLockIsABarrier() {
    auto& gpu = AgcDriver::GuestMemory::GpuMutex();
    AgcDriver::GuestMemory::BeginResolveLockTrace();
    static_cast<void>(gpu.HeldByThisThread());
    const auto checked = AgcDriver::GuestMemory::EndResolveLockTrace();
    Require(checked.barriers == 1 && checked.firstKind == static_cast<std::uint8_t>(AgcDriver::GuestMemory::BarrierKind::HeldCheck), "a lock-held check is a barrier");
    AgcDriver::GuestMemory::BeginResolveLockTrace();
    {
        const std::lock_guard lock(gpu);
    }
    const auto locked = AgcDriver::GuestMemory::EndResolveLockTrace();
    Require(locked.barriers >= 1 && locked.firstKind == static_cast<std::uint8_t>(AgcDriver::GuestMemory::BarrierKind::Lock), "taking the GPU lock is a barrier");
}

void EarlyAbortFiresOnce() {
    int site = 0;
    AgcDriver::GuestMemory::ArmEarlyResolve();
    bool thrown = false;
    try {
        AgcDriver::GuestMemory::ResolveBarrier(AgcDriver::GuestMemory::BarrierKind::Commands, &site);
    } catch (const AgcDriver::GuestMemory::EarlyResolveAbort& abort) {
        thrown = abort.kind == static_cast<std::uint8_t>(AgcDriver::GuestMemory::BarrierKind::Commands) && abort.site == &site;
    }
    Require(thrown, "an armed barrier throws the abort with its kind and site");
    AgcDriver::GuestMemory::ResolveBarrier(AgcDriver::GuestMemory::BarrierKind::Commands, &site);
    AgcDriver::GuestMemory::NoteResolveEffect(AgcDriver::GuestMemory::ResolveEffect::Submit);
    AgcDriver::GuestMemory::DisarmEarlyResolve();
}

void EarlyAbortWithoutTrace() {
    Require(!AgcDriver::GuestMemory::ResolveTraceActive(), "no trace is active");
    AgcDriver::GuestMemory::ArmEarlyResolve();
    bool thrown = false;
    try {
        AgcDriver::GuestMemory::NoteResolveEffect(AgcDriver::GuestMemory::ResolveEffect::Submit);
    } catch (const AgcDriver::GuestMemory::EarlyResolveAbort& abort) {
        thrown = abort.kind == static_cast<std::uint8_t>(AgcDriver::GuestMemory::BarrierKind::Submit);
    }
    AgcDriver::GuestMemory::DisarmEarlyResolve();
    Require(thrown, "an armed effect aborts without the statistics trace");
}

void EarlyAbortAtTheLock() {
    auto& gpu = AgcDriver::GuestMemory::GpuMutex();
    AgcDriver::GuestMemory::ArmEarlyResolve();
    bool locked = false;
    try {
        const std::lock_guard lock(gpu);
        locked = true;
    } catch (const AgcDriver::GuestMemory::EarlyResolveAbort& abort) {
        Require(abort.kind == static_cast<std::uint8_t>(AgcDriver::GuestMemory::BarrierKind::Lock), "taking the GPU lock aborts as a lock barrier");
    }
    AgcDriver::GuestMemory::DisarmEarlyResolve();
    Require(!locked && !gpu.HeldByThisThread(), "an aborted lock is not taken");
    AgcDriver::GuestMemory::ArmEarlyResolve();
    bool checked = false;
    try {
        static_cast<void>(gpu.HeldByThisThread());
    } catch (const AgcDriver::GuestMemory::EarlyResolveAbort& abort) {
        checked = abort.kind == static_cast<std::uint8_t>(AgcDriver::GuestMemory::BarrierKind::HeldCheck);
    }
    AgcDriver::GuestMemory::DisarmEarlyResolve();
    Require(checked, "a lock-held check aborts when armed");
}

void EarlyAbortNotWhileUnwinding() {
    static_cast<void>(AgcDriver::GuestMemory::TakeEarlyBarriersPassed());
    struct Barrier {
        ~Barrier() { AgcDriver::GuestMemory::ResolveBarrier(AgcDriver::GuestMemory::BarrierKind::Commands, nullptr); }
    };
    AgcDriver::GuestMemory::ArmEarlyResolve();
    try {
        const Barrier barrier;
        throw std::logic_error("unwind");
    } catch (const std::logic_error&) {
    }
    const auto [unwinding, completion] = AgcDriver::GuestMemory::TakeEarlyBarriersPassed();
    Require(unwinding == 1 && completion == 0, "a barrier during an unwind is passed and counted");
    bool thrown = false;
    try {
        AgcDriver::GuestMemory::ResolveBarrier(AgcDriver::GuestMemory::BarrierKind::Commands, nullptr);
    } catch (const AgcDriver::GuestMemory::EarlyResolveAbort&) {
        thrown = true;
    }
    AgcDriver::GuestMemory::DisarmEarlyResolve();
    Require(thrown, "the abort stays armed after a barrier passed during an unwind");
    AgcDriver::GuestMemory::ResolveBarrier(AgcDriver::GuestMemory::BarrierKind::Commands, nullptr);
}

}

int main() {
    try {
        HeldCountBalances();
        BarrierOutsideTraceIsInert();
        FirstBarrierIsKept();
        GpuLockIsABarrier();
        EarlyAbortFiresOnce();
        EarlyAbortWithoutTrace();
        EarlyAbortAtTheLock();
        EarlyAbortNotWhileUnwinding();
    } catch (const std::exception& error) {
        std::cerr << "resolve barrier tests failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "resolve barrier tests passed\n";
    return 0;
}
