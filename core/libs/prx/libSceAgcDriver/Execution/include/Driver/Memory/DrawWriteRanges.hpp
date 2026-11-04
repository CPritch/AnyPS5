#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWWRITERANGES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAWWRITERANGES_HPP

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages, bool exact);
enum class DispatchUnbounded : std::uint8_t { None, Gds, AddressWrites, AddressReadOnly, ImageDescriptor, Special, Count };
std::vector<std::pair<std::uint64_t, std::uint64_t>> DispatchWriteRanges(const ShaderRecompiler::RecompileResult& compiled, bool& bounded, DispatchUnbounded* reason = nullptr);
bool ExactDrawWrites();
std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawColorRanges(const Graphics::State& graphics);
std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawDepthRanges(const Graphics::State& graphics);
void DrawDepthRanges(const Graphics::State& graphics, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges);
std::vector<std::pair<std::uint64_t, std::uint64_t>> QueuedDrawReadRanges(const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> stages);

}

#endif
