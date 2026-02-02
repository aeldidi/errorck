#include <stdlib.h>

void log_error(void *p) { (void)p; }

// The return value is logged and then handled.
int main() {
  void *p = malloc(10);
  log_error(p);
  if (!p) {
    return 1;
  }
  return 0;
}
