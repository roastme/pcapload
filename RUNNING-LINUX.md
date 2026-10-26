# Running pcapload on Linux (Wine)

Verified working on this machine with **Wine 11.0** (Flatpak `org.winehq.Wine`).
Zuma and Bejeweled 2 both load and render correctly.

## Setup (once, per user, no root required)

```bash
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y flathub org.winehq.Wine/x86_64/stable-26.08
```

Alternative with a password (system install, then no Flatpak needed):

```bash
sudo dpkg --add-architecture i386
sudo apt update && sudo apt install -y wine32:i386
wine pcapload.exe zuma
```

## Run

```bash
./run.sh                 # launcher (game list)
./run.sh zuma            # Zuma
./run.sh bejeweled       # Bejeweled 2
PCAPLOAD_LOG=1 ./run.sh zuma   # write pcapload.log next to the exe
```

## Notes / gotchas

* The loader and all game DLLs are 32-bit PE files — the Wine build **must**
  support 32-bit (`i386`). The Flatpak build does (via the i386 compat
  extension); a 64-bit-only Wine will fail with
  `could not load kernel32.dll`.
* `run.sh` maps only the project folder into the Flatpak sandbox. Paths
  outside it (e.g. `../zumas/`) are not visible, which is fine here because
  `games/` is self-contained.
* Stale `pcapload_drive_*.lock` files came over from the Windows machine.
  They only matter for >~100 character game paths (the drive-letter
  mapping workaround); they are harmless for the bundled games.
* `savedata/` holds the portable registry-based saves; delete a game's
  subfolder to reset it.
