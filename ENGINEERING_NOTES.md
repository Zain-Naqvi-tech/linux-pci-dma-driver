# EDU PCI Driver — Engineering Log

> **Note on these notes:** These are my authentic engineering notes, written by me as I
> worked through this project. The original raw version (handwritten/exported to PDF) lives
> in this repo. This page is a cleaned-up copy of that file, refined for readability and
> formatting only. Nothing has been changed in substance. Every observation, mistake, and
> lesson here is genuinely mine and reflects my learning process :)

Main source: https://docs.kernel.org/PCI/pci.html#c.pci_device_id

To start up the device: Saving it here so I don't have to constantly refer to the long Notes document on Onenote:
- `./run.sh` to start up the device
- `sudo mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/host` for the pipeline between the guest and host

This will be used to log everything I found difficult, debugged through, and lessons learned. A different approach from my previous projects.

## New C Keyword: goto

Just learned a new C keyword: `goto`. It is an unconditional jump to a label in the same function. No stack frame, no return, no arguments, nothing pushed or popped. The CPU just branches like it does in assembly.

Example syntax:

```c
goto cleanup;
cleanup:
    Do_something();
```

Fully standard C, and valid forever. Driver code is disciplined and forward-only, which means it is highly recommended to use in kernel C.

Some constraints: we use this within the same function. Labels have function scope, so they are visible everywhere in the function regardless of block nesting. Remember: you can jump past variable declarations, so declare before using `goto`.

While writing the goto ladder, I made the mistake of declaring a variable after one of the goto statements, which we need to avoid. So we need to always declare variables at the top of the scope, whether the top of the function for local scope or the top of the file for global scope. Furthermore, when we reach the goto statement, it jumps to wherever we have the `cleanup:` label. It does not go back to where the goto statement was initially called. It keeps moving from the line after the jump.

---

# Phase 2: Character Device and File Operations Setup

Onto phase 2. Let's do some planning.

## 1. Character Devices and Space Architecture

We need to open `/dev/edu`, which is a character device, so userspace can talk to the driver.

* **Character Device:** A specific type of device driver in Linux. It handles data character by character as a continuous stream, like a file, keyboard, or serial port. Character device does not mean that the hardware moves exactly one byte at a time. It means that the kernel treats it as an unstructured byte stream, and not a fixed block size, which is how block devices work.
* **Kernel Space:** Highly privileged area of the OS where the core kernel and your driver live. Code here has direct access to the hardware.
* **Userspace:** Restricted area where normal applications run. Userspace programs cannot touch hardware directly.

Now the main point. We need a bridge for userspace to talk to the hardware. The character device is that bridge. Creating this `/dev/edu` gives our user-level programs a safe interface to talk to the low-level driver code. We basically need to create an interface with read, write, ioctl, close, and open functions for userspace to work with hardware.

## 2. Memory-Mapped I/O (MMIO) Registers

In terms of register work, we need to look into the MMIO area of the EDU documentation.

* **Status Register (0x20):** Controls the handshake. It has a bitwise OR of 0x01 (computing factorial, read-only) and 0x80 (raise interrupt after finishing factorial computation). Bit 0 is a busy flag set when the device is working, and the device clears it when the result is ready. This is the polling signal. Bit 7 is 'raise interrupt when done'. If we set this before starting, the device fires an MSI when the factorial finishes instead of making you poll.
* **Factorial Computation Register (0x08):** This is the input and output. We put in 5, we get 120 back from the same bit of this register. The stored value is taken and the factorial of it is put back here. This happens only after the factorial bit in the status register is cleared.

The factorial of a number is just 'some work that takes some measurable amount of time'. Therefore, we can observe the real-time transition from busy to done.

## 3. Device Identification and Registration

The device basically has 4 things:

