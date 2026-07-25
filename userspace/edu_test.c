#include <fcntl.h> //used for opening the device file
#include <unistd.h> //used for write(), read(), and close() functions
#include <stdint.h> 
#include <stdio.h> //used for printf()
#include <errno.h> //used for error handling
#include <string.h> //used for strerror() and memcmp()
#include <sys/ioctl.h> //used for ioctl function
#include <stdlib.h> //used for malloc() and free()
#include <math.h>

#include "../src/edu_ioctl.h" //shared header file between the userspace and device file

#define DMA_BUFFER_SIZE 4096 //size of the DMA buffer

int main() {

    int ourFile;
    uint32_t inputNum;
    uint32_t outputNum;
    ssize_t successWrite;
    ssize_t successRead;
    int ioctl_result;

    struct edu_dma_arg userspace_arg;

    inputNum = 5;

    //OPEN the file
    ourFile = open("/dev/edu", O_RDWR); //open function takes in the file address and the read/write access from the file

    if (ourFile == -1) { //failed to open file
        printf("Failed to open the device file\n");
        return -1; 
    }

    //PIO IOCTL WORK
    //we need to send in a number from the userspace

    int *pio_input = (int *)malloc(DMA_BUFFER_SIZE); //allocating 4KB of memory
    if (pio_input == NULL) {
        printf("Malloc Failed!\n");
        return -1;
    }

    int *pio_result = (int *)malloc(DMA_BUFFER_SIZE);
    if (pio_result == NULL) {
        printf("Malloc Failed\n");
        return -1; 
    }

    pio_input[0] = 5; //this is the value which will be read by the inversion register on the edu side of the system
    pio_result[0] = 8; //this is the predetermined value of pio_result for debugging in case everything goes smoothly but the result is not what we expected

    //Loop Starts

    for (size_t i = 2; i <= 12; i++) { //loop for all sizes - 2^2 (4) to 2^12 (4096)
        userspace_arg.size = pow(2,i);; //enough for one word (number 5 and number 8)
        userspace_arg.data_ptr = (uint64_t)(unsigned long)pio_input;

        ioctl_result = ioctl(ourFile, EDU_PIO_TO_DEVICE, &userspace_arg);
        if (ioctl_result) {
            printf("PIO IOCTL (1) FAILED with error code: %s\n", strerror(errno));
            return -1;
        }

        printf("PIO Time Taken TO device: %llu\n", userspace_arg.delta);

        userspace_arg.data_ptr = (uint64_t)(unsigned long)pio_result;

        ioctl_result = ioctl(ourFile, EDU_PIO_FROM_DEVICE, &userspace_arg);
        if (ioctl_result) {
            printf("PIO IOCTL (2) FAILED with error code: %s\n", strerror(errno));
            return -1; 
        }

        printf("PIO Time Taken FROM device: %llu\n", userspace_arg.delta);

        if (pio_result[0] == ~pio_input[0]) {
            printf("PIO Transfer Successful\n");
        }
        else {
            printf("PIO Transfer Has Failed :(\n");
        }

    }

    free(pio_input);
    free(pio_result);

    //DMA IOCTL WORK
    //fill in the userspace buffer with a known pattern
    int *start_buffer = (int *)malloc(DMA_BUFFER_SIZE); //allocate the userspace buffer with a known pattern
    if (start_buffer == NULL) {
        printf("Malloc Failed!\n");
        return -1;
    }
    for (int i = 0; i < 5; i++) { //takes up 5*4 = 20 bytes of memory
        start_buffer[i] = i; //expected to be [0,1,2,3,4]m
    }

    int *end_buffer = (int *)malloc(DMA_BUFFER_SIZE); //allocate the userspace buffer for the read-back 
    if (end_buffer == NULL) {
        printf("Malloc Failed!\n");
        return -1;
    }

    memset(end_buffer, 0xAA, DMA_BUFFER_SIZE); //fill the end_buffer with 0xAA to have a recognizable pattern for debugging during read-back
 
    userspace_arg.size = 20;
    userspace_arg.data_ptr = (uint64_t)(unsigned long)start_buffer;

    //ioctl for EDU_DMA_TO_DEVICE. userspace buffer -> CPU copies to DMA buffer (cpu_addr) -> Hardware reads from DMA buffer (dma_handle) and writes to the Device Buffer at 0x40000
    ioctl_result = ioctl(ourFile, EDU_DMA_TO_DEVICE, &userspace_arg); //use the ioctl function to kickstart the transfer TO the device FROM the userspace buffer
    if (ioctl_result) {
        printf("IOCTL (1) FAILED with error code: %s\n", strerror(errno));
        return -1;
    }

    printf("DMA Time Taken TO device: %llu\n", userspace_arg.delta);

    userspace_arg.data_ptr = (uint64_t)(unsigned long)end_buffer; //now point the data pointer to the end_buffer. The buffer which will be filled in by the hardware based on what its buffer is filled with
    //ioctl for EDU_DMA_FROM_DEVICE. Device buffer at 0x40000 -> hardware reads from itself anf writes to the DMA buffer (dma_handle) -> CPU reads from cpu_addr and copies it into the userspace buffer end_buffer
    ioctl_result = ioctl(ourFile, EDU_DMA_FROM_DEVICE, &userspace_arg);
    if (ioctl_result) {
        printf("IOCTL (2) FAILED with error code: %s\n", strerror(errno));
        return -1;
    }

    printf("DMA Time Taken FROM device: %llu\n", userspace_arg.delta);

    int final_result = memcmp(start_buffer, end_buffer, userspace_arg.size);

    if (final_result == 0) {
        printf("DMA Transfer Successful!\n");
    }
    else {
        printf("DMA Transfer Failed!\n");
    }

    free(start_buffer);
    free(end_buffer);

    //WRITE: successfully opened the file. Now, we write to it
    successWrite = write(ourFile, &inputNum, sizeof(inputNum)); //Writes the inputNum to the file. Returns the number of bytes written successfully 
    if (successWrite == -1) { //failed to write to the file

        printf("Driver write failed with error code: %s\n", strerror(errno));
        return -1;

    }

    //READ: successfully wrote to the file. Now, we read from it
    successRead = read(ourFile, &outputNum, sizeof(outputNum)); //Reads the number from the file. Returns the number read
    if (successRead == -1 || successRead != sizeof(outputNum)) { //failed to read from the file

        printf("Driver read failed with error code: %s\n", strerror(errno));
        return -1;

    }
    printf("The output from the driver is %u\n", outputNum);

    close(ourFile); //close the file
    return 0;

}   