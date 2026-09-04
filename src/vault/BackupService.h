#pragma once

#include <string>
#include <vector>

namespace wikicore::vault {

struct BackupResult {
  bool success = false;
  std::string errorMessage;   // set iff !success
  std::vector<char> archive;  // gzip-compressed tar bytes iff success
};

// Wraps the vault directory (config.toml's [vault].path) into one
// .tar.gz, purely read-only — no filesystem mutation, nothing written
// anywhere. Includes `.trash/` (soft-deleted documents belong in a backup
// too) and `.index.db*` (harmless to include even though the index is
// disposable per docs/deployment.md's "Backup" section — a restored copy
// just means a faster startup than a full rescan, not a correctness
// requirement; a torn snapshot of the db mid-write is possible if this
// runs while the server is live, but the unconditional startup rescan
// papers over that the same way it already does after any unclean
// shutdown). Excludes `.uploads-tmp/` — Drogon's own multipart-upload
// staging buffer (see main.cpp's setUploadPath comment), 256 pre-created
// empty sharded subdirectories that hold nothing but transient upload
// fragments mid-flight, never a real document; pure noise in a backup.
//
// Deliberately shells out to the system `tar` binary via fork()+execlp()
// — NEVER system()/popen(), which run the command through /bin/sh and
// would turn any shell metacharacter in vaultPath into a parsing hazard.
// execlp() takes an explicit argv with no shell involved at all, so
// nothing about vaultPath's actual content matters beyond being a valid
// path. `tar` is assumed present — true of every mainstream Linux
// distribution this app targets, including the real deployed Debian 9
// stretch SBC (see docs/deployment.md's cross-compilation section).
// Rolling a hand-written tar+gzip writer, or pulling in a new vcpkg
// archive dependency, isn't worth it for one rarely-used, admin-only
// button when a well-tested system tool already does exactly this.
//
// Buffers the entire compressed archive in memory before returning it —
// fine at the personal-knowledge-base scale this app is built for, but a
// vault that grows into the multi-GB range (large attachments) would want
// a streamed response instead; not implemented, flagged here rather than
// silently degrading under a load nobody asked this to handle yet.
BackupResult createVaultBackup(const std::string& vaultPath);

}  // namespace wikicore::vault
