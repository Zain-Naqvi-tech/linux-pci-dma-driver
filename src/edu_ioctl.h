#ifndef EDU_IOCTL_H
#define EDU_IOCTL_H

#include <linux/ioctl.h> //used for ioctl macros
#include <linux/types.h> //used for __u64 type

struct edu_dma_arg {
    __u64 size; //size of the transfer
    __u64 data_ptr; //holds the address to the data payload. This is later typecasted to a void pointer which points to the data
};

#define EDU_DMA_TO_DEVICE _IOW('J', 0, struct edu_dma_arg) //This is the command that we will use to tell the driver to perform a DMA transfer from RAM to the EDU device
#define EDU_DMA_FROM_DEVICE _IOW('J', 1, struct edu_dma_arg) //This is the command that tells the driver we want to perform a DMA transfer from the EDU device to RAM

#endif