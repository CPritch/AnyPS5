#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CPUIMAGETABLETRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_CPUIMAGETABLETRACE_HPP

#include "IntermediateRepresentation/IrMetadata/ResourcePlan.hpp"
#include "Recompiler.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver::Graphics {

enum class DeviceImageService { NoTable, Serves, FallenBack };

struct CpuImageTableGate {
    bool known = false;
    bool requested = false;
    bool gpuModeStage = false;
    bool int64 = false;
    bool bufferAddresses = false;
};

struct CpuImageTableImage {
    std::uint32_t resource = 0;
    std::uint32_t table = 0;
    std::string column;
    std::string access;
    std::string samplers;
    std::vector<std::string> reasons;
};

[[nodiscard]] bool CpuImageTablesTraced();
[[nodiscard]] CpuImageTableGate CpuImageTableGateOf(const ShaderRecompiler::RecompileRequest& request);
[[nodiscard]] std::vector<CpuImageTableImage> DescribeCpuImageTables(const ShaderRecompiler::IrResourcePlan& plan);
void NoteCpuImageTablePlan(const ShaderRecompiler::RecompileRequest& request, const std::shared_ptr<const ShaderRecompiler::IrResourcePlan>& plan);
void TraceCpuImageTable(const std::function<DeviceImageService(std::uint64_t codeAddress)>& service, ShaderRecompiler::ShaderStage stage, const ShaderRecompiler::RecompileResult& program, const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> map, bool heap);

}

#endif
