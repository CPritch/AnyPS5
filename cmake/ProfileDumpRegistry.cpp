#include <array>
#include <cstddef>
#include <mutex>

extern "C" [[noreturn]] void __real__Exit(int code);

namespace {

std::mutex profileDumpMutex;
std::array<void (*)(), 1024> profileDumps{};
std::size_t profileDumpCount = 0;

}

extern "C" void LibcRegisterProfileDump_nid_no_patch(void (*dump)()) {
    std::lock_guard lock(profileDumpMutex);
    if (profileDumpCount < profileDumps.size()) profileDumps[profileDumpCount++] = dump;
}

extern "C" void LibcUnregisterProfileDump_nid_no_patch(void (*dump)()) {
    std::lock_guard lock(profileDumpMutex);
    for (std::size_t index = 0; index < profileDumpCount; ++index) {
        if (profileDumps[index] != dump) continue;
        profileDumps[index] = profileDumps[--profileDumpCount];
        return;
    }
}

extern "C" void LibcDumpProfiles_nid_no_patch() {
    std::array<void (*)(), 1024> dumps{};
    std::size_t count = 0;
    {
        std::lock_guard lock(profileDumpMutex);
        dumps = profileDumps;
        count = profileDumpCount;
    }
    for (std::size_t index = 0; index < count; ++index) dumps[index]();
}

extern "C" [[noreturn]] void __wrap__Exit(int code) {
    LibcDumpProfiles_nid_no_patch();
    __real__Exit(code);
}
