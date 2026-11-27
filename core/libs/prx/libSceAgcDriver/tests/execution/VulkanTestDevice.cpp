#include "VulkanTestDevice.hpp"
#include "HeapSampleKernel.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BindlessHeap.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineBinaryStore.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

enum class Outcome { Device, Skipped, Unavailable, Failed };

void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    if (_putenv_s(name, value) != 0) throw std::runtime_error(std::string("cannot set ") + name);
#else
    if ((*value == '\0' ? unsetenv(name) : setenv(name, value, 1)) != 0) throw std::runtime_error(std::string("cannot set ") + name);
#endif
}

void Require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef _WIN32
bool HighIntegrity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    alignas(TOKEN_MANDATORY_LABEL) unsigned char label[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    const bool queried = GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(label), &size) != FALSE;
    CloseHandle(token);
    if (!queried) return false;
    const PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1u) >= SECURITY_MANDATORY_HIGH_RID;
}
#endif

Outcome Open(std::string& message) {
    message.clear();
    try {
        const auto device = OpenVulkanTestDevice();
        if (device) message = device->DeviceName();
        return device ? Outcome::Device : Outcome::Skipped;
    } catch (const AgcDriver::VulkanUnavailable& error) {
        message = error.what();
        return Outcome::Unavailable;
    } catch (const std::exception& error) {
        message = error.what();
        return Outcome::Failed;
    }
}

int DevicePresentTests() {
    SetVariable("ANYPS5_GPU", "");
    std::string message;
    const auto healthy = Open(message);
    if (healthy == Outcome::Skipped) return VulkanTestSkipped;
    Require(healthy == Outcome::Device, "opening the default device failed: " + message);
    const std::string name = message;

    SetVariable("ANYPS5_REQUIRE_VULKAN", "1");
    Require(Open(message) == Outcome::Device, "ANYPS5_REQUIRE_VULKAN=1 did not open " + name + ": " + message);

    SetVariable("ANYPS5_REQUIRE_VULKAN", "");
    SetVariable("ANYPS5_GPU", "no such device 5c1e0b7a");
    const auto failed = Open(message);
    Require(failed != Outcome::Skipped, "a device construction failure on a host with " + name + " was reported as a skip");
    Require(failed == Outcome::Failed, "a device construction failure on a host with " + name + " was reported as no usable Vulkan device: " + message);
    Require(message.find("ANYPS5_GPU=\"no such device 5c1e0b7a\"") != std::string::npos && message.find(name) != std::string::npos, "unexpected error: " + message);
    return 0;
}

int NoDriverTests() {
    SetVariable("ANYPS5_REQUIRE_VULKAN", "");
    SetVariable("ANYPS5_GPU", "");
    std::string message;
    const auto optional = Open(message);
#ifdef _WIN32
    if (optional == Outcome::Device && HighIntegrity()) {
        std::cout << "skipped, the Vulkan loader ignores VK_DRIVER_FILES in an elevated process and opened " << message << '\n';
        return VulkanTestSkipped;
    }
#endif
    Require(optional == Outcome::Skipped, "without a Vulkan driver the optional device was not skipped: " + message);

    SetVariable("ANYPS5_REQUIRE_VULKAN", "1");
    const auto required = Open(message);
    Require(required != Outcome::Skipped, "without a Vulkan driver ANYPS5_REQUIRE_VULKAN=1 was reported as a skip");
    Require(required == Outcome::Unavailable, "without a Vulkan driver ANYPS5_REQUIRE_VULKAN=1 failed with an unexpected error: " + message);
    return 0;
}

int PipelineBinaryStartupTests() {
    std::filesystem::path root;
    bool created = false;
    for (unsigned attempt = 0; attempt < 8 && !created; ++attempt) {
        root = std::filesystem::temp_directory_path() / ("aps5-binary-startup-" + std::to_string(std::random_device{}()));
        created = std::filesystem::create_directory(root);
    }
    Require(created, "cannot create an exclusive pipeline binary startup cache directory after 8 attempts");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    SetVariable("ANYPS5_SHADER_CACHE_DIR", root.string().c_str());
    SetVariable("ANYPS5_NO_SHADER_CACHE", "0");
    SetVariable("APS5_NO_PIPELINE_BINARIES", "");
    const auto device = OpenVulkanTestDevice();
    if (!device) return VulkanTestSkipped;
    const auto status = device->PipelineBinariesStatus();
    if (status == AgcDriver::VulkanDevice::PipelineBinaryStatus::Unavailable) {
        std::cout << "skipped, the device lacks VK_KHR_pipeline_binary\n";
        return VulkanTestSkipped;
    }
    const auto policy = AgcDriver::Graphics::PipelineBinaryStore::PolicyForDriver(device->DriverId());
    const bool gated = policy == AgcDriver::Graphics::PipelineBinaryPolicy::DisabledByDriver;
    const auto expected = gated ? AgcDriver::VulkanDevice::PipelineBinaryStatus::DisabledByDriver : AgcDriver::VulkanDevice::PipelineBinaryStatus::Ready;
    Require(status == expected, "VK_KHR_pipeline_binary is present but the store has an unexpected disable reason");
    std::cout << "pipeline binary startup: " << (gated ? "DisabledByDriver; device started successfully" : "global-key query completed successfully; store ready") << "; device=" << device->DeviceName() << "; driver=" << device->DriverInfo() << '\n';
    return 0;
}

