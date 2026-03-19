savedcmd_tdx_shm.mod := printf '%s\n'   tdx_shm.o | awk '!x[$$0]++ { print("./"$$0) }' > tdx_shm.mod
