# Store

Core packages and icons for CATui. Currently available for Windows.
`catalog.json` lists the packages, versions, download URLs and SHA-256 hashes.

To update a core:

1. Replace its package and edit `cores/<id>/manifest.json`.
2. Run `node store/build-catalog.mjs --ref store-YYYY-MM-DD` with a new tag name.
3. Run `node store/build-catalog.mjs --check`.
4. Commit the changes, create that tag and push both together.

Tags used by the catalog must stay on their original commit.
ZIP packages must include the DLL specified in `entryFilename` and its runtime files.
