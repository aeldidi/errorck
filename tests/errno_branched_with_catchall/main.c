#include <errno.h>
#include <stdlib.h>

int main() {
  unsigned long x = strtoull("", NULL, 10);
  switch (errno) {
  case 0:
    return (int)x;
  default:
    return 1;
  }
}
