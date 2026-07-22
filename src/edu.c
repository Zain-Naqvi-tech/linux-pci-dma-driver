//real drivers usually prefix every log line with the module name so you can grep for them
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#define STATUS_REGISTER 0x20
#define FACTORIAL_COMPUTATION_REGISTER 0x08

#define STATUS_REGISTER_BIT_0_MASK 0x01
#define STATUS_REGISTER_BIT_7_MASK 0x80

#define INTERRUPT_STATUS_REGISTER 0x24 //contains the value which raised the interrupt (used at the start of an ISR)
#define INTERRUPT_ACK_REGISTER 0X64 //clears an interrupt (used at the end of an ISR)

#define DMA_SOURCE_ADDRESS_REGISTER 0x80 //where to perform the DMA from
#define DMA_DESTINATION_ADDRESS_REGISTER 0x88 //where to perform the DMA to
#define DMA_TRANSFER_COUNT 0x90 //the size of the area to perform the DMA on
#define DMA_COMMAND_REGISTER 0x98 //Bitwise OR of 0x01, 0x02, and 0x04 to start, fill the direction, and raise interrupt respectively

#define DMA_BUFFER_SIZE 4096 //size of the DMA buffer
#define DEVICE_MEM_OFFSET 0x40000

#define PIO_TEST_REGISTER 0x04

#include <linux/module.h> //all kernel modules
#include <linux/pci.h> //used to interact with PCI drivers and devices
#include <linux/init.h> //__init and __exit macros 
#include <linux/miscdevice.h> //used to create a misc device
#include <linux/fs.h> //used for file operations struct
#include <linux/dma-mapping.h> //used for DMA operations
#include "edu_ioctl.h"

MODULE_LICENSE("GPL"); 
MODULE_AUTHOR("Zain");
MODULE_DESCRIPTION("PCI Driver Work");
MODULE_VERSION("1.0");

//ioctl function: this is the function that is called when the user calls ioctl on the device file in the userspace program. It takes in the file pointer, the command, and the argument. The command is used to determine what operation to perform, and the argument is used to pass data to the driver
static long edu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

//irq_handler function: This is the ISR that is called when the device generates an interrupt
static irqreturn_t irq_handler(int irq, void *dev_id);

//read function: polls the EDU's status register to see if the first bit clears. Then, it reads the result in factorial computation register (0x08) 
static ssize_t read_driver(struct file *filp, char __user *user_buf, size_t len, loff_t *offset);

//write function: Write the input number to the factorial computation register (0x08). 
static ssize_t write_driver(struct file *filp, const char __user *user_buf, size_t len, loff_t *offset);

static const struct pci_device_id pci_ids[] = { //an array of the struct pci_device_id listing what we match with 
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) }, //Give the device our specifics
    {} //need to add this empty element to tell the system that the list is now complete
};
MODULE_DEVICE_TABLE(pci, pci_ids); //helps the kernel match hardware devices to the appropriate driver

struct edu_device {
    struct miscdevice miscdev; //struct for the misc device (from the miscdevice.h file) 
    //the __iomem means that the pointer is to a memory-mapped I/O region. This is used to tell the compiler that the pointer is not a normal pointer, and that it should not optimize accesses to it
    void __iomem *io_base; //pointer to the base of the BAR0 region. This is used to read and write to the device
    u32 readFlag; //shared flag to indicate if something has been written to the register. This is used to prevent reading from the register before something has been written to it
    u32 vector;
    struct completion work_done; //completion struct to put the thread to sleep until the ISR wakes it up
    struct completion dma_work_done; //completion struct for the DMA process
    dma_addr_t dma_handle; //tells the device which memory address to read from and write to
    void *cpu_addr; //cpu address 
};

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = read_driver,
    .write = write_driver,
    .unlocked_ioctl = edu_ioctl
};

//DMA Transfer function: Moves memory
static int dma_transfer(struct edu_device *edudev, size_t size, int direction);

//PIO Transfer function: Moves memory using a loop rather than using hardware DMA. 
static int PIO_transfer(struct edu_device *edudev, size_t size, int direction, u64 *delta);

