# PLANK Client

PLANK Client is the desktop application for connecting to PLANK workstations
from Ubuntu or macOS. It provides low-latency video, audio and input for creative
work and general remote desktop use, with attention to color precision, tablets
and multi-display workflows.

This repository contains the shared Client source. The
[main PLANK repository](https://github.com/instinctual/plank) supplies its pinned
dependencies, native transport, packaging, releases and build instructions.
PLANK Linux and macOS Hosts use the same Client application.

[Downloads](https://github.com/instinctual/plank/releases) ·
[Documentation](https://github.com/instinctual/plank/blob/main/docs/README.md) ·
[Build from source](https://github.com/instinctual/plank/blob/main/docs/development/build/from-source.md)

## Platforms

| Client platform | Architecture | Distribution |
| --- | --- | --- |
| Ubuntu 26.04 | x86-64 | DEB |
| macOS 15 or newer | Apple Silicon | Signed, notarized PKG |

Wayland is the qualified Linux desktop path. One Mac Client package targets
macOS 15 and newer, using SDK 27 or newer and runtime checks for newer APIs.
This does not lower the separate macOS Host's minimum OS requirement.

Other platforms inherited from upstream are not qualified PLANK products.
See the [platform and acceptance documentation](https://github.com/instinctual/plank/blob/main/docs/development/platforms.md)
for the supported scope and remaining hardware tests.

## Features

- **Bookmarks:** save a Host address and nickname even while it is offline.
  Each bookmark has its own display, capture and encoding choices, with bitrate
  remembered per encoding profile.
- **Precise video:** H.264 and HEVC decoding for the selected Host profile,
  including supported 8/10-bit and 4:4:4 paths. Exact-format hardware decoding is
  tried first; software decoding preserves the same format when needed.
- **Presentation:** windowed/fullscreen operation, native-pixel and scaled
  presentation, and supported multi-display layouts. macOS uses native Metal
  presentation; Linux has profile-specific GPU presentation paths.
- **Input:** keyboard shortcuts, absolute mouse positioning, buttons, scrolling
  and Wacom/pen input. Forwarding capabilities and tablet application behavior
  depend on both platforms.
- **Audio and clipboard:** remote desktop sound and bidirectional text clipboard
  synchronization. Optional microphone forwarding is supported to macOS Hosts.
- **Session toolbar:** rendered frame rate, incoming bitrate, packet loss and
  RTT, an interactive encoder-target slider, and window/session controls.
- **Transport and recovery:** native encrypted QUIC with media error correction,
  account authentication, Host identity checks, session takeover and reconnect.

Hardware decoding is profile-specific. For example, the qualified Intel Linux
path can decode HEVC 10-bit 4:4:4 through VA-API/Y410, while H.264 High 10 4:4:4
uses FFmpeg software decoding on that hardware. PLANK does not silently replace
a requested format with 8-bit or 4:2:0 to obtain hardware acceleration.

GameStream interoperability, gamepads, generic touchscreen forwarding, HDR and
automatic router port mapping are not part of the current PLANK Client.
Use compatible PLANK Host/Client releases, not stock Sunshine or Moonlight peers.

## Getting started

1. Download the Client package for your platform from
   [PLANK releases](https://github.com/instinctual/plank/releases).
2. On Ubuntu, install the downloaded DEB with `sudo apt install ./PACKAGE.deb`,
   replacing `PACKAGE.deb` with its filename. On macOS, open the PKG.
3. Complete any OS [permission setup](https://github.com/instinctual/plank/blob/main/docs/user/permissions.md).
   Quit the Client before upgrading.
4. Open PLANK Client and add a bookmark. Reachable Hosts advertise their
   supported capture and encoding options; offline bookmarks retain manual choices.
5. Connect using your workstation's OS account.

The application is interactive: packages do not install a Client service or
autostart entry. Administrator settings are in `/etc/plank/client.conf` on
both platforms; bookmarks and ordinary preferences remain per-user. See the
[configuration template](https://github.com/instinctual/plank/blob/main/packaging/client/config/plank-client.conf).

The default Host port is 28989 on TCP and UDP. Routing and firewall access
must be provided by the administrator. The Client remembers a Host's identity
on first explicit connection and requires confirmation for a changed identity;
read the [trust model](https://github.com/instinctual/plank/blob/main/docs/security/host-identity-trust.md)
before treating first-use trust as verified identity.

Remove the Ubuntu package with `sudo apt remove plank-client`. macOS removal
and optional data purge are documented in the
[uninstall guide](https://github.com/instinctual/plank/blob/main/docs/user/macos-configuration.md#client-uninstall).

## Building and contributing

Build this component through the
[parent PLANK checkout](https://github.com/instinctual/plank), where it lives at
`apps/client/`. Follow the
[from-source guide](https://github.com/instinctual/plank/blob/main/docs/development/build/from-source.md)
and [release build runbook](https://github.com/instinctual/plank/blob/main/docs/development/build/release-build-runbook.md).

The current application uses Qt 6, SDL3, a pinned/patched FFmpeg and PLANK's Rust
transport. The parent repository owns exact dependency versions, platform build
scripts and package validation. Generic Moonlight build recipes or inherited
prebuilt dependency bundles are not PLANK's supported build workflow.

Client code changes belong here; shared transport, packaging and cross-product
changes also need the parent repository. Coordinate the parent Client pin with
component changes. See the
[contributor guide](https://github.com/instinctual/plank/blob/main/CONTRIBUTING.md).
Report PLANK-specific issues to PLANK, not upstream Moonlight.

## Credits and licensing

PLANK Client is derived from
[Moonlight Qt](https://github.com/moonlight-stream/moonlight-qt).
We retain its Git history and thank the Moonlight contributors for the foundation.

See [LICENSE](LICENSE) and the notices in individual files and dependencies.
The parent project's linked native transport is AGPL-3.0-or-later; it has its own
license and is not covered solely by this repository's license file.
