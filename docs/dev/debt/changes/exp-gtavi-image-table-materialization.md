# libSceAgcDriver

## add Functional

- An [image table](../../../core/libs/prx/libSceAgcDriver/Graphics/src/ShaderResources.cpp) that a program indexes at keys it computes, outside the bindless heap, has every record's texture made before the dispatch runs, whichever records the program reads: a PPSA38500 compute dispatch that binds a 28,012-record table spends about 24 s creating and uploading textures on first use while device-local memory is full. Looking up such records on the GPU and making textures on first use (#7642 on #9508) is not merged

# shader

## add Functional

- The [image table trace](../../../core/shader/recompiler/Optimization/src/ResourceMaterializer.cpp) `APS5_TRACE_IMAGE_TABLE` is part of the stored plan key, so turning it on rebuilds every plan and writes a second set of plan entries to the shader cache
