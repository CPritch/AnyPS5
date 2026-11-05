#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP

#include <cstdint>

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask();
std::uint64_t CommitterAffinityMask();
std::uint64_t PreparerAffinityMask();

void PinWorkerThread(const char* role);
void PinDrawWorkerThread(const char* role);
void PinFinisherThread(const char* role);
void PinFinisherHelperThread(const char* role);
void PinCommitterThread(const char* role);
void PinPreparerThread(const char* role);
void PinResolverThread(const char* role);
void PinResolverLaneThread(const char* role);

}

#endif
