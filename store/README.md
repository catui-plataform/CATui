# Store

Core packages and icons for CATui. Currently available for Windows.
`catalog.json` lists the packages, versions, download URLs and SHA-256 hashes.

To update a core:

1. Replace its package and edit `cores/<id>/manifest.json`.
2. Run `node store/build-catalog.mjs`.
3. Run `node store/build-catalog.mjs --check`.
4. Commit and push the changed files to `main`.

ZIP packages must include the DLL specified in `entryFilename` and its runtime files.

`cores/<id>/source/` holds the source archive, upstream revision, local changes
and dependency references. Build recipes are included where available.
The bot compares saved source bases with upstream commits. Package provenance
is recorded separately; releases are optional context for the update cards.

Store updates replace files directly. Releases are reserved for CATui builds.