//ioctl
static long edu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {

    struct miscdevice *mdev = filp->private_data; //filp->private_data is a pointer to the miscdev field inside the edudev
    struct edu_device *edudev = container_of(mdev, struct edu_device, miscdev);
    
    int result;
    int copy_check;

    struct edu_dma_arg local_arg; //local instance of the edu_dma_arg struct to hold the size and data pointer 
    copy_check = copy_from_user(&local_arg, (void __user*)arg, sizeof(local_arg)); //copy the data from the user space buffer to the local arg struct. This is done before the DMA so that the args data can be easily accessed
    if (copy_check) { return -EFAULT; } //return an error code to show that there were bytes which could not be successfully copied

    if (local_arg.size > DMA_BUFFER_SIZE) { //check if the size of the transfer is greater than the size of the allocated DMA buffer
        return -EINVAL; //return an error code that indicates that the argument is invalid
    }

    switch (cmd) {
    case EDU_DMA_TO_DEVICE:
        
        copy_check = copy_from_user(edudev->cpu_addr, (void __user *)(unsigned long)local_arg.data_ptr, local_arg.size); //copy the data from the user space buffer to the DMA buffer. This is done before the DMA transfer is started so that the data is in the CPU address space and can be accessed by the device
        if (copy_check) { return -EFAULT; } //return an error code to show that there were bytes which could not be successfully copied
        result = dma_transfer(edudev, local_arg.size, 0); //dma_transfer function to transfer from RAM to the EDU device. Direction=0 to indicate that the direction is from RAM to EDU
        return result; //return 0 on success or the error code on failure
        
    case EDU_DMA_FROM_DEVICE:

        memset(edudev->cpu_addr, 0xFF, DMA_BUFFER_SIZE); //fill the DMA buffer with 0xFF to ensure that the data is being read from the EDU device and not from the CPU address space. This is done before the DMA transfer is started so that the data is in the CPU address space and can be accessed by the device

        result = dma_transfer(edudev, local_arg.size, 1); //transfer from EDU to RAM using the transfer function. Direction is 1 for the opposite direction (EDU to RAM)
        if (result) { return result; } //return the error code if the DMA transfer failed (non-zero return)
        copy_check = copy_to_user((void __user *)(unsigned long)local_arg.data_ptr, edudev->cpu_addr, local_arg.size); //copy the data from the DMA buffer to the user space buffer. This is done after the DMA transfer is complete and the data is in the CPU address space
        if (copy_check) { return -EFAULT; } //return an error code to show that there were bytes which could not be successfully copied
        return result; //return 0 on success or the error code on failure
    
    default:
        return -ENOTTY; //return an error code that indicates that the command is not supported by the driver
    }

}

//PIO
static int PIO_transfer(struct edu_device *edudev, size_t size, int direction, u64 *delta) {

    u32 read_result;

    //timer declarations
    u64 start;
    u64 end; 

    //start timer
    start = ktime_get_ns(); //starting time

    for (int i = 0; i < size; i+=4) {
        if (direction) { //EDU to RAM. 0x40000 to cpu_addr
            read_result = ioread32(edudev->io_base + PIO_TEST_REGISTER); //read the result from 0x04
            *(u32 *)((char*)edudev->cpu_addr + i) = read_result; //this dereferences the address location and stotes the read_result from 0x40000
        }
        else { //RAM to EDU. cpu_addr to 0x40000
            read_result = *(u32 *)((char*)edudev->cpu_addr + i); //read the result from dereferencing cpu_addr + i
            iowrite32(read_result, edudev->io_base + PIO_TEST_REGISTER); //write the result to the device offset value
        }
    }

    //end timer
    end = ktime_get_ns(); //ending time



    return 0; //success

}

//The ISR
static irqreturn_t irq_handler(int irq, void *dev_id) {
    struct edu_device *edudev = dev_id; //get an edu_device instance using the dev_id being passed in
    u32 result;

    result = ioread32(edudev->io_base + INTERRUPT_STATUS_REGISTER); //read the interrupt status register to determine the cause of the interrupt
    if (result == 0) { //if the interrupt status register is 0, that means that the interrupt was not caused by our device
        return IRQ_NONE; //return IRQ_NONE to indicate that the interrupt was not handled
    }
    iowrite32(result, edudev->io_base + INTERRUPT_ACK_REGISTER); //write result to the interrupt acknowledge register to clear the interrupt
    if (result & 0x1) { //if the result shows that the cause is the factorial computation interrupt
        complete(&edudev->work_done); //wakes up the read() thread
    }
    if (result & 0x100) { //if the result shows that the cause is the DMA 'done' interrupt
        complete(&edudev->dma_work_done); //wakes up the DMA thread to complete the DMA transfer
    }

    return IRQ_HANDLED; 
}

