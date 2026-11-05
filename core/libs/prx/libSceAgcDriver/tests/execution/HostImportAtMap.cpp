#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <thread>
#include <atomic>
#include <chrono>
#include <exception>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t AllocationBytes = BlockBytes * 4;
constexpr std::size_t ReservationBytes = AllocationBytes + BlockBytes * 2;

class GuestAllocation {
public:
    GuestAllocation() {
#ifdef _WIN32
        allocation = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, ReservationBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        allocation = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, ReservationBytes));
#endif
        Require(allocation != nullptr, "host import at map: cannot allocate the guest memory");
        memory = allocation + BlockBytes;
        std::memset(memory, 0, AllocationBytes);
        Register();
    }

    ~GuestAllocation() {
        if (registered) Unregister();
#ifdef _WIN32
        VirtualFree(allocation, 0, MEM_RELEASE);
#else
        std::free(allocation);
#endif
    }

    GuestAllocation(const GuestAllocation&) = delete;
    GuestAllocation& operator=(const GuestAllocation&) = delete;

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(memory)); }

    void Register() {
        GuestAllocations::Mutation().Add(memory, AllocationBytes, true, true, true);
        registered = true;
    }

    void Unregister() {
        GuestAllocations::Mutation().Remove(memory);
        registered = false;
    }

    GuestAllocations::Mapped Ranges() const {
        GuestAllocations::Mapped ranges;
        for (const auto& range : GuestAllocations::GuestAllocationsAcquire_nid_postfix()) {
            if (range->address == Address()) ranges.push_back(range);
        }
        Require(ranges.size() == 1, "host import at map: the allocation is not registered");
        return ranges;
    }

private:
    std::uint8_t* allocation = nullptr;
    std::uint8_t* memory = nullptr;
    bool registered = false;
};

void Reconcile(AgcDriver::VulkanDevice& device, const GuestAllocation& probe) {
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    const std::array<std::uint32_t, 4> pattern{5u, 6u, 7u, 8u};
    Require(device.FillBuffer(probe.Address(), BlockBytes, pattern), "host import at map: the probe fill was not imported");
    device.WaitIdle();
}

void Import(AgcDriver::VulkanDevice& device, const GuestAllocations::Mapped& ranges, std::uint64_t generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix()) {
    if (device.ImportGuestMemory(ranges, generation, false)) return;
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    Require(device.ImportGuestMemory(ranges, generation, true), "host import at map: the import under the GPU lock refused");
}

bool Imported(VkDevice device, std::uint64_t address, std::size_t bytes) {
    AgcDriver::Graphics::Context probe{};
    probe.device = device;
    probe.hostImportAlignment = 1;
    return AgcDriver::Graphics::HostImportCovers(probe, address, bytes);
}


