#include <fcntl.h> //used for opening the device file
#include <unistd.h> //used for write(), read(), and close() functions
#include <stdint.h> 
#include <stdio.h> //used for printf()
#include <errno.h> //used for error handling
#include <string.h> //used for strerror()

int ourFile;
uint32_t inputNum;
uint32_t outputNum;
ssize_t successWrite;
ssize_t successRead;

int main() {

    inputNum = 5;

    //OPEN the file
    ourFile = open("/dev/edu", O_RDWR); //open function takes in the file address and the read/write access from the file

    if (ourFile == -1) { //failed to open file
        return -1; 
    }

    //WRITE: successfully opened the file. Now, we write to it
    //successWrite = write(ourFile, &inputNum, sizeof(inputNum)); //Writes the inputNum to the file. Returns the number of bytes written successfully 
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
    printf("The output from the driver is %d\n", outputNum);

}   