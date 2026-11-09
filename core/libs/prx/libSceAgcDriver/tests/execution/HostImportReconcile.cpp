#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/VerifyCounters.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <memory>

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;

constexpr std::uint64_t MiB = 1ull << 20u;
constexpr std::uint64_t Window = 8 * MiB;
constexpr std::uint64_t Windows = 4;
constexpr std::uint64_t Piece = 64 * 1024;
constexpr std::uint64_t ProbeBytes = 4096;
constexpr std::uint64_t HalfWindow = Window / 2;

std::string Hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

class Arena {
public:
    Arena() : base(static_cast<std::uint8_t*>(std::aligned_alloc(Window, Windows * Window))) {
        Require(base != nullptr, "host import reconcile: cannot allocate the guest arena");
        std::memset(base, 0, Windows * Window);
    }
    ~Arena() { std::free(base); }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    std::uint8_t* At(std::uint64_t offset) const { return base + offset; }
    std::uint64_t Address(std::uint64_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base)) + offset; }

private:
    std::uint8_t* base;
};

void Add(const Arena& arena, std::uint64_t offset, std::uint64_t bytes) {
    GuestAllocations::Mutation().Add(arena.At(offset), bytes, true, true, true);
}

void Remove(const Arena& arena, std::uint64_t offset) {
    GuestAllocations::Mutation().Remove(arena.At(offset));
}

