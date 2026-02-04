#include <stdlib.h>

void consume(void *p) { (void)p; }

int main() {
  void *p = malloc(10);
  consume(p);
  return 0;
}
