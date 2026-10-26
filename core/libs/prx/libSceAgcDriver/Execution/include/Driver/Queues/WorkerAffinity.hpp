#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP

#include <cstdint>

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask();
std::uint64_t CommitterAffinityMask();

void PinWorkerThread(const char* role);
void PinCommitterThread(const char* role);

}

#endif
