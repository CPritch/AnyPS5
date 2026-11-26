#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

constexpr std::uint32_t HostType = 0;
constexpr std::uint32_t DeviceType = 1;
constexpr std::uint32_t SecondDeviceType = 2;
constexpr std::uint64_t MiB = 1ull << 20u;

std::uint64_t reportedBudget = 0;
std::uint64_t reportedUsage = 0;
std::uint64_t reportedHostBudget = 0;
std::uint64_t reportedHostUsage = 0;
std::uint64_t reportedSecondBudget = 0;
std::uint64_t reportedSecondUsage = 0;

VKAPI_ATTR void VKAPI_CALL mockMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2* properties) {
    auto* reported = static_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(properties->pNext);
    reported->heapBudget[0] = reportedBudget;
    reported->heapUsage[0] = reportedUsage;
    reported->heapBudget[1] = reportedHostBudget;
    reported->heapUsage[1] = reportedHostUsage;
    reported->heapBudget[2] = reportedSecondBudget;
    reported->heapUsage[2] = reportedSecondUsage;
}

Context mockContext(bool budgetExtension) {
    Context context{};
    context.physical = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(1));
    context.memoryProperties2 = budgetExtension ? mockMemoryProperties2 : nullptr;
    context.memory.memoryHeapCount = 2;
    context.memory.memoryHeaps[0] = {4096 * MiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    context.memory.memoryHeaps[1] = {16384 * MiB, 0};
    context.memory.memoryTypeCount = 2;
    context.memory.memoryTypes[HostType] = {0, 1};
    context.memory.memoryTypes[DeviceType] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    return context;
}

void Choose(const char* what, bool budgetExtension, std::uint64_t budget, std::uint64_t usage, std::uint32_t mask, std::uint32_t expected) {
    reportedBudget = budget;
    reportedUsage = usage;
    const auto type = mockContext(budgetExtension).DeviceMemoryType(mask, 64 * MiB, "test");
    Expect(type == expected, std::string(what) + ": chose memory type " + std::to_string(type) + ", expected " + std::to_string(expected));
}

VkDeviceSize reclaimAsked = 0;
VkDeviceSize reclaimReturns = 0;
VkDeviceSize reclaimUsageAfter = 0;
bool reclaimNewImage = false;

VkDeviceSize mockReclaim(VkDeviceSize bytes, bool newImage) {
    reclaimAsked = bytes;
    reclaimNewImage = newImage;
    reportedUsage = reclaimUsageAfter;
    return reclaimReturns;
}

void ReclaimTests() {
    const std::uint32_t both = (1u << HostType) | (1u << DeviceType);
    Context context = mockContext(true);
    context.cacheReclaim = mockReclaim;
    const auto choose = [&](const char* what, std::uint64_t budget, std::uint64_t usage, VkDeviceSize returns, std::uint64_t usageAfter, std::uint32_t expected, VkDeviceSize asked) {
        reportedBudget = budget;
        reportedUsage = usage;
        reclaimAsked = 0;
        reclaimReturns = returns;
        reclaimUsageAfter = usageAfter;
        const auto type = context.DeviceMemoryType(both, 64 * MiB, "test");
        Expect(type == expected, std::string(what) + ": chose memory type " + std::to_string(type) + ", expected " + std::to_string(expected));
        Expect(reclaimAsked == asked, std::string(what) + ": asked the caches for " + std::to_string(reclaimAsked) + " bytes, expected " + std::to_string(asked));
    };
    choose("room in the budget leaves the caches alone", 3000 * MiB, 2000 * MiB, 0, 2000 * MiB, DeviceType, 0);
    choose("caches release the whole allocation", 3000 * MiB, 2937 * MiB, 64 * MiB, 2937 * MiB, DeviceType, 64 * MiB);
    Expect(reclaimNewImage, "the budget reclaim did not ask for a new image's room");
    choose("caches release the allocation and the overage", 2952 * MiB, 3633 * MiB, 745 * MiB, 3633 * MiB, DeviceType, 745 * MiB);
    choose("a partial release that the budget then shows", 3000 * MiB, 2990 * MiB, 32 * MiB, 2900 * MiB, DeviceType, 64 * MiB);
    choose("a partial release the budget does not show", 3000 * MiB, 2990 * MiB, 32 * MiB, 2990 * MiB, HostType, 64 * MiB);
    choose("caches release nothing", 3000 * MiB, 2990 * MiB, 0, 2990 * MiB, HostType, 64 * MiB);
}

