# patches/unverified — preserved, deliberately NOT built

`build.sh` applies `patches/*.mypatch`, plus `patches/diagnostic/*.mypatch` when
`INCLUDE_DIAGNOSTIC=1`. It does **not** look in this directory.

This is where dev-tree-only work goes when it is found (a "leaky export") but is too large,
too unreviewed, or too experimental to ship as-is. Preserving it here means a tree reset
cannot destroy it, while keeping unreviewed code out of every runtime.

A patch leaves this directory only after it has been reviewed, gated, and tested — at which
point it moves to `patches/` (or `patches/diagnostic/` if it is instrumentation).
