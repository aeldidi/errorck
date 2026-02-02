# CHECKS

This document defines the exact detection logic implemented by `errorck`.

## Notable functions input

`--notable-functions` expects a JSON array. Each entry is an object with:

- `name`: the function name to watch
- `reporting`: the error reporting style, either `return_value` or `errno`

Handler functions may be listed with:

- `name`: the function name that handles errors
- `type`: `"handler"`

Logger functions may be listed with:

- `name`: the function name that logs errors
- `type`: `"logger"`

Example:

```json
[
  {"name": "malloc", "reporting": "return_value"},
  {"name": "strtoull", "reporting": "errno"},
  {"name": "panic", "type": "handler"},
  {"name": "log_error", "type": "logger"}
]
```

## Implemented checks

`errorck` currently reports four handling types: `ignored`,
`assigned_not_read`, `used_other`, and `logged_not_handled`. The meaning
depends on the function’s `reporting` setting.

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

### assigned_not_read (return_value)

For `reporting = return_value`, a call is reported as `assigned_not_read` when
its return value is assigned directly to a local variable, but that value is
never read in a non-assignment context within the same compound statement.

Assignments that simply copy the value into another local variable are treated
as propagation. The propagation chain is followed until the value is used or
the block ends. The report includes the location of the final assignment source
that left the value unread.

If the value is passed to a handler during this scan, the call is reported as
`used_other`. If the value is passed to a logger and never otherwise handled in
the same compound statement, the call is reported as `logged_not_handled`.

### assigned_not_read (errno)

For `reporting = errno`, a call is reported as `assigned_not_read` when the
statement containing the call or the immediately following statement assigns
`errno` directly to a local variable, but that assigned value is never read in
a non-assignment context within the same compound statement.

### used_other (handler)

If an error value is passed to a function declared as a handler (`type:
"handler"`), the call is reported as `used_other` and analysis of that value
stops.

### logged_not_handled (logger)

If an error value is passed to a function declared as a logger (`type:
"logger"`), the call is reported as `logged_not_handled` when the value is not
otherwise handled within the same compound statement. Logging does not stop
analysis: if the value is later handled, no `logged_not_handled` report is
emitted.

For `return_value`, direct uses are detected when the call result is passed as
an argument to a handler or logger in the enclosing statement. For `errno`,
handler/logger usage is only checked in the call statement and its immediate
successor; later statements are not considered.

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
