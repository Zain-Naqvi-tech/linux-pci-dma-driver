//real drivers usually prefix every log line with the module name so you can grep for them
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

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
static ssize_t read_driver(struct file *flip, char __user *user_buf, size_t len, loff_t *offset) {

}
//write function: write into the status register (0x20) to set its first bit. Write the input number to the factorial computation register (0x08). 
static ssize_t write_driver(struct file *flip, const char __user *user_buf, size_t len, loff_t *offset) {
    
}

static const struct pci_device_id pci_ids[] = { //an array of the struct pci_device_id listing what we match with 
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) }, //Give the device our specifics
    {} //need to add this empty element to tell the system that the list is now complete
};
MODULE_DEVICE_TABLE(pci, pci_ids); //helps the kernel match hardware devices to the appropriate driver

struct edu_device {
    struct miscdevice miscdev; //struct for the misc device (from the miscdevice.h file) 
    //the __iomem means that the pointer is to a memory-mapped I/O region. This is used to tell the compiler that the pointer is not a normal pointer, and that it should not optimize accesses to it
    void __iomem *io_base; //pointer to the base of the BAR0 region. This is used to read and write to the device
};

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = read_driver,
    .write = write_driver
};

//breif function is called when a PCI device is registered. It takes in dev and id information
static int probe(struct pci_dev* dev, const struct pci_device_id* id) {

    u32 registerValue; //32 bit unsigned integer to store the value read from the BAR0 region
    int result;
    int misc_result;

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
        .name = "misc_device",
        .fops = &fops,
    };

    misc_result = misc_register(&edudev->miscdev); //register the misc device with the kernel. This creates a device file in /dev/misc device, which we can use to communicate with the driver
    if (misc_result) {
        goto err_misc_register; //goto is used to jump to the err_misc_register label, which unmaps the region, and continues with the fail-safe driver exit
    }

    //io_base is a pointer type. We can not dereference it, so we end up using ioread32
    registerValue = ioread32(edudev->io_base); //Used to read 32 bit values without the need for dereferencing a pointer. This protects us from caching and compiler optimizations

    pr_info("Successfully enabled PCI device\n");
    pr_info("Read value from BAR0: 0x%08x\n", registerValue); //writes the kernel log buffer which we read with dmesg
    
    
    return 0;

err_misc_register:
    misc_deregister(&edudev->miscdev);
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