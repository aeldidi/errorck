#include <stdlib.h>

// The return value is assigned to another value which isn't read later.
int main() {
  int *x = malloc(10);
  int *other = x;
}