bool Fill(AgcDriver::VulkanDevice& device, std::uint64_t address) {
    const std::array<std::uint32_t, 4> pattern{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    const bool filled = device.FillBuffer(address, ProbeBytes, pattern);
    device.WaitIdle();
    return filled;
}

bool JournalExpected() {
    return std::getenv("APS5_NO_JOURNAL_RECONCILE") == nullptr && std::getenv("APS5_NO_IMPORT_JOURNAL") == nullptr && std::getenv("APS5_NO_ALLOCATION_JOURNAL") == nullptr;
}

enum class Today : std::uint8_t { Absorbs, Refreshes };

void RequirePath(const Graphics::ImportChurnCounts& before, Today today, const std::string& what) {
    const auto after = Graphics::ImportChurnTotals();
    const auto journal = after.journalRefreshes - before.journalRefreshes;
    const auto full = after.fullRefreshes - before.fullRefreshes;
    const auto absorbed = after.absorbs - before.absorbs;
    const auto counts = " (journal " + std::to_string(journal) + ", full " + std::to_string(full) + ", absorbed " + std::to_string(absorbed) + ")";
    if (JournalExpected()) {
        Require(journal >= 1 && full == 0, "host import reconcile: " + what + ": the journal reconcile did not stand in for the full refresh" + counts);
    } else if (today == Today::Refreshes) {
        Require(journal == 0 && full >= 1, "host import reconcile: " + what + ": without the journal reconcile a full refresh was expected" + counts);
    } else {
        Require(journal == 0 && (absorbed >= 1 || full >= 1), "host import reconcile: " + what + ": without the journal reconcile the change was expected to be absorbed or refreshed" + counts);
    }
}

enum class Outcome : std::uint8_t { NoDevice, NoImport, Passed };

bool PinWaitJournalExpected() {
    return JournalExpected() && std::getenv("APS5_NO_PIN_WAIT_JOURNAL") == nullptr;
}

bool EarlyBarrierExpected() {
    return std::getenv("APS5_NO_EARLY_IMPORT_BARRIER") == nullptr;
}

void RequireJournalVerifyCounts() {
    if (std::getenv("APS5_VERIFY_JOURNAL_RECONCILE") == nullptr) return;
    using AgcDriver::DriverDetail::VerifyCount;
    const auto totals = AgcDriver::DriverDetail::VerifyTotals();
    const auto count = [&](VerifyCount which) { return totals[static_cast<std::size_t>(which)]; };
    std::printf("host import reconcile: journal reconcile verify: %llu decisions, %llu covers, %llu invariants, %llu during a pin wait, %llu skipped\n", static_cast<unsigned long long>(count(VerifyCount::JournalReconcileDecisions)), static_cast<unsigned long long>(count(VerifyCount::JournalReconcileCovers)), static_cast<unsigned long long>(count(VerifyCount::JournalReconcileInvariants)), static_cast<unsigned long long>(count(VerifyCount::JournalReconcileUnsettled)), static_cast<unsigned long long>(count(VerifyCount::JournalReconcileSkipped)));
    if (!JournalExpected()) return;
    Require(count(VerifyCount::JournalReconcileDecisions) != 0 && count(VerifyCount::JournalReconcileCovers) != 0 && count(VerifyCount::JournalReconcileInvariants) != 0, "host import reconcile: APS5_VERIFY_JOURNAL_RECONCILE counted no checks");
    Require((count(VerifyCount::JournalReconcileUnsettled) != 0) == PinWaitJournalExpected(), "host import reconcile: APS5_VERIFY_JOURNAL_RECONCILE did not count the journal reconcile it checked during the pin wait exactly when the pin-wait journal is on");
}

void RequireEarlyBarrierVerifyCounts() {
    if (std::getenv("APS5_VERIFY_EARLY_IMPORT_BARRIER") == nullptr) return;
    using AgcDriver::DriverDetail::VerifyCount;
    const auto totals = AgcDriver::DriverDetail::VerifyTotals();
    const auto checked = totals[static_cast<std::size_t>(VerifyCount::EarlyImportBarrierChecked)];
    const auto skipped = totals[static_cast<std::size_t>(VerifyCount::EarlyImportBarrierSkipped)];
    std::printf("host import reconcile: early Import barrier verify: %llu checked, %llu skipped\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(skipped));
    if (EarlyBarrierExpected()) Require(checked != 0 && skipped != 0, "host import reconcile: APS5_VERIFY_EARLY_IMPORT_BARRIER checked no early barrier against its refresh, or did not count the aborted one as skipped");
    else Require(checked == 0 && skipped == 0, "host import reconcile: APS5_VERIFY_EARLY_IMPORT_BARRIER counted with the early barrier off");
}

Outcome ReconcileFollowsTheChanges() {
    Arena arena;
    auto device = OpenVulkanTestDevice();
    if (!device) return Outcome::NoDevice;
    const auto context = device->graphicsContext();
    const auto first = arena.Address(0);
    const auto second = arena.Address(Window);
    const auto serial = [&](std::uint64_t address) { return Graphics::HostImportSerial(context, address, ProbeBytes, false); };

    Add(arena, 0, HalfWindow);
    if (!Fill(*device, first)) return Outcome::NoImport;
    if (serial(first) == 0) return Outcome::NoImport;
    const auto reconciled = [&](std::uint64_t address) { return Graphics::HostImportSerial(context, address, ProbeBytes, true); };
    Add(arena, HalfWindow + MiB, Piece);
    Require(reconciled(first) != 0, "host import reconcile: the import at " + Hex(first) + " was dropped by a registration apart from its run");
    auto proof = Graphics::HostImportsIdentity(context);
    const auto kept = serial(first);
    Require(kept != 0 && Graphics::HostImportsReconciledUnchanged(context, proof), "host import reconcile: the proof over the import at " + Hex(first) + " does not hold right after it was taken");

    auto before = Graphics::ImportChurnTotals();
    Add(arena, HalfWindow + 2 * MiB, Piece);
    Require(Graphics::HostImportsReconciledUnchanged(context, proof) && serial(first) == kept, "host import reconcile: a registration in the import's window, apart from its run, dropped the import at " + Hex(first));
    RequirePath(before, Today::Refreshes, "a registration in the import's window");

    before = Graphics::ImportChurnTotals();
    Add(arena, 2 * Window, Piece);
    Require(Graphics::HostImportsReconciledUnchanged(context, proof) && serial(first) == kept, "host import reconcile: a registration in another window dropped the import at " + Hex(first));
    RequirePath(before, Today::Absorbs, "a registration in another window");

    before = Graphics::ImportChurnTotals();
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(first + MiB), ProbeBytes);
    Require(Graphics::HostImportsReconciledUnchanged(context, proof) && serial(first) == kept, "host import reconcile: invalidating pages under an unchanged range dropped the import at " + Hex(first));
    RequirePath(before, Today::Refreshes, "pages invalidated under the import");

    before = Graphics::ImportChurnTotals();
    Add(arena, HalfWindow + 3 * MiB, Piece);
    std::uint8_t abortedAt = 0;
    std::thread resolver([&] {
        AgcDriver::GuestMemory::ArmEarlyResolve();
        try {
            static_cast<void>(reconciled(first));
        } catch (const AgcDriver::GuestMemory::EarlyResolveAbort& abort) {
            abortedAt = abort.kind;
        }
        AgcDriver::GuestMemory::DisarmEarlyResolve();
    });
    resolver.join();
    const auto armed = Graphics::ImportChurnTotals();
    Require(abortedAt == static_cast<std::uint8_t>(AgcDriver::GuestMemory::BarrierKind::Import), "host import reconcile: an armed early resolve that had to reconcile the imports did not abort at the Import barrier");
    Require(armed.rcAborted == before.rcAborted + 1 && armed.reconciles == before.reconciles + 1, "host import reconcile: the aborted reconcile was not counted as aborted");
    Require(armed.journalRefreshes == before.journalRefreshes && armed.fullRefreshes == before.fullRefreshes, "host import reconcile: an aborted reconcile refreshed the imports");
    if (EarlyBarrierExpected()) {
        Require(armed.earlyBarrierAborts == before.earlyBarrierAborts + 1 && armed.fullLeases == before.fullLeases, "host import reconcile: the armed reconcile took its lease before it reached the Import barrier");
    } else {
        Require(armed.earlyBarrierAborts == before.earlyBarrierAborts, "host import reconcile: the early Import barrier ran with APS5_NO_EARLY_IMPORT_BARRIER set");
    }
    Require(reconciled(first) == kept, "host import reconcile: the reconcile after an aborted one dropped the import at " + Hex(first));
    Remove(arena, HalfWindow + 3 * MiB);
    Require(reconciled(first) == kept, "host import reconcile: removing a registration apart from its run dropped the import at " + Hex(first));
    proof = Graphics::HostImportsIdentity(context);

    Add(arena, Window, HalfWindow);
    Require(reconciled(first) == kept, "host import reconcile: a registration in another window dropped the import at " + Hex(first));
    Require(Fill(*device, second), "host import reconcile: the range at " + Hex(second) + " cannot be filled");
    const auto pending = serial(second);
    Require(pending != 0, "host import reconcile: the range at " + Hex(second) + " is not imported");
    proof = Graphics::HostImportsIdentity(context);
    Require(proof.device != VK_NULL_HANDLE, "host import reconcile: the table is not reconciled right after an import");
    before = Graphics::ImportChurnTotals();
    Add(arena, 3 * Window, Piece);
    Require(Graphics::HostImportsReconciledUnchanged(context, proof) && serial(first) == kept && serial(second) == pending, "host import reconcile: an unrelated registration after a fresh import dropped an import");
    RequirePath(before, Today::Refreshes, "an unrelated registration after a fresh import");

    const auto waiting = arena.Address(3 * Window);
    auto held = std::make_unique<GuestAllocations::Lease>(GuestAllocations::GuestAllocationsAcquire_nid_postfix());
    std::thread mutator([&] {
        GuestAllocations::Mutation mutation;
        mutation.Add(arena.At(3 * Window + 2 * MiB), Piece, true, true, true);
        mutation.RequireUnpinned(arena.At(3 * Window), Piece);
    });
    const auto retiring = [&] {
        const auto live = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        return GuestAllocations::GuestAllocationsMutationWaiting_nid_postfix() && std::none_of(live.begin(), live.end(), [&](const auto& range) { return range->address == waiting; });
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!retiring() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool waited = retiring();
    before = Graphics::ImportChurnTotals();
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(static_cast<std::uintptr_t>(arena.Address(2 * Window)), ProbeBytes);
    const bool proofHeld = Graphics::HostImportsReconciledUnchanged(context, proof) && serial(first) == kept && serial(second) == pending;
    const auto during = Graphics::ImportChurnTotals();
    held.reset();
    mutator.join();
    Require(waited, "host import reconcile: the mutation never waited for the leased range at " + Hex(waiting));
    Require(proofHeld, "host import reconcile: a reconcile while a mutation waited on a pinned range dropped an import");
    const auto counts = " (fallbacks " + std::to_string(during.journalFallbacks - before.journalFallbacks) + ", journal " + std::to_string(during.journalRefreshes - before.journalRefreshes) + ", full " + std::to_string(during.fullRefreshes - before.fullRefreshes) + ", kept during the wait " + std::to_string(during.pinWaitJournal - before.pinWaitJournal) + ")";
    if (PinWaitJournalExpected()) {
        Require(during.journalRefreshes > before.journalRefreshes && during.pinWaitJournal > before.pinWaitJournal && during.fullRefreshes == before.fullRefreshes && during.journalFallbacks == before.journalFallbacks && during.pinWaitFallbacks == before.pinWaitFallbacks, "host import reconcile: a reconcile while a mutation waited on a pinned range left the journal" + counts);
    } else if (JournalExpected()) {
        Require(during.journalFallbacks > before.journalFallbacks && during.pinWaitFallbacks > before.pinWaitFallbacks && during.journalRefreshes == before.journalRefreshes && during.fullRefreshes > before.fullRefreshes && during.pinWaitJournal == before.pinWaitJournal, "host import reconcile: with APS5_NO_PIN_WAIT_JOURNAL a reconcile while a mutation waited on a pinned range trusted the journal" + counts);
    } else {
        Require(during.journalFallbacks == before.journalFallbacks && during.journalRefreshes == before.journalRefreshes && during.pinWaitJournal == before.pinWaitJournal, "host import reconcile: without the journal reconcile a fallback was counted" + counts);
    }
    before = Graphics::ImportChurnTotals();
    Require(reconciled(first) == kept, "host import reconcile: the import at " + Hex(first) + " was dropped once the waiting mutation ended");
    if (PinWaitJournalExpected()) RequirePath(before, Today::Refreshes, "the first reconcile after a pin wait, seeded during the wait");

    Remove(arena, HalfWindow + MiB);
    Remove(arena, HalfWindow + 2 * MiB);
    Require(Graphics::HostImportsReconciledUnchanged(context, proof) && serial(first) == kept, "host import reconcile: removing registrations apart from its run dropped the import at " + Hex(first));
    Add(arena, HalfWindow, HalfWindow);
    Require(!Graphics::HostImportsReconciledUnchanged(context, proof), "host import reconcile: a registration completed the window of " + Hex(first) + ", but the proof over its import held");
    Require(serial(first) != kept, "host import reconcile: the import at " + Hex(first) + " stayed although its run grew to the whole window");
    Remove(arena, HalfWindow);

    Require(Fill(*device, first), "host import reconcile: the range at " + Hex(first) + " cannot be filled again");
    Require(reconciled(first) != 0, "host import reconcile: the range at " + Hex(first) + " lost its new import at a reconcile");
    proof = Graphics::HostImportsIdentity(context);
    const auto again = serial(first);
    Require(again != 0 && Graphics::HostImportsReconciledUnchanged(context, proof), "host import reconcile: the range at " + Hex(first) + " is not imported again");
    Remove(arena, 0);
    Add(arena, 0, HalfWindow);
    Require(!Graphics::HostImportsReconciledUnchanged(context, proof), "host import reconcile: the range at " + Hex(first) + " was mapped again, but the proof over its import held");
    Require(serial(first) != again, "host import reconcile: the import at " + Hex(first) + " outlived the range it was made from");

    Remove(arena, 0);
    Remove(arena, Window);
    Remove(arena, 2 * Window);
    Remove(arena, 3 * Window);
    Remove(arena, 3 * Window + 2 * MiB);
    Require(!Graphics::HostImportsReconciledUnchanged(context, proof) && serial(first) == 0 && serial(second) == 0, "host import reconcile: imports outlived the ranges they were made from");
    return Outcome::Passed;
}

}

int main() {
    try {
        const auto outcome = ReconcileFollowsTheChanges();
        if (outcome == Outcome::NoDevice) return VulkanTestSkipped;
        if (outcome == Outcome::NoImport) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        RequireJournalVerifyCounts();
        RequireEarlyBarrierVerifyCounts();
        std::puts("host import reconcile tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
