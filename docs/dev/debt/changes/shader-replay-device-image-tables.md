# shader

## add Functional

- [Captured shader image table targets](../../../core/shader/recompiler/ControlFlow/src/RequestSerializer.cpp) - request format 66 does not preserve `SpirvTarget::deviceImageTables` or `bindlessImages`. A request the driver saved with `APS5_GPU_PATH=1` or `APS5_BINDLESS_IMAGES=1` therefore replays with both off and reads its table columns through the CPU table map; `agc_shader_replay --device-image-tables` sets the former on a replay, and no option sets the latter.
