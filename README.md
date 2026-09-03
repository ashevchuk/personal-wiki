# Personal Wiki

Персональна база знань: markdown-документи на диску, повнотекстовий пошук, Web UI з
WYSIWYG-редагуванням, авторизація з public/private контентом, MCP-сервер для LLM-клієнтів
(Claude Desktop/Code). Монолітний C++-сервіс, розрахований на розгортання на одноплатниках.

Архітектура й обґрунтування рішень: [`docs/architecture.md`](docs/architecture.md).
Повний план розробки з фазуванням: `/home/slayer/.claude/plans/zazzy-twirling-sundae.md`.

Інструкції для збірки — для контриб'юторів/агентів дивись `CLAUDE.md`; тут коротко:

```sh
git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

./build/wiki-server
```

Статус: **Milestone 0 (bootstrap)** — див. план для повного списку milestones.
