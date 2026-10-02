#!/usr/bin/env bash
# Re-vendors static/fonts/ubuntu-sans-mono/ — see that directory's own
# VENDORED.md for why (this was the one remaining live external network
# dependency anywhere in this app's frontend). Fetches Google's own
# generated @font-face CSS with a modern browser User-Agent (Google
# serves older/larger formats to non-browser clients by default),
# downloads every unique woff2 file it references, and rewrites a local
# copy of that CSS pointing at them — no manual URL bookkeeping, so
# bumping $FAMILY_QUERY below and re-running this script is the entire
# re-vendor process.
set -euo pipefail

STATIC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../static" && pwd)"
DEST="${STATIC}/fonts/ubuntu-sans-mono"
FAMILY_QUERY="family=Ubuntu+Sans+Mono:ital,wght@0,400;0,700;1,400;1,700&display=swap"
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"

echo "Fetching Google Fonts CSS for: ${FAMILY_QUERY}"
mkdir -p "${DEST}"
GOOGLE_CSS="$(mktemp)"
trap 'rm -f "${GOOGLE_CSS}"' EXIT
curl -sSf --max-time 30 -A "${UA}" "https://fonts.googleapis.com/css2?${FAMILY_QUERY}" -o "${GOOGLE_CSS}"

python3 - "${GOOGLE_CSS}" "${DEST}" <<'PYEOF'
import re
import sys
import urllib.request

google_css_path, dest = sys.argv[1], sys.argv[2]
with open(google_css_path) as f:
    content = f.read()

blocks = re.findall(
    r"/\* (\S+) \*/\s*@font-face\s*\{\s*"
    r"font-family: '([^']+)';\s*"
    r"font-style: (\w+);\s*"
    r"font-weight: (\d+);\s*"
    r"font-display: (\w+);\s*"
    r"src: url\((https://fonts\.gstatic\.com/[^)]+\.woff2)\) format\('woff2'\);\s*"
    r"unicode-range: ([^;]+);",
    content,
)
if not blocks:
    sys.exit("ERROR: no @font-face blocks parsed out of Google's CSS -- "
             "Google may have changed its response shape; this script's "
             "regex needs a look.")

# One filename per unique URL (a weight/style pair can share the exact
# same file as another -- confirmed true for this family: normal and
# bold serve identical files for every subset).
url_to_fname = {}
for subset, family, style, weight, display, url, urange in blocks:
    if url not in url_to_fname:
        slug = family.lower().replace(' ', '-')
        url_to_fname[url] = f"{slug}-{style}-{subset}.woff2"

print(f"Downloading {len(url_to_fname)} unique font file(s)...")
for url, fname in url_to_fname.items():
    urllib.request.urlretrieve(url, f"{dest}/{fname}")
    print(f"  {fname}")

out = ["/* Vendored: Ubuntu Sans Mono -- see VENDORED.md in this directory. */"]
for subset, family, style, weight, display, url, urange in blocks:
    fname = url_to_fname[url]
    out.append(f"""/* {subset} */
@font-face {{
  font-family: '{family}';
  font-style: {style};
  font-weight: {weight};
  font-display: {display};
  src: url('{fname}') format('woff2');
  unicode-range: {urange};
}}""")
with open(f"{dest}/fonts.css", "w") as f:
    f.write("\n".join(out) + "\n")
print(f"Wrote fonts.css with {len(blocks)} @font-face rules.")
PYEOF

(
  cd "${DEST}"
  find . -type f ! -name "VENDORED.md" ! -name "SHA256SUMS" | sort | sed 's|^\./||' \
    | xargs sha256sum > SHA256SUMS
)

echo "Done. ${DEST} now holds $(find "${DEST}" -type f | wc -l) files, $(du -sh "${DEST}" | cut -f1)."
echo "Re-read and update ${DEST}/VENDORED.md by hand if the family/weights changed."
