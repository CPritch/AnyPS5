# libSceAgcDriver

## add Functional

- [Retried dispatch under the GPU mutex](../../../core/libs/prx/libSceAgcDriver/Execution/src/Driver/Dispatch/Dispatch.cpp) (libSceAgcDriver) - a dispatch whose compute pipeline another thread is still building waits for that build before it takes the GPU mutex (`VulkanDevice::PrepareDispatch`), except when the mutex is already held. A dispatch resolved ahead whose device call throws is dispatched again from `Driver::dispatch`'s handler while that call still holds the mutex, so the retry, including a wait for another thread's build of its pipeline, blocks every other queue. With `APS5_LOCKED_BUILD` the dispatch's preparation runs under the mutex and still waits there. Waits made under the mutex are counted on the `[vk]` line under `APS5_PROFILE_DRAW`.
