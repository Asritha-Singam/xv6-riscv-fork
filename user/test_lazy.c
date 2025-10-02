// In user/segfaulttest.c
#include "kernel/types.h"
#include "user/user.h"

int main() {
  printf("Attempting to write to a null pointer...\n");
  int *p = 0;
  *p = 123; // This should cause a fault at an invalid address.
  printf("FAILED: This line should never be reached!\n");
  exit(0);
}