extern "C" void __gcov_dump();
extern "C" void LibcRegisterProfileDump_nid_no_patch(void (*dump)());
extern "C" void LibcUnregisterProfileDump_nid_no_patch(void (*dump)());
extern "C" void LibcDumpProfiles_nid_no_patch();
extern "C" [[noreturn]] void __real__Exit(int code);

namespace {

struct ProfileDumpRegistration {
    ProfileDumpRegistration() { LibcRegisterProfileDump_nid_no_patch(__gcov_dump); }
    ~ProfileDumpRegistration() { LibcUnregisterProfileDump_nid_no_patch(__gcov_dump); }
};

const ProfileDumpRegistration registration;

}

extern "C" [[noreturn]] void __wrap__Exit(int code) {
    LibcDumpProfiles_nid_no_patch();
    __real__Exit(code);
}
