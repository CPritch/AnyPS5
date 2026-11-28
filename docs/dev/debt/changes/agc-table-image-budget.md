# libSceAgcDriver

## add Functional

- [Image table entries deferred past a budget](../../../core/libs/prx/libSceAgcDriver/Graphics/src/ShaderResources.cpp) (libSceAgcDriver) - with `APS5_TABLE_IMAGE_BUDGET` set, a CPU resolve of an image table on the GPU heap stops creating textures once it has made that many image allocations on its thread; its remaining sampled entries read the heap's absent image, so a shader that selects one samples zeros (or the absent image's contents) instead of the guest texture until a later resolve of the table creates it. A compute shader that selects a deferred entry stores results from those zeros, and they stay in guest memory. Only allocations on the resolving thread count, storage images made in the same resolve count too, and nothing makes a table converge when the texture cache cannot hold all of its entries. Stored entries and tables outside the GPU heap are never deferred. Off by default. Labels: gpu, tested, linux.
