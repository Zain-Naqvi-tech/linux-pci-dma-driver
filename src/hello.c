//real drivers usually prefix every log line with the module name so you can grep for them
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h> //all kernel modules
#include <linux/kernel.h> //KERN_INFO log levels
#include <linux/init.h> //__init and __exit macros 

MODULE_LICENSE("GPL"); 
MODULE_AUTHOR("Zain");
MODULE_DESCRIPTION("hello.c file");
MODULE_VERSION("1.0");

//init function runs on load. Logs a message. 
static int __init hello_init(void) {
    pr_info("Hey! This is the hello.c file talking to you!\n"); //writes the kernel log buffer which we read with dmesg
    return 0;
}

static void __exit hello_exit(void) {
    pr_info("Goodbye! This is the hello.c file signing off!\n"); //writes the kernel log buffer which we read with dmesg
}

module_init(hello_init); //registers the init function
module_exit(hello_exit); //registers the exit function