struct ops { int (*foo)(void); };

static int impl(void) { return 1; }

int main(void) {
  struct ops o = { .foo = impl };
  o.foo();
  return 0;
}
