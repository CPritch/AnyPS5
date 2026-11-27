# libSceAgcDriver

## add Opt-in draw paths

- [GPU descriptor heap beside the bindless image heap](../../../core/libs/prx/libSceAgcDriver/Graphics/src/BindlessHeap.cpp) (`APS5_GPU_HEAP`, `APS5_BINDLESS_IMAGES`) - the two heaps are not combined: with `APS5_BINDLESS_IMAGES=1` the GPU heap stays off with one log line, because bindless image programs read their tables through an update-after-bind descriptor set that heap stages do not map. Heap stages have no run-time image table lookups of their own yet.
- [GPU heap border color tables](../../../core/libs/prx/libSceAgcDriver/Graphics/src/BindlessHeap.cpp) (`APS5_GPU_HEAP`) - a sampler that reads a border color table entry is refused with an error in a heap stage: the heap encodes samplers from their create info without a registered custom border color, and an embedded sampler cannot read the draw's table.
- [GPU heap split table calls](../../../core/libs/prx/libSceAgcDriver/Execution/src/Driver/Dispatch/Dispatch.cpp) (`APS5_GPU_HEAP`) - a dispatch whose table calls are split and continued takes the per-dispatch descriptor path with the heap on and is counted as a heap exclusion; its split state set has no heap mapping.
