//real drivers usually prefix every log line with the module name so you can grep for them
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#define STATUS_REGISTER 0x20
#define FACTORIAL_COMPUTATION_REGISTER 0x08
#define STATUS_REGISTER_BIT_0_MASK 0x01

#include <linux/module.h> //all kernel modules
#include <linux/pci.h> //used to interact with PCI drivers and devices
#include <linux/init.h> //__init and __exit macros 
#include <linux/miscdevice.h> //used to create a misc device
#include <linux/fs.h> //used for file operations struct

MODULE_LICENSE("GPL"); 
MODULE_AUTHOR("Zain");
MODULE_DESCRIPTION("PCI Driver Work");
MODULE_VERSION("1.0");

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
};

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = read_driver,
    .write = write_driver
};

static ssize_t read_driver(struct file *filp, char __user *user_buf, size_t len, loff_t *offset) {
    struct miscdevice *mdev = filp->private_data; //filp->private_data is a pointer to the miscdev field inside the edudev
    struct edu_device *edudev = container_of(mdev, struct edu_device, miscdev); //this is a macro that takes in a pointer to a struct, the type of the struct, and the name of the field inside the struct. It returns a pointer to the struct that contains the field. In this case, we are getting a pointer to the edu_device struct that contains the miscdev field

    //we can only read from the register if something has been written to it
    if (!(edudev->readFlag)) { //if the shared flag is not zero, that means that something has not been written to the register, and we will end up reading garbage data
        return -EAGAIN; //return an error code that indicates that the operation should be tried again later
    }

    //poll the status register until the device clears it
    u32 statusRegister = ioread32(edudev->io_base + STATUS_REGISTER); //reads the status register (0x20) to check if the first bit is cleared. 
    while (statusRegister & STATUS_REGISTER_BIT_0_MASK) { //while the first bit is 1, keep polling
        statusRegister = ioread32(edudev->io_base + STATUS_REGISTER); //read the status register again
        cpu_relax(); //this is a macro that tells the CPU to relax. This is used to prevent the CPU from spinning too fast and wasting power. It also allows other threads to run on the CPU
    }

    u32 result = ioread32(edudev->io_base + FACTORIAL_COMPUTATION_REGISTER); //reads the result from the factorial computation register (0x08). ioread32 is used to read 32 bit values without the need for dereferencing a pointer. This protects us from caching and compiler optimizations

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

    iowrite32(input_number, edudev->io_base + FACTORIAL_COMPUTATION_REGISTER); //writes the input number to factorial computation register (0x08). 
    edudev->readFlag = 1; //set the shared flag to indicate that something has been written to the register

    return sizeof(input_number); //we need to return the number of bytes written, which is the sizeof(input_number)
}

//breif function is called when a PCI device is registered. It takes in dev and id information
static int probe(struct pci_dev* dev, const struct pci_device_id* id) {

    u32 registerValue; //32 bit unsigned integer to store the value read from the BAR0 region
    int result;

    //using device manager kzalloc to allocate memory for the edu_device struct. 
    struct edu_device *edudev = devm_kzalloc(&dev->dev, sizeof(*edudev), GFP_KERNEL);
    if (!edudev) {
        return -ENOMEM;
    }

    //STORE
    dev_set_drvdata(&dev->dev, edudev); //store the pointer to the edu_device struct in the dev->dev->driver_data field. This is used to retrieve the struct in remove()

    //ENABLE
    result = pci_enable_device(dev); //takes in the pointer to the PCI device. Wakes up the device and PCI bridge, along with allocation of resources like I/O ports and memory regions
    if (result) { //error is non-zero
        dev_err(&dev->dev, "Failed to enable PCI device\n");
        return result; //straight return, no goto ladder used
    }

    //  CLAIM BAR0 REGION
    result = pci_request_region(dev, 0, "edu");
    if (result) { //request the first BAR region of the device. This is the memory region that the device uses to communicate with the CPU. The second argument is the BAR number, and the third argument is a name for the region
        dev_err(&dev->dev, "Failed to request PCI region\n");
        //we need to use goto here to GO TO the area which disables it
        goto err_disable_device; //goto is used to jump to a label. In this case, we are jumping to the label that disables the device and returns an error code
    } 

    //MAP BAR0 REGION
    edudev->io_base = ioremap(pci_resource_start(dev, 0), pci_resource_len(dev, 0)); //Base of BAR0 is the first argument, and the length of BAR0 is the second argument. This maps the physical address of BAR0 to a virtual address in kernel space. The return value is a pointer to the virtual address, which we store in io_base
    if (!(edudev->io_base)) { //if NULL
        dev_err(&dev->dev, "Failed to Map\n");
        result = -ENOMEM; //macro that returns an error code for 'out of memory'
        goto err_release_region;        
    }

    edudev->miscdev = (struct miscdevice){
        .minor = MISC_DYNAMIC_MINOR,
        .name = "edu",
        .fops = &fops,
    };

    result = misc_register(&edudev->miscdev); //register the misc device with the kernel. This creates a device file in /dev/misc device, which we can use to communicate with the driver
    if (result) {
        goto err_misc_register; //goto is used to jump to the err_misc_register label, which unmaps the region, and continues with the fail-safe driver exit
    }

    //io_base is a pointer type. We can not dereference it, so we end up using ioread32
    registerValue = ioread32(edudev->io_base); //Used to read 32 bit values without the need for dereferencing a pointer. This protects us from caching and compiler optimizations

    pr_info("Successfully enabled PCI device\n");
    pr_info("Read value from BAR0: 0x%08x\n", registerValue); //writes the kernel log buffer which we read with dmesg
    
    
    return 0;

err_misc_register:
    iounmap(edudev->io_base); //unmap the BAR0 region from virtual Kernel Space
err_release_region:
    pci_release_region(dev, 0); //to avoid leaks
err_disable_device:
    pci_disable_device(dev); //disables the device and releases resources
    return result;
}

//brief function is called when a PCI device is unregistered. It takes in dev information
static void remove(struct pci_dev* dev){
    //RECOVER the struct, deregister the misc device, and continue with the fail-safe exit
    struct edu_device *edudev = dev_get_drvdata(&dev->dev); //get the pointer to the edu_device struct that we allocated in probe. This is stored in the dev->dev->driver_data field, which we can access using dev_get_drvdata
    misc_deregister(&edudev->miscdev); //deregister the misc device inside the edudev
    
    //We unmap, then release the claimed memory in said order
    iounmap(edudev->io_base); //unmap the BAR0 region from virtual Kernel Space
    pci_release_region(dev, 0); //release the claimed BAR0 region
    pci_disable_device(dev);
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