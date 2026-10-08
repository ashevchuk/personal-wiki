# Docker

**This is not the recommended path for a weak or old ARM SBC.** Docker
itself needs a reasonably modern kernel and glibc, and the actual
real-hardware target this project is verified against (Debian 9 stretch,
armv7, glibc 2.24, end-of-life) is too old to run a modern Docker Engine
at all — it's also too old for native on-device compilation, which needs
a C++20 toolchain the distro doesn't have. For that class of device, use
the zig cross-compile path instead (see `docs/sbc-deployment.md`).

This Dockerfile is for the "I want to try this right now" path: a
desktop, a NAS, a cloud VM, or a Pi 4/5 running a 64-bit OS new enough to
run Docker properly.

Two of `tools/wiki-ops.sh`'s container variants have actually been run
end-to-end on a real aarch64 SBC (an Orange Pi One Plus): `container-arm`
(a `buildx`-built, QEMU-emulated ARM image) and `container-native` (a
`buildx`-built `linux/arm64` binary, deployed as a plain systemd install
rather than a running container). See `docs/sbc-deployment.md`'s
"Real-hardware verification status" for exactly what each one exercises.
The plain `container` variant below is verified on x86_64 only, not yet
on ARM hardware specifically.

## Quick start

```sh
git clone https://github.com/ashevchuk/personal-wiki.git wiki && cd wiki
mkdir -p vault_data
docker compose up -d
docker compose exec wiki wiki-server --create-admin   # interactive, one time
```

`http://localhost:8080` should now answer. `./vault_data` on the host is
bind-mounted into the container at `/data/vault` — the same principle as
every other deployment path in this repo: the markdown files are the
source of truth, meant to stay directly reachable (browsable, `git`-able,
restic/rsync-able) from outside the container, not locked inside a
Docker-only volume.

Without compose:

```sh
docker build -t personal-wiki .
docker run -d --name wiki -p 8080:8080 -v "$PWD/vault_data:/data/vault" personal-wiki
docker exec -it wiki wiki-server --create-admin
```

## What's actually in the image

It's a two-stage build (`Dockerfile`). A `debian:bookworm-slim` builder
stage runs the exact same `cmake`/vcpkg/`ctest` sequence documented in the
main [`README`](../README.md#quick-start) — including the full `ctest`
security suite; the build fails outright if it doesn't pass. That stage
then runs `cmake --install` into a second, minimal
`debian:bookworm-slim` runtime stage. vcpkg's Linux triplets are static by
default, so the compiled binaries carry almost no dynamic dependencies
beyond glibc itself (confirmed with `ldd` — see `CLAUDE.md`), which means
the runtime image doesn't need a copy of the build toolchain sitting
around.

One deliberate trick: the image copies in just `vcpkg.json` before the
rest of the source, as its own Docker layer. That's a layer-caching
optimization — editing a `.cpp` file and rebuilding the image does
**not** re-trigger a from-scratch Drogon+OpenSSL+trantor build; only
touching `vcpkg.json` itself does. The very first build is still slow
regardless (this is the same dependency chain `docs/sbc-deployment.md`
warns takes "substantially longer than on a desktop" on weak hardware —
even on a normal x86_64 machine it's not instant). Every build after that
reuses the cached layer.

The container runs as a fixed, non-root `uid:gid 1000:1000`, which
matches the default first-user id on most Linux distros — so a freshly
`mkdir -p vault_data`'d host directory usually just works as a bind mount
with no extra step. If your host user has a different uid, either run
`chown -R 1000:1000 vault_data` once, or add `user: "1000:1000"` (or your
own `$(id -u):$(id -g)`) to `docker-compose.yml` and rebuild.

## Configuration

The image ships its own `docker/config.docker.toml` (baked in as
`/opt/wiki/config.toml` at build time). The one setting that has to differ
from `config.example.toml` is `[vault].path`/`[index].db_path`, which
points at `/data/vault` (the volume) instead of the relative
`./vault_data` every other deployment path uses — everything else matches
the example defaults.

To change a setting (log level, MCP scope, embeddings provider, Draft
agent, etc.), edit `docker/config.docker.toml` and rebuild the image —
`AppConfig` doesn't read config keys from the environment. The exception
is runtime secrets: the embeddings and LLM API keys.
`CloudEmbeddingProvider`/`CloudChatClient` call `getenv()` on whatever
`[embeddings].api_key_env`/`[llm].api_key_env` names. Pass those when
starting the container (`docker run -e WIKI_EMBEDDINGS_API_KEY=... -e
ANTHROPIC_API_KEY=...`, or compose's `environment:`) — never bake a key
into the image itself. Leave them unset if you're using
`provider = "none"`, embeddings `"local"`, or a loopback
OpenAI-compatible server that doesn't authenticate. See
`docs/embeddings.md` and `docs/llm.md`.

## `wiki-mcp` from a container

The image also ships `wiki-mcp` (in `/opt/wiki/bin/`), even though the
container's own `ENTRYPOINT` runs `wiki-server`. An MCP client that can
spawn an arbitrary command can point straight at `docker exec`:

```json
{
  "mcpServers": {
    "personal-wiki": {
      "command": "docker",
      "args": ["exec", "-i", "wiki", "wiki-mcp"]
    }
  }
}
```

The remote (HTTP) MCP transport (`docs/mcp.md`) works exactly the same as
always, through `wiki-server` itself — nothing Docker-specific is needed
there; it's just another route on the same `8080` port already published.

## Backup

`docker compose exec wiki wiki-server --reindex` works the same as the
native path, and so does the one-click Web UI backup button (Account page
— see `docs/sbc-deployment.md`'s "Backup" section), unmodified. The opt-in
`systemd` timer (`systemd/wiki-backup.*`) is systemd-specific and doesn't
apply here, since `./vault_data` is just a plain host directory — back it
up with whatever the host already uses for backups (cron + `tar`, restic,
a NAS's own snapshot feature, and so on).
