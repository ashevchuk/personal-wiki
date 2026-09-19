#pragma once

#include <atomic>
#include <cstdint>

namespace wikicore::index {

// A lightweight, thread-safe progress counter for IndexBuilder::
// fullRescan() — separate from RescanStats (the FINAL result struct
// fullRescan() returns) because this one is read WHILE a rescan is still
// running, from a different thread than the one doing the rescanning:
// the background startup rescan (main.cpp) or a synchronous --reindex /
// POST /api/admin/reindex both run on one thread, while
// GET /api/admin/reindex-status (AdminRoutes.cpp) reads this from an
// HTTP request-handling thread concurrently. All-atomic fields, no
// mutex needed — each is independently meaningful without needing to be
// read as a consistent snapshot together (a caller seeing
// documentsIndexed tick up one step ahead of inProgress flipping false
// is a harmless, momentary staleness, not a correctness issue).
//
// Exists specifically because a large vault's first-ever embeddings
// rescan (or any --reindex on a big vault) can take real, human-visible
// time — see docs/embeddings.md's "Non-blocking startup rescan" — and
// "is it done yet, and how far did it get" was previously answerable
// only by tailing server logs.
struct RescanProgress {
  std::atomic<bool> inProgress{false};
  std::atomic<int64_t> documentsIndexed{0};
};

}  // namespace wikicore::index
