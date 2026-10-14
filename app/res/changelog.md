## 1.1.037 — Client session recovery candidate

### Client
- Fix tablet-cursor cleanup that could crash the Linux client on Wayland.
- Prevent late input callbacks from using a destroyed session input handler.
- Handle failed toolbar/cursor allocations safely and improve stalled-callback diagnostics.
- Retain all Wacom recovery changes from the previous candidate.

### Host
- No additional Host changes; retains the previous candidate's Wacom recovery fixes.

## 1.1.036 — Wacom recovery candidate

### Client
- Improve Linux tablet handling during focus changes and reconnects.
- Keep tablet control requests from blocking pen input.
- Distribute the Mac Client directly as a signed PKG, without a disk-image wrapper.

### Host
- Release held pen and button state reliably when a Linux session ends or is replaced.
- Recover unhealthy forwarded tablet devices while preserving healthy device identities.
- Handle short input bursts without immediately ending the connection.

## 1.1.035 — Consolidated mainline build

### Client
- Combine the Mac window-scaling improvements and Linux Wacom recovery fixes in one build.
- Preserve fine detail in smaller Mac windows and keep the original stream size when leaving fullscreen.
- Keep Linux pen input and cursor updates responsive during tablet queries and temporary connection congestion.

### Host
- Improve Linux GPU downscaling while preserving color precision.
- Improve tablet reply, suspend and reconnect handling.

## 1.1.034 — Scaling quality candidate

### Client
- Use the Mac's current display resolution when choosing the stream size.
- Smoothly reduce video in smaller Mac windows without skipping fine lines.
- Keep the stream resolution unchanged when switching between fullscreen and windowed mode.

### Host
- Improve Linux NvFBC/NVENC downscaling with GPU area averaging, preserving RGB identity and 10-bit conversion.
- Keep exact-size capture conversion unchanged.

## 1.1.033 — Wacom recovery candidate

### Client
- Preserve clicks and releases when the connection temporarily cannot accept more input.
- Keep Linux tablet queries from blocking pen input and cursor updates.
- Update the Wayland tablet cursor independently of video playback.

### Host
- Preserve tablet replies through temporary connection congestion.
- Improve Linux tablet suspend and reconnect handling without changing a healthy tablet's identity.
- Keep pen pressure and contact intact when pressing pen buttons.

## 1.1.030 — Audio synchronization and mainline release

### Client
- Keep audio aligned to Mac Host timestamps using smooth, bounded resampling.
- Protect playback from excessive catch-up when the audio device needs more data.
- Restore video-clock tracking on the Linux hardware-rendering path.
- Include microphone and camera forwarding, Mac permission setup, and the recent connection and tablet fixes.

### Host
- Include PLANK Output, microphone and camera support on macOS, plus the recent login and display recovery fixes.
- Upgrade Host and Client together: network recovery is incompatible with the previous published 1.0.143 release.

## 1.1.029 — Protect audio playback during synchronization

### Client
- Use actual audio-output requests to avoid draining playback while correcting sync.
- Resume catch-up gradually after the output has recovered, without adding a new playback buffer.
- Recover low audio headroom through smooth resampling, including when the device clock runs faster.
- Record output shortfalls separately from network loss for real-world qualification.

### Host
- No changes.

## 1.1.027 — Correct sustained audio drift

### Client
- Use Host audio/video timestamps to correct drifting sound when connected to a macOS Host.
- Stop stale timing corrections during video interruptions and protect against audio-buffer starvation.
- Keep the existing Linux Host synchronization policy; no Host update is required.

## 1.1.026 — Restore hardware-rendered video timing

### Client
- Keep the video clock available to audio correction when hardware rendering consumes a frame.
- Record source audio/video timing to help verify sustained synchronization.

### Host
- No changes.

## 1.1.025 — Audio synchronization diagnostics

