#include "prx/libc/include/general/ThreadName.hpp"
#include <string>
#ifdef _WIN32

int main() {
    constexpr const char* name = "AnyPS5 queue worker 0x2a";
    const HRESULT set = ThreadName::SetCurrent(name);
    if (set == S_FALSE) return 0;
    if (FAILED(set)) return 1;
    using GetThreadDescriptionFunction = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto getThreadDescription = ThreadName::Resolve<GetThreadDescriptionFunction>("GetThreadDescription");
    if (getThreadDescription == nullptr) return 0;
    PWSTR description = nullptr;
    if (FAILED(getThreadDescription(GetCurrentThread(), &description))) return 2;
    const bool matches = description != nullptr && std::wstring(description) == L"AnyPS5 queue worker 0x2a";
    if (description != nullptr) LocalFree(description);
    return matches ? 0 : 3;
}
#elif defined(__linux__)
#include <fstream>
#include <thread>

namespace {

std::string Comm() {
    std::ifstream stream("/proc/thread-self/comm");
    std::string name;
    std::getline(stream, name);
    return name;
}

}

int main() {
    const auto process = Comm();
    if (ThreadName::SetCurrent("renamed main") || Comm() != process) return 1;
    int result = 0;
    std::thread worker([&result] {
        if (!ThreadName::SetCurrent("AnyPS5 queue worker 0x2a")) result = 2;
        else if (Comm() != "AnyPS5 queue wo") result = 3;
        else if (ThreadName::SetCurrent("") || ThreadName::SetCurrent(nullptr)) result = 4;
    });
    worker.join();
    return result;
}
#else
int main() {
    return 0;
}
#endif
