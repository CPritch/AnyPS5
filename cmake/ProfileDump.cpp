extern "C" void __gcov_dump();
extern "C" void LibcRegisterProfileDump_nid_no_patch(void (*dump)());
extern "C" void LibcUnregisterProfileDump_nid_no_patch(void (*dump)());

namespace {

struct ProfileDumpRegistration {
    ProfileDumpRegistration() { LibcRegisterProfileDump_nid_no_patch(__gcov_dump); }
    ~ProfileDumpRegistration() { LibcUnregisterProfileDump_nid_no_patch(__gcov_dump); }
};

const ProfileDumpRegistration registration;

}
