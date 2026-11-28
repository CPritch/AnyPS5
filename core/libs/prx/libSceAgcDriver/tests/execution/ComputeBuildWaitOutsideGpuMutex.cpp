#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BindlessHeap.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineJournal.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include "ExecutionHelpers.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t Lanes = 32;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t Unwritten = 0xcafebabeu;
constexpr std::uint32_t Constant = 0x1000u;

alignas(256) std::array<std::uint32_t, 12> ReaderCode{0xf4040100u, 0xfa000000u, 0xf4080201u, 0xfa000000u, 0xbf8cc07fu, 0xf4000182u, 0xfa000000u, 0xbf8cc07fu, 0x7e000206u, 0xe0700000u, 0x80020000u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, 13> AddressCode{0xf4000200u, 0xfa000000u, 0x34020082u, 0xdc308000u, 0x03020001u, 0xbf8cc07fu, 0x7e040208u, 0x4a040481u, 0xbf8c3f70u, 0x4a040503u, 0xe0701000u, 0x80010201u, 0xbf810000u};

struct Slot {
    alignas(256) std::array<std::uint32_t, 4> value{};
    alignas(256) std::array<std::uint32_t, 2> table{};
    alignas(256) std::array<std::uint32_t, 4> output{};
    alignas(256) std::array<std::uint32_t, 4> outputDescriptor{};
    alignas(256) volatile std::uint32_t done = 0;
};

std::array<Slot, 2> Slots;
alignas(256) volatile std::uint32_t Gate = 0;
alignas(256) volatile std::uint32_t AddressDone = 0;

std::uint32_t* Constants = nullptr;
std::uint32_t* Input = nullptr;
std::uint32_t* Output = nullptr;
std::uint32_t* Arguments = nullptr;

std::atomic<std::uint64_t> HeldProgram{0};
std::atomic<int> HeldBuilds{0};
std::atomic<bool> BuildReleased{false};
std::atomic<bool> BlockerStarted{false};
std::atomic<bool> BlockerReleased{false};

void HoldBuild(std::uint64_t programAddress) {
    if (programAddress != HeldProgram.load(std::memory_order_acquire)) return;
    HeldBuilds.fetch_add(1, std::memory_order_acq_rel);
    while (!BuildReleased.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void ReleaseBuild() {
    BuildReleased.store(true, std::memory_order_release);
}

void ReleaseBlocker() {
    BlockerReleased.store(true, std::memory_order_release);
}

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::uint32_t Low(const volatile void* pointer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

std::uint32_t High(const volatile void* pointer) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)) >> 32u);
}

std::uint32_t* NewBlock() {
#ifdef _WIN32
    auto* block = static_cast<std::uint32_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    auto* block = static_cast<std::uint32_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
    Require(block != nullptr, "cannot allocate a guest block");
    std::memset(block, 0, BlockBytes);
    return block;
}

void AddBlock(std::uint32_t* block) {
    GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
}

void SetShaderRegister(std::vector<std::uint32_t>& words, std::uint32_t offset, std::uint32_t value) {
    words.insert(words.end(), {0xc0017600u, offset, value});
}

void SetProgram(std::vector<std::uint32_t>& words, const void* code, std::array<std::uint32_t, 3> threads, std::initializer_list<std::uint32_t> userData) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code));
    SetShaderRegister(words, 0x20c, static_cast<std::uint32_t>(address >> 8u));
    SetShaderRegister(words, 0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (std::uint32_t axis = 0; axis < 3; ++axis) SetShaderRegister(words, 0x207 + axis, threads[axis]);
    SetShaderRegister(words, 0x212, 0);
    SetShaderRegister(words, 0x213, static_cast<std::uint32_t>(userData.size()) << 1u);
    std::uint32_t offset = 0x240;
    for (const auto value : userData) SetShaderRegister(words, offset++, value);
}

void WriteData(std::vector<std::uint32_t>& words, volatile std::uint32_t* target, std::uint32_t value) {
    words.insert(words.end(), {0xc0033700u, 0x00100200u, Low(target), High(target), value});
}

void DispatchSlot(std::vector<std::uint32_t>& words, Slot& slot, std::uint32_t value) {
    slot.value[0] = value;
    slot.table = {Low(slot.value.data()), High(slot.value.data())};
    slot.outputDescriptor = {Low(slot.output.data()), High(slot.output.data()) & 0xffffu, static_cast<std::uint32_t>(sizeof(slot.output)), 0x31016facu};
    SetProgram(words, ReaderCode.data(), {1, 1, 1}, {Low(slot.table.data()), High(slot.table.data()), Low(slot.outputDescriptor.data()), High(slot.outputDescriptor.data())});
    words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
    WriteData(words, &slot.done, 1);
}

