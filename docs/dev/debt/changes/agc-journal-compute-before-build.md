# libSceAgcDriver

## add Functional

- [Compute pipelines are journaled when their build starts](../../../core/libs/prx/libSceAgcDriver/Execution/src/VulkanDevice.cpp) (libSceAgcDriver) - a compute pipeline is recorded in the pipeline journal when its build starts (`VulkanDevice::computePipeline`) or when a dispatch resolved ahead starts it on the prebuild pool (`VulkanDevice::RecordComputePipelineAhead`), so a build the run does not finish is built again by the next run's start-up replay. A pipeline resolved ahead from memory that the dispatch later reads differently is journaled and replayed at every start-up although nothing uses it; a build that fails every time is retried by every replay; a pipeline that crashes the driver's compiler crashes the next run's start-up replay too (`APS5_NO_PIPELINE_JOURNAL=1` or deleting the journal file avoids that).
