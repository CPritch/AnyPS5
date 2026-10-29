#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_WORKERAFFINITY_HPP

#include <cstdint>

namespace AgcDriver::DriverDetail {

std::uint64_t WorkerAffinityMask();
std::uint64_t CommitterAffinityMask();
// The recording thread (Graphics::RecordOnRecordingThread): APS5_RECORDING_AFFINITY_MASK, else the
// efficient cores of a hybrid processor, so it does not share a core with the threads above.
std::uint64_t RecordingAffinityMask();

void PinWorkerThread(const char* role);
void PinCommitterThread(const char* role);
void PinRecordingThread(const char* role);

}

#endif
