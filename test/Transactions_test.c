#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>

// =====================
// 🔧 DEFINE THESE YOURSELF
// =====================
#define BEGIN_TRANSACTION  _IO('S', 4)   // <-- replace
#define END_TRANSACTION    _IO('S', 3)   // <-- replace

// If your ioctl needs args, define structs here
// struct txn_args { ... };

#define FILE_PATH "testfile.bin"

int main(int argc , char ** argv) {
    int fd;
    ssize_t ret;
    if(argc != 2){
	printf("<Usage> : %s <file>\n" , argv[0]);
	perror("Usage");
	return -1;
    }  
 
    char * filename = argv[1];
	
  
    // Open file
    fd = open(filename, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    printf("File opened successfully.\n");

    // =====================
    // 🚀 BEGIN TRANSACTION
    // =====================
    if (ioctl(fd, BEGIN_TRANSACTION) < 0) {
        perror("ioctl BEGIN_TRANSACTION");
        close(fd);
        return 1;
    }

    printf("Transaction started.\n");

    // =====================
    // ✍️ WRITES AT DIFFERENT OFFSETS
    // =====================

    const char *data1 = "Hello";
    const char *data2 = "Kernel";
    const char *data3 = "Transaction";

    // Write at offset 0
    ret = pwrite(fd, data1, strlen(data1), 0);
    if (ret < 0) perror("pwrite 1");

    // Write at offset 4096 (next block assuming 4KB block size)
    ret = pwrite(fd, data2, strlen(data2), 4096);
    if (ret < 0) perror("pwrite 2");

    // Write at offset 8192
    ret = pwrite(fd, data3, strlen(data3), 8192);
    if (ret < 0) perror("pwrite 3");

    printf("Writes completed.\n");

    // =====================
    // 🏁 END TRANSACTION
    // =====================
    if (ioctl(fd, END_TRANSACTION) < 0) {
        perror("ioctl END_TRANSACTION");
        close(fd);
        return 1;
    }

    printf("Transaction ended.\n");

    close(fd);
    return 0;
}
