#!/usr/bin/env bash
# Generates .gz sidecar files for static assets Drogon's StaticFileRouter
# serves via sendfile(): HttpResponseImpl::shouldBeCompressed() refuses to
# compress any response with a non-empty sendfileName_, and newFileResponse
# switches to sendfile() for any file over its hardcoded 200 KiB cutoff — so
# without a sidecar, every such file goes out fully uncompressed regardless
# of enableGzip/enableBrotli. setGzipStatic (on by default) picks up a
# "$file.gz" sidecar automatically if one exists next to the original.
#
# Run against an install/staging tree's static/ directory, not committed to
# the repo — see every call site in CMakeLists.txt and tools/wiki-ops.sh.
set -euo pipefail

static_dir=${1:?usage: compress-static.sh <static-dir>}

# Drogon's HttpResponse::newFileResponse: `length > 1024 * 200`.
sendfile_threshold=204800

# Already-compressed or non-text formats: gzip buys nothing here.
skip_ext_re='\.(png|jpe?g|gif|webp|ico|woff2?|gz|br|zip|mp4|mp3|pdf)$'

find "$static_dir" -type f -size +${sendfile_threshold}c -print0 |
  while IFS= read -r -d '' f; do
    [[ "$f" =~ $skip_ext_re ]] && continue
    gzip -9 -k -f "$f"
  done
