#include <errno.h>
#include <stdlib.h>

// The errno is assigned locally and then returned when it indicates an error.
int main() {
  unsigned long x = strtoull("", NULL, 10);
  int err = errno;
  if (err) {
    return err;
  }
  return (int)x;
}
