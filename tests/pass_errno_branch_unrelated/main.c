#include <errno.h>
#include <stdlib.h>

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
