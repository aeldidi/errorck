#include <errno.h>
#include <stdlib.h>

int main() {
  errno = 0;
  unsigned long x = strtoull("", NULL, 10);
  if (errno == ERANGE) {
    return 1;
  }
  return (int)x;
}