static int dma_transfer(struct edu_device *edudev, size_t size, int direction) {

    int dma_command_register = 0;
    int completion_result;

    if (size > DMA_BUFFER_SIZE) { //check if the size of the transfer is greater than the size of the allocated DMA buffer
        return -EINVAL; //return an error code that indicates that the argument is invalid
    }

    if (direction) { //EDU to RAM - Reading from the device
        reinit_completion(&edudev->dma_work_done); //reinitialize the completion struct to reset the 'done' field to 0 and the waiting queue to empty
        iowrite32(DEVICE_MEM_OFFSET, edudev->io_base + DMA_SOURCE_ADDRESS_REGISTER);
        iowrite32(edudev->dma_handle, edudev->io_base + DMA_DESTINATION_ADDRESS_REGISTER);
        iowrite32(size, edudev->io_base + DMA_TRANSFER_COUNT);
        dma_command_register = (0x01 | 0x02 | 0x04); //start transfer, EDU to RAM (direction is 1), raise interrupt after finishing
        iowrite32(dma_command_register, edudev->io_base + DMA_COMMAND_REGISTER);
        completion_result = wait_for_completion_interruptible(&edudev->dma_work_done); //wait for the DMA transfer to complete
        if (completion_result) { 
            return -ERESTARTSYS; //return an error code that indicates that the operation should be restarted
        }
        else {
            return 0; //Return 0 to indicate success
        }
    }

    else { //RAM to EDU - Writing to the device
        reinit_completion(&edudev->dma_work_done); //reinitialize the completion struct to reset the 'done' field to 0 and the waiting queue to empty
        iowrite32(edudev->dma_handle, edudev->io_base + DMA_SOURCE_ADDRESS_REGISTER); //write the dma_handle to the Source address register
        iowrite32(DEVICE_MEM_OFFSET, edudev->io_base + DMA_DESTINATION_ADDRESS_REGISTER); //write the 0x40000 address to DMA destination address register
        iowrite32(size, edudev->io_base + DMA_TRANSFER_COUNT); //write the size variable to the Transfer count register to determine the SIZE of the memory being transferred
        dma_command_register = (0x01 | 0x00 | 0x04); //Start transfer, RAM to edu (direction is 0), raise interrupt after finishing 
        iowrite32(dma_command_register, edudev->io_base + DMA_COMMAND_REGISTER); //write the command to the command register to start the transfer
        completion_result = wait_for_completion_interruptible(&edudev->dma_work_done); //wait for the DMA transfer to complete. This puts the thread to sleep until the ISR wakes it up
        if (completion_result) { 
            return -ERESTARTSYS; //return an error code that indicates that the operation should be restarted
        }
        else {
            return 0; //return 0 to indicate success
        }
    }
}

static ssize_t read_driver(struct file *filp, char __user *user_buf, size_t len, loff_t *offset) {
    struct miscdevice *mdev = filp->private_data; //filp->private_data is a pointer to the miscdev field inside the edudev
    struct edu_device *edudev = container_of(mdev, struct edu_device, miscdev); //this is a macro that takes in a pointer to a struct, the type of the struct, and the name of the field inside the struct. It returns a pointer to the struct that contains the field. In this case, we are getting a pointer to the edu_device struct that contains the miscdev field
    u32 result;
    int completion_result;

    //we can only read from the register if something has been written to it
    if (!(edudev->readFlag)) { //if the shared flag is not zero, that means that something has not been written to the register, and we will end up reading garbage data
        return -EAGAIN; //return an error code that indicates that the operation should be tried again later
    }

    //so we need to remove the polling part and replace it with the interrupt stuff. For now, we need to figure out WHERE the function goes to sleep and it is right here
    //after it wakes up, it then reads and copies to user - so let's just put the wait_for_completion here and see if it works. 

    completion_result = wait_for_completion_interruptible(&edudev->work_done); //this is a macro that puts the thread to sleep until the ISR wakes it up
    if (completion_result) { //if the thread was interrupted by a signal, we need to return an error code
        return -ERESTARTSYS; //return an error code that indicates that the operation should be restarted
    }

    result = ioread32(edudev->io_base + FACTORIAL_COMPUTATION_REGISTER); //reads the result from the factorial computation register (0x08). ioread32 is used to read 32 bit values without the need for dereferencing a pointer. This protects us from caching and compiler optimizations

    if (copy_to_user(user_buf, &result, sizeof(result))) {
        return -EFAULT; //bytes failed to copy
    }

    edudev->readFlag = 0; //reset the shared flag to indicate that the register has been read and is now empty
    return sizeof(result); //we need to return the number of bytes read, which is the sizeof(result)

}   