alignas(256) std::array<std::uint8_t, 1024> BinaryTexture{};
alignas(4096) std::array<float, 1024> BinaryOutput{};

int PipelineBinaryRestoreTests(const char* root, std::string_view mode, const std::string& pass) {
    Require(mode == "heap" || mode == "layout", "expected heap or layout, not " + std::string(mode));
    const bool heap = mode == "heap";
    SetVariable("ANYPS5_SHADER_CACHE_DIR", root);
    SetVariable("ANYPS5_NO_SHADER_CACHE", "0");
    SetVariable("APS5_NO_PIPELINE_JOURNAL", "1");
    SetVariable("APS5_PIPELINE_BINARY_MIN_WORDS", "0");
    SetVariable("APS5_PIPELINE_BINARY_MIN_MS", "0");
    SetVariable("APS5_NO_PIPELINE_BINARIES", "");
    SetVariable("APS5_GPU_HEAP", heap ? "1" : "");
    FillTexels(BinaryTexture, SampleTexel);
    BinaryOutput.fill(-1.0f);
    const auto device = OpenVulkanTestDevice();
    if (!device) return VulkanTestSkipped;
    if (heap && SkipWithoutDescriptorHeap()) return VulkanTestSkipped;
    const auto status = device->PipelineBinariesStatus();
    if (status == AgcDriver::VulkanDevice::PipelineBinaryStatus::Unavailable) {
        std::cout << "skipped, the device lacks VK_KHR_pipeline_binary\n";
        return VulkanTestSkipped;
    }
    if (status == AgcDriver::VulkanDevice::PipelineBinaryStatus::DisabledByDriver) {
        std::cout << "skipped, pipeline binaries are disabled for this driver on this platform\n";
        return VulkanTestSkipped;
    }
    Require(status == AgcDriver::VulkanDevice::PipelineBinaryStatus::Ready, "the pipeline binary store is not ready");
    const auto result = CompileSampleKernel(*device, BinaryTexture.data(), BinaryOutput.data(), false);
    Require(result.gpuResolvedImages == heap, heap ? "the sampling compute stage is not GPU mode" : "the sampling compute stage took the heap without it");
    const auto heapStages = AgcDriver::Graphics::HeapComputeStages();
    device->Dispatch(result, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(SampleCode.data()));
    device->WaitIdle();
    RequireSampled(BinaryOutput.data(), SampleTexel, pass + ": the sampled texture", heap);
    Require(device->ComputePipelinesCreated() == 1, pass + ": the dispatch did not create exactly one compute pipeline");
    Require(device->DescriptorHeapComputePipelines() == (heap ? 1u : 0u), pass + (heap ? ": the compute pipeline was not built on the descriptor heap" : ": a compute pipeline was built on the descriptor heap"));
    if (heap) Require(AgcDriver::Graphics::HeapComputeStages() > heapStages, pass + ": the heap did not take the sampling compute stage");
    std::cout << "pipeline binary " << pass << ": the " << (heap ? "descriptor heap" : "pipeline layout") << " compute pipeline sampled the texture; device=" << device->DeviceName() << "; driver=" << device->DriverInfo() << '\n';
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        Require(mode.empty() || mode == "--no-driver" || mode == "--pipeline-binary-startup" || (mode == "--pipeline-binary-restore" && argc == 5), "unknown mode " + std::string(mode));
        const int result = mode == "--pipeline-binary-restore" ? PipelineBinaryRestoreTests(argv[2], argv[3], argv[4]) : mode == "--pipeline-binary-startup" ? PipelineBinaryStartupTests() : mode.empty() ? DevicePresentTests() : NoDriverTests();
        if (result == 0) std::cout << "Vulkan test device tests passed\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
