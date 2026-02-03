#include <stdlib.h>

void log_error(void *p) { (void)p; }

// The return value is logged and then handled without branching.
int main() {
  void *p = malloc(10);
  log_error(p);
  (void)p;
  return 0;
}
