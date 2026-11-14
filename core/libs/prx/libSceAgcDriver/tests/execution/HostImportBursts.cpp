#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/VerifyCounters.hpp"
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
#include <utility>
#include <vector>

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;

constexpr std::uint64_t MiB = 1ull << 20u;
constexpr std::uint64_t Window = 8 * MiB;
constexpr std::uint64_t Windows = 4;
constexpr std::uint64_t Piece = 64 * 1024;
constexpr std::uint64_t ProbeBytes = 4096;
constexpr std::uint64_t HalfWindow = Window / 2;
constexpr std::uint64_t BurstPieces = 8;
constexpr std::uint64_t BurstStride = 512 * 1024;

std::string Hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

class Arena {
public:
    Arena() : base(static_cast<std::uint8_t*>(std::aligned_alloc(Window, Windows * Window))) {
        Require(base != nullptr, "host import bursts: cannot allocate the guest arena");
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

bool Enabled(const char* off) {
    return std::getenv(off) == nullptr;
}

bool JournalExpected() {
    return Enabled("APS5_NO_JOURNAL_RECONCILE") && Enabled("APS5_NO_IMPORT_JOURNAL") && Enabled("APS5_NO_ALLOCATION_JOURNAL");
}

std::string Counts(const Graphics::ImportChurnCounts& before, const Graphics::ImportChurnCounts& after) {
    return " (journal " + std::to_string(after.journalRefreshes - before.journalRefreshes) + ", full " + std::to_string(after.fullRefreshes - before.fullRefreshes) + ", unseeded " + std::to_string(after.rcUnseeded - before.rcUnseeded) + ", reseeded " + std::to_string(after.reseeded - before.reseeded) + ", waiter retires " + std::to_string(after.retireWaiter - before.retireWaiter) + ", survived " + std::to_string(after.proofsSurvived - before.proofsSurvived) + ", region checks " + std::to_string(after.regionChecks - before.regionChecks) + ", region skips " + std::to_string(after.regionSkips - before.regionSkips) + ")";
}

enum class Outcome : std::uint8_t { NoDevice, NoImport, Passed };

Outcome BurstsStayOnTheJournal() {
    Arena arena;
    auto device = OpenVulkanTestDevice();
    if (!device) return Outcome::NoDevice;
    const auto context = device->graphicsContext();
    const auto first = arena.Address(0);
    const auto serial = [&](std::uint64_t address) { return Graphics::HostImportSerial(context, address, ProbeBytes, false); };
    const auto reconciled = [&](std::uint64_t address) { return Graphics::HostImportSerial(context, address, ProbeBytes, true); };
    const bool journal = JournalExpected();
    const bool reseed = journal && Enabled("APS5_NO_JOURNAL_RESEED");
    const bool regionProofs = Enabled("APS5_NO_REGION_PROOFS");
    const bool regionCheck = regionProofs && Enabled("APS5_NO_IMPORT_JOURNAL") && Enabled("APS5_NO_ALLOCATION_JOURNAL") && Enabled("APS5_NO_REGION_CHECK");

    Add(arena, 0, HalfWindow);
    if (!Fill(*device, first) || serial(first) == 0) return Outcome::NoImport;
    Add(arena, HalfWindow + MiB, Piece);
    Require(reconciled(first) != 0, "host import bursts: the import at " + Hex(first) + " was dropped by a registration apart from its run");
    const auto kept = serial(first);
    auto plain = Graphics::HostImportsIdentity(context);
    auto proof = Graphics::HostImportsIdentity(context);
    Require(plain.device != VK_NULL_HANDLE, "host import bursts: the table is not reconciled after priming");

    auto before = Graphics::ImportChurnTotals();
    {
        Graphics::GuestBufferMemory building(context);
        building.AcquireRegistered();
        Add(arena, 2 * Window, Piece);
        building.Upload(true);
        building.WriteBack();
    }
    auto after = Graphics::ImportChurnTotals();
    Require(after.fullOutside > before.fullOutside, "host import bursts: a build with a lease older than the registry did not refresh the imports from it" + Counts(before, after));
    if (reseed) Require(after.reseeded > before.reseeded, "host import bursts: a refresh from an older settled lease did not re-seed the journal" + Counts(before, after));
    Require(Graphics::HostImportsUnchanged(context, plain) == regionProofs, regionProofs ? "host import bursts: a full refresh that retired nothing broke a plain proof" : "host import bursts: with APS5_NO_REGION_PROOFS a full refresh kept a plain proof");
    Require(serial(first) == kept, "host import bursts: the build's refresh from its own lease dropped the import at " + Hex(first) + Counts(before, after));
    before = Graphics::ImportChurnTotals();
    Add(arena, HalfWindow + 2 * MiB, Piece);
    const auto reconciledFirst = reconciled(first);
    after = Graphics::ImportChurnTotals();
    Require(reconciledFirst == kept, "host import bursts: a registration beside the import's run after the build dropped the import at " + Hex(first) + Counts(before, after));
    if (reseed) {
        Require(after.journalRefreshes > before.journalRefreshes && after.rcUnseeded == before.rcUnseeded && after.fullRefreshes == before.fullRefreshes, "host import bursts: the reconcile after a build's refresh did not take the journal" + Counts(before, after));
    } else if (journal) {
        Require(after.rcUnseeded > before.rcUnseeded && after.journalRefreshes == before.journalRefreshes, "host import bursts: without the re-seed the reconcile after a build's refresh was expected to go full for want of a seed" + Counts(before, after));
    }

    std::vector<std::uint64_t> pieces;
    for (std::uint64_t index = 0; index < BurstPieces; ++index) {
        const auto offset = Window + index * BurstStride;
        Add(arena, offset, Piece);
        Require(Fill(*device, arena.Address(offset)), "host import bursts: the piece at " + Hex(arena.Address(offset)) + " cannot be filled");
        Require(serial(arena.Address(offset)) != 0, "host import bursts: the piece at " + Hex(arena.Address(offset)) + " is not imported");
        pieces.push_back(offset);
    }
    Require(reconciled(first) == kept, "host import bursts: importing the burst's pieces dropped the import at " + Hex(first));
    proof = Graphics::HostImportsIdentity(context);
    const auto stale = proof;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> keptRegion{{{first, first + ProbeBytes}}};
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> burstRegion{{{arena.Address(pieces.front()), arena.Address(pieces.front()) + ProbeBytes}}};
    before = Graphics::ImportChurnTotals();
    bool survived = true;
    for (const auto offset : pieces) {
        Remove(arena, offset);
        const auto check = Graphics::HostImportsUnchangedOver(context, proof, keptRegion, true);
        if (check == Graphics::ImportsProofCheck::Changed) survived = false;
        Require(serial(first) == kept, "host import bursts: the burst retired the import at " + Hex(first));
        Require(serial(arena.Address(offset)) == 0, "host import bursts: the import of the removed piece at " + Hex(arena.Address(offset)) + " survived its removal");
    }
    after = Graphics::ImportChurnTotals();
    Require(after.retires - before.retires >= BurstPieces, "host import bursts: removing imported pieces did not retire their imports" + Counts(before, after));
    Require(survived == regionProofs, regionProofs ? "host import bursts: a retire burst elsewhere broke the proof over " + Hex(first) + Counts(before, after) : "host import bursts: with APS5_NO_REGION_PROOFS a proof survived a retire burst");
    auto old = stale;
    Require(Graphics::HostImportsUnchangedOver(context, old, burstRegion, true) == Graphics::ImportsProofCheck::Changed, "host import bursts: a proof over a retired piece survived its retire");
    if (regionCheck) {
        Require(after.regionSkips - before.regionSkips >= BurstPieces && after.fullRefreshes == before.fullRefreshes, "host import bursts: a proof over a region the burst did not touch reconciled the imports" + Counts(before, after));
    } else {
        Require(after.regionSkips == before.regionSkips, "host import bursts: a proof skipped its reconcile with the region check off" + Counts(before, after));
        if (journal) Require(after.journalRefreshes - before.journalRefreshes >= BurstPieces && after.fullRefreshes == before.fullRefreshes, "host import bursts: the retire burst left the journal path" + Counts(before, after));
    }

    Require(reconciled(first) == kept, "host import bursts: the reconcile after the burst dropped the import at " + Hex(first));
    auto nearby = Graphics::HostImportsIdentity(context);
    Require(nearby.device != VK_NULL_HANDLE, "host import bursts: the table is not reconciled after the burst");
    before = Graphics::ImportChurnTotals();
    Add(arena, HalfWindow + 3 * MiB, Piece);
    const auto nearbyCheck = Graphics::HostImportsUnchangedOver(context, nearby, keptRegion, true);
    after = Graphics::ImportChurnTotals();
    Require(nearbyCheck != Graphics::ImportsProofCheck::Changed && serial(first) == kept, "host import bursts: a registration in the import's window apart from its run broke the proof over " + Hex(first) + Counts(before, after));
    Require(after.regionSkips == before.regionSkips && after.reconciles > before.reconciles, "host import bursts: a proof skipped the reconcile although the registry changed beside its region's import" + Counts(before, after));
    Remove(arena, HalfWindow + 3 * MiB);

    const bool absorbs = Enabled("APS5_NO_IMPORT_JOURNAL") && Enabled("APS5_NO_ALLOCATION_JOURNAL");
    const bool witness = absorbs && Enabled("APS5_NO_ABSORB_WITNESS");
    before = Graphics::ImportChurnTotals();
    const auto staleFirst = Graphics::HostImportsIdentity(context);
    const auto staleAgain = Graphics::HostImportsIdentity(context);
    Add(arena, 3 * Window, Piece);
    const auto staleLater = Graphics::HostImportsIdentity(context);
    after = Graphics::ImportChurnTotals();
    const auto refusals = after.absorbRefusals - before.absorbRefusals;
    const auto hits = after.absorbWitnessHits - before.absorbWitnessHits;
    std::printf("host import bursts: three currency checks after a removal in the import's window: walks that refused %llu, refused by the remembered import %llu\n", static_cast<unsigned long long>(refusals), static_cast<unsigned long long>(hits));
    Require(staleFirst.device == VK_NULL_HANDLE && staleAgain.device == VK_NULL_HANDLE && staleLater.device == VK_NULL_HANDLE, "host import bursts: the table absorbed a removal in the window of the import at " + Hex(first));
    if (witness) {
        Require(refusals == 1 && hits == 2, "host import bursts: the checks after the first did not refuse by the import the first walk found touched, before or after a later registration elsewhere" + Counts(before, after));
    } else {
        Require(hits == 0 && refusals == (absorbs ? 3u : 0u), absorbs ? "host import bursts: with APS5_NO_ABSORB_WITNESS every check should walk the table and refuse" : "host import bursts: without the journals a check walked the table to absorb" + Counts(before, after));
    }
    Require(reconciled(first) == kept, "host import bursts: the reconcile after the refused absorbs dropped the import at " + Hex(first));
    Require(Graphics::HostImportsIdentity(context).device != VK_NULL_HANDLE, "host import bursts: the table is not current after the reconcile that followed the refused absorbs");
    Remove(arena, 3 * Window);
    if (std::getenv("APS5_VERIFY_ABSORB_WITNESS") != nullptr) {
        using AgcDriver::DriverDetail::VerifyCount;
        const auto checked = AgcDriver::DriverDetail::VerifyTotals()[static_cast<std::size_t>(VerifyCount::AbsorbWitnessChecked)];
        std::printf("host import bursts: absorb witness verify: %llu refusals checked\n", static_cast<unsigned long long>(checked));
        Require(witness ? checked >= 2 : checked == 0, "host import bursts: APS5_VERIFY_ABSORB_WITNESS did not check the refusals by the remembered import");
    }

    if (std::getenv("APS5_VERIFY_REGION_CHECK") != nullptr) {
        using AgcDriver::DriverDetail::VerifyCount;
        const auto totals = AgcDriver::DriverDetail::VerifyTotals();
        const auto checked = totals[static_cast<std::size_t>(VerifyCount::RegionCheckChecked)];
        const auto skipped = totals[static_cast<std::size_t>(VerifyCount::RegionCheckSkipped)];
        std::printf("host import bursts: region check verify: %llu imports checked, %llu skipped\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(skipped));
        Require(regionCheck ? checked != 0 : checked == 0 && skipped == 0, "host import bursts: APS5_VERIFY_REGION_CHECK did not count the imports it checked");
    }

    Remove(arena, 0);
    Remove(arena, HalfWindow + MiB);
    Remove(arena, HalfWindow + 2 * MiB);
    Remove(arena, 2 * Window);
    Require(serial(first) == 0 || reconciled(first) == 0, "host import bursts: the import at " + Hex(first) + " outlived its range");
    return Outcome::Passed;
}

}

int main() {
    try {
        const auto outcome = BurstsStayOnTheJournal();
        if (outcome == Outcome::NoDevice) return VulkanTestSkipped;
        if (outcome == Outcome::NoImport) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        std::puts("host import burst tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
