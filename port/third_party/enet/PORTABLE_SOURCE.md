# ENet Unix source provenance

`unix.c` is unmodified ENet 1.3.13 from
`https://github.com/lsalzman/enet`, tag `v1.3.13`, commit
`f7c46f03fd8d883ac2811948aa71c7623069d070`.

SHA-256: `dd4db651354be878d047839b75d89cd07b18bb5a8a78a3711c7e49ac6d02151d`.

The existing Melee Unlocked copies of `host.c`, `packet.c`, `peer.c`,
`protocol.c`, `win32.c`, `include/enet/enet.h`, and `include/enet/unix.h`
were compared with this tag and match exactly. Its `LICENSE` also matches
the existing license here (SHA-256
`6f9dc806ef7e1d61f296cd1bb03f5f8522f5aa7c2e85158e07c65a55f8b741af`).

The added file fills the missing Unix host implementation without changing
the vendored ENet protocol version. Slippi's separate ENet 1.3.17 submodule
must not be mixed with these 1.3.13 sources.

The upstream CMake platform feature probes are required on generic Unix:
`HAS_FCNTL`, `HAS_POLL`, `HAS_GETHOSTBYNAME_R`, `HAS_GETHOSTBYADDR_R`,
`HAS_INET_PTON`, `HAS_INET_NTOP`, `HAS_MSGHDR_FLAGS`, `HAS_SOCKLEN_T`.
Define only features the host actually provides. The source itself supplies
the original macOS feature overrides and uses `select` on macOS.
