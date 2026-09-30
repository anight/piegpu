# piegpu documentation

Building, wiring and using piegpu: the [README](../README.md) at the top.

| Document | What |
|---|---|
| [installation.md](installation.md) | putting piegpu on a card and connecting to it, step by step on Linux, Windows and macOS, with troubleshooting |
| [protocol.md](protocol.md) | the wire protocol between a host and the RPi (normative): the I2S link and its side-band signals, packets, commands, replies, limits; the USB stream (§13) |
| [host-library.md](host-library.md) | the host side: libpgpu (pgpu and pgl), how pgl maps OpenGL ES to the wire and where it differs from GL, video from files, pgl on a PC |
| [development.md](development.md) | the RPi side inside: the link, rendering, the screen, video, the run log, the control list checker |

The code cites the protocol by section, e.g. `docs/protocol.md 7.12`.
