#include <stdlib.h>

void log_error(void *p) { (void)p; }

// The return value is logged but not otherwise handled.
int main() {
  void *p = malloc(10);
  log_error(p);
}