void DispatchAddress(std::vector<std::uint32_t>& words, bool indirect) {
    const auto output = BufferDescriptor(Output, Lanes * sizeof(std::uint32_t));
    SetProgram(words, AddressCode.data(), {Lanes, 1, 1}, {Low(Constants), High(Constants), Low(Input), High(Input), output[0], output[1], output[2], output[3]});
    if (indirect) words.insert(words.end(), {0xc0021600u, Low(Arguments), High(Arguments), 0x8041u});
    else words.insert(words.end(), {0xc0031500u, 1, 1, 1, 0x8041u});
    WriteData(words, &AddressDone, 1);
}

template <typename Ready>
bool WaitFor(Ready ready, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

}

int main(int argc, char** argv) {
    using AgcDriver::VulkanDevice;
    std::thread locker;
    const bool journal = argc == 2 && std::string_view(argv[1]) == "compute-queue-journal";
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code error;
            if (!directory.empty()) std::filesystem::remove_all(directory, error);
        }
    } cleanup;
    if (journal) {
        cleanup.directory = std::filesystem::temp_directory_path() / ("aps5-compute-build-wait-journal-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(cleanup.directory);
        SetEnvironment("ANYPS5_NO_SHADER_CACHE", "0");
        SetEnvironment("ANYPS5_SHADER_CACHE_DIR", cleanup.directory.string());
        SetEnvironment("APS5_NO_PIPELINE_JOURNAL", "0");
    }
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const bool indirect = argc == 2 && std::string_view(argv[1]) == "indirect";
        const bool unprebuilt = argc == 2 && std::string_view(argv[1]) == "unprebuilt";
        const bool computeQueue = journal || (argc == 2 && std::string_view(argv[1]) == "compute-queue-wait");
        Require(argc == 1 || indirect || unprebuilt || computeQueue, "unknown fixture mode");
        Constants = NewBlock();
        Input = NewBlock();
        Output = NewBlock();
        Arguments = NewBlock();
        Constants[0] = Constant;
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            Input[lane] = lane * 3u;
            Output[lane] = Unwritten;
        }
        Arguments[0] = 1;
        Arguments[1] = 1;
        Arguments[2] = 1;
        for (auto* block : {Constants, Input, Output, Arguments}) AddBlock(block);
        HeldProgram.store(reinterpret_cast<std::uintptr_t>(AddressCode.data()), std::memory_order_release);
        VulkanDevice::SetComputePipelineBuildHookForTest(&HoldBuild);

        std::vector<std::uint32_t> warm;
        DispatchSlot(warm, Slots[1], 0x9abcu);
        Packet warmPacket{warm.data(), static_cast<std::uint32_t>(warm.size()), 0, {}};
        Require(sceAgcDriverSubmitAcb(0x28, &warmPacket) == 0, "the warm-up submit failed");
        Require(WaitFor([] { return Slots[1].done != 0; }, std::chrono::seconds(60)), "the warm-up dispatch did not finish");
        Require(Slots[1].output[0] == 0x9abcu, "the warm-up dispatch stored " + std::to_string(Slots[1].output[0]));

        std::vector<std::uint32_t> words;
        if (journal) AgcDriver::Graphics::FlushPipelineJournal();
        const auto journalBefore = AgcDriver::Graphics::PipelineJournalTotals().recordedCompute;
        if (computeQueue) {
            AgcDriver::DriverDetail::SubmitComputePrebuild([] {
                BlockerStarted.store(true, std::memory_order_release);
                while (!BlockerReleased.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
            Require(WaitFor([] { return BlockerStarted.load(std::memory_order_acquire); }, std::chrono::seconds(20)), "the prebuild pool did not start the compute prebuild that no dispatch waits for");
            words.insert(words.end(), {0xc0053c00u, 0x13u, Low(&Gate), High(&Gate), 1u, 0xffffffffu, 0x19u});
            DispatchAddress(words, false);
            Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
            Require(sceAgcDriverSubmitAcb(0x28, &packet) == 0, "the compute queue submit failed");
        } else {
            DispatchSlot(words, Slots[0], 0x1234u);
            words.insert(words.end(), {0xc0053c00u, 0x13u, Low(&Gate), High(&Gate), 1u, 0xffffffffu, 0x19u});
            DispatchAddress(words, indirect);
            Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
            Require(sceAgcDriverSubmitDcb(&packet) == 0, "the graphics queue submit failed");
            Require(WaitFor([] { return Slots[0].done != 0; }, std::chrono::seconds(60)), "the dispatch before the wait did not finish");
            Require(Slots[0].output[0] == 0x1234u, "the dispatch before the wait stored " + std::to_string(Slots[0].output[0]));
        }
        if (unprebuilt) Require(HeldBuilds.load(std::memory_order_acquire) == 0, "the pipeline of the dispatch behind the wait was built ahead with the queue 0 prebuild off");
        else if (computeQueue) Require(WaitFor([] { return HeldBuilds.load(std::memory_order_acquire) != 0; }, std::chrono::seconds(20)), "the compute pipeline of a dispatch behind a compute queue's unsatisfied wait did not start building on the prebuild pool while the queue waited, with an unawaited compute prebuild holding the pool's other budgeted slot");
        else Require(WaitFor([] { return HeldBuilds.load(std::memory_order_acquire) != 0; }, std::chrono::seconds(20)), "the prebuild pool did not start building the pipeline of the dispatch behind the wait");
        if (journal) {
            Require(AgcDriver::Graphics::PipelineJournalEnabled(), "the pipeline journal is off in the journal fixture");
            AgcDriver::Graphics::FlushPipelineJournal();
            const auto recorded = AgcDriver::Graphics::PipelineJournalTotals().recordedCompute - journalBefore;
            Require(recorded != 0, "the compute pipeline of a dispatch resolved ahead was not recorded in the pipeline journal before its build ran");
        }

        const auto before = VulkanDevice::ComputePipelineCounts();
        Gate = 1;
        if (unprebuilt) Require(WaitFor([] { return HeldBuilds.load(std::memory_order_acquire) != 0; }, std::chrono::seconds(20)), "the queue 0 dispatch did not build its compute pipeline");
        else Require(WaitFor([&] { return VulkanDevice::ComputePipelineCounts().awaited > before.awaited; }, std::chrono::seconds(20)), "the queue 0 dispatch did not wait for its compute pipeline's build on the prebuild pool");
        std::atomic<bool> locked{false};
        locker = std::thread([&locked] {
            std::lock_guard gpuLock(AgcDriver::GuestMemory::GpuMutex());
            locked.store(true, std::memory_order_release);
        });
        const bool lockedWhileWaiting = WaitFor([&locked] { return locked.load(std::memory_order_acquire); }, std::chrono::seconds(5));
        const bool waitedForRelease = AddressDone == 0;
        ReleaseBuild();
        locker.join();
        Require(lockedWhileWaiting, unprebuilt ? "another thread could not take the GPU mutex within 5 s while a queue 0 dispatch built its compute pipeline" : "another thread could not take the GPU mutex within 5 s while a queue 0 dispatch waited for its compute pipeline's build on the prebuild pool");
        Require(waitedForRelease, "the dispatch behind the wait finished while its compute pipeline's build was held");

        Require(WaitFor([] { return AddressDone != 0; }, std::chrono::seconds(60)), "the dispatch behind the wait did not finish");
        AgcDriverWaitIdle_nid_postfix();
        AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output), Lanes * sizeof(std::uint32_t));
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            const auto expected = Constant + 1u + lane * 3u;
            Require(Output[lane] == expected, "lane " + std::to_string(lane) + " of the dispatch behind the wait stored " + Hex(Output[lane]) + ", expected " + Hex(expected));
        }
        const auto after = VulkanDevice::ComputePipelineCounts();
        Require(after.awaitedUnderGpuMutex == before.awaitedUnderGpuMutex, std::to_string(after.awaitedUnderGpuMutex - before.awaitedUnderGpuMutex) + " waits for another thread's compute pipeline build were made under the GPU mutex");
        Require(after.underGpuMutex == before.underGpuMutex, "a compute pipeline was created under the GPU mutex");
        Require(HeldBuilds.load(std::memory_order_acquire) == 1, "the pipeline of the dispatch behind the wait was built " + std::to_string(HeldBuilds.load()) + " times");
        ReleaseBlocker();
        VulkanDevice::SetComputePipelineBuildHookForTest(nullptr);
        std::cout << "compute build wait outside GPU mutex test passed (" << (indirect ? "indirect" : unprebuilt ? "unprebuilt" : journal ? "compute queue, journal" : computeQueue ? "compute queue" : "direct") << " dispatch, GPU-resolved images " << (AgcDriver::Graphics::DescriptorHeapEnabled() ? "on" : "off") << ")\n";
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ReleaseBuild();
        ReleaseBlocker();
        Gate = 1;
        if (locker.joinable()) locker.join();
        VulkanDevice::SetComputePipelineBuildHookForTest(nullptr);
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
