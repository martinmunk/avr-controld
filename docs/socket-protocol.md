# avr-controld socket protocol

The contract between this daemon and its clients: the ALSA ctl plugins,
`avr-cec-relay` and `avr-power` in the `alsa-avr-control` repo, and anything else
that wants the receiver. Implemented in `src/avrcontrold/protocol.py`
(`dispatch()`); served on a Unix socket (`avr_socket` in the config, e.g.
`/run/avr-controld/nad.sock`).

Text, one request per line, one reply line per request:
`OK`, `OK <payload>` or `ERR <message>`. Unknown values are `-`.

| Request | Reply | Notes |
|---|---|---|
| `PING` | `OK` | liveness |
| `NAME` | `OK <backend name>` | e.g. `NAD Serial`, `Opalum Serial` |
| `CONNECTED` | `OK 0|1` | receiver link up |
| `CAPS` | `OK abs= query= mute= input= spk= standby= minv= maxv=` | capabilities, 0/1 and volume range |
| `HWMAX` | `OK <n>` | receiver's native max volume |
| `GET volume|mute|power|input` | `OK <value>` | 0-100, 0/1, 0/1, source name; `-` if unknown (Opalum is write-only) |
| `SET volume <0-100>` | `OK` / `ERR failed` | **clamped to the daemon's `max_volume` ceiling** |
| `SET mute 0|1` | `OK` / `ERR failed` | |
| `SET power 0|1` | `OK` / `ERR failed` | bypasses the power policy's bookkeeping |
| `SET input <name>` | `OK` / `ERR failed` | source name as listed by `INPUTS` (rest of line) |
| `VOLUP` / `VOLDOWN` | `OK` / `ERR failed` | one receiver step; clients gate VOLUP against the ceiling themselves |
| `INPUTS` | `OK a,b,c` | source names |
| `DEMAND <name> 0|1 [input]` | `OK` / `OK -` | power-policy demand (e.g. `DEMAND cec 1 TV`); `OK -` when no policy. Edge-triggered: re-asserting an active demand is a no-op |
| `RAW <command>` | `OK <reply>` | raw receiver protocol passthrough, for exploration/config; bypasses the clamp |
| `META` / `PLAYSTATE` | `OK <snapshot>` | shairport metadata / play state from the D-Bus watcher |
| `SPKMIN` / `SPKMAX` / `SPEAKERS` | fixed replies | legacy speaker-level verbs, kept for the Denon plugin |

Safety: every absolute volume the daemon sets, from any path (socket, MQTT, AirPlay
slider), goes through `clamp_absolute(v, max_volume)`. The ceiling is the last line of
defence; clients must not assume anything above it will be honoured.
