#include <errno.h>
#include <stdlib.h>

void handle(int x) { (void)x; }

// The errno is assigned to another value which is then passed to a handler.
int main() {
  unsigned long x = strtoull("", NULL, 10);
  int other = errno;
  handle(other);
}
