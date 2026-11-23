# HookshotVR Alpha v1.0.1

The launcher now updates itself, and keeps everything on the drive you unpacked to.

**v1.0.0 can't update itself: download v1.0.1 once by hand; from v1.0.1 on, the launcher updates itself.**
v1.0.0's update button looked in the wrong place and only ever rebuilt the game, never the launcher.

## What changed

- **Updates come from this page's GitHub releases.** The launcher looks once each time it starts (quietly: offline or
  GitHub's rate limit just means no banner), shows what is new in **Update Log**, and **View Online** opens the
  release here. Every download is checked against the release's `SHA256SUMS.txt` before it is used.
- **Launcher-only updates keep your game.** The banner says **Launcher only** or **Game rebuild**, with the free space
  it needs. A launcher-only update swaps in the new launcher, source and builder and restarts: no build. The game is
  built again only when the code it is built from changed (each release carries a game stamp, `GAME_STAMPS.txt`, and
  your game folder remembers the one it was built from). A rebuild goes into a second folder and switches only once it
  is whole, so your game keeps working if it fails.
- **Everything on the drive you chose.** **Free space** now checks the folder you unpacked HookshotVR into, not your home
  drive (C: on Windows): v1.0.0 refused to build when C: was low even with room on the other drive. The game file's
  check, the build's temporary files and the update's download now stay on that drive too. What still lives on C:
  (saves, settings, the graphics driver's shader cache) is listed in
  [Troubleshooting](docs/vr-troubleshooting.md#which-drive-is-used).

The game itself is the same as in v1.0.0. Your saves, settings, `hookshot_env.txt` and crash reports are kept by every
update.

## Download

| System | File |
| --- | --- |
| Windows 10 / 11 (64-bit) | `HookshotVR-Alpha-v1.0.1-windows-x64.zip` |
| Linux (64-bit) | `HookshotVR-Alpha-v1.0.1-linux-x86_64.tar.gz` |

`SHA256SUMS.txt` has the checksums, `GAME_STAMPS.txt` the game stamps (the launcher reads both: upload them with the
archives). Everything else is as in v1.0.0: see its release notes, the [install guide](docs/vr-install.md) and
[known issues](docs/vr-known-issues.md).

**Coming from v1.0.0?** Unpack v1.0.1 into a new folder and build there, or unpack it over your old folder to keep the
game you built (the launcher then builds it again once, at its next update). Saves and settings are kept either way.