bool futile = false;

bool mockFutile() {
    return futile;
}

void FutileReclaimTests() {
    const std::uint32_t both = (1u << HostType) | (1u << DeviceType);
    Context context = mockContext(true);
    context.cacheReclaim = mockReclaim;
    context.cacheReclaimFutile = mockFutile;
    reportedBudget = 3000 * MiB;
    reportedUsage = 2990 * MiB;
    reclaimReturns = 64 * MiB;
    reclaimUsageAfter = 2990 * MiB;
    futile = true;
    reclaimAsked = 0;
    auto type = context.DeviceMemoryType(both, 64 * MiB, "test");
    Expect(type == HostType, "an allocation over the budget after a futile reclaim chose memory type " + std::to_string(type) + ", expected the host type");
    Expect(reclaimAsked == 0, "the caches were asked for " + std::to_string(reclaimAsked) + " bytes after a futile reclaim");
    futile = false;
    type = context.DeviceMemoryType(both, 64 * MiB, "test");
    Expect(type == DeviceType, "an allocation over the budget once the futile reclaim lapsed chose memory type " + std::to_string(type) + ", expected the device-local type");
    Expect(reclaimAsked == 64 * MiB, "the caches were asked for " + std::to_string(reclaimAsked) + " bytes once the futile reclaim lapsed, expected 64 MiB");
    reportedUsage = 2000 * MiB;
    futile = true;
    reclaimAsked = 0;
    type = context.DeviceMemoryType(both, 64 * MiB, "test");
    Expect(type == DeviceType && reclaimAsked == 0, "an allocation with room in the budget did not stay device-local without the caches");

    Expect(!TextureCacheReclaimFutile(), "a reclaim was futile before any ran");
    Expect(ReleaseTextureCaches(MiB) == 0, "empty texture caches released memory");
    Expect(TextureCacheReclaimFutile(), "a reclaim over empty caches was not futile");
    Recorder::CountPresent();
    Expect(!TextureCacheReclaimFutile(), "a futile reclaim outlived a present");
    Expect(ReleaseTextureCaches(0) == 0, "a reclaim of no bytes released memory");
    Expect(!TextureCacheReclaimFutile(), "a reclaim of no bytes was counted as futile");
    Expect(ReleaseTextureCaches(MiB) == 0, "empty texture caches released memory after a present");
    Expect(TextureCacheReclaimFutile(), "a reclaim over empty caches after a present was not futile");
    std::this_thread::sleep_for(FutileReclaimWindow + std::chrono::milliseconds(20));
    Expect(!TextureCacheReclaimFutile(), "a futile reclaim outlived its window");
}

VkDeviceSize ceilingReturned = 0;

VkDeviceSize mockCeiling(const Context&, std::uint32_t heap) {
    return heap == 0 ? ceilingReturned : 0;
}

void RefusedCeilingTests() {
    const std::uint32_t both = (1u << HostType) | (1u << DeviceType);
    Context context = mockContext(true);
    context.refusedHeapCeiling = mockCeiling;
    reportedBudget = 3000 * MiB;
    reportedUsage = 2000 * MiB;
    ceilingReturned = 0;
    Expect(context.DeviceMemoryType(both, 64 * MiB, "test") == DeviceType, "an allocation within the budget left device-local memory without a learned ceiling");
    ceilingReturned = 2010 * MiB;
    Expect(context.DeviceMemoryType(both, 64 * MiB, "test") == HostType, "an allocation past the ceiling a refusal taught stayed in device-local memory");
    Expect(context.DeviceMemoryType(both, 8 * MiB, "test") == DeviceType, "an allocation below the learned ceiling left device-local memory");
    ceilingReturned = 3500 * MiB;
    Expect(context.DeviceMemoryType(both, 64 * MiB, "test") == DeviceType, "a learned ceiling above the reported budget lowered it");
    ceilingReturned = 0;
}

