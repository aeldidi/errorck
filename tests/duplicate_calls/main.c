#include <stdlib.h>

int main() {
  // Force identical source locations to exercise output de-duplication.
#line 1 "dup.inc"
  malloc(10);
#line 1 "dup.inc"
  malloc(10);
  return 0;
}
