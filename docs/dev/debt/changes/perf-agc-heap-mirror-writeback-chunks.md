# libSceAgcDriver

## add Functional

- [Heap mirror write-back of a whole declared range](../../../core/libs/prx/libSceAgcDriver/Graphics/src/GuestBufferMemory.cpp) (libSceAgcDriver) - a writable buffer descriptor that a dispatch declares is bound resident, read back from the device and compared with the mirror's host copy over its whole range on every write-back, whatever the shader stored. A descriptor larger than `maxStorageBufferRange` is clamped to the end of its guest mapping first, so one unbounded descriptor costs gigabytes of device memory and a full read-back per dispatch: Grand Theft Auto VI (PPSA38500) declared 3.44 GB at 0x1086cc9f00, and the compute queue that declared it was still binding and writing it back 17 s later, when the title's own 30 s watchdog ended the run. The read-back runs in 32 MiB pieces, so host memory stays bounded. Closes when: a write-back reads only the blocks the shader stored.
