# phobos-kernel

Bootloader and kernel sources for PHOBOS.

## Contents

- `bootloader/boot.asm` - BIOS boot sector + long-mode switch
- `kernel/` - scheduler, syscalls, VFS/FAT32, drivers, linker script
- `uapi/libsys.h` - canonical userspace syscall ABI header

## Build

```bash
make
```

Produces:

- `boot.bin`
- `kernel.bin`

Create a local boot image directly from this submodule:

```bash
make image
```

## Important Variables

```bash
make ENABLE_SHELL=1 BUILD_DIR=build-shell1 EXTRA_OBJS="../bridge_lib.o ../bridge_shell.o"
```

- `ENABLE_SHELL` - compile-time shell toggle (`CONFIG_ENABLE_SHELL`)
- `BUILD_DIR` - output dir (use separate dirs for shell on/off builds)
- `EXTRA_OBJS` - extra objects linked into `kernel.bin` (used by superproject)

In normal workflow, build from the superproject root (`make run`).
