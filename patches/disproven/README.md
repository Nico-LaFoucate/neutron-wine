# Disproven patches — NOT applied by build.sh

`build.sh` copies only `patches/*.mypatch`, so anything in this directory is inert. Kept for the
record so the same idea is not rediscovered.

## neutron-async-branchc-socket-gate.mypatch (2026-09-06)
Gated IOCP branch C on `get_fd_type( async->fd ) != FD_TYPE_SOCKET`. Compiled clean and made the
synthetic CLR repro pass — **but the real aescripts manager still crashed at the identical
`clr+0x2C19A5`**. Measurement: all 7,898 branch-C packets in a live session are `FD_TYPE_DEVICE`
(message-mode named pipes), including the 2,125 that reach the .NET threadpool port. The socket
premise came from a repro that used `Socket.Receive()` and was unrepresentative of the app.

⛔ No fd-type gate can work: Premiere's branch-C traffic and the manager's are the SAME type.
✅ Superseded by `neutron-iocp-zz-branchc-nopost.mypatch`.
