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

## MSI interrupt-driven I/O
 
Phase 2 worked by polling: `read()` spun on the status register until the device
cleared the busy bit, burning a CPU core just to wait. It replaces that with
a real interrupt. The device signals when it is finished, and the reading process
sleeps until then instead of spinning.
 
### How it works
 
The flow spans two contexts. The `write()`/`read()` path runs in process context and
can sleep. The interrupt handler runs in interrupt context and cannot sleep. They coordinate
through a `struct completion`.
 
`write()` arms the interrupt by setting bit 7 (`0x80`) in the status register (0x20),
then writes the input to the factorial register (0x08) to start the computation. Order
matters: the device checks the interrupt-enable bit when the computation finishes, so
arming must happen before the trigger.
 
`read()` calls `wait_for_completion_interruptible`, which sleeps and hands the CPU back
to the system. When the device finishes, it raises an MSI. The handler reads the interrupt
status register (0x24) to confirm the cause, acknowledges it by writing that value back to
the acknowledge register (0x64), then calls `complete()` to wake the reader. The reader
resumes, reads the result from 0x08, copies it to userspace, and returns.
 
### Key design decisions

Check my [ENGINEERING_NOTES](ENGINEERING_NOTES.md) file for more information on these design decisions. A lot of debugging went into completing this phase of the project. The difference was a single line `pci_set_master`. 
 
- **`struct completion` over a raw wait queue.** A completion is a wait queue plus a done
  flag plus the locking that makes them race-free. The device can finish and fire the
  interrupt before `read()` reaches its sleep call. A bare wait queue would lose that wake
  and the reader would sleep forever; a completion records that the event happened, so a
  later wait returns immediately. `reinit_completion` runs at the start of each write so a
  stale completion cannot let the next read return early.
- **MSI, not INTx.** The driver requests one MSI vector with `pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI)`
  and gets the IRQ number from `pci_irq_vector`, not `pdev->irq`. The `request_irq` flags are 0,
  since an MSI vector is dedicated, not a shared wire.
- **The acknowledge quirk.** The device requires the acknowledge register to be written even
  under MSI. Under INTx, skipping it leaves the line asserted and the handler fires forever.
  Under MSI there is no line, but the status register keeps its stale bits, which corrupts the
  next decode. So the handler acks the exact value it read.
- **Bus mastering.** `probe()` calls `pci_set_master`. An MSI is delivered as a memory write,
  and a PCI device can only initiate writes if it is a bus master. Without this the device
  raised the interrupt but the write was dropped and the handler never ran. DMA also depends on the same permission.
- **Concurrency.** The only state shared between the handler and the file operations is the
  completion, which is internally locked, so no spinlock is needed here. A mutex would not be
  an option anyway, since the handler cannot sleep and a mutex can.
### Teardown
 
`remove()` releases the handler with `free_irq` before releasing the vector with
`pci_free_irq_vectors`, so no interrupt can arrive after the handler is gone. Bus mastering is
cleared with `pci_clear_master`, and the rest unwinds in reverse order of setup.
 
### Verifying it works
 
```bash
cd /mnt/host/src && make && sudo insmod edu.ko
 
# Handler registered, count starts at 0
cat /proc/interrupts | grep edu
 
# Trigger a factorial
printf '\x05\x00\x00\x00' | sudo tee /dev/edu > /dev/null
sudo od -An -tu4 /dev/edu
 
# Count is now 1: the result came via a real interrupt, not the old busy-poll
cat /proc/interrupts | grep edu
```