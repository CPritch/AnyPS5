#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>

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
        std::puts("host import reconcile tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
