#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
}

int main() {
    const auto* first = reinterpret_cast<const void*>(&AgcDriver::GuestMemory::CodeOffset);
    const auto* second = reinterpret_cast<const void*>(&AgcDriver::GuestMemory::FlushGpuWrites);
    const auto firstOffset = AgcDriver::GuestMemory::CodeOffset(first);
    const auto secondOffset = AgcDriver::GuestMemory::CodeOffset(second);
    const auto firstAddress = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(first));
    const auto secondAddress = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(second));
    Require(firstOffset != 0 && firstOffset < firstAddress, "a driver code address is not made relative to its module");
    Require(secondOffset - firstOffset == secondAddress - firstAddress, "two addresses in one module are not offset from the same base");
    std::array<std::byte, 64> source{};
    std::array<std::byte, 64> destination{};
    for (int read = 0; read < 512; ++read) AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(source.data())), destination);
    const auto own = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&Require));
    bool sampled = false;
    for (const auto& [caller, count] : AgcDriver::GuestMemory::ReadCallerOffsets()) sampled = sampled || (count >= 256 && caller != 0 && caller < own);
    Require(sampled, "the sampled callers of guest reads are not module offsets");
    std::puts("driver code addresses are module offsets");
    return 0;
}
