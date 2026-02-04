#include <stdlib.h>

void handle(void *p) { (void)p; }

// The return value is passed to a handler and reported as passed_to_handler_fn.
int main() {
  void *p = malloc(10);
  handle(p);
}
