This will be used to log everything I found difficult, debugged through, and lessons learned. A different approach from my previous projects

Just learned a new C keyword: goto
It is an unconditional jump to a label in the same function. No stack frame, no return, no arguments, nothing pushed or popped. The CPU just branches like it does in assembly I guess.
Example Syntax use:
```C
    goto cleanup;
    cleanup:
        Do_something();
```
			
Fully standard C, and valid forever. Driver code is disciplined and forward-only which means it is highly recommended to use it in kernel C
Some constraints: We use this within the SAME function. They have a function scope. Visible everywhere in the function regardless of block nesting. REMEMBER: you can jump past variable declarations - so do it before using goto. 

While writing the goto ladder, I made the mistake of declaring a variable AFTER one of the goto statement which we need to avoid. So, we need to ALWAYS declare variables at the top of the scope, be it the top of the function for local scope or top of the file for global scope. Furthermore, when we reach the goto statement, it jumps to wherever we have the 'cleanup:' statement. Then, it does not go back to where the goto statement was initially called. It keeps moving from the line AFTER the jump

ok onto phase 2, let's do some planning

# Phase 2: Character Device & File Operations Setup

## 1. Character Devices & Space Architecture
We need to open `/dev/edu` which is a character device so userspace can talk to the driver[cite: 1]. 
* **Character Device:** A specific type of device driver in Linux[cite: 1]. It handles data character by character as a continuous stream, like a file, keyboard, or serial port[cite: 1]. Character device does not mean that the hardware moves exactly one byte at a time[cite: 1]. It means that the kernel treats it as an unstructured byte stream, and not a fixed block size which is how block devices work[cite: 1].
* **Kernel Space:** Highly privileged area of the OS where the core kernel and your driver live[cite: 1]. Code here has direct access to the hardware[cite: 1].
* **Userspace:** Restricted area where normal applications run[cite: 1]. Userspace programs cannot touch hardware directly[cite: 1].

NOW the main point[cite: 1]. We need a bridge for the userspace to talk to the hardware[cite: 1]. The Character Device is that bridge[cite: 1]. Creating this `/dev/edu` gives our user-level programs a safe interface to talk to the low-level driver code[cite: 1]. We basically need to create an interface with read, write, ioctl, close, and open functions for the userspace to work with hardware[cite: 1].

## 2. Memory-Mapped I/O (MMIO) Registers
In terms of register work, we need to look into the MMIO area of the edu documentation[cite: 1]. 

* **Status Register (0x20):** Controls the handshake[cite: 1]. It has a bitwise OR of 0x01 (computing factorial, read-only) and 0x80 (raise interrupt after finishing factorial computation)[cite: 1]. Bit 0 is a busy flag set when the device is working, and the device clears it when the result is ready[cite: 1]. This is the POLLING signal[cite: 1]. Bit 7 is 'raise interrupt when done'[cite: 1]. If we set this before starting, the device fires an MSI when the factorial finishes instead of making you poll[cite: 1].
* **Factorial Computation Register (0x08):** This is the input and output[cite: 1]. We put in 5, we get 120 back from the same bit of this register[cite: 1]. The stored value is taken and the factorial of it is put back here[cite: 1]. This happens only after the factorial bit in the status register is cleared[cite: 1].

The factorial of a number is just 'some work that takes some measureable amount of time'[cite: 1]. Therefore, we can observe the real-time transition from busy to done[cite: 1].

