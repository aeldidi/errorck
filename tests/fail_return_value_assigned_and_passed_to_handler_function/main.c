#include <stdlib.h>

void handle(void *x) { (void)x; }

// The return value is passed to a function which is considered a "handler", so
// this should pass the check with no report.
int main() {
  int *x = malloc(10);
  int *y = x;
  handle(y);
}
