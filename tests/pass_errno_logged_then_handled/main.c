#include <errno.h>
#include <stdlib.h>

void log_errno(int value) { (void)value; }

// The errno is logged and handled in the same statement.
int main() {
  unsigned long x = strtoull("", NULL, 10);
  if (errno != 0) {
    log_errno(errno);
    return 1;
  }
  return 0;
}
