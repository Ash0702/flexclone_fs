#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define SCORW_IOCTL_MAGIC 'S'
#define SCORW_UPDATE_VERSION _IO(SCORW_IOCTL_MAGIC, 1)

int main(int argc, char *argv[])
{
int fd;
int ret;

if (argc != 2) {
    fprintf(stderr, "Usage: %s <child_file>\n", argv[0]);
    return 1;
}

/* Open the child file */
fd = open(argv[1], O_RDONLY);
if (fd < 0) {
    perror("open failed");
    return 1;
}

printf("Opened child file: %s (fd=%d)\n", argv[1], fd);

/* Call the ioctl */
ret = ioctl(fd, SCORW_UPDATE_VERSION);
if (ret < 0) {
    perror("ioctl SCORW_UPDATE_VERSION failed");
    close(fd);
    return 1;
}

printf("IOCTL executed successfully\n");

close(fd);
return 0;


}

