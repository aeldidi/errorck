#include <stdlib.h>

int main() {
  void *p = malloc(10);
  int flag = 0;
  if (flag) {
    flag = 1;
  } else {
    flag = 2;
  }
  return p != NULL;
}