### Client
- Preserve Host audio timestamps through decoding and record estimated audio/video alignment for investigation.
- Playback timing is unchanged; this candidate measures the reported drift before a synchronization correction is applied.

### Host
- No changes.

## 1.1.024 — Wacom focus and contact recovery

### Client

- macOS: follow the active stream window when switching Spaces, windows or applications, so tablet input can resume on focus return.

### Host

- Linux: release held tablet contact when forwarding pauses, without recreating the tablet or changing its identity.
- Handle interrupted contact reads and partial release writes, and verify cleanup before reporting success.

## 1.1.023 — Host setup opens in front

### Host

- Bring macOS setup in front of Installer when it opens, without keeping it always-on-top.
- Preserve the accepted centering behavior as the desktop resolution changes.

## 1.1.022 — Host setup follows display changes

### Host

- Keep macOS setup centered when the desktop resolution changes after installation.
- Stop automatic centering once you drag the setup window yourself.

## 1.1.021 — Consistent Mac setup windows

### Client

- Give installer permission setup native macOS styling and the same clear columns as Host setup.
- Keep Refresh and Close together at the lower right.

### Host

- Open setup centered on the usable desktop.
- Move Refresh and Close to the lower right.

## 1.1.020 — Permission setup after installation

### Client

- macOS: open permission setup automatically after installation, without connecting or opening bookmarks.
- Keep launch-time permission checks for new users and changed approvals.

### Host

- macOS: retain automatic installer setup and clearly defer approval when nobody is logged in.
- Remove the unverifiable System Audio status indicator while retaining consent setup and a Settings shortcut.

## 1.1.019 — Clearer Mac setup and uninstall

### Client

- macOS: review permission status and open the relevant system settings from Configuration.
- macOS: include an uninstaller, with optional cleanup of your saved Client data.

### Host

- macOS: show permissions and optional devices in an aligned status panel with clear actions.
- macOS: remove PLANK Camera safely during uninstall; leave the Host intact if approval or a restart is needed.

## 1.1.018 — Distinct Mac app icons

### Client

- macOS: use the approved monitor-framed artwork to distinguish Client from Host.

### Host

- macOS: use the approved open-landscape artwork, keeping the shared warm PLANK style.

## 1.1.017 — Mac workstation names

### Host

- macOS: use the operating-system hostname by default, matching Linux; custom names remain configurable.
- Upgrade the old installer-generated name to the automatic default without changing machine identity or other settings.

## 1.1.016 — Permission setup and Mac Host settings

### Client

- macOS: request microphone and attached USB Wacom permissions at app launch instead of during a session.
- Keep sessions usable without microphone permission; enabling it no longer opens a permission prompt over the stream.

### Host

- macOS: include system-audio consent in the Host app's permission setup before remote use.
- macOS: optionally show the desktop username beside In Session; publication is off by default.
- macOS: make the connection inactivity timeout configurable, with fully documented settings.

## 1.1.015 — Workstation session indicator

### Client

- Show In Session when a workstation has a logged-in desktop or a remote session.
- Show the desktop username only when the Linux Host administrator enables it.
- Optionally remember the last successful sign-in username for each bookmark; passwords are never saved.
- Rename the physical-display layout option to Match Host; display behavior is unchanged.
- macOS: center the toolbar below the camera-notch area instead of shifting it sideways.
- macOS: install the Client and its administrator configuration together with a PKG installer.

### Host

- Report session status without account lookups or session cleanup during discovery.
- Include connections to the Mac login screen. Same-user access and takeover rules are unchanged.
- macOS: use /etc/plank/host.conf for administrator settings, preserving existing settings and machine identity on upgrade.

## 1.1.014 — Mac virtual audio clock correction

### Host

- Correct the clock timestamp period advertised by PLANK Output and PLANK Microphone to meet Core Audio requirements.
- Keep microphone packets at 10 ms and support larger bounded Core Audio reads.

## 1.1.013 — Mac capture clock

