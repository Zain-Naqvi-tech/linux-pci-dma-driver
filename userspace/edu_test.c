#include <fcntl.h> //used for opening the device file
#include <unistd.h> //used for write(), read(), and close() functions
#include <stdint.h> 

int ourFile;
int inputNum;

int main() {

    inputNum = 5;

    //OPEN the file
    ourFile = open("/dev/edu", O_RDWR); //open function takes in the file address and the read/write access from the file

    if (ourFile == -1) { //failed to open file
        return -1; 
    }

    //WRITE: successfully opened the file. Now, we write to it
    write(ourFile, inputNum, sizeof(inputNum)); 

}   