1. **An identity:** Major and minor part, basically a device number. Identity is not like the PCI table. It has a major (identifies the specific driver handling the device) and a minor (distinguishes instances) part to it. They make up the `dev_t` data type together. `alloc_chrdev_region` hands this to us, and it is the thing `/dev/edu` is actually a userspace name for.
2. **A method table:** Connects userspace system calls like read and write to kernel-space driver functions. (Also, we don't need a new file, we keep working in the same one.)
3. **A registration:** Links the current method table and per-instance state structure with the Linux kernel VFS. Informs the kernel that the driver is ready to handle requests.
4. **Per-instance state:** Ensures drivers can manage multiple hardware instances without data corruption.

### Growth Checkpoint: Correcting the Flowchart

* **Initial Perception:** Device number and method table being done before the `probe()` function, with the device number living in `hello_init`.
* **The Reality:** A little issue in my flowchart, let's fix it. The internet source describing the device number living in `hello_init` applies to classic drivers that allocate device numbers at module load. This is not my situation. We have a PCI enumeration setup, so everything device-specific lives in `probe()`. My actual flow is correct: enable, claim, map, read, register char device, all in `probe()`. Using `misc_register` makes this moot anyway.

## 4. The Per-Instance State Problem (Crucial Concept)

This is the most important concept in a message. Currently, `io_base` goes in a file-scope static variable. This works because we have one EDU device. If we had a second one, its probe will overwrite the `io_base` value from the first one. So device one ends up reading registers from device two as well.

Per-instance state is the fix. We bundle everything into one struct, allocated per probe. Then in remove, we get it back and free it, meaning each device gets its own struct and no overwrites are possible. The chain of file, per-device struct, `io_base`, registers is the central problem of a char device. Per-instance state makes it solve cleanly.

### Growth Checkpoint: Struct Confusion Solved

* **Initial Perception:** I thought the per-device struct needed the minor field, name field, and method table.
* **The Reality:** OK, so my points for the per-device struct above are wrong. Those points are for the `miscdevice` struct. This is the kernel's registration type, and its fields (name, minor, fops) are what we fill into the register. The kernel defines the struct, not us.
* **The Solution:** The `edu_device` struct is mine. It holds the device state: `io_base` right now, and IRQ number, DMA buffer, wait queue, and locks later. This is the thing you allocate one of per probe. So the `miscdevice` struct has to live inside the `edu_device` struct as a member. The reason for this is that `container_of` walks from 'pointer to member' back to 'the parent that contains it'. So we must make `miscdevice` a physical member of `edu_device` in order for `container_of` to work.

## 5. Misc Registration Under the Hood

We're going with `misc_register`, but learning what happens underneath is key. The setup we provide is basically a `miscdevice` struct, which has a minor field, a name field, and an fops field that takes the address of the method table.

1. It piggybacks on one shared major number (10). We just rent a minor under it.
2. It allocates the minor. Walks an internal bitmap of used minors, finds the next empty one, and assigns it to the `miscdevice`. Who owns which minor needs to be tracked. We set it to `MISC_DYNAMIC_MINOR` to find the next available minor name.
3. It creates the `/dev` node for us. Calls `device_create` for us internally. The kernel uses the sysfs filesystem and udev to create the new device node.
4. Wires the fops in via a layer of indirection. It is a one-time indirection where one shared char device fans out to hundreds of misc drivers by minor.

## 6. The Recovery Chain and Read/Write Logic

`misc_open` scans `misc_list` by minor and swaps the file operation pointer. There is another step I missed earlier. When `misc_open` finds the matching `miscdevice`, it also does `file->private_data = <that miscdevice>`. It hands the file a pointer to the `miscdevice` for free before the ops ever run.

**The Full Chain:**

Userspace calls Read on `/dev/edu`. The `read()` function fires. The `file->private_data` is the `miscdevice*` (`misc_open` put it here). We use `container_of()`. We hold the `edu_device->io_base->ioread32`. This is the central problem of a char device solved.

**Register Work Flow:**

1. **Write:** Driver writes the input number to 0x08 (factorial computation). Driver writes 0x20 to set bit 0, which is the 'go' signal. You set the 0x08 register with the number N, but you don't set bit 0 yourself because it is read-only. The device or system sets it for you, and you don't clear it. Return. Done. No waiting, nothing to clear.
2. **Processing:** Device goes to work. While busy, bit 0 reads as 1. Device finishes, writes the factorial back to 0x08, and clears bit 0 itself so it now reads 0.
3. **Read:** Driver polls 0x20, sees bit 0 is now 0, and knows the result in 0x08 is ready to read. Poll bit 0 of 0x20 until the device clears it, then `ioread32` from 0x08, `copy_to_user`, return.

*Note on polling:* We can use `cpu_relax()` in these systems for polling inside a while loop. It tells the processor that the loop is a spin-wait so the core can throttle the pipeline and reduce power. Why use this when we can use a simple busy-wait while loop? It allows us to poll politely. We are not stopping everything from happening while wasting CPU cycles. It allows other important things to run while polling for the bit to be cleared.

*Edge case logic:* If two reads happen, or a read happens before a write, we just need simple handling. Get a flag that tells us if something has already been written to, or set up a counter that resets every write and prohibits the system from getting more than 1 read before a new write.

## 7. Final Validated Sequence

For allocation, we have two options: `devm_kzalloc()` or `kzalloc()`. `kzalloc()` requires manual deallocation, while `devm_kzalloc()` binds the memory to a specific struct device and automatically frees it when the driver detaches or the device is removed. Devm means 'device manager'. This means there are potentially no chances of leakage unless something fails internally, so we are good on using `devm_kzalloc()`.

**The Flowchart Pipeline:**

* **Struct Work:** `edu_device` struct holding `io_base`, with `miscdevice` inside it to ensure `container_of` works. We also have the fops table.
* **Probe Work:** Allocate the struct, enable device, request region, map, store `io_base` into the `edu_device`. Fill the `miscdevice` field, register it, set drvdata to make sure remove can find it, read version, return 0. A `misc_register` failure must goto the label that unwinds everything acquired before it: unmap, release, disable.
* **Remove Work:** `pci_get_drvdata` to recover the struct, deregister the misc device, unmap, release region, disable device.

---

# Recovery Chain and Running Notes

**Recovery Chain:** `misc_open` scans `misc_list` by minor and swaps `f_op`. There is another step I missed earlier. When `misc_open` finds the matching `miscdevice`, it also does `file->private_data = <that miscdevice>`. It hands the file a pointer to the `miscdevice`, for free, before the ops ever run.

Chain:

Userspace calls Read on `/dev/edu` -> `read()` function fires -> `file->private_data` is the `miscdevice*` (`misc_open` put it here) -> `container_of()` -> we hold the `edu_device->io_base->ioread32`. That is file -> miscdevice -> edu_device -> io_base -> register. So this is the central problem of a char device solved.

So for the register work, we need to follow the steps below:

* Driver writes the input number to 0x08 (factorial computation).
* Driver writes 0x20 to set bit 0. This is the 'go' signal. You set the 0x08 register with the number N. You don't set bit 0 itself because it is read-only. The device/system sets it for you; you don't clear it.
* Device goes to work. While busy, bit 0 reads as 1.
* Device finishes, writes the factorial back to 0x08, and clears bit 0 itself. Bit 0 now reads 0.
* Driver polls 0x20, sees bit 0 is now 0, and knows the result in 0x08 is ready to read.

So write is: write 0x08, set bit 0 in 0x20, return. Done. No waiting, nothing to clear.
Read is: poll bit 0 of 0x20 until the device clears it, then `ioread32` from 0x08, `copy_to_user`, return.

Interesting thing I just learned: we can use `cpu_relax()` in these systems for polling. We have a while loop, and inside the while loop we add the `cpu_relax()` function. It tells the processor that the loop is a spin-wait so the core can throttle the pipeline, reduce power, etc. But remember, it does not let other processes run. We are still busy-waiting, still holding the CPU, not yielding to the scheduler. It lets the CPU work on pipeline/power/SMT-sibling.

Now the question in my mind is: why use this when we can use a simple busy-wait while loop like we do in bare-metal C?

* So this allows us to poll politely. We are not stopping everything from happening while wasting CPU cycles. It is not the same as an interrupt, but it follows a similar pattern in that it allows other important things to run (like throttling pipeline, etc.) while also polling for the bit to be cleared.

Another thing to wonder here is what happens if two reads happen, or a read happens before a write. So for this, we just need simple handling. Get a flag like I did in the CAN project that tells us if something has already been written to. If so, read, else no. For double reads, we can set up a counter perhaps that resets every write and prohibits the system from getting more than 1 read before a new write.

Just a final `container_of` definition: this macro allows the Linux kernel to find the starting address of a parent structure when it only has a pointer to one of its internal fields. (This is why the `miscdevice` struct needs to be inside the `edu_device` struct, which holds the `io_base`, IRQ, and memory stuff. It's like doing OOP.)

**Final Sequence** (incorporated into what we have right now):

* Struct work: `edu_device` struct (`io_base` right now), `miscdevice` inside it to ensure `container_of` works like it is supposed to. Then we also have the fops table (`file_operations`). Edu_device, miscdevice inside it, fops.
* Probe work: store `io_base` into the `edu_device`. Fill the `miscdevice` field, register it, set drvdata to make sure remove can find it. A `misc_register` failure must goto the label that unwinds everything acquired before it: unmap, release, disable.
* Remove work: `pci_get_drvdata` to recover the struct, deregister the misc device, unmap, release, disable device, kfree the struct (what). Well, we used `devm_kzalloc()`, so we didn't need to use `kfree()`. However, I did get stuck at `pci_get_drvdata`. See below for more info on that.
* Now we add in write and read.

For allocation, we have two options: `devm_kzalloc()` or `kzalloc()`.

* `kzalloc()` requires manual deallocation, while `devm_kzalloc()` binds the memory to a specific struct device and automatically frees it when the driver detaches or the device is removed. This means that there are potentially no chances of leakage unless something fails internally, so we are good on using `devm_kzalloc()`. Also, devm means 'device manager'.

So drvdata is a single pointer-sized slot that lives inside the `pci_dev` struct itself. It is just a `void*` locker attached to the device.

We need to set in probe. Allocate `edudev` in probe. Later, remove runs. Different function, all it receives is a `pci_dev*`. Remove needs to find this pointer you created in probe. We need to store it somewhere.

Basically, we have `edudev`. It is allocated in probe. `remove()` is a different function, and it doesn't take in `edudev` as a parameter either. So in order to recover the struct, it must have access to it somehow, right? So rather than using some global thing, we are using drvdata set/get in order to keep it safe and retrievable.

Also, just a little clearup on the device names in here:

* `struct pci_dev` is the kernel's handle for the physical PCI device. The bus layer owns it and hands it to probe/remove. The window is enable/claim/map.
* `struct miscdevice` is the kernel's registration record for a `/dev` node. This is the character device stuff. We hand it to `misc_register`, which is the 'simpler' way of setting up a character device without using the classical steps. Userspace will open to find it.
* `struct edu_device` is ours. The container that owns the `io_base` and embeds the `miscdevice` so `container_of` can walk home. This is the one we made. The other two are obviously used from Linux pre-composed structs, which we fill in.

Flowchart:

`struct edu_device` -> probe -> allocate the struct -> enable device -> request region -> map -> store `io_base` into `edu_device` -> fill `miscdevice` -> `pci_set_drvdata` -> read version, return 0.
-> remove: `get_drvdata` -> deregister -> unmap -> release region -> disable.
Write and read: read the steps I outlined above.

Notes while coding are in the syntax notes section.

One thing where I'm kinda stuck is how to access `io_base` in the read/write functions, which are their own things outside of probe. So for this exact reason, there's the file parameter and the `container_of` macro:

* We know from the basic steps of `misc_register` that the misc core sets `filp->private_data` to point at the `miscdevice` struct that was opened. So basically, `filp->private_data` is a `miscdevice*`, a pointer to the `miscdev` field in the `edudev`.
* Miscdevice to edudev via `container_of`. We want the `edudev`'s `io_base` field. `container_of` will help me move from `miscdevice` to `io_base` there. Let's try coding that.
* One thing to remember: `miscdevice` is visible to both sides. It sits at the intersection of 'what the kernel knows' and 'what you can navigate from'.
* So the misc core wrote the `miscdev` pointer into that slot back during `open()`. Now we pull it from there. Now I get it.

Almost done with the sequence described above. Now I'm looking for potential issues. One thing is the read and write happening at the same time. There is no sync, which could result in race conditions. So for this we can use a mutex (which is technically a lock) and ensure that while a device is being written to or being read from, we don't read into it or write to it. We wait entirely after the key has been handed over and the flag set or cleared. I won't implement this since we are not working with multiple reads/writes at the same time for now, so I can leave that for a later part. Just an idea: we can also use atomic statements like we did in the RTOS project, where we set up a specific sequence that could not be interrupted or overwritten to during the process. We could potentially discuss this later on.

Some error codes I'm picking up:

* `-EFAULT`: bad address. The pointer userspace handed me points somewhere I can't legally touch. Memory being unreachable.
* `-EINVAL`: invalid argument. The given arguments don't make sense for the given operation.

So in our case, we will compare the `len` parameter with the size of the `u32` `inputNumber`. I'll do `if (len != sizeof(input_number))`, then return a `-EINVAL` error. The reason I used `!=` is because I want the exact word size to match.

Also, we are storing our result in a `u32`, which means it won't be able to store numbers higher than 4 billion something. So we can only send in numbers from 0 to 12. 13! would be over range. So for this, let's see what other types we can use. With a `u64`, we can go up until 20!. But still, there will always be a limit (obviously, unless we use a library like GMP).

x86 is little endian. The least significant byte is stored at the lowest memory address. We need to ensure we know this because when sending in a number for `write_driver`, we need to send it in bytes using printf: `printf '\x05\x00\x00\x00' > /dev/edu`.

So let's review memory and endianness again. Little endian: LSB stored at the lowest memory address. Big endian: MSB stored at the highest memory address.
Reminder, the rule of memory: left is low, right is high.

Ok so to write to the driver: `printf '\x05\x00\x00\x00' | sudo tee /dev/edu > /dev/null`

Ok so to read from the driver: `sudo od -An -tu4 /dev/edu`

Result:
od: /dev/edu: Resource temporarily unavailable 
        120
120 means we are good. The od print means our error handling and flag stuff worked. We used `-EAGAIN` which prints out 'temporarily unavailable' so this means that on the read where the flag is cleared and nothing has been written to it, so this error comes out. 

# Userspace Test Application (edu_test.c)

Small userspace C application which opens `/dev/edu` and exercises the operations. This is not the real or final library we have planned out for Phase 3. This is just to show that the two statements we use to write and read from the device can be turned into a userspace library, and it lays the foundation for the final one.

This small userspace C application opens the file, writes an integer to it, and reads back the returned factorial of that integer. Basically, it is a way to do what we do in the terminal, but in a more automated way.

## Design

- Main purpose: rewire the read and write stuff into this userspace application.
- What is a userspace library: a collection of pre-compiled code that runs in userspace.
- So basically an abstraction, a system-level wrapper.
- The flowchart can be seen in the rough notes section (it looks a bit low quality, so refer to the rough notes).

### Expected behavior

| Input | Output |
| :--- | :--- |
| 5 | 120 |
| 13 | -EOVERFLOW |
| Read before write | -EAGAIN |
| Wrong length | -EINVAL |

## Coding

- First thing, we need a new file: a `.c` file built with gcc.
- Named it `edu_test.c` in a new `/userspace` folder.
- Clarification: this is a wrapper application, by definition not a library.

### Libraries needed for read/write

- `fcntl.h`: for `open()`.
- `unistd.h`: for `write()`, `read()`, and `close()`.
- We don't need anything special for the errors. We just need the basics like `stdint.h` for int types.
- We also need `errno.h` and `stdio.h` for the errno macro and error handling.

### Notes learned while coding

- `open(const char *__file, int __oflag, ...) __nonnull((1))` returns an int, and returns -1 on failure (confirmed).
- The three dots in a function declaration mean we can pass a third or more parameters if needed. For our case we only need two: the file path (`/dev/edu`) and `O_RDWR`. This int flag allows us to both write to and read the result from the file. Something new I picked up.
- Revision: I made a mistake where I overlooked the rule that variables with static storage (global) must be initialized with constant literals, not function calls.
    - So for the `open()` return value to be stored directly in a variable at that scope, the variable must be declared globally. Only then can we store a function's return directly into it.
- When the write and read functions return -1, they also set a variable called `errno`, which comes from the driver side. So we need to output that as well.
- Done writing the code. Had some type mismatches when saving the read/write return values. The rest seems good.

## Build

Something new, so I researched how to compile and link this to the driver, and the difference between gcc and make.

- The driver is kernel code. The app is userspace code. They do not share headers and they do not link.
- This is the ordinary case, so one `.c` produces a normal executable.

### Issue: userspace folder not visible in guest

- The `userspace` folder was not showing under the QEMU guest when I ran `ls`. This meant it could not run on the device.
- Fix: I changed the guest's shared path to include the whole `edu-driver` folder, not just `src`.

## Verification

- Input 5: output was "The output from the driver is 120". Good.
- Input 13 (should return overflow): returned overflow. Good.
- Commented out the write line so a read happens without a write (should return `-EAGAIN`): output was "Resource temporarily unavailable". Good.

All cases pass.

---

---

# Phase 3: MSI Interrupt-Driven I/O

We need to use MSI for interrupts. It is what PCI devices use. The one reason we're doing this is to make the read non-polling and interrupt based, instead of the busy-wait loop from Phase 2.

## Interrupt Register Map

* **Status Register (0x20), bit 7 (0x80):** The document describes this as "raise interrupt after finishing factorial computation." So when this bit is set, the device raises an interrupt when the factorial finishes. That interrupt is what takes us into read. read no longer needs to poll with a while loop and `cpu_relax()`. The result collection now only happens as a consequence of the interrupt.
* **Interrupt Status Register (0x24), RO:** Contains the value(s) that raised the interrupt. This is what the ISR reads to decode the cause.
* **Interrupt Raise Register (0x60), WO:** Writing here raises an interrupt. The written value gets placed into the interrupt status register. We never write here in normal flow, the device raises on its own.
* **Interrupt Acknowledge Register (0x64), WO:** Writing here clears an interrupt. The value gets cleared from the interrupt status register. This must be done from the ISR to stop generating interrupts.

**IRQ controller note:** an IRQ is generated when the interrupt raise register is written. The device runs on INTx by default. Even if the driver only uses MSI, it still needs to update the ack register at the end of the IRQ handler routine.

Source worth keeping: https://blog.davidv.dev/posts/learning-pcie/

## INTx vs MSI

**INTx:** the legacy model as described in the documentation. It is a physical wire that the device holds asserted. It is shared among devices, so the line stays high until something causes the device to deassert it. Because it is shared, a handler on a real system also has to check whether it was actually its own device before doing anything. Four shared lines (A to D). Slower. Supported by older OS and BIOS.

**MSI:** a memory write. The device writes a specific data payload to a specific address, and that write is the interrupt. Edge-triggered. No shared line. One event, one message. Faster. Requires OS and hardware support (which we have). Each interrupt can be specialized to a different purpose.

Reference: https://docs.kernel.org/PCI/msi-howto.html#what-are-msis

### MSI vs MSI-X

* MSI uses a single contiguous block of memory for up to 32 interrupts.
* MSI-X is an extension allowing up to 2048 independent interrupts.

## Allocating and Registering the Interrupt

**`pci_alloc_irq_vectors`:** allocate the vector.
* Returns an integer, the number of allocated vectors. Returns `-ENOSPC` if fewer than `min_vecs` interrupt vectors are available, other errnos otherwise (quoted from the kernel website).
* Takes `struct pci_dev *dev`, our PCI device to operate on.
* Takes `unsigned int min_vecs`, the minimum required number of vectors, which must be greater than or equal to 1.
* Takes `unsigned int max_vecs`, the max vectors.
* Takes `unsigned int flags`. We use `PCI_IRQ_MSI`, which is used for MSI vector allocation.

**Fetch the IRQ number:** `pci_irq_vector`.

**Register the ISR:** `request_irq`, the handler for an interrupt line.
* `unsigned int irq`: interrupt line to allocate. With MSI there is no line, but the kernel keeps the old abstraction and treats it as if it were a line. So we fetch this using `pci_irq_vector`.
* `irq_handler_t handler`: the function called when the IRQ occurs, which is our ISR. So how do we make that? We write a function declaration first, use its name in `request_irq`, then fill it in later.
* `unsigned long flags`: handling flags.
* `const char *name`: name of the device generating the interrupt.
* `void *dev`: cookie passed to the handler function.

On the flags argument: upon research, this should be **zero**. Flags describe properties of an interrupt line. MSI has no wire, so nothing needs to be put there.

## The Concurrency Rule for ISRs

One important note: the ISR cannot sleep, and a mutex can sleep, so a mutex in interrupt context is a bug. Do not use a mutex for state shared with an ISR. We cannot.

## The Mental Model: Sleep, Get Woken, Wake Up, Collect

I understand how interrupts work normally for a generic function like `read()`, but from different videos and sources I was unclear on how we actually treat the interrupt in this specific case. It did not seem like the normal version where we enable interrupts, the interrupt happens, we call an ISR, and it wakes up the function.

Final picture, clear now: our `read()` function arms the device the way we would arm interrupts in bare-metal. The device starts the work, but then `read()` sleeps in the middle, like the task functions from the RTOS project. While it is sleeping, the CPU is free to do anything, there is no busy-wait polling. To wake it up, we use an ISR.

Initially I kept thinking that `read()` itself should be the ISR, but no. We need to keep read and write together in the same scope, and there are reasons to keep the ISR separate (it uses a spinlock discipline to keep things safe, and read/write need protection from overwriting or reading the wrong thing). So the ISR wakes up the `read()` function from the point where it was sleeping, and `read()` continues to collect the result. The ISR fires because the hardware raised an interrupt, and the hardware raised the interrupt because the device finished computing the factorial of the input written by `write()`.

Concept in one line: sleep, get woken, wake up, and collect.

### Big-Picture Flow

Allocate (probe: MSI vector + `request_irq`) -> `read()` runs: arms the interrupt trigger -> sleeps -> (device computes until then) -> device raises the IRQ on its own -> ISR runs: read 0x24 (which cause) -> write it to 0x64 -> wake the sleeper -> `read()` resumes -> `ioread32` result from 0x08 -> `copy_to_user` -> return.

## Notes While Coding

* Updating the goto ladder as we go.
* `pci_alloc_irq_vectors` -> `pci_irq_vector` to store the vector number of the first MSI vector we just allocated -> `request_irq`, which needs the vector number, the `irq_handler` (our ISR, which I have to write myself), the `flags` long (figured out to be zero), the name, and `edudev`.

### Working on the ISR

My thought process:
* This is the ISR, it handles the moment the interrupt fires.
* We need to wake the `read()` function from its sleep state using this ISR.
* Is there a specific Linux way to do this, or do I play with registers directly? Let's see.
* One thing we definitely do here is clear the interrupt using the interrupt ack register.
* But how does it wake the function, and how do we send the function to sleep in the first place? Researching that. It turns out you use `wait_event_interruptible()` to sleep and `wake_up_interruptible()` to wake up.
* We need to declare the wait queue first and have a flag condition variable.
* So a skeleton of the ISR would be: set the flag to something the `read()` thread recognizes as a change, then wake up the queue using the queue declared at the start with `DECLARE_WAIT_QUEUE_HEAD`, then return.
* Now that I know the basic way, let's use something better that the internet suggests: `struct completion`. It removes the need to manage a custom flag and a manual sleep/wake.
* https://www.kernel.org/doc/Documentation/scheduler/completion.txt
* https://embetronicx.com/tutorials/linux/device-drivers/completion-in-linux/
* I don't know why the first link renders weird, but it has all the info we need on completion.

## Completion Notes

* It is a struct with an unsigned int `done` and a `wait_queue_head_t`.
* You can use `DECLARE_COMPLETION` to declare the struct instance at the start.
* I did not use `DECLARE_COMPLETION` in the end, because I realized it should be an `edudev`-specific component, like the vector and `io_base`.
* So I used the normal declaration: `struct completion work_done;` inside the struct, then in probe I called `init_completion(&edudev->work_done)`. That initializes the completion struct, which sets the done flag to 0 and clears the wait queue.

### Why Completion Instead of a Raw Wait Queue

One thing I was thinking about: there is an obvious race where `read()` might sleep after the interrupt has already been raised. If that happens, it can never be woken up. To handle this we use `struct completion`, which has a wait queue and a done flag. The wakeup is remembered. It does not matter whether the ISR fires before or after we reach the wait. This is mainly for synchronization.

## The Sequence I'm Working With

1. Use `pci_alloc_irq_vectors` first to allocate the vectors.
2. Extract the vector, then use `request_irq`.
3. Register the ISR. (Set the DMA mask now too, since I'll need it later anyway.)
4. Check the readFlag, and if good, do `reinit_completion`.
5. Set 0x80 in the status register to arm the interrupt, write the input to 0x08, then `wait_for_completion_interruptible` to sleep. On wake, `ioread32` the result from 0x08, `copy_to_user` as usual, and we're good.
6. The ISR: this fires after the device finishes. `ioread32` from 0x24, `iowrite32` that value to 0x64, `complete`, and return `IRQ_HANDLED`.

## Final Flow (Written)

* Declare a completion struct at the top (as an `edudev` member).
* Declare the `irqreturn_t` function (our ISR) at the top.
* Write the ISR itself: read the interrupt status register (0x24) to determine the cause, write that value to the interrupt acknowledge register (0x64) to clear the interrupt, wake the thread using `complete(&edudev->work_done)`, and return `IRQ_HANDLED`.
* In `write()`: do `reinit_completion` to reset the done field to 0 and empty the wait queue, arm the interrupt by setting bit 7 of the status register, then write the input number to the factorial computation register.
* I got confused about when to actually trigger the interrupt, but it makes sense that the trigger is device-side, not something our software does.

## Spinlock Reasoning

* **Spinlock:** a low-level synchronization primitive where a thread or CPU core continuously polls a lock variable in a tight loop until it becomes available. It burns CPU while waiting. Taking the lock disables interrupts on that core, for one specific reason: to stop the ISR from firing on the same core while process-context code holds the lock.
* The danger case: `read()` grabs the lock and gets to work. An interrupt fires on the same CPU, and the ISR tries to grab the same lock. But `read()` holds it and stays suspended, waiting on the ISR to wake it. The ISR spins forever. Deadlock.
* In our case, the ISR writes to device registers and does not touch `readFlag`, which is the variable shared between read and write. So for this design I concluded no spinlock is needed.

## New Pattern: Read-Modify-Write on Registers

A cleaner way to set a bit in a register for Linux driver work:
* Read the register value into a variable.
* `value |= bit;` to set the specific bit in that value.
* Write the variable back into the register with `iowrite32`.

---

# Syntax Notes

* A new return type I just learned about is `ssize_t`. This returns the amount of bytes that were read successfully.
* The `__user` macro means it points to a userspace buffer.
* Using `__` overall means low-level, platform-dependent, or compiler-specific macro.
* `size_t` is an unsigned integer guaranteed to be large enough to represent the size of any object in memory. It matches the host's memory architecture.
* I just learned that we don't have predetermined names or masks for registers and bits. We need to use `ioread32` and `iowrite32` in order to make reads and writes to the registers. We work with a given base address (`io_base`) in our case.
* So it's something like `iowrite32(value, io_base + offset)`.
* The parameters for `iowrite32` are a `u32` and a `void __iomem *`. Just remember that our base address was also `void __iomem *`, so it is all just register work now.
* A question I had was whether, if the return type is `ssize_t`, we need to add a return statement in the function. Yes.

* `copy_to_user(void __user *to, const void *from, unsigned long n);`
  * Kernel to userspace.
  * Returns the number of bytes that failed to copy. So this must be zero for the process to be complete.
  * Doesn't move a value. It copies `n` bytes from one memory location to another, from a kernel address to a user address.
  * The `to` is going to be `user_buf` from our params.
  * I mixed up `long n` with the register value we just read. No, this is the number of bytes to copy, so the size of the variable we are copying: `sizeof(registerValue)`.
  * `from` is the current register value we just found. It is passed in as `&registerValue` because we need to pass in a pointer type, so we use the address operator.

* `copy_from_user(void *to, const void __user *from, unsigned long n);`
  * Userspace to kernel.
  * Returns the number of bytes that failed to copy.
  * Used to handle a write request.

* I messed up on writing the `write_driver`. I called `copy_from_user()` inside `iowrite32()`. They need to be separate, as `copy_from_user()` returns error codes and not values.