### Host

- Use PLANK Output as the explicit clock for remote audio capture.
- Add bounded capture-timing diagnostics for intermittent audio gaps during camera use.

## 1.1.012 — Lower overhead for Mac session checks

### Host

- Check desktop ownership in the background every 500 ms while keeping user-switch and sleep notifications responsive.
- Expire stale session information after one second without blocking audio or typing.

## 1.1.011 — Mac audio and input scheduling

### Host

- Move repeated macOS session checks off the media queue to reduce audio dropouts and typing delays during application startup and camera use.
- Retain session revocation and enforce a freshness limit when system checks stall.

## 1.1.010 — Camera and microphone timing

### Client

- Default microphone activation to Manual for new configurations; preserve saved preferences.
- Ubuntu: retain capture timing with stereo Opus audio when supported by the Host.
- Preserve microphone compatibility with Hosts using the previous stereo format.

### Host

- Request a recovery frame when another application joins PLANK Camera.
- Use timestamped microphone playback to align camera presentation when both are active.
- Preserve camera-only operation and compatibility with previous Clients.

## 1.1.009 — Remote output routing

### Host

- Forward audio sent to PLANK Output while physical speakers retain normal local playback.
- Fix automatic PLANK Output selection when a session connects.
- Keep remote volume and mute tied to PLANK Output when another device is selected locally.

## 1.1.008 — Camera setup status

### Host

- Recognize an enabled PLANK Camera during macOS setup and stop offering Enable Camera again.
- Update an already-enabled camera extension while retaining the existing approval when macOS permits.

## 1.1.007 — Mac remote audio output

### Host

- Add PLANK Output as the macOS playback destination during remote sessions.
- Restore previous playback and alert devices after disconnect, with routing recovery after Host failures.
- Keep remote volume and mute independent of the physical output after selection.
- Recommend restarting after installation to load updated PLANK audio devices; allow restarting later.

## 1.1.006 — Microphone queue recovery

### Host

- Discard stale microphone audio after scheduling stalls and re-prime with fresh samples.
- Improve stereo microphone clock correction when capture delivers packets in batches.

## 1.1.005 — Mac Client display discovery

### Client

- Start sessions when macOS omits the native display-mode flag, using the current backing-pixel dimensions.
- Show a connection error if the display layout cannot be read.
- Retain independent media negotiation; the 1.1.004 Host can remain installed.

## 1.1.004 — Media compatibility candidate

### Client

- Negotiate each Mac media feature independently, preserving desktop connections when optional features are unavailable.
- Connect to supported older Mac Hosts; disable incompatible microphone or camera forwarding.
- Ubuntu: select a native H.264/MJPEG webcam and enable it from the session toolbar. Camera starts off on each connection.

### Host

- Accept Mac launch schemas 4, 5 and 6 alongside independently versioned media features.
- Keep desktop audio and input available to schema-4 Clients, with their incompatible mono microphone disabled.
- macOS: offer native compressed camera output and decoded NV12 through the PLANK Camera extension.

## 1.1.002 — Stereo microphone candidate

Update Host and Client together for this candidate's microphone protocol.

### Client

- Forward microphone audio in stereo using 192 kbps variable-bitrate Opus.

### Host

- macOS: expose a 48 kHz stereo PLANK Microphone input and preserve both channels through decoding.
- Require the matching stereo driver before offering microphone forwarding.

## 1.1.001

Update Host and Client together. This version is not compatible with the previous release's transport.

### Client

- Add microphone forwarding to macOS Hosts, with automatic/manual selection and a toolbar mute switch.
- Remember Host identity before sending login credentials, and ask before trusting a replacement Host.
- Improve connection cancellation, rendering shutdown and audio/video startup reliability.

### Host

- Upgrade loss recovery to RaptorQ 2.0.1 and remove the application datagram pacer.
- macOS: add the PLANK Microphone sound input and improve first-login startup after reboot.
- Bound malformed network data and stalled authentication work.

