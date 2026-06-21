//real drivers usually prefix every log line with the module name so you can grep for them
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#include <linux/module.h> //all kernel modules
#include <linux/pci.h> //used to interact with PCI drivers and devices
#include <linux/init.h> //__init and __exit macros 

MODULE_LICENSE("GPL"); 
MODULE_AUTHOR("Zain");
MODULE_DESCRIPTION("PCI Driver Work");
MODULE_VERSION("1.0");

static const struct pci_device_id pci_ids[] = { //an array of the struct pci_device_id listing what we match with 
    { PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) }, //Give the device our specifics
    {} //need to add this empty element to tell the system that the list is now complete
};
MODULE_DEVICE_TABLE(pci, pci_ids); //helps the kernel match hardware devices to the appropriate driver

//breif function is called when a PCI device is registered. It takes in dev and id information
static int probe(struct pci_dev* dev, const struct pci_device_id* id) {
    int result = pci_enable_device(dev); //takes in the pointer to the PCI device. Wakes up the device and PCI bridge, along with allocation of resources like I/O ports and memory regions
    if (result) { //error is non-zero
        dev_err(&dev->dev, "Failed to enable PCI device\n");
        return result;
    }
    
    pr_info("Successfully enabled PCI device\n");
    
    
    return 0;
}

//brief function is called when a PCI device is unregistered. It takes in dev information
static void remove(struct pci_dev* dev){
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