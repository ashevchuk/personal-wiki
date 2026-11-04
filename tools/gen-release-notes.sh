#!/usr/bin/env bash
# Builds a GitHub Release body for a vX.Y.Z tag: the prebuilt-binaries
# section (asset table + install instructions), which is identical for
# every release since release.yml always builds the same four targets, is a
# fixed template here rather than hand-written per release (as v0.3.3's
# release was). The version-specific part is pulled straight from
# CHANGELOG.md's own section for that version instead of being duplicated.
#
# Usage: gen-release-notes.sh vX.Y.Z
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "usage: $0 vX.Y.Z" >&2
  exit 1
fi

tag="$1"
version="${tag#v}"
repo="${GITHUB_REPOSITORY:-ashevchuk/personal-wiki}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

changelog_section=$(awk -v ver="$version" '
  /^## \[/ {
    if (found) exit
    line = $0
    sub(/^## \[/, "", line)
    sub(/\].*/, "", line)
    if (line == ver) { found = 1; next }
    next
  }
  found { print }
' "$repo_root/CHANGELOG.md")

if [ -z "$changelog_section" ]; then
  echo "gen-release-notes.sh: no CHANGELOG.md section for [$version]" >&2
  exit 1
fi

cat <<EOF
## Prebuilt binaries via GitHub Actions

\`.github/workflows/release.yml\` builds a fully static \`wiki-server\`/\`wiki-mcp\` for four targets and publishes them to this release, alongside a \`SHA256SUMS\` for verification:

| Asset | Target |
|---|---|
| \`wiki-${version}-arm-musl.tar.gz\` | 32-bit ARMv7 (Cortex-A7-class) — e.g. Raspberry Pi 2/3 armhf |
| \`wiki-${version}-armv6-musl.tar.gz\` | ARM1176JZF-S (BCM2835) — Raspberry Pi 1/Zero/Zero W |
| \`wiki-${version}-aarch64-musl.tar.gz\` | 64-bit ARM (Cortex-A53-class) — Pi 3/4/5 64-bit OS, other aarch64 SBCs |
| \`wiki-${version}-x64-musl.tar.gz\` | Any x86_64 Linux — fully static, no glibc version to match |

All four are statically linked (zig + musl), with no runtime dependency on the target's own libc.

### Two ways to install one

- **Directly on the device**, no build tooling at all:
  \`\`\`sh
  curl -fsSLO https://raw.githubusercontent.com/${repo}/main/tools/wiki-install.sh
  chmod +x wiki-install.sh
  sudo ./wiki-install.sh
  \`\`\`
- **From a dev machine, over SSH**, using \`tools/wiki-ops.sh\`:
  \`\`\`sh
  tools/wiki-ops.sh deploy --target=user@host --variant=release --triplet=arm-musl --first-time
  \`\`\`

Both paths download the matching asset, verify it against \`SHA256SUMS\`, and refuse up front if the target's existing \`config.toml\` already configures an \`[embeddings]\`/\`[llm]\` provider other than \`"none"\` — these binaries are built with local/cloud embeddings support compiled out, and swapping one onto such a target would crash-loop the service.

See [\`docs/sbc-deployment.md\`](https://github.com/${repo}/blob/main/docs/sbc-deployment.md#path-c--download-a-prebuilt-release-binary) ("Path C") for the full picture.

## Also in this release
${changelog_section}
EOF

prev_tag=$(git -C "$repo_root" describe --tags --abbrev=0 "${tag}^" 2>/dev/null || true)
if [ -n "$prev_tag" ]; then
  echo
  echo "**Full Changelog**: https://github.com/${repo}/compare/${prev_tag}...${tag}"
fi
