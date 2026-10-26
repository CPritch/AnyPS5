#include "prx/libkernel/include/Api.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::GuestMemory;

constexpr std::uint64_t Block = 65536;
constexpr std::uint64_t GuestPage = 0x4000;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void Poke(std::uint64_t address, std::uint8_t value) {
    *reinterpret_cast<volatile std::uint8_t*>(address) = value;
}

std::uint8_t Peek(std::uint64_t address) {
    return *reinterpret_cast<volatile const std::uint8_t*>(address);
}

struct DirectViews {
    std::int64_t physical = -1;
    void* first = nullptr;
    void* second = nullptr;

    DirectViews() {
        Require(sceKernelAllocateDirectMemory(0, 0x10000000000ll, 4 * Block, Block, 3, &physical) == 0, "cannot allocate direct memory");
        Require(sceKernelMapDirectMemory(&first, 4 * Block, 3, 0, physical, Block) == 0, "cannot map the first view");
        MapSecond();
    }

    DirectViews(const DirectViews&) = delete;
    DirectViews& operator=(const DirectViews&) = delete;

    ~DirectViews() {
        if (second != nullptr) sceKernelMunmap(B(), 2 * Block);
        if (first != nullptr) sceKernelMunmap(A(), 4 * Block);
        if (physical >= 0) sceKernelReleaseDirectMemory(physical, 4 * Block);
    }

    void MapSecond() {
        second = nullptr;
        Require(sceKernelMapDirectMemory(&second, 2 * Block, 3, 0, physical + static_cast<std::int64_t>(Block), Block) == 0, "cannot map the second view");
    }

    void UnmapSecond() {
        Require(sceKernelMunmap(B(), 2 * Block) == 0, "cannot unmap the second view");
        second = nullptr;
    }

    std::uint64_t A() const { return reinterpret_cast<std::uint64_t>(first); }
    std::uint64_t B() const { return reinterpret_cast<std::uint64_t>(second); }
};

void CheckBothViewsWatched(const DirectViews& views) {
    const auto a = views.A();
    const auto b = views.B();
    Require(Watched(a, 4 * Block) && Watched(b, 2 * Block), "a view of aliased direct memory is not watched");
    Require(Aliased(a + Block, 2 * Block) && Aliased(b, 2 * Block), "the alias of the two views is not recorded");
    Require(!Aliased(a, Block) && !Aliased(a + 3 * Block, Block), "pages only one view maps are recorded as aliased");
    Require(AliasSerial() != 0, "the alias serial is zero while the alias watch is on");
    Require(CollectWritesUncached(a, 4 * Block) != 0 && CollectWritesUncached(b, 2 * Block) != 0, "a view of aliased direct memory is not collected");
}

void CheckWriteThroughEitherView(const DirectViews& views) {
    const auto a = views.A();
    const auto b = views.B();
    CollectWritesUncached(a, 4 * Block);
    const auto before = CollectWritesUncached(b, 2 * Block);
    Require(before != 0 && UnchangedSince(a, 4 * Block, before) && UnchangedSince(b, 2 * Block, before), "the views are not unchanged after their collects");
    Poke(b + 100, 1);
    Require(Peek(a + Block + 100) == 1, "the views do not share their pages");
    const auto seen = CollectWritesUncached(a, 4 * Block);
    Require(seen != 0, "the collect over the first view failed");
    Require(!UnchangedSince(a + Block, Block, before), "a collect over one view missed a write through the other");
    Require(UnchangedSince(a, Block, before) && UnchangedSince(a + 2 * Block, 2 * Block, before), "a write through the other view stamped blocks it did not write");
    Require(!UnchangedSince(b, Block, before), "a write found through the first view's collect did not stamp the view it went through");
    Require(UnchangedSince(b + Block, Block, before), "a write through the second view stamped another of its blocks");
    Require(!UnchangedSinceCollected(a + Block, Block, before), "a write through the other view is not a CPU write of this one");
    Poke(a + 2 * Block + 5, 2);
    Require(CollectWritesUncached(b, 2 * Block) != 0, "the collect over the second view failed");
    Require(!UnchangedSince(b + Block, Block, seen), "a collect over the second view missed a write through the first");
    Require(!UnchangedSince(a + 2 * Block, Block, seen), "a write found through the second view's collect did not stamp the view it went through");
    Require(UnchangedSince(a + Block, Block, seen) && UnchangedSince(b, Block, seen), "a write through the first view stamped another block");
}

