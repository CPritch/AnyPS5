# libSceAgcDriver

## add Functional

- [Buffer pool trim](../../../core/libs/prx/libSceAgcDriver/Graphics/src/BufferPool.cpp) (libSceAgcDriver) - after a refused device-local allocation, the pool trims only device-local memory. It picks the staging tiers (small, large and host image) by tier, not by the heap their memory came from. Slabs and image blocks it picks by heap. On a device whose staging memory type is in a host-visible device-local heap (resizable BAR, an integrated GPU), that idle staging memory is not given back to the refused allocation. It is given back only when a host allocation fails too. Not measured on such a device.
