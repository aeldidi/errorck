#include <stdlib.h>

int main() {
  void *p = malloc(10);
  if (!p) {
    return 1;
  }
  free(p);
  return 0;
}