static ssize_t write_driver(struct file *filp, const char __user *user_buf, size_t len, loff_t *offset) {
    struct miscdevice *mdev = filp->private_data; //filp->private_data is a pointer to the miscdev field inside the edudev
    struct edu_device *edudev = container_of(mdev, struct edu_device, miscdev);

    u32 input_number;
    if (len != sizeof(input_number)) { //check if the length of the input is equal to the size of the input_number variable
        return -EINVAL; //invalid argument
    }
    if (copy_from_user(&input_number, user_buf, sizeof(input_number))) { //copies the input from user_buf into the input_number local to this function
        return -EFAULT; //bytes failed to copy
    } 

    if (input_number > 12) {
        return -EOVERFLOW; //overflow error code. This is because the factorial of numbers greater than 12 will overflow a 32-bit unsigned integer
    }

    reinit_completion(&edudev->work_done); //reinitialize the completion struct to reset the 'done' field to 0 and the waiting queue to empty. This is used to prevent the thread from waking up before the ISR has been called
    
    //arm the interrupt
    iowrite32(STATUS_REGISTER_BIT_7_MASK, edudev->io_base + STATUS_REGISTER); //write to the status register to arm the interrupt. This is done by setting the 7th bit of the status register to 1

    iowrite32(input_number, edudev->io_base + FACTORIAL_COMPUTATION_REGISTER); //writes the input number to factorial computation register (0x08). 
    edudev->readFlag = 1; //set the shared flag to indicate that something has been written to the register

    return sizeof(input_number); //we need to return the number of bytes written, which is the sizeof(input_number)
}

//breif function is called when a PCI device is registered. It takes in dev and id information
static int probe(struct pci_dev* pcidev, const struct pci_device_id* id) {

    u32 registerValue; //32 bit unsigned integer to store the value read from the BAR0 region
    int result;

    //using device manager kzalloc to allocate memory for the edu_device struct. 
    struct edu_device *edudev = devm_kzalloc(&pcidev->dev, sizeof(*edudev), GFP_KERNEL);
    if (!edudev) {
        return -ENOMEM;
    }

    init_completion(&edudev->work_done); //initialize the completion struct to set the 'done' field to 0 and the waiting queue to empty. This is used to put the thread to sleep until the ISR wakes it up
    init_completion(&edudev->dma_work_done); //initialize the completion struct for dma_work_done

    //STORE
    dev_set_drvdata(&pcidev->dev, edudev); //store the pointer to the edu_device struct in the dev->dev->driver_data field. This is used to retrieve the struct in remove()

    //ENABLE
    result = pci_enable_device(pcidev); //takes in the pointer to the PCI device. Wakes up the device and PCI bridge, along with allocation of resources like I/O ports and memory regions
    if (result) { //error is non-zero
        dev_err(&pcidev->dev, "Failed to enable PCI device\n");
        return result; //straight return, no goto ladder used
    }

    pci_set_master(pcidev); //enable bus mastering for the device (for both interrupts and DMA)

    //  CLAIM BAR0 REGION
    result = pci_request_region(pcidev, 0, "edu");
    if (result) { //request the first BAR region of the device. This is the memory region that the device uses to communicate with the CPU. The second argument is the BAR number, and the third argument is a name for the region
        dev_err(&pcidev->dev, "Failed to request PCI region\n");
        //we need to use goto here to GO TO the area which disables it
        goto err_disable_device; //goto is used to jump to a label. In this case, we are jumping to the label that disables the device and returns an error code
    } 

    //MAP BAR0 REGION
    edudev->io_base = ioremap(pci_resource_start(pcidev, 0), pci_resource_len(pcidev, 0)); //Base of BAR0 is the first argument, and the length of BAR0 is the second argument. This maps the physical address of BAR0 to a virtual address in kernel space. The return value is a pointer to the virtual address, which we store in io_base
    if (!(edudev->io_base)) { //if NULL
        dev_err(&pcidev->dev, "Failed to Map\n");
        result = -ENOMEM; //macro that returns an error code for 'out of memory'
        goto err_release_region;        
    }

    //allocate MSI vectors
    result = pci_alloc_irq_vectors(pcidev, 1, 1, PCI_IRQ_MSI); //request 1 MSI vector. The first argument is the pointer to the PCI device, the second argument is the minimum number of vectors we want, the third argument is the maximum number of vectors we want, and the fourth argument is the type of interrupt we want (MSI in this case)
    if (result < 0) { //returns the number of vectors allocated or an error code (negative)
        dev_err(&pcidev->dev, "Failed to allocate MSI vector\n");
        goto err_alloc_irqvectors;
    }

    edudev->vector = pci_irq_vector(pcidev, 0); //get the vector number of the first MSI vector hence the nr as 0. This needs to be passed into the request_irq function
    //request irq
    result = request_irq(edudev->vector,irq_handler,0,"edu",edudev);
    if (result) {
        dev_err(&pcidev->dev, "Failed to request IRQ\n");
        goto err_irq_request;
    }

    //set DMA Coherent mask
    result = dma_set_mask_and_coherent(&pcidev->dev, DMA_BIT_MASK(28));
    if (result) {
        dev_err(&pcidev->dev, "Failed to set DMA mask\n");
        goto err_dma_mask;
    }

    //allocate RAM for the region
    edudev->cpu_addr = dma_alloc_coherent(&pcidev->dev, DMA_BUFFER_SIZE, &edudev->dma_handle, GFP_KERNEL); //allocate 4KB of coherent memory. It returns the virtual address which you can use to access it from the CPU and the dma_handle (changed by pass-by-reference) which is the physicall address that the device can use

    if (!(edudev->cpu_addr)) { //if NULL
        dev_err(&pcidev->dev, "Failed to allocate DMA buffer\n");
        result = -ENOMEM; //returns an error code for 'out of memory'
        goto err_dma_mask;        
    }

    edudev->miscdev = (struct miscdevice){
        .minor = MISC_DYNAMIC_MINOR,
        .name = "edu",
        .fops = &fops,
    };

    result = misc_register(&edudev->miscdev); //register the misc device with the kernel. This creates a device file in /dev/misc device, which we can use to communicate with the driver
    if (result) {
        goto err_misc_register; //goto is used to jump to the err_misc_register label, which now frees the allocated vectors, unmaps the region, and continues with the fail-safe driver exit
    }

    //io_base is a pointer type. We can not dereference it, so we end up using ioread32
    registerValue = ioread32(edudev->io_base); //Used to read 32 bit values without the need for dereferencing a pointer. This protects us from caching and compiler optimizations

    pr_info("Successfully enabled PCI device\n");
    pr_info("Read value from BAR0: 0x%08x\n", registerValue); //writes the kernel log buffer which we read with dmesg
    
    
    return 0;

err_misc_register:
    dma_free_coherent(&pcidev->dev, DMA_BUFFER_SIZE, edudev->cpu_addr, edudev->dma_handle); //free the allocated RAM for the DMA region
err_dma_mask:
    free_irq(edudev->vector, edudev); //free the allocated vector
err_irq_request:
    pci_free_irq_vectors(pcidev); //free the allocations
err_alloc_irqvectors:
    iounmap(edudev->io_base); //unmap the BAR0 region from virtual Kernel Space
err_release_region:
    pci_release_region(pcidev, 0); //to avoid leaks
err_disable_device:
    pci_disable_device(pcidev); //disables the device and releases resources
    return result;
}

