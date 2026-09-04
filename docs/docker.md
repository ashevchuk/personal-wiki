# Docker

**Not the recommended path for a weak/old ARM SBC.** Docker itself needs a reasonably
modern kernel/glibc — the actual verified-on-real-hardware target this project targets
(Debian 9 stretch, armv7, glibc 2.24, EOL) is too old to run a modern Docker Engine at
all. For that class of device, use native build or the zig cross-compile path instead
— see `docs/deployment.md` and `docs/sbc-deployment.md`. This Dockerfile is for the
"I want to try this right now" path: a desktop, a NAS, a cloud VM, or a Pi 4/5 on a
64-bit OS new enough to run Docker properly.

## Quick start

```sh
git clone <this-repo> wiki && cd wiki
mkdir -p vault_data
docker compose up -d
docker compose exec wiki wiki-server --create-admin   # interactive, one time
```

`http://localhost:8080` should now answer. `./vault_data` on the host is bind-mounted
into the container (`/data/vault`) — same principle as every other deployment path in
this repo: the markdown files are the source of truth, meant to be directly reachable
(browsable, `git`-able, restic/rsync-able) outside the container, not locked inside a
Docker-only volume.

Without compose:

```sh
docker build -t personal-wiki .
docker run -d --name wiki -p 8080:8080 -v "$PWD/vault_data:/data/vault" personal-wiki
docker exec -it wiki wiki-server --create-admin
```

## What's actually in the image

Two-stage build (`Dockerfile`): a `debian:bookworm-slim` builder stage runs the exact
same `cmake`/vcpkg/`ctest` sequence documented in the main [`README`](../README.md#quick-start)
— including the full `ctest` security suite; the build fails if it doesn't pass — then
`cmake --install`s into a second, minimal `debian:bookworm-slim` runtime stage. vcpkg's
Linux triplets are static by default, so the compiled binaries carry almost no dynamic
dependencies beyond glibc itself (confirmed via `ldd` — see `CLAUDE.md`); the runtime
image doesn't need a copy of the build toolchain sitting around.

The image's own `vcpkg.json`-only layer (copied before the rest of the source) is a
deliberate Docker layer-caching trick: editing a `.cpp` file and rebuilding the image
does NOT re-trigger a from-scratch Drogon+OpenSSL+trantor build — only touching
`vcpkg.json` itself does. The first build is genuinely slow regardless (this is the
same dependency chain `docs/sbc-deployment.md` warns takes "substantially longer than
on a desktop" on weak hardware — even on a normal x86_64 machine it's not instant);
every build after that reuses the cached layer.

Runs as a fixed, non-root `uid:gid 1000:1000` — matches the default first-user id on
most Linux distros, so a freshly `mkdir -p vault_data`'d host directory usually just
works as a bind mount with no extra step. If your host user is a different uid,
`chown -R 1000:1000 vault_data` once, or add `user: "1000:1000"` (or your own
`$(id -u):$(id -g)`) to `docker-compose.yml` and rebuild.

## Configuration

The image ships its own `docker/config.docker.toml` (baked in as `/opt/wiki/config.toml`
at build time) — the one thing that has to differ from `config.example.toml` is
`[vault].path`/`[index].db_path`, pointed at `/data/vault` (the volume) instead of the
relative `./vault_data` every other deployment path uses. Everything else matches the
example defaults. To change a setting (log level, MCP scope, etc.), edit
`docker/config.docker.toml` and rebuild the image — there's no environment-variable
override (`wiki-server` doesn't read any env var at all, see `config.example.toml`'s
own header comment).

## `wiki-mcp` from a container

The image also ships `wiki-mcp` (in `/opt/wiki/bin/`) even though the container's own
`ENTRYPOINT` runs `wiki-server`. An MCP client that can spawn an arbitrary command can
point straight at `docker exec`:

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

The remote (HTTP) MCP transport (`docs/mcp.md`) works the same as always through
`wiki-server` itself — nothing Docker-specific needed there, it's just another route
on the same `8080` port already published.

## Backup

`docker compose exec wiki wiki-server --reindex` works the same as the native path.
The one-click Web UI backup button (Account page — see `docs/deployment.md`'s
"Backup" section) works unmodified too. The opt-in `systemd` timer
(`systemd/wiki-backup.*`) is systemd-specific and doesn't apply here — since
`./vault_data` is a plain host directory, back it up with whatever the host already
uses for backups (cron + `tar`, restic, a NAS's own snapshot feature, ...).
