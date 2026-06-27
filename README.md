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