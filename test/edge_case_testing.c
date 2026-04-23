#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXT4_BEGIN_TRANSACTION _IO('S', 4)
#define EXT4_END_TRANSACTION _IO('S', 3)

void open_close_only(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }
  printf("Opened successfully\n");

  close(fd);
  printf("Closed successfully\n");

}

void test_only_start(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }
  printf("[INFO] Calling START_TRANSACTION...\n");
  if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
    printf("Error in calling IOCTL\n");
    close(fd);
    return;
  }
  printf("[INFO] Exiting program natively without END_TRANSACTION.\n");
  close(fd);
}

void test_only_end(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }
  printf("[INFO] Calling END_TRANSACTION natively without START...\n");
  int ret = ioctl(fd, EXT4_END_TRANSACTION);
  if (ret < 0) {
    printf("Error in calling IOCTL\n");
    close(fd);
    return;
  }
  printf("[INFO] ioctl return code: %d\n", ret);
  close(fd);
}

void test_concurrent_starts(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }
  int ret;
  printf("[INFO] Forking to create 2 racing START_TRANSACTION clients.\n");
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return;
  } else if (pid == 0) {
    // Child calling START
    ret = ioctl(fd, EXT4_BEGIN_TRANSACTION);
    printf("ret value:%d \n", ret);
    if (ret < 0) {
      printf("Error in calling IOCTL child\n");
      close(fd);
      exit(1);
    }
    printf("[CHILD] Gained transaction context.\n");
    sleep(5); // Hold context for 5 second
    if (ioctl(fd, EXT4_END_TRANSACTION) < 0) {
      printf("Error in calling IOCTL child\n");
      close(fd);
      exit(1);
    }
    exit(0);
  } else {
    // Parent calling START immediately synchronously tracking child execution
    int ret = ioctl(fd, EXT4_BEGIN_TRANSACTION);
    printf("ret value:%d \n", ret);
    if (ret < 0) {
      printf("Error in calling IOCTL parent\n");
      close(fd);
      return;
    }
    printf("[PARENT] Gained secondary transaction context | ret: %d\n", ret);
    sleep(5);
    if (ioctl(fd, EXT4_END_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      return;
    }
  }
  close(fd);
}

void test_poweroff_during_write_transaction(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }

  if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
    printf("Error in calling IOCTL\n");
    close(fd);
    return;
  }

  for (int i = 0; i < 1024 * 100; i++) { // 100MB of data
    pwrite(fd, "Waiting for kill -9", 19, 4096);
  }
  printf("Failed to kill in time! Try again!\n");
  if (ioctl(fd, EXT4_END_TRANSACTION) < 0) {
    printf("Error in calling IOCTL\n");
    close(fd);
    return;
  }
  close(fd);
}

void test_poweroff_after_start(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }

  printf("\n======================================================\n");
  printf("[!] INITIATING TRANSACTION BUT NEVER CLOSING!\n");
  printf("[!] Go to your Host OS terminal right now and kill the VM!\n");
  printf("[!] command: kill -9 <VM_PID>\n");
  printf("======================================================\n\n");

  if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
    printf("Error in calling IOCTL\n");
    close(fd);
    return;
  }
  pwrite(fd, "DIRTY UNCOMMITTED EXT4 CACHE BUFFER", 35, 0);

  printf("[INFO] Transaction hooked and blocked in sleep(1)... Awaiting hard "
         "poweroff!\n");
  while (1) {
    sleep(1);
  }
}

void test_multiple_starts(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }

  printf("[INFO] Calling 1st START_TRANSACTION...\n");
  if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
    printf("Error in calling 1st IOCTL\n");
  }

  // This is the second call to START_TRANSACTION
  // It shouldn't crash but behavior might depend on implementation
  printf("[INFO] Calling 2nd START_TRANSACTION...\n");
  if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
    printf("Error in calling 2nd IOCTL\n");
  }

  // Third and final consecutive call to START_TRANSACTION
  printf("[INFO] Calling 3rd START_TRANSACTION...\n");
  if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
    printf("Error in calling 3rd IOCTL\n");
  }

  printf("[INFO] Exiting program natively.\n");
  close(fd);
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    printf("Usage: %s <mode> <file_path>\n", argv[0]);
    printf("Modes:\n");
    printf("  --only-start\n");
    printf("  --only-end\n");
    printf("  --concurrent-starts\n");
    printf("  --poweroff-during-write-transaction\n");
    printf("  --poweroff-after-start\n");
    printf("  --multiple-starts\n");
    printf("  --open-close-only\n");

    return 1;
  }

  const char *mode = argv[1];
  const char *path = argv[2];

  if (strcmp(mode, "--only-start") == 0)
    test_only_start(path);
  else if (strcmp(mode, "--only-end") == 0)
    test_only_end(path);
  else if (strcmp(mode, "--concurrent-starts") == 0)
    test_concurrent_starts(path);
  else if (strcmp(mode, "--poweroff-during-write-transaction") == 0)
    test_poweroff_during_write_transaction(path);
  else if (strcmp(mode, "--poweroff-after-start") == 0)
    test_poweroff_after_start(path);
  else if (strcmp(mode, "--multiple-starts") == 0)
    test_multiple_starts(path);
  else if (strcmp(mode, "--open-close-only") == 0)
    open_close_only(path);
  else
    printf("Unknown mode: %s\n", mode);

  return 0;
}

