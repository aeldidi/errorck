#include <errno.h>
#include <stdlib.h>

// Errno is explicitly discarded, which is reported as used_other.
int main() {
  unsigned long x = strtoull("", NULL, 10);
  int err = errno;
  int flag = 0;
  if (flag) {
    flag = 1;
  } else {
    flag = 2;
  }
  (void)err;
  return (int)x;
}