void LeaseWaitLockTests(AgcDriver::VulkanDevice& device, const GuestAllocation& probe) {
    if (!device.CanWaitUnlocked()) {
        std::puts("host import at map: no timeline semaphores, the lease wait lock tests are skipped");
        return;
    }
    const auto context = device.graphicsContext();
    const auto createSemaphore = context.Function<PFN_vkCreateSemaphore>("vkCreateSemaphore");
    const auto destroySemaphore = context.Function<PFN_vkDestroySemaphore>("vkDestroySemaphore");
    const auto signalSemaphore = context.Function<PFN_vkSignalSemaphoreKHR>("vkSignalSemaphoreKHR");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    VkSemaphore gate = VK_NULL_HANDLE;
    Require(createSemaphore(context.device, &info, nullptr, &gate) == VK_SUCCESS, "host import at map: cannot create the lease wait gate");
    const auto open = [&](std::uint64_t value) {
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signal.semaphore = gate;
        signal.value = value;
        Require(signalSemaphore(context.device, &signal) == VK_SUCCESS, "host import at map: cannot open the lease wait gate");
    };
    auto& mutex = AgcDriver::GuestMemory::GpuMutex();
    const std::array<std::uint32_t, 4> pattern{13u, 14u, 15u, 16u};
    const auto heldBatch = [&](std::uint64_t value) {
        Require(device.FillBuffer(probe.Address(), BlockBytes, pattern), "host import at map: the lease wait fill was not imported");
        auto* recorder = AgcDriver::Graphics::Recorder::Active();
        Require(recorder != nullptr, "host import at map: no active recorder for the lease wait");
        recorder->Commands();
        recorder->HoldNextSubmit(gate, value);
        recorder->Submit();
        return std::pair{recorder, recorder->Submissions()};
    };
    std::uint64_t gateValue = 1;
    {
        std::unique_lock gpu(mutex);
        const auto [recorder, serial] = heldBatch(gateValue);
        {
            std::lock_guard nested(mutex);
            Require(!recorder->FinishUpToUnlocked(serial), "host import at map: a nested GPU lock was given up around a lease wait");
        }
        Require(recorder->CompletedThrough() < serial, "host import at map: the refused wait finished a batch its gate still holds");
        open(gateValue++);
        recorder->FinishUpTo(serial);
    }
    if (std::getenv("APS5_NO_LEASE_WAIT_UNLOCKED") != nullptr) {
        std::unique_lock gpu(mutex);
        const auto [recorder, serial] = heldBatch(gateValue);
        Require(!recorder->FinishUpToUnlocked(serial), "host import at map: APS5_NO_LEASE_WAIT_UNLOCKED left the lease wait unlocked");
        Require(recorder->CompletedThrough() < serial, "host import at map: the refused wait finished a batch its gate still holds");
        open(gateValue++);
        recorder->FinishUpTo(serial);
    } else {
        std::atomic<bool> holding{false};
        std::atomic<bool> finished{false};
        std::exception_ptr failure;
        AgcDriver::Graphics::Recorder* waiting = nullptr;
        std::uint64_t waitedSerial = 0;
        std::thread waiter([&] {
            try {
                std::lock_guard gpu(mutex);
                const auto [recorder, serial] = heldBatch(gateValue);
                waiting = recorder;
                waitedSerial = serial;
                holding = true;
                Require(recorder->FinishUpToUnlocked(serial), "host import at map: the lease wait refused to release the GPU lock");
                Require(mutex.HeldByThisThread(), "host import at map: the unlocked lease wait did not retake the GPU lock");
                Require(recorder->CompletedThrough() >= serial, "host import at map: the unlocked lease wait did not finish its batch");
                finished = true;
            } catch (...) {
                failure = std::current_exception();
                holding = true;
            }
        });
        while (!holding) std::this_thread::yield();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        bool taken = false;
        while (!taken && !failure && std::chrono::steady_clock::now() < deadline) {
            taken = mutex.try_lock();
            if (!taken) std::this_thread::yield();
        }
        const bool finishedWhileGated = finished.load();
        if (taken) mutex.unlock();
        open(gateValue++);
        waiter.join();
        if (failure) std::rethrow_exception(failure);
        Require(taken, "host import at map: the lease wait never released the GPU lock while its batch was held");
        Require(!finishedWhileGated, "host import at map: the lease wait finished although its gate was closed");
        Require(finished.load() && waiting != nullptr && waitedSerial != 0, "host import at map: the lease wait did not finish after its gate opened");
    }
    device.WaitIdle();
    destroySemaphore(context.device, gate, nullptr);
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_PIN_WAIT_MS", "2000");
#else
        setenv("APS5_PIN_WAIT_MS", "2000", 1);
#endif
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestAllocation first;
        Import(*device, first.Ranges());
        const bool importedFirst = Imported(device->Device(), first.Address(), AllocationBytes);
        first.Unregister();
        Require(!Imported(device->Device(), first.Address(), AllocationBytes), "host import at map: unmapping an allocation imported before any lookup kept its import");
        GuestAllocation probe;
        bool filled = false;
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            const std::array<std::uint32_t, 4> pattern{1u, 2u, 3u, 4u};
            filled = device->FillBuffer(probe.Address(), BlockBytes, pattern);
            device->WaitIdle();
        }
        if (!filled) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        Require(importedFirst, "host import at map: a mapping before any lookup was not imported");
        GuestAllocation guest;
        const auto handle = device->Device();
        Require(!Imported(handle, guest.Address(), AllocationBytes), "host import at map: the allocation was imported before anything asked for it");
        Import(*device, guest.Ranges());
        Require(Imported(handle, guest.Address(), AllocationBytes), "host import at map: the mapped allocation was not imported");

        GuestAllocation stale;
        Import(*device, stale.Ranges());
        stale.Unregister();
        GuestAllocation mapped;
        Import(*device, mapped.Ranges());
        Require(Imported(handle, mapped.Address(), AllocationBytes), "host import at map: a mapping made after another changed was not imported");

        GuestAllocation remapped;
        Import(*device, remapped.Ranges());
        remapped.Unregister();
        remapped.Register();
        Import(*device, remapped.Ranges());
        Require(!Imported(handle, remapped.Address(), AllocationBytes), "host import at map: a range mapped again was imported at map instead of on first use");
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            const std::array<std::uint32_t, 4> pattern{9u, 10u, 11u, 12u};
            Require(device->FillBuffer(remapped.Address(), BlockBytes, pattern), "host import at map: the first use of a range mapped again was not imported");
            device->WaitIdle();
        }
        GuestAllocation gone;
        const auto goneRanges = gone.Ranges();
        gone.Unregister();
        Import(*device, goneRanges);
        Require(!Imported(handle, gone.Address(), AllocationBytes), "host import at map: a mapping unregistered before its import was imported");
        gone.Register();

        GuestAllocation late;
        const auto lateRanges = late.Ranges();
        const auto lateAt = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        GuestAllocation newer;
        Reconcile(*device, newer);
        Import(*device, lateRanges, lateAt);
        Require(!Imported(handle, late.Address(), AllocationBytes), "host import at map: an import older than the last reconcile was made");

        GuestAllocation contended;
        std::atomic<bool> held{false};
        std::atomic<bool> release{false};
        std::thread holder([&] {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            held = true;
            while (!release) std::this_thread::yield();
        });
        while (!held) std::this_thread::yield();
        const bool unlocked = device->ImportGuestMemory(contended.Ranges(), GuestAllocations::GuestAllocationsGeneration_nid_postfix(), false);
        release = true;
        holder.join();
        Require(unlocked && Imported(handle, contended.Address(), AllocationBytes), "host import at map: a mapping on a device that already imports needed the GPU lock");
        LeaseWaitLockTests(*device, probe);
        std::puts("host import at map tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
