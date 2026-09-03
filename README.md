# Personal Wiki

A personal knowledge base: markdown documents on disk, full-text search, a Web UI with
WYSIWYG editing, auth with public/private content, an MCP server for LLM clients
(Claude Desktop/Code). A monolithic C++ service, built for deployment on single-board
computers.

Architecture and design rationale: [`docs/architecture.md`](docs/architecture.md).
Full phased development plan: `/home/slayer/.claude/plans/zazzy-twirling-sundae.md`.
Deployment on real hardware: [`docs/deployment.md`](docs/deployment.md) and
[`docs/sbc-deployment.md`](docs/sbc-deployment.md) (a self-contained runbook).

Build instructions — for contributors/agents see `CLAUDE.md`; the short version:

```sh
git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

./build/wiki-server
```

Status: **Milestones M0–M5 complete** (bootstrap, auth, CRUD/WYSIWYG, search/nav, MCP,
hardening/deployment) — see the plan for the full milestone breakdown. Deployed and
verified on real ARM hardware via cross-compilation; see `docs/deployment.md`'s
verification status for exactly what that does and doesn't cover.
