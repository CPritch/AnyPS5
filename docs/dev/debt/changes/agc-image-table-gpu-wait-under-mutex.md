# libSceAgcDriver

## add Functional

- [Image table resolve](../../../core/libs/prx/libSceAgcDriver/Graphics/src/ShaderResources.cpp) (libSceAgcDriver) - an image table entry whose surface still has GPU-written storage results pending is stored to guest memory before its texels are read. That store waits for the GPU through `Recorder::Sync` while the dispatch holds the GPU mutex, so every other queue waits too, queue 0's draw committer included. On PPSA38500 with device-local memory full, one such wait during a 28,000-record table lasted about 10 s at full GPU load. Not resolved: whether the wait can release the mutex, and whether only the entries the program reads need the store.
