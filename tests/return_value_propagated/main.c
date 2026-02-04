#include <stdlib.h>

void *alloc(void) {
  void *p = malloc(10);
  if (!p) {
    return p;
  }
  return p;
}

int main() { return alloc() != NULL; }
