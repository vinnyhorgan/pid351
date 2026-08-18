# pid351 conventions

A single-process OS for one Anbernic RG351P. See PLAN.md for the roadmap.

## C style

- 4-space indent, 80 columns. Not kernel style: 8-wide tabs are unreadable at
  80 columns with this much nesting.
- `snake_case` throughout. Subsystem prefix on anything crossing a file
  boundary (`plat_`, `rga_`, `core_`).
- Braces on their own line for function definitions, same line for control
  flow.
- `-std=c11` with `-D_POSIX_C_SOURCE=200809L`. Strict ISO hides POSIX
  declarations, which is how `nanosleep` broke the first device build.
- Warnings are errors in spirit: `-Wall -Wextra -Wpedantic -Wshadow
  -Wconversion` must stay clean on both targets before anything is committed.
- Comments explain **why**, never what. A comment restating the code is worse
  than no comment. Block comments at the top of each file explain what the
  file is for and what it deliberately does not do.
- No dependencies. libc, and SDL3 for the host-only backend. Nothing else gets
  linked without a specific argument for it.

## Commits

    pid351: imperative summary under ~50 chars

    Body wrapped at 72, explaining why the change exists rather than what it
    does - the diff already says what it does. Mention anything that was
    considered and rejected, since that is the part that is expensive to
    rediscover later.

Subsystem prefix, imperative mood, blank line, wrapped body. Standard kernel
and git convention.

## Design rules

- Anything that can be a compile-time constant is one. There are no config
  files and there will not be any.
- This targets exactly one machine. Autodetection, capability probing and
  fallbacks for hardware that is not present are all dead weight - hardcode
  what the recon data proves is there.
- Battery first, then minimalism, then performance. When they conflict, that
  is the order.
- Never busy-wait. Blocking on vblank or on the audio buffer is the only
  acceptable way to wait for time to pass.