void CheckUnalignedView(const DirectViews& views) {
    const auto a = views.A();
    const auto b = views.B();
    void* mapped = nullptr;
    Require(sceKernelMapDirectMemory(&mapped, Block, 3, 0, views.physical + static_cast<std::int64_t>(GuestPage), Block) == 0, "cannot map the third view");
    const auto c = reinterpret_cast<std::uint64_t>(mapped);
    Require(Watched(c, Block) && Aliased(c, Block), "the third view is not watched as an alias");
    CollectWritesUncached(a, 4 * Block);
    CollectWritesUncached(b, 2 * Block);
    const auto before = CollectWritesUncached(c, Block);
    Require(before != 0 && UnchangedSince(a, 4 * Block, before), "the views are not unchanged after their collects");
    Poke(c + 0xf000, 3);
    Require(Peek(a + GuestPage + 0xf000) == 3, "the third view does not share the first view's pages");
    CollectWritesUncached(a, 2 * Block);
    Require(!UnchangedSince(a + Block, Block, before), "a write through a view at another block offset missed its block");
    Require(UnchangedSince(a, Block, before), "a write through a view at another block offset stamped the wrong block");
    Require(!UnchangedSince(b, Block, before), "a write through the third view did not stamp the second");
    Require(!UnchangedSince(c, Block, before), "a write through the third view did not stamp it");
    Require(sceKernelMunmap(c, Block) == 0, "cannot unmap the third view");
    Require(Aliased(a + Block, 2 * Block) && !Aliased(a, Block), "unmapping the third view changed the other aliases");
}

void CheckDriverStores(const DirectViews& views) {
    const auto a = views.A();
    const auto b = views.B();
    CollectWritesUncached(a, 4 * Block);
    CollectWritesUncached(b, 2 * Block);
    const auto before = TrackerGeneration();
    std::array<std::byte, 64> label{};
    label.fill(std::byte{0x44});
    Write(a + Block + 64, label);
    Require(Peek(b + 64) == 0x44, "the driver store did not reach the other view");
    Require(!UnchangedSince(b, Block, before), "a driver store over one view left the other unchanged");
    Require(StoredOver(b + 64, label.size(), before), "a driver store over one view is not seen as a store over the other");
    Require(UnchangedSince(b + Block, Block, before), "a driver store stamped another block of the other view");
    const auto stored = TrackerGeneration();
    CollectWritesUncached(b, 2 * Block);
    CollectWritesUncached(a, 4 * Block);
    Require(UnchangedSince(a, 4 * Block, stored) && UnchangedSince(b, 2 * Block, stored), "a walk after a driver store reports the store again as a newer write");
    const auto beforeGpu = TrackerGeneration();
    Require(MarkWritten(b + Block + 16, 16) != 0, "a GPU write over watched aliased memory is not trusted");
    Require(!UnchangedSince(a + 2 * Block, Block, beforeGpu), "a GPU write over one view left the other unchanged");
    Require(UnchangedSince(a + Block, Block, beforeGpu), "a GPU write stamped another block of the other view");
    const auto beforeShared = TrackerGeneration();
    std::vector<std::byte> pages(8192, std::byte{0x55});
    Write(a + Block, pages);
    Require(Peek(b) == 0x55 && Peek(b + pages.size() - 1) == 0x55, "the shared-backing store did not reach the other view");
    Require(!UnchangedSince(b, Block, beforeShared), "a shared-backing store over one view left the other unchanged");
    const auto beforeOwn = TrackerGeneration();
    StoreOwnBytes(b + Block + 32, 1, [&] { Poke(b + Block + 32, 0x66); });
    Require(!UnchangedSince(a + 2 * Block, Block, beforeOwn), "an own store over one view left the other unchanged");
}

