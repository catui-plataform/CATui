# RPCS3 source

`source.tar.gz` holds the local source tree, including local changes.
`snapshot.json` records its checksum, tree and pinned submodule revisions.
Fetch submodules using the archived `.gitmodules` and those revisions.
`upstream.json` records the upstream base; `integration.patch` contains
source changes from that base. Git history, secrets, compiled libraries
and build/cache directories are excluded. Exclusions are listed in the snapshot.

`integration/` contains the adapter files.

The recorded upstream base is retained for package comparison. This
local source snapshot is not a new build receipt for the shipped binary.
