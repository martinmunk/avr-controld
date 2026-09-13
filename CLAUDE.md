# CLAUDE.md

Guidance for working in this repository.

## What this is

ALSA control plugins plus **`avr-controld`**, a broker daemon that owns exactly one
real connection to each AV receiver (Denon over telnet:23, Opalum/NAD over serial)
and serves clients on a Unix socket. The ALSA ctl plugins and the `avr-power` tool
are socket clients; the real backends link only into the daemon. It runs on the
Tærten AirPlay Pis: **StueTærten** (192.168.20.240, Denon AVR-X1100W at .216) and
**VærkstedsTærten** (192.168.20.217, Opalum serial).

## Working practices

- **Commit and push as you go.** Make small, focused commits and push each one
  immediately to the working branch. Do not batch a whole session into one commit,
  and do not leave work unpushed — the remote is the source of truth.
- **Work on a feature branch, never commit directly to `main`.** Current line of
  work: `feature/denon-reconnect-and-input`. If asked where the changes are, it is
  the branch, not `main`.
- Run the test tiers before committing: `./build/unit_tests`,
  `bash tests/run_integration_tests.sh`, `bash tests/broker/run_broker_tests.sh`.
- End commit messages with the session attribution line when the harness provides one.

## Build, deploy, release

- **Never build on the Pis.** Cross-build the arm64 package here with
  `TARGET_ARCH=arm64 ./buildpackage.sh`, then `dpkg -i` on the Pi. Everything
  installed on a Pi must be tracked by dpkg — no hand-compiled or hand-copied binaries.
- Development/test builds use a `~devN` version: `dch -b -v 0.3.1~devN ...`, build,
  then `git checkout -- debian/changelog` to restore. (Watch for interactive `cp`.)
- **Releases go out only via `git tag release/X.Y.Z`**, which CI (`.forgejo/workflows/publish.yml`)
  builds and publishes. Never curl-upload a package to the registry by hand.
- CI runs on Forgejo (`.forgejo/`), `runs-on: docker`, Debian trixie, amd64 + arm64.

## Hardware safety — read this before anything that changes volume/power

This software drives real receivers and speakers. A bug can blast speakers at full
volume. This has already happened once (enabling `ignore_volume_control=yes` together
with an experimental volume-sync drove the Denon to 100% because shairport reported
full volume and a clamp path was missed).

- **Never enable a feature that can command receiver volume, live on a real system,
  when it rests on an untested assumption or an unproven safety cap.** Prove the clamp
  and the mapping first.
- Test any volume-affecting change with the **receiver hard-limited to a low volume
  and the audio source silent or near-silent** as the starting point — never from a
  configuration that can reach full output.
- `ignore_volume_control = "yes"` makes shairport output full-scale digital; it is only
  safe if a *proven* controller owns the receiver's attenuation. Leave it `no` by default.
- For power/input/volume changes, take the smallest reversible step and verify the
  receiver's actual state (read it back) rather than assuming the command took.

## Topology notes

- The Denon accepts only ONE telnet connection — the daemon owns it. HA's `denonavr`
  integration must stay HTTP-only to coexist.
- Home Assistant (`homeassistant.egaa.loc.martinmunk.com`) also gets AirPlay state via
  shairport-sync MQTT autodiscovery (per-host topic, `publish_retain = "yes"`).