## 1.0.157

### Host

- macOS: allow certificate preparation to finish during a slow first login after reboot, while keeping stalled operations bounded.

## 1.0.156 — Microphone candidate

### Client

- Add automatic or manual microphone forwarding to macOS Hosts.
- Add a toolbar microphone switch and recording status; muting closes local capture.
- Remember the microphone switch during session reconnection.
- Deliver already-decoded audio/video promptly when setup packets arrive late.

### Host

- macOS: add the PLANK Microphone virtual sound input for remote applications.
- Offer automatic input selection with restoration of the previous device after disconnect.
- Keep microphone failure isolated from video and speaker audio.
- Prevent a startup identity reply from racing connection cleanup.

## 1.0.154

### Client

- Upgrade loss recovery to RaptorQ 2.0.1; requires a matching updated Host.
- Reject malformed or oversized incoming transport messages safely.
- Cancel connection startup promptly and prevent a render-thread shutdown hang.
- Preserve the correct audio/video packet when receive queues overflow.
- Remove the Experimental label from macOS capture.

### Host

- Upgrade loss recovery to RaptorQ 2.0.1; update Host and Client together.
- Remove the unused application datagram pacer; retain Quinn scheduling and transport headroom.
- Bound malformed incoming data and cancel stalled connection setup.
- Linux: keep stalled operating-system authentication from blocking other requests.

## 1.0.153

### Client

- Remember each Host's identity before sending login credentials.
- Ask before trusting a replacement Host; cancelling keeps the previous identity.
- Keep Host trust when bookmarks are deleted or recreated.

### Host

- macOS: keep one machine identity across the login screen and different desktop users.
- Preserve machine identity when renewing Host certificates.

## 1.0.152

### Client

- Match the primary client monitor when ordering a Linux Host's virtual displays.
- Keep the existing image proportions when host and client monitor sizes differ.
- Allow manual two-display bookmarks when the client has a different monitor layout.

### Host

- Linux: place the first virtual connector on the client's primary side for applications that choose the first monitor.

## 1.0.151

### Client

- Offer Take Over or Cancel when another client is connected to the same Mac account.
- Use the new client's display size after an approved takeover.
- Preserve valid packets when the connection adjusts its network packet size.
- Ubuntu: include the image plugin needed to display dialog icons.

### Host

- macOS: transfer an active session to another client after explicit confirmation, including while locked.
- macOS: avoid a temporary loss of connectivity when switching between the login screen and desktop.
- Preserve valid packets during network packet-size recovery on Linux and macOS.
- Reduce diagnostic logging overhead during streaming.

## 1.0.146

### Client

- Click the version number to see what's new, even when offline.

### Host

- macOS: retry failed desktop-service startup after login or user switching.

## 1.0.143

### Client

- Fix mouse positioning at the right and bottom edges of the remote screen.

### Host

- Linux: deliver mouse clicks immediately and improve high-bitrate sending.
- macOS: keep the connection open when the screen locks.
- macOS: let administrators read the Host's system logs.

## 1.0.137

### Client

- Copy and paste plain text between Mac Clients and Mac Hosts.
- Improve clipboard transfers when the connection is busy.

### Host

- macOS: add plain-text clipboard sharing with Mac Clients.
- macOS and Linux: limit shared text to 512 KiB and reject invalid text safely.

## 1.0.135

### Client

- macOS: send Command-Tab and Command-Space to the Host, including after switching Spaces.
- macOS: request keyboard-capture permission before starting a session.
- macOS: improve multi-monitor and Wacom support; support macOS 15 and newer.
- Add network round-trip time to the toolbar and reduce toolbar flicker on macOS.
- Copy and paste plain text between Mac Clients and Linux Hosts.

### Host

- Linux: add plain-text clipboard sharing with Mac Clients.
- Improve mouse-button ordering and keyboard release handling.
