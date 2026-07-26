#include <fcntl.h> //used for opening the device file
#include <unistd.h> //used for write(), read(), and close() functions
#include <stdint.h> 
#include <stdio.h> //used for printf()
#include <errno.h> //used for error handling
#include <string.h> //used for strerror() and memcmp()
#include <sys/ioctl.h> //used for ioctl function
#include <stdlib.h> //used for malloc() and free() and qsort()
#include "../src/edu_ioctl.h" //shared header file between the userspace and device file

#define DMA_BUFFER_SIZE 4096 //size of the DMA buffer
#define PIO_N 30 //total runs for PIO transfer
#define DMA_N 11 //total runs for DMA transfer

#define PIO_WARMUP 5
#define DMA_WARMUP 1

int compare_function (const void *a, const void *b);

int compare_function (const void *a, const void *b) {
    uint64_t val_A = *(const uint64_t *)a;
    uint64_t val_B = *(const uint64_t *)b;

    if (val_A < val_B) {
        return -1;
    }

    if (val_A > val_B) {
        return 1;  
    }

    return 0; //if equal

}

uint64_t findMedian(uint64_t arr[], int n);

//find median of the To and From arrays for the DMA/PIO transfers. Adding this function due to the even/odd complications. Takes in the array and its size. Returns the median VALUE
uint64_t findMedian(uint64_t arr[], int n) {

    int index;
    uint64_t avg;

    //even number of elements 
    if (n % 2 == 0) {
        index = n / 2; //find the right-middle index
        avg = (uint64_t)((arr[index] + arr[index - 1]) / 2); //finds the average of the two middle numbers
        return avg; //return average
    }

    //odd number of elements
    else {
        index = n / 2; //rounds down to the middle element. For n=11, it will give index 5 (11/2 = 5.5 = 5) - exact middle
        return arr[index]; //return the actual value typecasted to double
    }

}

