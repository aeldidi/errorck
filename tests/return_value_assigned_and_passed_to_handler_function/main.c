#include <stdlib.h>

void handle(void *x) { (void)x; }

// The return value is passed to a handler and reported as passed_to_handler_fn.
int main() {
  int *x = malloc(10);
  int *y = x;
  handle(y);
}
