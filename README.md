# QEMU EDU Linux Kernel PCI Driver

A Linux kernel PCI driver for QEMU's EDU virtual device. MMIO, MSI interrupts,
and DMA, plus a userspace library and a latency/throughput benchmark.
Developed and tested against the EDU device inside a QEMU VM. Work in progress.

## Phase 1: PCI enumeration

`src/edu.c` is a minimal PCI driver that binds to the QEMU EDU device
(`1234:11e8`). It registers a `pci_driver` whose ID table matches the device,
so the kernel calls the driver's `probe()` automatically when the device is
present. `probe()` enables the device (`pci_enable_device`, error-checked);
`remove()` disables it on unload.

### Build & load (inside the guest)
```bash
cd /mnt/host
make
sudo insmod edu.ko      # kernel calls probe() → "Successfully enabled PCI device"
sudo dmesg | tail
sudo rmmod edu          # runs remove()
```

`probe()` enables the device, claims BAR 0, maps it into kernel space with `ioremap()`, and reads the EDU identification register at offset 0x00 over MMIO. The expected value for this is `0x010000ed`. The teardown in case of errors includes the use of a `goto` keyword so any failure unwinds smoothly and as intended, in reverse. `remove()` unmaps and releases in LIFO order. 

## Phase 2: Character device and file operations

The driver exposes the EDU device to userspace as a character device at
`/dev/edu`, registered with `misc_register` (dynamic minor under the shared
misc major). Per-device state lives in a `struct edu_device` allocated in
`probe()` with `devm_kzalloc`, which embeds the `miscdevice` so that read and
write can recover the device from `filp->private_data`.

`write()` takes a 4-byte unsigned integer and stores it in the factorial
computation register (0x08), which triggers the device. `read()` polls bit 0
of the status register (0x20) until the device signals completion, then returns
the result from 0x08. The interface validates the transfer length, checks the
result of every userspace copy, guards against reading before a write, and
rejects inputs above 12 (13! exceeds a 32-bit result).

### Usage (inside the guest, module loaded)
```bash
# Write the integer 5 as raw little-endian bytes
printf '\x05\x00\x00\x00' | sudo tee /dev/edu > /dev/null

# Read the 4-byte result back as an unsigned integer
sudo od -An -tu4 /dev/edu      # -> 120
```

The device node is created by `misc_register` in `probe()` and removed by
`misc_deregister` in `remove()`, so it exists only while the module is loaded.

## Userspace Test App (edu_test.c)

A small userspace C app that opens `/dev/edu`, writes an integer to it, and reads back
the factorial the device computed. Basically the terminal `printf`/`od` handshake from
Phase 2, but automated in one program.

This is not the real library (that comes later). It just proves the write and read
path can be wrapped in userspace code, and lays the groundwork for the actual library later.

### What it uses
- `fcntl.h` for `open()`
- `unistd.h` for `write()`, `read()`, `close()`
- `errno.h` and `stdio.h` for error reporting
- `stdint.h` for fixed-width int types

Opens the device with `O_RDWR` so the same file descriptor can both write and read.
On any failure the syscall returns -1 and sets `errno`, which the app prints so you can
see exactly what the driver rejected.

### Build and run

```bash
cd /mnt/host/src && make && sudo insmod edu.ko
cd /mnt/host/userspace && gcc -Wall -Wextra -o edu_test edu_test.c
sudo ./edu_test
```

### What it checks
- Input 5 returns 120
- Input 13 returns -EOVERFLOW (13! overflows u32)
- Read before write returns -EAGAIN
- Wrong length returns -EINVAL