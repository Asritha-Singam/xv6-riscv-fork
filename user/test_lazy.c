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
  stack_test(10);
  printf("Stack test OK.\n");
  // 2. Test Heap Growth (sbrk) & Page Replacement
  printf("\n--- Testing Heap (sbrk) & Page Replacement ---\n");
  
  // --- MODIFICATION FOR TESTING PAGE REPLACEMENT ---
  // We increase this number to a very large value to exhaust physical memory.
  // 4000 pages * 4KB/page = ~16 MB of memory.
  int num_pages = 1000;
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

  printf("\n--- Triggering Swap-Ins ---\n");
  for (int i = 0; i < num_pages; i++) {
    volatile char c = heap_ptr[i * page_size];  // read only
    if (i > 0 && i % 500 == 0)
      printf("... reread page %d (got '%c')\n", i, c);
  }
  printf("Swap-in verification complete.\n");

  
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
//SWAPIN tests & memstat syscall
/*#include "user.h"
#include "kernel/memstat.h"
#include "kernel/types.h"
#define PAGE_SIZE 4096
#define PAGES_TO_ALLOCATE 64  // Adjust based on your MAX_RESIDENT_PAGES

int
main(void)
{
  struct proc_mem_stat st;
  char *ptrs[PAGES_TO_ALLOCATE];

  printf("=== STRESS TEST: Lazy Paging + Swapping ===\n");

  // Step 1: Allocate and write to pages
  for (int i = 0; i < PAGES_TO_ALLOCATE; i++) {
    ptrs[i] = sbrk(PAGE_SIZE);
    if (ptrs[i] == (char*)-1) {
      printf("sbrk failed at page %d\n", i);
      break;
    }

    // Write to force page fault allocation
    ptrs[i][0] = (char)i;
    if (i % 4 == 0)
      printf("Allocated and touched page %d at %p\n", i, ptrs[i]);
  }

  // Step 2: Get memstat
  if (memstat(&st) < 0) {
    printf("memstat failed\n");
    exit(1);
  }

  printf("\n=== Snapshot After Allocation ===\n");
  printf("PID=%d Total=%d Resident=%d Swapped=%d NextSeq=%d\n",
         st.pid, st.num_pages_total, st.num_resident_pages,
         st.num_swapped_pages, st.next_fifo_seq);

  // Show a few sample entries
  for (int i = 0; i < st.num_pages_total && i < MAX_PAGES_INFO; i++) {
    if (st.pages[i].state == RESIDENT || st.pages[i].state == SWAPPED)
      printf("VA=0x%x state=%s dirty=%d seq=%d slot=%d\n",
             st.pages[i].va,
             st.pages[i].state == RESIDENT ? "RESIDENT" :
             (st.pages[i].state == SWAPPED ? "SWAPPED" : "UNMAPPED"),
             st.pages[i].is_dirty,
             st.pages[i].seq,
             st.pages[i].swap_slot);
  }

  // Step 3: Trigger some page-ins by accessing early pages again
  printf("\n=== Triggering Page-Ins ===\n");
  for (int i = 0; i < PAGES_TO_ALLOCATE / 2; i++) {
    ptrs[i][0] += 1;  // Access to potentially swapped-out page
  }

  // Step 4: Second memstat snapshot
  if (memstat(&st) < 0) {
    printf("memstat failed\n");
    exit(1);
  }

  printf("\n=== Snapshot After Reaccess ===\n");
  printf("Resident=%d Swapped=%d\n",
         st.num_resident_pages, st.num_swapped_pages);

  for (int i = 0; i < st.num_pages_total && i < MAX_PAGES_INFO; i++) {
    if (st.pages[i].state == RESIDENT || st.pages[i].state == SWAPPED)
      printf("VA=0x%x state=%s dirty=%d seq=%d slot=%d\n",
             st.pages[i].va,
             st.pages[i].state == RESIDENT ? "RESIDENT" :
             (st.pages[i].state == SWAPPED ? "SWAPPED" : "UNMAPPED"),
             st.pages[i].is_dirty,
             st.pages[i].seq,
             st.pages[i].swap_slot);
  }

  printf("\n=== Done ===\n");
  exit(0);
}*/
// Stack growth test
/*#include "user.h"
#include "kernel/memstat.h"


int
main(void)
{
  struct proc_mem_stat st;

  printf("=== Fixed Stack Growth Test ===\n");

  uint64 sp;
  asm volatile("mv %0, sp" : "=r"(sp));
  printf("Current SP = 0x%lx\n", sp);

  // Go slightly *below* stack top (should be near 0x3fe0)
  uint64 target = (sp & ~0xFFF) - 0x1000; // one page below current SP
  printf("Writing at target address 0x%lx (below stack)\n", target);

  volatile uint64 *below_stack = (uint64 *)target;
  *below_stack = 0xDEADBEEF;  // should trigger PAGEFAULT cause=stack
  printf("Wrote successfully!\n");

  if (memstat(&st) == 0) {
    for (int i = 0; i < st.num_pages_total && i < MAX_PAGES_INFO; i++) {
      if (st.pages[i].state == RESIDENT)
        printf("VA=0x%x state=RESIDENT seq=%d\n", st.pages[i].va, st.pages[i].seq);
    }
  }

  printf("=== Done ===\n");
  exit(0);
}
*/
//Discard exec test
/*#include "user.h"
#include "kernel/memstat.h"



#define PAGE 4096

int main() {
  printf("=== DISCARD exec test ===\n");

  // Make sure we fault in at least one text page
  asm volatile("nop\nnop\nnop\n");

  // Allocate/touch a lot to force eviction of early pages
  int N = 120; // bump until you see MEMFULL
  char *base = sbrk(N * PAGE);
  for (int i = 0; i < N; i++)
    base[i * PAGE] = (char)i; // write

  printf("=== done ===\n");
  exit(0);
}
*/