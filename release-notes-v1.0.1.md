# HookshotVR Alpha v1.0.1

A hotfix for v1.0.0.

**Fixed:** the launcher's **Free space** check looked at your home drive (C: on Windows) instead of the folder you
unpacked HookshotVR into. If you unpacked to another drive and C: was low on space, it refused to build. It now checks
the drive the game is built on.

## Download

| System | File |
| --- | --- |
| Windows 10 / 11 (64-bit) | `HookshotVR-Alpha-v1.0.1-windows-x64.zip` |
| Linux (64-bit) | `HookshotVR-Alpha-v1.0.1-linux-x86_64.tar.gz` |

`SHA256SUMS.txt` has the checksums. Everything else is as in v1.0.0: see its release notes, the
[install guide](docs/vr-install.md) and [known issues](docs/vr-known-issues.md).

**Already on v1.0.0?** You only need this if Free space blocked your first build. As a workaround on v1.0.0:
Settings › Hookshot Folder › Change, and pick the folder you unpacked (the one with `builder` inside).