int main() {

    int ourFile;
    uint32_t inputNum;
    uint32_t outputNum;
    ssize_t successWrite;
    ssize_t successRead;
    int ioctl_result;

    uint64_t TO_maxPIO_ns;
    uint64_t TO_minPIO_ns; 

    uint64_t FROM_maxPIO_ns;
    uint64_t FROM_minPIO_ns; 

    uint64_t TO_maxDMA_ns; 
    uint64_t TO_minDMA_ns;

    uint64_t FROM_maxDMA_ns;
    uint64_t FROM_minDMA_ns; 

    struct edu_dma_arg userspace_arg;

    uint64_t TO_medianPIO_ns;
    uint64_t FROM_medianPIO_ns;

    uint64_t TO_medianDMA_ns; 
    uint64_t FROM_medianDMA_ns; 

    uint64_t PIO_TO_array_delta[PIO_N];
    uint64_t PIO_FROM_array_delta[PIO_N];

    uint64_t DMA_TO_array_delta[DMA_N];
    uint64_t DMA_FROM_array_delta[DMA_N];

    int TO_PIO_Samples;
    int FROM_PIO_Samples;

    int TO_DMA_Samples;
    int FROM_DMA_Samples;

    inputNum = 5;

    //OPEN the file
    ourFile = open("/dev/edu", O_RDWR); //open function takes in the file address and the read/write access from the file

    if (ourFile == -1) { //failed to open file
        printf("Failed to open the device file\n");
        return -1; 
    }

    //CSV File Work
    FILE *filePointer = fopen("csv_result_file","w"); //open and enable 'write' to the file

    if (filePointer == NULL) {
        printf("Failed to open csv file\n");
        return -1; 
    }

    fprintf(filePointer, "size,type,direction,median_ns,min_ns,max_ns,samples\n"); //sets up the row which explains what everything is

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

    for (int i = 0; i < (DMA_BUFFER_SIZE / 4); i++) { //fill all array indices: 0-1024
        pio_input[i] = 5; //fills every index with the integer 5. Only the last one matters, but we need to fill it all out in order for the kernel to work with SOMETHING
    }

    //External loop Starts (PIO)
    for (size_t i = 2; i <= 12; i++) { //loop for all 11 sizes - 2^2 (4) to 2^12 (4096)

        //reset sample values
        TO_PIO_Samples = 0;
        FROM_PIO_Samples = 0;

        //internal loop starts
        for (int j = 0; j < PIO_N; j++) {

            userspace_arg.size = (1 << i); //0x0001 << 2 = 0x0100 (2^2), << 3 = 0x1000 (2^3)...
            userspace_arg.data_ptr = (uint64_t)(unsigned long)pio_input;

            ioctl_result = ioctl(ourFile, EDU_PIO_TO_DEVICE, &userspace_arg);
            if (ioctl_result) {
                printf("PIO IOCTL (1) FAILED with error code: %s\n", strerror(errno));
                return -1;
            }

            printf("PIO Time Taken TO device: %llu\n", userspace_arg.delta);

            if (j >= PIO_WARMUP) { //only account for samples taken AFTER the warmup runs are done
                PIO_TO_array_delta[j - PIO_WARMUP] = userspace_arg.delta; //save the delta in an array
                TO_PIO_Samples++;
            }

            userspace_arg.data_ptr = (uint64_t)(unsigned long)pio_result;

            ioctl_result = ioctl(ourFile, EDU_PIO_FROM_DEVICE, &userspace_arg);
            if (ioctl_result) {
                printf("PIO IOCTL (2) FAILED with error code: %s\n", strerror(errno));
                return -1; 
            }

            printf("PIO Time Taken FROM device: %llu\n", userspace_arg.delta);

            if (j >= PIO_WARMUP) { //only account for samples taken AFTER the warmup runs are done
                PIO_FROM_array_delta[j - PIO_WARMUP] = userspace_arg.delta; //save the delta in an array
                FROM_PIO_Samples++;
            }

            if (pio_result[(userspace_arg.size / 4) - 1] == ~pio_input[(userspace_arg.size / 4) - 1]) { //compare the last elements of both arrays
                printf("PIO Transfer Successful\n");
            }
            else {
                printf("PIO Transfer Has Failed :(\n");
            }

        }

        //the arrays are now populated, so we can work with them for the median value (sort them first)
        qsort(PIO_TO_array_delta, TO_PIO_Samples, sizeof(PIO_TO_array_delta[0]), compare_function);
        qsort(PIO_FROM_array_delta, FROM_PIO_Samples, sizeof(PIO_FROM_array_delta[0]), compare_function);

        TO_maxPIO_ns = PIO_TO_array_delta[TO_PIO_Samples - 1];
        TO_minPIO_ns = PIO_TO_array_delta[0];

        FROM_maxPIO_ns = PIO_FROM_array_delta[FROM_PIO_Samples - 1];
        FROM_minPIO_ns = PIO_FROM_array_delta[0];

        TO_medianPIO_ns = findMedian(PIO_TO_array_delta, TO_PIO_Samples);
        FROM_medianPIO_ns = findMedian(PIO_FROM_array_delta, FROM_PIO_Samples);

        //for every size, we make a row
        fprintf(filePointer, "%llu,%s,%s,%lu,%lu,%lu,%d\n", userspace_arg.size, "PIO", "TO", TO_medianPIO_ns, TO_minPIO_ns, TO_maxPIO_ns, TO_PIO_Samples);
        fprintf(filePointer, "%llu,%s,%s,%lu,%lu,%lu,%d\n", userspace_arg.size, "PIO", "FROM", FROM_medianPIO_ns, FROM_minPIO_ns, FROM_maxPIO_ns, FROM_PIO_Samples);

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
    for (int i = 0; i < (DMA_BUFFER_SIZE / 4); i++) { //fill all array indices: 0-1024
        start_buffer[i] = i; //fills every index with its placement number 
    }

    int *end_buffer = (int *)malloc(DMA_BUFFER_SIZE); //allocate the userspace buffer for the read-back 
    if (end_buffer == NULL) {
        printf("Malloc Failed!\n");
        return -1;
    }

    //external loop starts
    for (size_t i = 2; i <= 12; i++) {

        //reset the samples before the next size run starts
        TO_DMA_Samples = 0;
        FROM_DMA_Samples = 0;

        for (int j = 0; j < DMA_N; j++) {

            memset(end_buffer, 0xAA, DMA_BUFFER_SIZE); //fill the end_buffer with 0xAA to have a recognizable pattern for debugging during read-back
        
            userspace_arg.size = (1 << i); //0x0001 << 2 = 0x0100 (2^2), << 3 = 0x1000 (2^3)...
            userspace_arg.data_ptr = (uint64_t)(unsigned long)start_buffer;

            //ioctl for EDU_DMA_TO_DEVICE. userspace buffer -> CPU copies to DMA buffer (cpu_addr) -> Hardware reads from DMA buffer (dma_handle) and writes to the Device Buffer at 0x40000
            ioctl_result = ioctl(ourFile, EDU_DMA_TO_DEVICE, &userspace_arg); //use the ioctl function to kickstart the transfer TO the device FROM the userspace buffer
            if (ioctl_result) {
                printf("IOCTL (1) FAILED with error code: %s\n", strerror(errno));
                return -1;
            }

            printf("DMA Time Taken TO device: %llu\n", userspace_arg.delta);

            if (j >= DMA_WARMUP) {
                DMA_TO_array_delta[j - DMA_WARMUP] = userspace_arg.delta; //save the delta in an array
                TO_DMA_Samples++;
            }

            userspace_arg.data_ptr = (uint64_t)(unsigned long)end_buffer; //now point the data pointer to the end_buffer. The buffer which will be filled in by the hardware based on what its buffer is filled with
            //ioctl for EDU_DMA_FROM_DEVICE. Device buffer at 0x40000 -> hardware reads from itself anf writes to the DMA buffer (dma_handle) -> CPU reads from cpu_addr and copies it into the userspace buffer end_buffer
            ioctl_result = ioctl(ourFile, EDU_DMA_FROM_DEVICE, &userspace_arg);
            if (ioctl_result) {
                printf("IOCTL (2) FAILED with error code: %s\n", strerror(errno));
                return -1;
            }

            printf("DMA Time Taken FROM device: %llu\n", userspace_arg.delta);

            if (j >= DMA_WARMUP) {
                DMA_FROM_array_delta[j - DMA_WARMUP] = userspace_arg.delta;
                FROM_DMA_Samples++;
            }

            int final_result = memcmp(start_buffer, end_buffer, userspace_arg.size);

            if (final_result == 0) {
                printf("DMA Transfer Successful!\n");
            }
            else {
                printf("DMA Transfer Failed!\n");
            }
        
        }

        //right after the inner loop ends, we have populated arrays which we can use to find the median
        qsort(DMA_TO_array_delta, TO_DMA_Samples, sizeof(DMA_TO_array_delta[0]), compare_function);
        qsort(DMA_FROM_array_delta, FROM_DMA_Samples, sizeof(DMA_FROM_array_delta[0]), compare_function);

        TO_maxDMA_ns = DMA_TO_array_delta[TO_DMA_Samples - 1];
        TO_minDMA_ns = DMA_TO_array_delta[0];

        FROM_maxDMA_ns = DMA_FROM_array_delta[FROM_DMA_Samples - 1];
        FROM_minDMA_ns = DMA_FROM_array_delta[0];

        TO_medianDMA_ns = findMedian(DMA_TO_array_delta, TO_DMA_Samples);
        FROM_medianDMA_ns = findMedian(DMA_FROM_array_delta, FROM_DMA_Samples);

        //for every size, we make a row
        fprintf(filePointer, "%llu,%s,%s,%lu,%lu,%lu,%d\n", userspace_arg.size, "DMA", "TO", TO_medianDMA_ns, TO_minDMA_ns, TO_maxDMA_ns, TO_DMA_Samples);
        fprintf(filePointer, "%llu,%s,%s,%lu,%lu,%lu,%d\n", userspace_arg.size, "DMA", "FROM", FROM_medianDMA_ns, FROM_minDMA_ns, FROM_maxDMA_ns, FROM_DMA_Samples);


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