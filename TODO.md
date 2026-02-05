# ALSA AVR Control Plugin (C++) - TODO List

## Ideas / Future Improvements

### Add your C++ plugin ideas below (newest at top)
---
- When i slide the volume slider slowly, it seems that not all volume commands are sent to the receiver. This may be because its slow to respond, but it means that the last bit of slide before i let go never gets reflected to the receiver.
- Some kind of terminal ui for the backends. Preferably exposing more settings than just what our alsa device needs.
- Task tracking in gitea
- Debian packaging for shairport sync and nqptp
- What if the receiver does not have a static ip? Can we find it by normal dns name, or some kind of zeroconf?
- Gitea pipelines to build shairport sync debian packages
- Keep an eye on the state of the volume, and update airplay sources accordingly?
- Opalum keeps being powered on
- Reset cec stuff on 240
- Opalum has a record Master control?
- Power off hdmi video on pi?
---


## Completed
- ✅ Refactored all backends with consistent connection pool pattern (2026-02-02)
- ✅ Fixed Opalum "works once then stops" bug - connection now persists across calls (2026-02-02)
- ✅ Deployed Opalum to Pi 116 with Docker shairport-sync (2026-02-02)
- ✅ CEC backend refactored with CECConnectionPool for state sharing (2026-02-02)
- ✅ System-wide configuration via /etc/alsa/conf.d/ with automatic installation (2026-01-30)
- ✅ Removed generic avr plugin (use backend-specific plugins: denon, nad, opalum, cec) (2026-01-30)
- ✅ Input Source mixer control for input selection (2026-01-30)
- ✅ Systemd service with per-Pi configuration files (2026-01-30)
- ✅ CEC backend implementation (2026-01-29)
- ✅ Input display names shown to user instead of internal codes (PROJECT vs MPLAY) (2026-01-30)
- ✅ Dynamic input detection via SSFUN query with custom display names (2026-01-30)
- ✅ Removed default IP address from asound.conf (requires explicit configuration) (2026-01-30)
- ✅ Cleaned up tmp/ directory from version control (2026-01-30)
- ✅ Renamed mixer control to "Master Playback Volume/Switch/Power" (2026-01-30)
- ✅ Speaker level controls for Denon AVR with all 11 speakers (FL, FR, C, SW, SL, SR, SBL, SBR, SB, FHL, FHR) (2026-01-30)
- ✅ Fixed ALSA control name length limit (44 chars max) by shortening speaker names (2026-01-30)
- ✅ Added SND_CTL_EXT_ACCESS_PLAYBACK flag to prevent controls appearing in capture view (2026-01-30)
- ✅ Separate ALSA plugins for each backend (denon, nad, opalum, cec) (2026-01-30)
- ✅ Renamed opalum_rs232 to opalum_serial throughout codebase (2026-01-30)
- ✅ Alpine/musl cross-compilation setup for official Docker image compatibility (2026-01-30)
- ✅ Automated cross-build workflow with cross-build-alpine.sh script (2026-01-30)
- ✅ Use official mikebrady/shairport-sync Docker image with mounted plugin (2026-01-30)
- ✅ Docker group permissions fix for cross-compilation without sudo (2026-01-30)
- ✅ Serial port connection pooling for Opalum backend (2026-01-29)
- ✅ Serial port connection pooling for NAD backend (2026-01-29)
- ✅ Opalum RS232 backend with input switching support (2026-01-29)
- ✅ NAD Serial backend with query support (2026-01-29)
- ✅ Deployed Opalum solution to Raspberry Pi for production use (2026-01-29)
- ✅ Fixed shairport-sync configuration to use hardware mixer (-m parameter) (2026-01-29)
- ✅ Logfile from the alsa plugin with configurable log levels (2026-01-29)
- ✅ Plugin-level connection pooling with TCP keep-alive (2026-01-29)
- ✅ Shared state for input switching coordination across backends (2026-01-29)
- ✅ Fix input switching to only switch when actually needed (2026-01-29)
- ✅ Reduce log noise - backend/logger init/shutdown at DEBUG level (2026-01-29)
- ✅ Map asound.conf externally to avoid container rebuilds (2026-01-29)
- ✅ Fix TLV dB range to match Denon's actual +18dB maximum (2026-01-29)
- ✅ Add TLV dB range support for shairport-sync hardware volume control (2026-01-29)
- ✅ Add automatic input switching when AirPlay starts (2026-01-29)
- ✅ Identify correct Denon input name (MPLAY for HDMI 3) (2026-01-29)
