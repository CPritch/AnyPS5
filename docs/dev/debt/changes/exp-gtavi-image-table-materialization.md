# libSceAgcDriver

## add Functional

- An [image table](../../../core/libs/prx/libSceAgcDriver/Graphics/src/ShaderResources.cpp) that a program indexes at keys it computes, outside the bindless heap, has every record's texture made before the dispatch runs, whichever records the program reads: a PPSA38500 compute dispatch that binds a 28,012-record table spends about 24 s creating and uploading textures on first use while device-local memory is full. Looking up such records on the GPU and making textures on first use (#7642 on #9508) is not merged

- A [join](../../../core/libs/prx/libSceAgcDriver/Graphics/src/GuestBufferMemory.cpp) over live host imports is charged their bytes as well as its own while they stay pinned through their batch, so a region that crosses two large imports is copied on the CPU whenever the import budget has no room for both: a PPSA38500 dispatch copies a 2.7 GiB region crossing two imports, about 12 s, when the budget is about 13 GiB with 11.7 GiB of imports live

# shader

## add Functional

- The [image table trace](../../../core/shader/recompiler/Optimization/src/ResourceMaterializer.cpp) `APS5_TRACE_IMAGE_TABLE` is part of the stored plan key, so turning it on rebuilds every plan and writes a second set of plan entries to the shader cache
