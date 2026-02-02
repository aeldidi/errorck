#include <errno.h>
#include <stdlib.h>

void log_errno(int value) { (void)value; }

// The errno is logged but not otherwise handled.
int main() {
  unsigned long x = strtoull("", NULL, 10);
  log_errno(errno);
}
