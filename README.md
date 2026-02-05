`errorck` is a Clang libTooling–based static analysis tool for measuring how
often error-returning functions are ignored, partially handled, or handled
with future-proof catch-all logic in real-world C and C++ codebases.

It is intended for large-scale, empirical analysis across many projects,
not as an interactive linter.

## Overview

`errorck` analyzes calls to a user-supplied set of error-reporting functions
(provided via `--notable-functions`) and classifies how each call’s return
value is handled. It also detects simple one-layer wrapper functions that
forward error codes (e.g. project-specific allocation wrappers) and treats
calls through those wrappers equivalently.

The tool writes its output into a SQLite database suitable for later
statistical analysis.

## What `errorck` detects

For each call to a watched function (or trivial wrapper), `errorck` classifies
the handling into one of the following categories:

    ignored
    	The return value is discarded and not read.

    cast_to_void
    	The return value is explicitly discarded via a cast to void. This
    	category applies to return-value reporting only.

    assigned_not_read
    	The return value is assigned to a local variable but never read,
    	branched on, returned, logged, or passed to a handler.

    branched_no_catchall
    	The return value is used in an if or switch, but there is no
    	else/default branch to handle future error cases.

    branched_with_catchall
    	The return value is branched on and includes an else or default.

    propagated
    	The return value is returned to the caller, including return
    	expressions that contain the error value.

    passed_to_handler_fn
    	The return value is passed to a handler function (any argument
    	expression tree containing the error counts).

    used_other
    	The return value is used in some other way that does not match the
    	categories above.

    logged_not_handled
    	The return value is logged but not otherwise handled.

`errorck` emits a row for each watched call site. Duplicate rows (same name,
filename, line, column, and handling type) are dropped within a run and the
SQLite output enforces this uniqueness. There is no pass/fail classification;
interpretation is deferred to later analysis.

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
- Limited function pointer naming (direct member access only)
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

`errorck` requires a compilation database and a list of functions to watch.

    $ `errorck` --notable-functions /path/to/functions.json \
        --db results.sqlite -p /path/to/build file1.c file2.cpp ...

If you need extra compiler arguments applied to every file, provide a
`compile_flags.txt` file (one argument per line, `#` comments ignored) and pass
it with `--compile-flags`:

    $ `errorck` --notable-functions /path/to/functions.json \
        --compile-flags /path/to/compile_flags.txt \
        --db results.sqlite -p /path/to/build file1.c file2.cpp ...

Additional selection examples:

    $ `errorck` --all-non-void \
        --db results.sqlite -p /path/to/build file1.c file2.cpp ...

    $ `errorck` --exclude-notable-functions \
        --notable-functions /path/to/functions.json \
        --db results.sqlite -p /path/to/build file1.c file2.cpp ...

    $ `errorck` --list-non-void-calls \
        --db results.sqlite -p /path/to/build file1.c file2.cpp ...

`--all-non-void` analyzes every call whose callee returns a non-void type using
return-value handling and does not require `--notable-functions` (though it can
still be provided to supply handler/logger names). `--exclude-notable-functions`
treats the functions file as a blacklist, requires `--notable-functions`, and
cannot be combined with `--all-non-void`.
`--list-non-void-calls` only reports unique non-void-returning function calls,
stores them with `handlingType = observed_non_void`, and cannot be combined
with the other selection flags.

The functions file is a JSON array. Entries describing error-reporting
functions include `name` and `reporting` (either `return_value` or `errno`).
Handler functions use `name` with `"type": "handler"` and omit `reporting`.
Logger functions use `name` with `"type": "logger"` and omit `reporting`.

Schema (informal):

```json
[
  {"name": "fn", "reporting": "return_value" | "errno"},
  {"name": "handler_fn", "type": "handler"},
  {"name": "logger_fn", "type": "logger"}
]
```

If the database path already exists, `errorck` exits with an error unless
`--overwrite-if-needed` is provided to clobber it.

Results are written to the `watched_calls` table in the SQLite database with
columns: `name`, `filename`, `line`, `column`, `handling_type`, and optional
`assigned_filename`, `assigned_line`, `assigned_column` data for
`assigned_not_read` findings.

## FAQ

**Why am I seeing `<dynamic function call>` in my report?**

`errorck` uses the callee name when it can be determined directly (including
struct member function pointers like `ops.foo()`, which as an example would be
reported as `foo`), but some indirect calls don't have a stable identifier,
in cases like `(*get_func())()`. For those, the call is reported with the
placeholder name `<dynamic function call>`. These should be rare enough that
you don't see it too often.

## Scripts

`scripts/run_errorck_analysis.py` runs the common analysis pipeline.
Specifically, it first just reports all function calls, and outputs to a number
of `all_funcs*` files (`all_funcs.txt` and `all_funcs_report.db`), then runs
an error report on all function calls (outputting `all_report.db` and
`all_ignored.txt` which shows all the functions in `all_report.db` with
"ignored" handling), then runs a report on all function calls excluding the
ones listed in the `--ignored-functions` file (outputting `report.db` and
`ignored.txt` containing only the calls with "ignored" handling), and lastly
runs a final report on only the notable functions (outputting
`notable_report.db` and `notable_ignored.txt` containing only the calls with
"ignored" handling). It can be run like so:

    $ scripts/run_errorck_analysis.py \
        --notable-functions notable.json \
        --ignored-functions ignore.json \
        --compdb /path/to/compile_commands.json \
        --compile-flags /path/to/compile_flags.txt \
        --errorck /path/to/errorck \
        --output-dir out \
        file1.c file2.c

The script only analyzes files present in the compilation database; any input
paths that are not listed in `compile_commands.json` are skipped.

`scripts/list_calls.py` prints call sites from a report database by handling
type. For example,

    $ scripts/list_calls.py report.db ignored

will list all of the calls with the "ignored" handling type in `report.db`.

## Intended use

`errorck` is designed for:

- empirical studies of error handling
- large-scale analysis across many repositories
- research and auditing, not enforcement

It is not intended to replace compiler warnings or linters.

## License

Public domain, Unlicense, 0BSD, or CC0. Whichever you prefer. The 0BSD license
is included in the source distribution.
