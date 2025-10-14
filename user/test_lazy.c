// In user/test_lazy.c

#include "kernel/types.h"
#include "user/user.h"

// A large global array to test faulting on the data segment.
char global_array[8192];
// Add this helper function to your test file

void print_stack(int n_bytes) {
  char dummy; // A local variable to get an address on the current stack frame
  char *sp = &dummy; // Use the address of the local variable as a starting point

  printf("\n--- Printing User Stack (approx. %d bytes) ---\n", n_bytes);
  for (int i = 0; i < n_bytes; i++) {
    // Print a new line with the base address every 16 bytes for readability
    if (i % 16 == 0) {
      // The stack grows down, so we print from sp up to see older stack frames
      printf("\n0x%lx: ", (uint64)(sp + i));
    }
    // Print each byte as a two-digit hex number
    printf("%x ", (unsigned char)sp[i]);
  }
  printf("\n---------------------------------------------\n\n");
}
// A recursive function to test stack growth.
void stack_test(int n) {
  char local_buf[100];

  if (n > 0) {
    local_buf[0] = (char)(n % 26 + 'a');
    // We print less often to avoid spamming the console during this deep recursion.
    if (n % 5 == 0) {
      printf("Stack test recursion depth: %d (touched '%c')\n", n, local_buf[0]);
    }
    stack_test(n - 1);
  }
}

int main(int argc, char *argv[]) {
  printf("--- Demand Paging & Page Replacement Test ---\n");

  // 1. Test Data Segment Faults
  printf("\n--- Testing Data Segment ---\n");
  global_array[0] = 'a';
  global_array[sizeof(global_array) - 1] = 'z';
  printf("Data segment test OK.\n");
  printf("\n--- Testing Stack Growth ---\n");
  stack_test(300);
  printf("Stack test OK.\n");
  // 2. Test Heap Growth (sbrk) & Page Replacement
  printf("\n--- Testing Heap (sbrk) & Page Replacement ---\n");
  
  // --- MODIFICATION FOR TESTING PAGE REPLACEMENT ---
  // We increase this number to a very large value to exhaust physical memory.
  // 4000 pages * 4KB/page = ~16 MB of memory.
  int num_pages = 4000;
  // --- END MODIFICATION ---

  int page_size = 4096;
  char *heap_ptr = sbrk(num_pages * page_size);

  if (heap_ptr == (char*)-1) {
    printf("sbrk failed!\n");
    exit(-1);
  }

  printf("sbrk allocated %d pages. Now touching each page to trigger faults and replacement...\n", num_pages);
  for (int i = 0; i < num_pages; i++) {
    // Write to the first byte of each page. This will trigger ALLOC faults
    // and eventually page replacement.
    heap_ptr[i * page_size] = (char)(i % 26 + 'A');
    // Print progress less often to avoid spamming the console.
    if (i > 0 && i % 500 == 0) {
      printf("... touched heap page %d\n", i);
    }
  }
  printf("Finished touching all pages.\n");

  printf("Verifying all heap pages to ensure correctness after replacement...\n");
  int success = 1;
  for (int i = 0; i < num_pages; i++) {
    if (heap_ptr[i * page_size] != (char)(i % 26 + 'A')) {
      printf("!!! VERIFICATION FAILED at page %d! Expected '%c', got '%c'\n", 
             i, (char)(i % 26 + 'A'), heap_ptr[i * page_size]);
      success = 0;
      break;
    }
  }
  
  if (success) {
    printf("Heap and Page Replacement test OK.\n");
  } else {
    printf("!!! TEST FAILED: Heap verification failed after page replacement.\n");
  }

  // 3. Test Stack Growth Faults
  printf("-------------reading stack--------------\n");
  print_stack(250);

  // 4. Test Invalid Access
  printf("\n--- Testing Invalid Access ---\n");
  printf("Attempting to write to a NULL pointer. The process should be killed.\n");
  
  *(char*)0 = 'a';

  printf("!!! TEST FAILED: Invalid access did not terminate the process.\n");
  exit(0);
}