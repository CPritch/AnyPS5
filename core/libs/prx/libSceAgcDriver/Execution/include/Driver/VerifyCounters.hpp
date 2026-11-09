#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_VERIFYCOUNTERS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_VERIFYCOUNTERS_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace AgcDriver::DriverDetail {

enum class VerifyCount : std::size_t {
    PacketOverlapChecked,
    PacketOverlapSkipped,
    HeldWritesChecked,
    HeldWritesSkipped,
    PendingWriteIndexChecked,
    PendingWriteIndexSkipped,
    RegisterKeyMemoChecked,
    RegisterKeyHashChecked,
    RegisterKeySkipped,
    HolderNoteChecked,
    HolderNoteUnnamed,
    UnlockedImportChecked,
    UnlockedImportSkipped,
};

constexpr std::size_t VerifyCountSize = static_cast<std::size_t>(VerifyCount::UnlockedImportSkipped) + 1;

using VerifyCounts = std::array<std::uint64_t, VerifyCountSize>;

void CountVerify(VerifyCount which, std::uint64_t amount = 1);
VerifyCounts VerifyTotals();
bool VerifyRequested(VerifyCount which);
bool AnyVerifyRequested();
const char* VerifyFieldName(VerifyCount which);
const char* VerifySwitchName(VerifyCount which);

}

#endif
