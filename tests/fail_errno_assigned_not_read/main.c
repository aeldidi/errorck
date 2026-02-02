#include <errno.h>
#include <stdlib.h>

// The errno is assigned to another value which isn't read later.
int main() {
  unsigned long x = strtoull("", NULL, 10);
  int other = errno;
}
