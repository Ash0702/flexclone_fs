#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EXT4_BEGIN_TRANSACTION _IO('S', 4)
#define EXT4_END_TRANSACTION _IO('S', 3)
#define EXT4_UPDATE_VERSION _IO('S', 1)

void print_elapsed(struct timespec *start, struct timespec *end,
                   const char *msg) {
  double elapsed =
      (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1e9;
  printf("[%s] Time: %.6f seconds\n", msg, elapsed);
}

void print_avg(struct timespec *start, struct timespec *end, int iterations,
               const char *msg) {
  double elapsed =
      (end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec) / 1e9;
  printf("[%s AVG %d runs] Total: %.6f s, Avg Time: %.6f s\n", msg, iterations,
         elapsed, elapsed / iterations);
}

void test_simple(const char *path, int iterations) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }

  size_t chunk_sz = 10 * 1024 * 1024; // 10MB
  char *buf = malloc(chunk_sz);
  memset(buf, 'S', chunk_sz);

  struct timespec start, end;

  clock_gettime(CLOCK_MONOTONIC, &start);
  for (int iter = 0; iter < iterations; iter++) {
    if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      free(buf);
      return;
    }
    pwrite(fd, buf, chunk_sz, 0);
    if (ioctl(fd, EXT4_END_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      free(buf);
      return;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &end);

  close(fd);
  free(buf);

  if (iterations == 1)
    print_elapsed(&start, &end, "SIMPLE (10MB)");
  else
    print_avg(&start, &end, iterations, "SIMPLE (10MB)");
}

void test_no_transaction(const char *path, int iterations) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }
  printf("File opened\n");
  size_t chunk_sz = 10 * 1024 * 1024; // 10MB
  char *buf = malloc(chunk_sz);
  memset(buf, 'X', chunk_sz);

  struct timespec start, end;

  clock_gettime(CLOCK_MONOTONIC, &start);
  for (int iter = 0; iter < iterations; iter++) {
    pwrite(fd, buf, chunk_sz, 0); // 10MB write per iteration
  }
  clock_gettime(CLOCK_MONOTONIC, &end);

  close(fd);
  free(buf);

  if (iterations == 1)
    print_elapsed(&start, &end, "NO TRANSACTION (10MB)");
  else
    print_avg(&start, &end, iterations, "NO TRANSACTION (10MB)");
}

void test_large(const char *path, int iterations) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }

  size_t chunk_sz = 1024 * 1024; // 1MB
  char *buf = malloc(chunk_sz);
  memset(buf, 'A', chunk_sz);

  struct timespec start, end;

  clock_gettime(CLOCK_MONOTONIC, &start);
  for (int iter = 0; iter < iterations; iter++) {
    if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      free(buf);
      return;
    }
    off_t offset = 0;
    for (int i = 0; i < 500; i++) { // 500 MB write
      pwrite(fd, buf, chunk_sz, offset);
      offset += chunk_sz;
    }
    if (ioctl(fd, EXT4_END_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      free(buf);
      return;
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &end);

  close(fd);
  free(buf);

  if (iterations == 1)
    print_elapsed(&start, &end, "LARGE (500MB)");
  else
    print_avg(&start, &end, iterations, "LARGE (500MB)");
}

// Created a new function that handles calling the test functions N times as requested.
void test_averages(const char *mode, const char *path, int iter) {
  if (strcmp(mode, "--avg-simple") == 0) {
    printf("Running simple test %d times...\n", iter);
    test_simple(path, iter);
  } else if (strcmp(mode, "--avg-large") == 0) {
    printf("Running large test (500MB) %d times...\n", iter);
    test_large(path, iter);
  } else if (strcmp(mode, "--avg-no-transaction") == 0) {
    printf("Running no-transaction test %d times...\n", iter);
    test_no_transaction(path, iter);
  }
}

void test_crash(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }

  printf("Starting transaction, holding it without commit to receive "
         "SIGKILL...\n");
  if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
    printf("Error in calling IOCTL\n");
    close(fd);
    return;
  }
  pwrite(fd, "Data before crash\n", 18, 0);

  // Simulate hanging or crashing without calling COMMIT or END_TRANSACTION
  while (1) {
    sleep(1);
  }
}

void test_concurrent(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT, 0666);
  if (fd < 0) {
    perror("open");
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return;
  } else if (pid == 0) {
    // Child
    if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      exit(1);
    }
    pwrite(fd, "Child Write\n", 12, 0);
    if (ioctl(fd, EXT4_END_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      exit(1);
    }
    exit(0);
  } else {
    // Parent
    if (ioctl(fd, EXT4_BEGIN_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      return;
    }
    pwrite(fd, "Parent Write\n", 13, 0);
    if (ioctl(fd, EXT4_END_TRANSACTION) < 0) {
      printf("Error in calling IOCTL\n");
      close(fd);
      return;
    }
    wait(NULL);
  }
  close(fd);
  printf("Concurrent transaction accesses dispatched.\n");
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    printf("Usage: %s <mode> <file_path> [iterations]\n", argv[0]);
    printf("Modes:\n");
    printf("  Standard run: --simple, --large, --crash, --concurrent, --no-transaction\n");
    printf("  Average run:  --avg-simple, --avg-large, --avg-no-transaction\n");
    printf("  [iterations] is optional. Defaults to 1 for standard, 1000 for --avg-\n");
    return 1;
  }

  const char *mode = argv[1];
  const char *path = argv[2];

  int iter = 1;
  if (strncmp(mode, "--avg-", 6) == 0) iter = 1000;
  if (argc >= 4) iter = atoi(argv[3]);
  if (iter <= 0) iter = 1;

  if (strcmp(mode, "--simple") == 0)
    test_simple(path, iter);
  else if (strcmp(mode, "--large") == 0)
    test_large(path, iter);
  else if (strcmp(mode, "--no-transaction") == 0)
    test_no_transaction(path, iter);
  else if (strcmp(mode, "--crash") == 0)
    test_crash(path);
  else if (strcmp(mode, "--concurrent") == 0)
    test_concurrent(path);
  else if (strncmp(mode, "--avg-", 6) == 0)
    test_averages(mode, path, iter);
  else
    printf("Unknown mode: %s\n", mode);

  return 0;
}

