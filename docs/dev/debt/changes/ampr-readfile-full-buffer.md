# libSceAmpr

## add Unknown function info

- [sceAmprAprCommandBufferReadFile in a full buffer](../../../core/libs/prx/libSceAmpr/Export.cpp) (libSceAmpr) - the read is recorded in its 20- or 24-byte console form and returns `SCE_KERNEL_ERROR_EBUSY` when it does not fit, as long as the buffer holds only console-sized records (reads and `Nop`s). Once the buffer holds a host-encoded command, which is larger than its console form (see AMPR command sizes), a read that does not fit expands the buffer as those commands do, since the console would still have room for it. Grand Theft Auto VI (PPSA38500) records reads after `sceAmprCommandBufferWriteAddress_04_00` (32 bytes here) and stops at its own assert when a read returns `EBUSY`. Whether the console's `EBUSY` accounting matches this for every command mix is unmeasured. Closes when: the other commands are recorded at their console sizes.