//brief function is called when a PCI device is unregistered. It takes in dev information
static void remove(struct pci_dev* pcidev){
    //RECOVER the struct, deregister the misc device, and continue with the fail-safe exit
    struct edu_device *edudev = dev_get_drvdata(&pcidev->dev); //get the pointer to the edu_device struct that we allocated in probe. This is stored in the dev->dev->driver_data field, which we can access using dev_get_drvdata
    misc_deregister(&edudev->miscdev); //deregister the misc device inside the edudev
    
    //We unwind in the opposite order of the probe function
    dma_free_coherent(&pcidev->dev, DMA_BUFFER_SIZE, edudev->cpu_addr, edudev->dma_handle); //free the allocated RAM for the DMA region
    free_irq(edudev->vector, edudev); //free the allocated vector
    pci_free_irq_vectors(pcidev);
    iounmap(edudev->io_base); //unmap the BAR0 region from virtual Kernel Space
    pci_release_region(pcidev, 0); //release the claimed BAR0 region
    pci_disable_device(pcidev);
    pr_info("Removed Device Successfully\n");
}

//struct pci_driver is the main struct that defines a PCI driver. It contains the name of the driver, the id table, and the probe and remove functions
static struct pci_driver edu_driver = { //pci_driver is the struct datatype that defines a PCI driver
    .name = "edu",
    .id_table = pci_ids,
    .probe = probe,
    .remove = remove
};



//init function runs on load. Logs a message. 
static int __init hello_init(void) {
    pr_info("Hey! Registering the PCI driver!\n"); //writes the kernel log buffer which we read with dmesg
    return pci_register_driver(&edu_driver);
}

static void __exit hello_exit(void) {
    pr_info("Goodbye! Unregistering the PCI driver!\n"); //writes the kernel log buffer which we read with dmesg
    pci_unregister_driver(&edu_driver);
}

module_init(hello_init); //registers the init function
module_exit(hello_exit); //registers the exit function