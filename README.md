`errorck` is a Clang libTooling–based static analysis tool for measuring how
often error-returning functions are ignored, partially handled, or handled
with future-proof catch-all logic in real-world C and C++ codebases.

It is intended for large-scale, empirical analysis across many projects,
not as an interactive linter.

## Overview

`errorck` analyzes calls to a fixed set of error-returning functions
(hardcoded in the source for now) and classifies how each call’s return
value is handled. It also detects simple one-layer wrapper functions that
forward error codes (e.g. project-specific allocation wrappers) and treats
calls through those wrappers equivalently.

The tool emits machine-readable output suitable for ingestion into a
database and later statistical analysis.

## What `errorck` detects

For each call to a watched function (or trivial wrapper), `errorck` classifies
the handling into one of the following categories:

    ignored
    	The return value is discarded and not read.

    assigned_not_read
    	The return value is assigned to a local variable but never read,
    	branched on, or returned.

    branched_no_catchall
    	The return value is used in an if or switch, but there is no
    	else/default branch to handle future error cases.

    branched_with_catchall
    	The return value is branched on and includes an else or default.

    propagated
    	The return value is returned directly to the caller.

    used_other
    	The return value is used in some other way (logging, passed to
    	another function, etc.).

For reporting purposes, the following are considered “ignored” error
conditions:

- ignored
- assigned_not_read
- branched_no_catchall

## Trivial wrapper detection

`errorck` detects one layer of trivial wrappers around watched functions.
A function is considered a trivial wrapper if it:

- returns a watched function call directly, or
- assigns the result of a watched function call to a local variable,
  optionally branches or logs based on that value, and then returns the
  value unchanged.

Wrappers are reported explicitly, and calls through wrappers are attributed
to the underlying base function.

## Limitations

`errorck` deliberately trades completeness for scalability and clarity.

Current limitations include:

- One-layer wrapper detection only
- No interprocedural dataflow
- No function pointer resolution
- Simplified wrapper body patterns (single return, single result variable)
- Catch-all detection limited to else/default
- Analysis is per-translation-unit

These limitations are documented and must be considered when interpreting
results.

## Building

`errorck` requires LLVM/Clang with libTooling. Assuming you have a build of
LLVM 18.1.8 at `/opt/llvm-18.1.8`, run from the build directory:

```
cmake -G Ninja -DLLVM_ROOT=/opt/llvm-18.1.8
```

then build using `ninja`.

## Running

`errorck` requires a compilation database.

    $ `errorck` -p /path/to/build file1.c file2.cpp ...

Output is written to stdout as JSON Lines (one record per line).

Example:

    $ `errorck` -p . src/*.c > results.jsonl

## Intended use

`errorck` is designed for:

- empirical studies of error handling
- large-scale analysis across many repositories
- research and auditing, not enforcement

It is not intended to replace compiler warnings or linters.

## License

Public domain, Unlicense, 0BSD, or CC0. Whichever you prefer. The 0BSD license
is included in the source distribution.
