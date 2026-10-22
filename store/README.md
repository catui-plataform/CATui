# Store

CATui packages are organized as `<platform>/<architecture>/<category>/<emulator>/`.
Current targets: `windows/x86_64` and `linux/x86_64`. Categories are `cores` (Libretro) and `seamless`
(complete native emulator distributions). Future targets can use `android`,
`macos`, and architectures `x86_64`, `x86_32`, `arm64`, `arm32`.

`catalog.json` schema 2 declares each package's target, runtime, version, SHA-256,
exact entry point, icon, extensions, upstream repository and RetroAchievements capability.
CATui filters targets before showing or installing a package. Seamless also requires
the optional `catui-seamless` addon and a compatible native extension.

To update a package:

1. Replace its binary/archive and update `<platform>/<architecture>/<category>/<id>/manifest.json`.
2. Run `node store/build-catalog.mjs`.
3. Run `node store/build-catalog.mjs --check`.
4. Publish all changed files and the generated catalog together.

The initial Linux x86_64 test batch contains Citra, mGBA, Snes9x, Mesen and
Genesis Plus GX, matching their Windows package IDs and metadata. Linux cores
use unmodified `.so` binaries from the Libretro buildbot and `packageType: "so"`.
Each `SOURCE.md` records the download URL and archive/binary checksums. The
catalog builder validates ELF format, shared library type and CPU architecture.
Publishing is for manual CATui testing and does not establish game compatibility.

Linux x86_64 Seamless packages now include Eden 0.2.1, shadPS4 0.20.0, Cemu 2.6,
RPCS3 0.0.43-20371 and PCSX2 2.8.2. Each package contains its official Linux
AppImage inside a ZIP, with the exact entry declared in the manifest. The
shadPS4 ZIP is retained byte for byte; the other official AppImages are wrapped
without altering their contents. These require the Linux Seamless backend,
the updated Linux-aware CATui Store installer, and working AppImage/FUSE support.
The installer restores execute permissions after extraction. The generator
checks the ZIP entry's ELF format and CPU architecture before publishing.
Upstream downloads and checksums are recorded in each `SOURCE.md`.

Core source archives and integration patches remain alongside their existing packages
in `windows/x86_64/cores/<id>/source/`. Native distributions retain their complete
runtime files in ZIP format; `entryFilename` is relative to the ZIP root and is never
inferred from the first EXE. RPCS3's official 7z is repacked as ZIP without modifying
its files; `SOURCE.md` records upstream release and checksums. No ROMs, firmware,
BIOS or encryption keys are redistributed. Those remain the user's responsibility.

RetroAchievements availability describes CATui's memory-backed frontend integration,
not emulator certification. PS3, PS4, Wii U and Switch packages are currently marked
unavailable. Reference: https://docs.retroachievements.org/general/emulator-support-and-issues.html

CATui always reads the published catalog and downloads packages from this repository.
It does not ship a fallback package inventory. If the catalog has no matching target,
the corresponding Store category is empty. Installed runtimes remain on the user's machine.
