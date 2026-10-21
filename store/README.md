# Store

CATui packages are organized as `<platform>/<architecture>/<category>/<emulator>/`.
Current target: `windows/x86_64`. Categories are `cores` (Libretro) and `seamless`
(complete native emulator distributions). Future targets can use `linux`, `android`,
`macos`, and architectures `x86_64`, `x86_32`, `arm64`, `arm32`.

`catalog.json` schema 2 declares each package's target, runtime, version, SHA-256,
exact entry point, icon, extensions, upstream repository and RetroAchievements capability.
CATui filters targets before showing or installing a package. Seamless also requires
the optional `catui-seamless` addon and a compatible native extension.

To update a package:

1. Replace its archive and update `windows/x86_64/<category>/<id>/manifest.json`.
2. Run `node store/build-catalog.mjs`.
3. Run `node store/build-catalog.mjs --check`.
4. Publish all changed files and the generated catalog together.

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
