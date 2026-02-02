#include <stdlib.h>

// Return value isn't ignored, but errno is, and this function requires errno
// to be checked.
int main() { unsigned long x = strtoull("", NULL, 10); }
