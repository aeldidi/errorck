typedef int (*fn_t)(void);

static int impl(void) { return 1; }

static fn_t get_fn(void) { return impl; }

struct ops {
  fn_t foo;
};

int main(void) {
  struct ops o = { .foo = impl };
  o.foo();
  fn_t f = get_fn();
  f();
  return 0;
}
