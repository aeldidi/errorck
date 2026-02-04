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

`errorck` always emits a handling type for each watched call. If a call does
not match any specific category, it is reported as `used_other`. The meaning
depends on the function’s `reporting` setting.

`errorck` reports nine handling types: `ignored`, `cast_to_void`,
`assigned_not_read`, `branched_no_catchall`, `branched_with_catchall`,
`propagated`, `passed_to_handler_fn`, `used_other`, and `logged_not_handled`.

Precedence notes:

- `cast_to_void` overrides `ignored`.
- `passed_to_handler_fn` overrides branching when both occur in the same
  statement.
- `propagated` overrides branching when the error value is returned inside a
  branch.
- Logging does not stop analysis; a later handling outcome overrides
  `logged_not_handled`.

### cast_to_void (return_value)

For `reporting = return_value`, a call is reported as `cast_to_void` when the
return value (directly or via an assigned local) is explicitly cast to void in
statement position.

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
`passed_to_handler_fn`. If the value is passed to a logger and never otherwise
handled in the same compound statement, the call is reported as
`logged_not_handled`.

### branched_no_catchall (return_value)

For `reporting = return_value`, a call is reported as `branched_no_catchall`
when its value is used in an `if` or `switch` condition and there is no
catch-all branch.

- `if` chains only count as catch-all when there is a final `else` that is not
  another `if`.
- `switch` statements only count as catch-all when they contain a `default`
  label.

### branched_with_catchall (return_value)

For `reporting = return_value`, a call is reported as `branched_with_catchall`
when its value is used in an `if` or `switch` condition and a catch-all branch
is present (final `else` or `default`).

### propagated (return_value)

For `reporting = return_value`, a call is reported as `propagated` when the
error value is returned to the caller. Return expressions that contain the
error value anywhere are treated as propagation, including returns inside
branch bodies.

### passed_to_handler_fn (return_value)

For `reporting = return_value`, a call is reported as `passed_to_handler_fn`
when the error value is passed to a function declared as a handler (`type:
"handler"`). This includes any argument expression tree that contains the
error value.

### assigned_not_read (errno)

For `reporting = errno`, a call is reported as `assigned_not_read` when the
statement containing the call or the immediately following statement assigns
`errno` directly to a local variable, but that assigned value is never read in
a non-assignment context within the same compound statement.

### propagated (errno)

For `reporting = errno`, a call is reported as `propagated` when `errno` (or a
local assigned from `errno`) is returned to the caller. Return expressions that
contain the error value anywhere are treated as propagation.

### branched_no_catchall (errno)

For `reporting = errno`, a call is reported as `branched_no_catchall` when an
`if` or `switch` condition references `errno` in the call statement or the
immediately following statement, and there is no catch-all branch.

If `errno` is assigned to a local variable in the call statement or the
immediately following statement, and a later `if`/`switch` condition uses that
local within the same compound statement, the call is also reported as
`branched_no_catchall`.

### branched_with_catchall (errno)

For `reporting = errno`, a call is reported as `branched_with_catchall` when an
`if` or `switch` condition references `errno` in the call statement or the
immediately following statement, and a catch-all branch is present.

If `errno` is assigned to a local variable in the call statement or the
immediately following statement, and a later `if`/`switch` condition uses that
local within the same compound statement, the call is also reported as
`branched_with_catchall`.

### passed_to_handler_fn (handler)

If an error value is passed to a function declared as a handler (`type:
"handler"`), the call is reported as `passed_to_handler_fn` and analysis of
that value
stops.

### logged_not_handled (logger)

If an error value is passed to a function declared as a logger (`type:
"logger"`), the call is reported as `logged_not_handled` when the value is not
otherwise handled within the same compound statement. Logging does not stop
analysis: if the value is later handled, no `logged_not_handled` report is
emitted.

Branch detection takes priority over handler or logger detection when the error
value is used in an `if` or `switch` condition unless a handler use is also
present in the same statement.

For `return_value`, direct uses are detected when the call result is passed as
an argument to a handler or logger in the enclosing statement. For `errno`,
direct handler/logger usage is only checked in the call statement and its
immediate successor; if `errno` is assigned to a local there, later statements
in the same compound statement are tracked for handler/logger use.

### used_other

If an error value is used but does not match any other category, the call is
reported as `used_other`. Examples include passing the error to a non-handler
function or using it in arithmetic expressions. For `errno`, explicit casts to
void of a local copy are also reported as `used_other`.

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
