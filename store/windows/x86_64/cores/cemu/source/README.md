# Cemu source

`source.tar.gz` holds the local source tree, including local changes.
`snapshot.json` records its checksum, tree and pinned submodule revisions.
Fetch submodules using the archived `.gitmodules` and those revisions.
`upstream.json` records the upstream base; `integration.patch` contains
source changes from that base. Git history, secrets, compiled libraries
and build/cache directories are excluded. Exclusions are listed in the snapshot.

This is the supplied source baseline used for upstream commit comparison.
The shipped package has no build receipt linking it to this snapshot.
