When you are asked to write code, do so in a minimal way. Try to emphasize
clarity of expression in code above verbosity. For example, An array index used
on every line of a loop needn't be named any more elaborately than i. Saying
index or elementnumber is more to type (or calls upon your text editor) and
obscures the details of the computation. As in all other aspects of readable
programming, consistency is important in naming. If you call one variable
maxphysaddr, don't call its cousin lowestaddress. Whenever there's a convention
for something, follow it. The code should strive to be simple enough to be
obviously bug free.

When code appears non-trivial, include a comment above the non-trivial section
explaining the "why", not the "what" behind the code, with any references where
a developer can read more about why code does something.

Be critical of any and all code, but whenever it makes sense do things the way
we do it in other places throughout our codebase. Whenever you see something
suspicious or shoddy, you should leave a comment marking it as so, and explain
why. Each of these such comments should be marked with a `TODO`, explaining
briefly what the issue is, and what might need to be done to fix it. Do not try
to address many things at once, but always keep them in mind.

Whenever a requirement is unclear, you should ask for clarification.

clang-format is available, and should always be used to format code before
considering it completed. clang-tidy is available, and should also be used.
Code with any lint warnings or errors should not be considered complete. Some
lint warnings are not really relevant, and if one seems to require some
arbitrary fix which doesn't affect the quality or robustness of the code we
will remove it, however you should always ask before disabling any checks.

CMake is used to build the project. The build directory is called `build`, and
is likely already generated. You can regenerate it if needed, but this fetches
and build LLVM, which is a very large dependency and takes a long time to
compile so you should try to avoid doing so.

Test directories use prefixes: `fail_*` cases should produce findings, and
`pass_*` cases should produce none.

CHECKS.md should always be updated to reflect the logic used to perform the
various checks. Every check should have both pass and fail tests demonstrating
their functionality.
