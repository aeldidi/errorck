int skip_me(void) { return 1; }
int analyze_me(void) { return 2; }

int main(void) {
  skip_me();
  analyze_me();
  return 0;
}
