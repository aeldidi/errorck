# CHECKS

This document defines the exact detection logic implemented by `errorck`.

## Notable functions input

`--notable-functions` expects a JSON array. Each entry is an object with:

- `name`: the function name to watch
- `reporting`: the error reporting style, either `return_value` or `errno`

Example:

```json
[
  {"name": "malloc", "reporting": "return_value"},
  {"name": "strtoull", "reporting": "errno"}
]
```

## Implemented checks

`errorck` currently reports only one handling type: `ignored`. The meaning of
`ignored` depends on the function’s `reporting` setting.

### ignored (return_value)

For `reporting = return_value`, a call is reported as `ignored` when the call’s
result is unused. The tool walks upward through expression wrappers and treats
the call as ignored when it appears in statement position:

- a direct child of a compound statement, or
- the then/else body of an `if`, or
- the body of `while`, `do`, `for`, or `switch`, or
- the substatement of a `case`, `default`, `label`, or `attributed` statement.

If the call’s value is used in any expression context (assignment, comparison,
argument to another call, return statement, etc.), it is not reported as
`ignored` for `return_value`.

### ignored (errno)

For `reporting = errno`, the return value is ignored for reporting purposes and
the tool instead checks whether `errno` is referenced immediately after the
call. A call is reported as `ignored` when no `errno` reference is found in:

- the statement containing the call, or
- the immediately following statement in the same compound statement.

An `errno` reference is detected if the AST contains:

- a reference to a variable named `errno`, or
- a direct call to `__errno_location` or `__error` (common macro expansions).

Direct assignments to `errno` (for example `errno = 0`) are not treated as
checks. No control-flow or dataflow analysis is performed; the search is
strictly local to the call’s statement and its immediate successor.
