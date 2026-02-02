#include <stdlib.h>

void handle(void *p) { (void)p; }

// Because the function is passed to another one which was registered as a
// "handler", it's ok.
int main() {
  void *p = malloc(10);
  handle(p);
}
