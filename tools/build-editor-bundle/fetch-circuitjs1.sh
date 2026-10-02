#!/usr/bin/env bash
# Re-vendors static/js/circuitjs1/ — see static/js/circuitjs1/VENDORED.md
# for the full story (why pfalstad/circuitjs1's `master` branch, built
# from source, and not a scrape of some live deployment, and what the
# two source patches below are for). Needs JDK 8 specifically on PATH
# (GWT 2.8.2, the pinned toolchain, does not run on newer JDKs) and
# internet access to clone the upstream repo and let Gradle pull its own
# dependencies. Separate from fetch.sh (Toast UI Editor/mermaid/Prism)
# because this is a from-source build with source patches, not a handful
# of single-file curls.
set -euo pipefail

if ! command -v javac >/dev/null 2>&1; then
  echo "ERROR: javac not found on PATH. This script needs JDK 8 specifically" >&2
  echo "  (GWT 2.8.2 doesn't run on newer JDKs) -- e.g. on Arch:" >&2
  echo "  sudo pacman -S jdk8-openjdk" >&2
  echo "  export JAVA_HOME=/usr/lib/jvm/java-8-openjdk PATH=\"\$JAVA_HOME/bin:\$PATH\"" >&2
  exit 1
fi

STATIC_JS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../static/js" && pwd)"
DEST="${STATIC_JS}/circuitjs1"
COMMIT="5a707168778216bb6ed01bfdd62e8bbf7ae0a032"  # master branch, 2026-09-23
WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

echo "Cloning pfalstad/circuitjs1 @ ${COMMIT} into ${WORKDIR} ..."
git clone --quiet https://github.com/pfalstad/circuitjs1.git "${WORKDIR}/src"
(cd "${WORKDIR}/src" && git checkout --quiet "${COMMIT}")

SRC="${WORKDIR}/src/src/com/lushprojects/circuitjs1/client"

# Patch 1: the red padlock icon drawn over the canvas whenever the embed
# is editable=false -- meaningless for a display-only, no-controls
# embed. See VENDORED.md point 1. Literal strings hardcoded directly in
# the Python heredoc below (no shell variable interpolation into it) --
# multi-line text with its own quotes/braces is exactly the kind of
# content that's fragile to pass through bash string substitution first.
python3 - "${SRC}/UIManager.java" <<'PYEOF'
import sys
path = sys.argv[1]
old = """        if (menus.noEditCheckItem.getState())
            g.drawLock(20, 30);

        g.setColor(Color.white);"""
new = "        g.setColor(Color.white);"
with open(path) as f:
    content = f.read()
count = content.count(old)
if count != 1:
    sys.exit(
        f"ERROR: expected exactly 1 occurrence of the drawLock block in "
        f"{path}, found {count}. Upstream has likely moved this code -- "
        f"re-locate g.drawLock(20, 30) by hand and update this script."
    )
with open(path, "w") as f:
    f.write(content.replace(old, new, 1))
PYEOF

# Patch 2: with no startCircuit/cct param (this embed never sets
# either), openDefault=true silently fetches and loads some default
# example circuit the moment the examples list arrives, racing with and
# overwriting whatever circuit-embed.js just imported via
# CircuitJS1.importCircuit(). See VENDORED.md point 2.
python3 - "${SRC}/CirSim.java" <<'PYEOF'
import sys
path = sys.argv[1]
old = "menus.getSetupList(true);"
new = "menus.getSetupList(false);"
with open(path) as f:
    content = f.read()
count = content.count(old)
if count != 1:
    sys.exit(
        f"ERROR: expected exactly 1 occurrence of '{old}' in {path}, found "
        f"{count}. Upstream has likely moved this code -- re-locate the "
        f"final-else-branch menus.getSetupList(true) call by hand and "
        f"update this script."
    )
with open(path, "w") as f:
    f.write(content.replace(old, new, 1))
PYEOF

echo "Building via Gradle (compileGwt + makeSite) ..."
(
  cd "${WORKDIR}/src"
  ./gradlew --console plain compileGwt
  ./gradlew --console plain makeSite
)

SITE="${WORKDIR}/src/site"
echo "Assembling trimmed vendor copy into ${DEST} ..."
rm -rf "${DEST}"
mkdir -p "${DEST}/circuitjs1"
cp "${SITE}/circuitjs.html" "${DEST}/"
cp -r "${SITE}/font" "${DEST}/"
cp "${SITE}"/circuitjs1/circuitjs1.nocache.js "${DEST}/circuitjs1/"
cp "${SITE}"/circuitjs1/*.cache.js "${DEST}/circuitjs1/"
cp "${SITE}"/circuitjs1/clear.cache.gif "${DEST}/circuitjs1/"
cp "${SITE}"/circuitjs1/setuplist.txt "${DEST}/circuitjs1/"
cp "${SITE}"/circuitjs1/style.css "${DEST}/circuitjs1/"
cp "${SITE}"/circuitjs1/locale_*.txt "${DEST}/circuitjs1/"
cp -r "${SITE}"/circuitjs1/gwt "${DEST}/circuitjs1/"
# circuits/ (the "Circuits" example-menu files) is NOT cosmetic: the
# app always fetches circuits/setuplist.txt at startup regardless of
# hideMenu, and (pre-patch-2) would try to auto-open a default example
# from here too. Dropping it produced a real 404 and a blocking
# Window.alert("Can't load circuit!") the first time this was tried.
cp -r "${SITE}"/circuitjs1/circuits "${DEST}/circuitjs1/"

(
  cd "${DEST}"
  find . -type f ! -name "VENDORED.md" ! -name "SHA256SUMS" | sort | sed 's|^\./||' \
    | xargs sha256sum > SHA256SUMS
)

echo "Done. ${DEST} now holds $(find "${DEST}" -type f | wc -l) files, $(du -sh "${DEST}" | cut -f1)."
echo "Re-read and update ${DEST}/VENDORED.md by hand if COMMIT changed."
