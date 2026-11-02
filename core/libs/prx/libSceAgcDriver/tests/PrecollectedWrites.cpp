#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t Block = 65536;
constexpr std::uint64_t Page = 4096;
constexpr std::uint64_t Base = std::uint64_t{1} << 32;

struct FakeWatch {
    std::mutex mutex;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> written;
    std::size_t walks = 0;
};

FakeWatch& Fake() {
    static FakeWatch fake;
    return fake;
}

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void Write(std::uint64_t address) {
    const std::lock_guard lock(Fake().mutex);
    Fake().written.emplace_back(address, address + Page);
}

std::size_t Walks() {
    const std::lock_guard lock(Fake().mutex);
    return Fake().walks;
}

}

namespace GuestWriteWatch {

extern "C" {

bool GuestWriteWatchAvailable_nid_postfix() {
    return true;
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    return address >= Base && bytes <= 4 * Block && address - Base <= 4 * Block - bytes;
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
    auto& fake = Fake();
    const auto end = address + bytes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
    {
        const std::lock_guard lock(fake.mutex);
        ++fake.walks;
        for (auto it = fake.written.begin(); it != fake.written.end();) {
            if (it->first < end && address < it->second) {
                runs.emplace_back(std::max(it->first, address), std::min(it->second, end));
                it = fake.written.erase(it);
            } else ++it;
        }
    }
    for (const auto& [from, to] : runs) written(context, from, to);
    return true;
}

}

}

namespace {

using namespace AgcDriver::GuestMemory;

PrecollectedWrites PreparedOn(std::uint64_t token, std::uint64_t address, std::size_t bytes) {
    PrecollectedWrites walks;
    walks.epoch = token;
    std::thread preparer([&] { Require(Precollect(walks, address, bytes), "the preparer's walk failed"); });
    preparer.join();
    return walks;
}

void WalksServeTheCommitOfTheirEpoch() {
    constexpr std::uint64_t Token = 7;
    FollowCollectToken(Token);
    BumpCollectEpoch();
    const auto before = CollectWritesUncached(Base, Block);
    BumpCollectEpoch();
    Write(Base + Page);
    const auto walks = PreparedOn(Token, Base, Block);
    Require(walks.walks.size() == 1 && !UnchangedSince(Base, Block, before), "the preparer's walk did not stamp the write made before it");
    const auto stamped = walks.walks.front().generation;
    Write(Base + 2 * Page);
    const auto walked = Walks();
    const auto hits = PrecollectedHits();
    {
        const PrecollectedScope scope(&walks);
        const auto collected = CollectWrites(Base + Page, 2 * Page);
        Require(collected != 0 && Walks() == walked && PrecollectedHits() == hits + 1, "a collect inside the walked range walked again on the committer");
        Require(UnchangedSince(Base, Block, stamped), "a write after the preparer's walk, inside its epoch, was seen by the commit");
        MarkWritten(Base + 3 * Page, Page);
        Require(!UnchangedSince(Base, Block, stamped), "a driver store after the preparer's walk was hidden from the commit");
        CollectWrites(Base + Block, Page);
        Require(Walks() == walked + 1, "a collect outside the walked ranges was served from them");
    }
    CollectWrites(Base + Page, 2 * Page);
    Require(Walks() == walked + 2, "the walks served a collect after their commit's scope");
}

void WalksOfAnotherEpochAreIgnored() {
    FollowCollectToken(9);
    BumpCollectEpoch();
    const auto walks = PreparedOn(8, Base, Block);
    const auto stamped = walks.walks.front().generation;
    Write(Base + Page);
    const auto walked = Walks();
    const PrecollectedScope scope(&walks);
    CollectWrites(Base, Block);
    Require(Walks() == walked + 1 && !UnchangedSince(Base, Block, stamped), "a commit of another epoch used the preparer's walk");
}

void PreparersShareTheWalksOfOneEpoch() {
    constexpr std::uint64_t Token = 11;
    const auto first = PreparedOn(Token, Base + Block, Block);
    const auto walked = Walks();
    const auto second = PreparedOn(Token, Base + Block + Page, Page);
    Require(Walks() == walked && second.walks.size() == 1 && second.walks.front().begin == first.walks.front().begin, "a second preparation of the same epoch walked a range the first had walked");
    const auto other = PreparedOn(Token + 1, Base + Block, Block);
    Require(Walks() == walked + 1 && other.walks.size() == 1, "a preparation of the next epoch reused a walk of the last");
}

std::uint64_t CollectOnFollower(std::uint64_t token, std::uint64_t address, std::size_t bytes) {
    std::uint64_t collected = 0;
    std::thread follower([&] {
        MarkProofThread();
        BumpCollectEpoch();
        FollowCollectToken(token);
        collected = CollectWrites(address, bytes);
    });
    follower.join();
    return collected;
}

void FollowersShareTheWalksOfOneToken() {
    constexpr std::uint64_t Token = 21;
    constexpr std::uint64_t Range = Base + 2 * Block;
    const auto walked = Walks();
    const auto first = CollectOnFollower(Token, Range, Page);
    Require(first != 0 && Walks() == walked + 1, "the first follower of a token did not walk");
    Require(CollectOnFollower(Token, Range + Page, Page) != 0 && Walks() == walked + 1, "a second follower of the same token walked a range the first had walked");
    Write(Range + Page);
    Require(CollectOnFollower(Token + 1, Range, Page) != 0 && Walks() == walked + 2, "a follower of the next token reused a walk of the last");
    Require(!UnchangedSince(Range + Page, Page, first), "the next token's walk did not stamp a write made after the last token's walk");
    const auto preparer = PreparedOn(Token + 2, Range, Block);
    Require(CollectOnFollower(Token + 2, Range, Page) != 0 && Walks() == walked + 3, "a follower walked a range the preparer had walked under its token");
}

}

int main() {
    try {
        WalksServeTheCommitOfTheirEpoch();
        WalksOfAnotherEpochAreIgnored();
        PreparersShareTheWalksOfOneEpoch();
        FollowersShareTheWalksOfOneToken();
    } catch (const std::exception& error) {
        std::cerr << "precollected writes test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "precollected writes test passed\n";
    return 0;
}