## 3. Device Identification & Registration
The device basically has 4 things:
1. **An identity:** Major and minor part, basically a device NUMBER[cite: 1]. Identity is not like the PCI table[cite: 1]. It has a major (identifies the specific driver handling the device) and a minor (distinguishes instances) part to it[cite: 1]. They make up the `dev_t` data type together[cite: 1]. `alloc_chrdev_region` hands this to us, and it is the thing `/dev/edu` is actually a userspace name for[cite: 1].
2. **A method table:** Contacts user-space system calls like read and write to kernel-space driver functions[cite: 1]. (Also we don't need a new file, we keep working in the same one)[cite: 1].
3. **A registration:** Links the current method table and per-instance state structure with the Linux kernel VFS[cite: 1]. Informs the kernel that the driver is ready to handle requests[cite: 1].
4. **Per-instance state:** Ensures drivers can manage multiple hardware instances without data corruption[cite: 1].

### 💡 Growth Checkpoint: Correcting the Flowchart
* **Initial Perception:** Device Number and method table being done BEFORE the `probe()` function, with the device number living in `hello_init`[cite: 1].
* **The Reality:** A little issue in my flowchart, let's fix it[cite: 1]. The internet source describing the device number living in `hello_init` applies to classic drivers that allocate device numbers at module load[cite: 1]. This is not MY situation[cite: 1]. We have a PCI enumeration setup so everything device-specific lives in `probe()`[cite: 1]. My actual flow is correct: enable, claim, map, read, register char device, all in `probe()`[cite: 1]. Using `misc_register` makes this moot anyways[cite: 1].

## 4. The Per-Instance State Problem (Crucial Concept)
This is the most important concept in a message[cite: 1]. Currently, `io_base` goes in a file-scope static variable[cite: 1]. This works because we have ONE EDU DEVICE[cite: 1]. If we had a second one, its probe will overwrite the `io_base` value from the first one[cite: 1]. So, device ONE ends up reading registers from device TWO as well[cite: 1].

Per-instance state is the fix[cite: 1]. We bundle everything into one struct, allocated per probe[cite: 1]. Then in remove, we get it back and free it, meaning each device gets its own struct and no overwrites are possible[cite: 1]. The chain of file, per-device struct, `io_base`, registers is the central problem of a char device[cite: 1]. Per-instance state makes it solve cleanly[cite: 1].

### 💡 Growth Checkpoint: Struct Confusion Solved
* **Initial Perception:** I thought the per-device struct needed the minor field, name field, and method table[cite: 1].
* **The Reality:** O OK SO MY POINTS FOR PER-DEVICE STRUCT ABOVE IS WRONG[cite: 1]. Those points are for the struct `miscdevice`[cite: 1]. This is the kernel's registration type, and its fields (name, minor, fops) are what we FILL into the register[cite: 1]. The kernel defines the struct, not us[cite: 1].
* **The Solution:** Struct `edu_device` is MINE[cite: 1]. It holds the device state: `io_base` right now, and IRQ number, DMA buffer, wait queue, locks in later[cite: 1]. This is the thing you allocate one of per probe[cite: 1]. SOOOO, the `misc_device` struct has to live INSIDE the `edu_device` struct as a member[cite: 1]. The reason for this is `container_of` walks from 'pointer to member' back to 'the parent that contains it'[cite: 1]. So we must make `miscdevice` the physical member of the `edu_device` in order for `container_of` to work[cite: 1].

## 5. Misc Registration Under the Hood
So we're going with `misc_register`, but learning what happens underneath is key[cite: 1]. The setup we provide is basically a struct `miscdevice` which has a minor field, a name field, and a fops field which takes in the address of the method table[cite: 1]. 
1. It piggybacks on one shared major number (10)[cite: 1]. We just rent a minor under it[cite: 1].
2. It allocates the minor[cite: 1]. Walks an internal bitmap of used minors, finds the next empty one, and assigns it to the `miscdevice`[cite: 1]. Who owns which minor needs to be tracked[cite: 1]. We set it to `MISC_DYNAMIC_MINOR` to find the next available minor name[cite: 1].
3. It creates the `/dev` node for us[cite: 1]. Calls `device_create` for us internally[cite: 1]. The kernel uses the sysfs filesystem and udev to create the new device node[cite: 1].
4. Wires the fops in via a layer of indirection[cite: 1]. It is a one-time indirection where ONE shared char device fans out to hundreds of misc drivers by minor[cite: 1].

## 6. The Recovery Chain & Read/Write Logic
`misc_open` scans `misc_list` by minor and swaps the file operation pointer[cite: 1]. There is another step I missed earlier[cite: 1]. When `misc_open` finds the matching `miscdevice`, it also does `file->private_data = <that miscdevice>`[cite: 1]. It hands the file a pointer to the `miscdevice` for free before the ops ever run[cite: 1].

**The Full Chain:**
Userspace calls Read on `/dev/edu`[cite: 1]. The `read()` function fires[cite: 1]. The `file->private_data` is the struct `miscdevice*` (miscopen put it here)[cite: 1]. We use `container_of()`[cite: 1]. We hold the `edu_device->io_base->ioread32`[cite: 1]. THIS IS THE CENTRAL PROBLEM OF A CHAR DEVICE SOLVED[cite: 1].

**Register Work Flow:**
1. **Write:** Driver writes the input number to 0x08 (factorial computation)[cite: 1]. Driver writes 0x20 to set bit 0, which is the 'go' signal[cite: 1]. You set the 0x08 register with the number N, but you don't set bit 0 yourself because it is read-only[cite: 1]. The device or system sets it for you, and you don't clear it[cite: 1]. Return. Done. No waiting, nothing to clear[cite: 1].
2. **Processing:** Device goes to work[cite: 1]. While busy, bit 0 reads as 1[cite: 1]. Device finishes, writes the factorial back to 0x08, and clears bit 0 itself so it now reads 0[cite: 1]. 
3. **Read:** Driver polls 0x20, sees bit 0 is now 0, and knows the result in 0x08 is ready to read[cite: 1]. Poll bit 0 of 0x20 until the device clears it, then `ioread32` from 0x08, `copy_to_user`, return[cite: 1].

*Note on polling:* We can use `cpu_relax()` in these systems for polling inside a while loop[cite: 1]. It tells the processor that the loop is a spin-wait so the core can throttle the pipeline and reduce power[cite: 1]. Why use this when we can use a simple busy-wait while loop? It allows us to poll politely[cite: 1]. We are not stopping EVERYTHING from happening while wasting CPU cycles[cite: 1]. It allows other IMPORTANT things to run while polling for the bit to be cleared[cite: 1].
*Edge case logic:* If two reads happen, or a read happens before a write, we just need simple handling[cite: 1]. Get a flag which tells us if something has already been written to, or set up a counter which resets every write and prohibits the system from getting more than 1 read before a new write[cite: 1].

## 7. Final Validated Sequence
For allocation, we have two options: `dvm_kzalloc()` or `kzalloc()`[cite: 1]. `Kzalloc()` requires manual deallocation, while `devm_kzalloc()` binds the memory to a specific struct device and automatically frees it when the driver detaches or the device is removed[cite: 1]. Devm means 'device manager'[cite: 1]. This means there are potentially no chances of leakages unless something fails internally, so we are good on using `devm_kzalloc()`[cite: 1].

**The Flowchart Pipeline:**
* **Struct Work:** `edu_device` struct holding `io_base`, with `miscdevice` inside it to ensure `container_of` works[cite: 1]. We also have the fops table[cite: 1].
* **Probe Work:** Allocate the struct, enable device, request region, map, store `io_base` into the `edu_device`[cite: 1]. Fill the `miscdevice` field, register it, set drvdata to make sure remove can find it, read version, return 0[cite: 1]. A `misc_register` failure must goto the label that unwinds everything acquired before it: unmap, release, disable[cite: 1].
* **Remove Work:** `pci_get_drvdata` to recover the struct, deregister the misc device, unmap, release region, disable device[cite: 1].