void RefusedFallbackTests() {
    const auto context = mockContext(true);
    const std::uint32_t both = (1u << HostType) | (1u << DeviceType);
    const auto refused = context.RefusedDeviceMemoryFallback(both, DeviceType);
    Expect(refused.has_value() && *refused == HostType, "a device-local type out of device memory did not fall back to the host type");
    Expect(!context.RefusedDeviceMemoryFallback(both, HostType).has_value(), "a host type out of memory was given a fallback");
    Expect(!context.RefusedDeviceMemoryFallback(1u << DeviceType, DeviceType).has_value(), "a fallback was chosen outside the allowed type mask");
    Expect(!context.RefusedDeviceMemoryFallback(both, 7).has_value(), "an unknown memory type was given a fallback");
}

void BufferPlacementTests() {
    constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    Context context = mockContext(true);
    context.memory.memoryTypeCount = 4;
    context.memory.memoryTypes[0] = {coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    context.memory.memoryTypes[1] = {coherent, 1};
    context.memory.memoryTypes[2] = {coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0};
    context.memory.memoryTypes[3] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 1};
    reportedBudget = 3000 * MiB;
    reportedUsage = 2936 * MiB;
    reportedHostBudget = 8000 * MiB;
    reportedHostUsage = 2000 * MiB;
    const auto choose = [&](const char* what, std::uint32_t mask, VkMemoryPropertyFlags flags, std::uint32_t expected) {
        const auto type = context.BufferMemoryType(mask, flags, 64 * MiB);
        Expect(type == expected, std::string(what) + ": chose " + std::to_string(type) + ", expected " + std::to_string(expected));
    };
    choose("coherent buffer fits exactly in preferred cached heap", 15, coherent, 0);
    ++reportedUsage;
    choose("coherent buffer moves before allocating from a full heap", 15, coherent, 1);
    choose("buffer respects its allowed type mask", 5, coherent, 0);
    choose("another type on the same full heap is not an alternative", 7, coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    choose("noncoherent alternative is not eligible", 9, coherent, 0);
    choose("explicit device-local requirement is preserved", 15, coherent | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    choose("uncached-only coherent buffer preserves its type", 2, coherent, 1);
    reportedHostUsage = reportedHostBudget - 64 * MiB + 1;
    choose("alternative must have enough reported room", 15, coherent, 0);
    reportedHostBudget = 0;
    choose("unreported alternative budget is not assumed free", 15, coherent, 0);
    reportedHostBudget = 8000 * MiB;
    reportedHostUsage = 2000 * MiB;
    reportedBudget = 0;
    choose("unreported preferred budget retains cached preference", 15, coherent, 0);
    reportedBudget = 3000 * MiB;
    context.memoryProperties2 = nullptr;
    choose("no budget query retains cached preference", 15, coherent, 0);
    context.memoryProperties2 = mockMemoryProperties2;
    context.physical = VK_NULL_HANDLE;
    choose("no physical device retains cached preference", 15, coherent, 0);
}

void HeadroomTests() {
    Context context = mockContext(true);
    const auto check = [&](const char* what, std::uint32_t type, std::uint64_t budget, std::uint64_t usage, std::uint64_t bytes, bool expected) {
        reportedBudget = budget;
        reportedUsage = usage;
        Expect(context.DeviceLocalHeadroom(type, bytes) == expected, std::string(what) + ": expected " + (expected ? "room" : "no room"));
    };
    check("room below seven eighths of the budget", DeviceType, 4096 * MiB, 3000 * MiB, 64 * MiB, true);
    check("allocation ends at seven eighths exactly", DeviceType, 4096 * MiB, 3520 * MiB, 64 * MiB, true);
    check("allocation crosses seven eighths", DeviceType, 4096 * MiB, 3521 * MiB, 64 * MiB, false);
    check("usage already past seven eighths", DeviceType, 4096 * MiB, 3700 * MiB, 1 * MiB, false);
    check("budget not reported", DeviceType, 0, 4000 * MiB, 64 * MiB, true);
    check("a host memory type is not held to the device-local budget", HostType, 4096 * MiB, 4000 * MiB, 64 * MiB, true);
    check("unknown memory type", 7, 4096 * MiB, 4000 * MiB, 64 * MiB, true);
    context.memoryProperties2 = nullptr;
    check("no VK_EXT_memory_budget", DeviceType, 4096 * MiB, 4000 * MiB, 64 * MiB, true);
    context.memoryProperties2 = mockMemoryProperties2;
    context.memory.memoryHeaps[0].flags = 0;
    check("no device-local heap", DeviceType, 4096 * MiB, 4000 * MiB, 64 * MiB, true);
}

void SecondHeapHeadroomTests() {
    Context context = mockContext(true);
    context.memory.memoryHeapCount = 3;
    context.memory.memoryHeaps[2] = {4096 * MiB, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    context.memory.memoryTypeCount = 3;
    context.memory.memoryTypes[SecondDeviceType] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 2};
    reportedBudget = 4096 * MiB;
    reportedSecondBudget = 4096 * MiB;
    const auto check = [&](const char* what, std::uint32_t mask, std::uint64_t firstUsage, std::uint64_t secondUsage, std::uint32_t expectedType, bool expected) {
        reportedUsage = firstUsage;
        reportedSecondUsage = secondUsage;
        const auto type = context.BufferMemoryType(mask, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 64 * MiB);
        Expect(type == expectedType, std::string(what) + ": chose memory type " + std::to_string(type) + ", expected " + std::to_string(expectedType));
        Expect(context.DeviceLocalHeadroom(type, 64 * MiB) == expected, std::string(what) + ": expected " + (expected ? "room" : "no room"));
    };
    const std::uint32_t secondOnly = (1u << HostType) | (1u << SecondDeviceType);
    const std::uint32_t all = secondOnly | (1u << DeviceType);
    check("mask excludes the first device-local type, first heap under pressure", secondOnly, 3700 * MiB, 1000 * MiB, SecondDeviceType, true);
    check("mask excludes the first device-local type, second heap under pressure", secondOnly, 1000 * MiB, 3700 * MiB, SecondDeviceType, false);
    check("mask allows both device-local types, first heap under pressure", all, 3700 * MiB, 1000 * MiB, DeviceType, false);
    check("mask allows both device-local types, second heap under pressure", all, 1000 * MiB, 3700 * MiB, DeviceType, true);
    reportedSecondBudget = 0;
    reportedSecondUsage = 0;
}

}

int main() {
    try {
        const std::uint32_t both = (1u << HostType) | (1u << DeviceType);
        Choose("no VK_EXT_memory_budget", false, 0, 0, both, DeviceType);
        Choose("budget not reported", true, 0, 4000 * MiB, both, DeviceType);
        Choose("room in the device-local budget", true, 3000 * MiB, 2000 * MiB, both, DeviceType);
        Choose("allocation fills the budget exactly", true, 3000 * MiB, 2936 * MiB, both, DeviceType);
        Choose("allocation exceeds the budget", true, 3000 * MiB, 2937 * MiB, both, HostType);
        Choose("usage already over the budget", true, 2952 * MiB, 3633 * MiB, both, HostType);
        Choose("no host type allowed", true, 3000 * MiB, 3000 * MiB, 1u << DeviceType, DeviceType);
        BufferPlacementTests();
        RefusedFallbackTests();
        ReclaimTests();
        FutileReclaimTests();
        RefusedCeilingTests();
        HeadroomTests();
        SecondHeapHeadroomTests();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    if (failures != 0) return 1;
    std::puts("device memory placement tests passed");
    return 0;
}