void CheckMemo(const DirectViews& views) {
    const auto a = views.A();
    const auto b = views.B();
    CollectWritesUncached(a, 4 * Block);
    BumpCollectEpoch();
    const auto memoized = CollectWrites(a, 4 * Block);
    Require(memoized != 0 && UnchangedSince(a, 4 * Block, memoized), "the first view was not collected");
    Poke(b + 200, 4);
    Require(CollectWrites(b, 2 * Block) != 0, "the collect over the second view failed");
    const auto hit = CollectWrites(a, 4 * Block);
    Require(hit >= memoized && !UnchangedSince(a + Block, Block, memoized), "a memoized collect over one view hid a write the other view's collect found");
    const auto settled = CollectWritesUncached(a, 4 * Block);
    Poke(b + Block + 300, 5);
    BumpCollectEpoch();
    Require(CollectWrites(a, 4 * Block) != 0, "the next epoch's collect failed");
    Require(!UnchangedSince(a + 2 * Block, Block, settled), "the next epoch's collect over one view missed a write through the other");
    Require(UnchangedSince(a + Block, Block, settled), "the next epoch's collect stamped a block nothing wrote");
}

void CheckUnmapReturnsToNormal(DirectViews& views) {
    const auto a = views.A();
    const auto b = views.B();
    CollectWritesUncached(b, 2 * Block);
    const auto before = CollectWritesUncached(a, 4 * Block);
    Poke(b + Block + 9, 6);
    views.UnmapSecond();
    Require(!Aliased(a, 4 * Block), "unmapping the other view left an alias");
    Require(Watched(a, 4 * Block), "unmapping the other view left this one unwatched");
    const auto after = CollectWritesUncached(a, 4 * Block);
    Require(after != 0 && !UnchangedSince(a + 2 * Block, Block, before), "a write through the other view before its unmap was lost");
    Require(UnchangedSince(a, Block, before) && UnchangedSince(a + 3 * Block, Block, before), "the unmap stamped blocks no other view mapped");
    Poke(a + 7, 7);
    Require(CollectWritesUncached(a, 4 * Block) != 0, "the view alone is not collected");
    Require(!UnchangedSince(a, Block, after) && UnchangedSince(a + Block, 3 * Block, after), "the view alone is not watched as before");
    views.MapSecond();
    Require(Aliased(views.B(), 2 * Block) && Watched(views.B(), 2 * Block), "a view mapped again is not watched as an alias");
}

void CheckUnwatchedAlias(const DirectViews& views) {
    const auto a = views.A();
    const auto b = views.B();
    Unwatch(b, 2 * Block);
    Require(!Watched(b, 2 * Block), "the unwatched view is still watched");
    Require(!Watched(a + Block, Block), "a view whose alias is unwatched is still trusted");
    Require(CollectWritesUncached(a + Block, Block) == 0 && CollectWritesUncached(a, 4 * Block) == 0, "a collect over a view whose alias is unwatched succeeded");
    Require(Watched(a, Block) && Watched(a + 3 * Block, Block), "unwatching the alias unwatched pages it does not map");
}

}

int main() {
    try {
        if (!WriteWatched()) {
            std::cout << "no write watching: skipped\n";
            return 77;
        }
        Require(GuestWriteWatch::GuestWriteWatchAliasesEnabled_nid_postfix(), "the alias watch is off (APS5_ALIAS_WATCH=1 is not set)");
        DirectViews views;
        CheckBothViewsWatched(views);
        CheckWriteThroughEitherView(views);
        CheckUnalignedView(views);
        CheckDriverStores(views);
        CheckMemo(views);
        CheckUnmapReturnsToNormal(views);
        CheckUnwatchedAlias(views);
    } catch (const std::exception& error) {
        std::cerr << "alias watch test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "alias watch tests passed\n";
    return 0;
}
