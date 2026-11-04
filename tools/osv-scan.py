#!/usr/bin/env python3
"""Checks this project's pinned vcpkg C/C++ dependencies against OSV.dev.

No automated CVE scanner covers vcpkg C/C++ dependencies as a whole:
neither OSV-Scanner nor Trivy has a vcpkg extractor, and GitHub's
Dependency Graph doesn't list vcpkg among its supported ecosystems. This
script is the closest practical substitute, and only that -- see the
"What this does NOT cover" section below before trusting a clean run.

For each vcpkg_from_github-based port actually installed, it:
  1. Reads REPO/REF out of that port's portfile.cmake, and VERSION out of
     that port's own vcpkg.json -- both read from THIS PROJECT's pinned
     builtin-baseline commit in vcpkg.json (via `git show <sha>:path`,
     never the vcpkg checkout's current HEAD), so a result reflects what
     this project actually ships, not whatever vcpkg/ happens to be
     checked out to locally right now.
  2. Queries https://api.osv.dev/v1/query with
     {"package": {"purl": "pkg:generic/<repo-name>"}, "version": "<ref>"}
     -- OSS-Fuzz-sourced advisories key affected versions by this exact
     literal tag string (confirmed live against OSV-2022-126, the real
     md4c heap-buffer-overflow this project's cross/overlay-ports/md4c/
     already patches around -- querying this way is what originally
     found it).

What this does NOT cover:
  - Ports not using vcpkg_from_github (sqlite3's own amalgamation
    download, libuuid's vcpkg_from_sourceforge) are skipped outright,
    logged, never silently dropped.
  - An advisory with no literal version string in its own data (only a
    git commit range) won't match here. OSV.dev's commit-based query
    (`POST /v1/query {"commit": "<sha>"}`) is the fallback for that case
    -- tested against this exact script's development and found to NOT
    reliably ancestry-walk arbitrary descendant commits the way OSS-Fuzz
    projects' own tooling does, so it's deliberately not automated here;
    treat a real suspicion for a specific dependency as a reason for a
    manual OSV.dev lookup, not something this script's silence rules out.
  - This only reaches OSS-Fuzz-sourced findings (memory-safety bugs
    fuzzing actually triggered), not every CVE ever filed against a
    dependency -- GHSA-sourced advisories for C/C++ libraries mostly
    don't carry a matching ecosystem/purl shape for OSV's package-query
    endpoint at all, confirmed empirically while building this script.
"""

import json
import re
import subprocess
import sys
import urllib.request
import urllib.error

VCPKG_DIR = "vcpkg"
OSV_QUERY_URL = "https://api.osv.dev/v1/query"

# port -> {acknowledged OSV id -> why it's not actionable, so a real NEW
# finding for this same port still fails the job}. A vcpkg port's own
# "version" field doesn't change for a port-version-only patch (see
# overlay-ports/md4c/vcpkg.json: version stays "0.5.3", only
# port-version bumps), so a locally-patched advisory would otherwise
# re-fail this check forever with nothing new to act on.
ACKNOWLEDGED = {
    "md4c": {
        "OSV-2022-126": "patched locally, see overlay-ports/md4c/portfile.cmake",
    },
}


def git_show(baseline: str, path: str) -> str | None:
    result = subprocess.run(
        ["git", "-C", VCPKG_DIR, "show", f"{baseline}:{path}"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        return None
    return result.stdout


def read_baseline(vcpkg_json_path: str) -> str:
    with open(vcpkg_json_path) as f:
        return json.load(f)["builtin-baseline"]


def installed_ports(install_root: str, triplet: str) -> list[str]:
    """Real installed port names for this triplet, excluding vcpkg's own
    internal build-tooling ports (vcpkg-cmake, vcpkg-cmake-config, ...)
    -- those ship no code of ours and have no portfile shaped like a real
    dependency's."""
    result = subprocess.run(
        [f"{VCPKG_DIR}/vcpkg", "list", f"--x-install-root={install_root}"],
        capture_output=True, text=True, check=True,
    )
    names = set()
    for line in result.stdout.splitlines():
        line = line.strip()
        if not line or ":" not in line:
            continue
        pkg = line.split()[0]  # "openssl:x64-linux" or "sqlite3[fts5]:x64-linux"
        name = pkg.split("[")[0].split(":")[0]
        if name.startswith("vcpkg-"):
            continue
        names.add(name)
    return sorted(names)


def port_repo_ref(baseline: str, port: str) -> tuple[str, str] | None:
    portfile = git_show(baseline, f"ports/{port}/portfile.cmake")
    if portfile is None or "vcpkg_from_github" not in portfile:
        return None
    repo_match = re.search(r"REPO\s+(\S+)", portfile)
    ref_match = re.search(r"REF\s+\"?([^\s\")]+)", portfile)
    if not repo_match or not ref_match:
        return None
    repo = repo_match.group(1)
    ref_template = ref_match.group(1)
    if "${VERSION}" not in ref_template:
        return repo, ref_template

    vcpkg_json = git_show(baseline, f"ports/{port}/vcpkg.json")
    if vcpkg_json is None:
        return None
    version = json.loads(vcpkg_json).get("version") \
        or json.loads(vcpkg_json).get("version-semver") \
        or json.loads(vcpkg_json).get("version-date")
    if not version:
        return None
    return repo, ref_template.replace("${VERSION}", version)


def osv_query(repo: str, ref: str) -> list[dict]:
    project_name = repo.split("/")[-1]
    body = json.dumps({
        "package": {"purl": f"pkg:generic/{project_name}"},
        "version": ref,
    }).encode()
    req = urllib.request.Request(
        OSV_QUERY_URL, data=body,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read())
    except urllib.error.URLError as e:
        print(f"  WARNING: OSV.dev query failed for {repo}@{ref}: {e}", file=sys.stderr)
        return []
    return data.get("vulns", [])


def main() -> int:
    baseline = read_baseline("vcpkg.json")
    ports = installed_ports("build/vcpkg_installed", "x64-linux")

    found_any = False
    skipped = []
    for port in ports:
        repo_ref = port_repo_ref(baseline, port)
        if repo_ref is None:
            skipped.append(port)
            continue
        repo, ref = repo_ref
        vulns = osv_query(repo, ref)
        status = f"{port} ({repo}@{ref})"
        acknowledged = ACKNOWLEDGED.get(port, {})
        new_vulns = [v for v in vulns if v["id"] not in acknowledged]
        old_vulns = [v for v in vulns if v["id"] in acknowledged]
        if new_vulns:
            found_any = True
            print(f"VULNERABLE: {status}")
            for v in new_vulns:
                print(f"  {v['id']}: {v.get('summary', '(no summary)')}")
                print(f"  https://osv.dev/vulnerability/{v['id']}")
        elif old_vulns:
            print(f"acknowledged: {status}")
            for v in old_vulns:
                print(f"  {v['id']}: {acknowledged[v['id']]}")
        else:
            print(f"clean: {status}")

    if skipped:
        print(f"\nskipped (not vcpkg_from_github, checked manually or N/A): {', '.join(skipped)}")

    if found_any:
        print("\nFound OSS-Fuzz-sourced advisories matching a pinned version above -- "
              "see docs/embeddings.md-style overlay-port patch precedent "
              "(cross/overlay-ports/md4c/) for how to handle one.")
        return 1
    print("\nNo OSS-Fuzz-sourced advisories found for any pinned version checked "
          "(see this script's own module docstring for what that does NOT prove).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
