#!/bin/bash
qemu-system-x86_64 \
  -machine q35 \
  -m 2048 \
  -smp 2 \
  -drive file=ubuntu.img,format=qcow2,if=virtio \
  -drive file=seed.img,format=raw,if=virtio \
  -device edu \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -virtfs local,path=$HOME/edu-driver,mount_tag=hostshare,security_model=none \
  -nographic
