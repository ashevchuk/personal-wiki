# Розгортання

## Статус перевірки на реальному залізі

**Чесно, без прикрас**: усе нижче перевірено на x86_64 Linux (Arch) — повна збірка, `cmake
--install`, запуск встановленого дерева, повний security/E2E-прохід (`ctest`), у т.ч.
живий VaultWatcher. **Нативної збірки й запуску на реальному Raspberry Pi НЕ виконано** —
у середовищі розробки немає фізичного ARM-заліза. Код не містить нічого свідомо
x86-специфічного (жодних інтринсиків, жодних архітектурних припущень — Drogon/SQLite/
libargon2/усі vcpkg-залежності мають офіційну підтримку ARM64/Linux), і install-шлях
однаковий для будь-якої архітектури, оскільки це той самий native-build-on-target підхід.
Але "має працювати за конструкцією" — не те саме, що "перевірено". Перший реальний запуск
на Pi — обов'язковий крок перед тим, як довіряти цьому в проді, і саме на цьому кроці
конкретна модель Pi/дистрибутив/версія системного sqlite3 можуть викопати щось несподіване.

## Передумови (на цільовому пристрої — Raspberry Pi чи інший Linux/ARM64/x86_64 SBC)

- GCC з підтримкою C++20 (перевірено на GCC 16.2.1; C++20 у GCC доступний з версії 10,
  але чим свіжіший — тим менше сюрпризів)
- CMake ≥ 3.21, Ninja
- git, curl
- Python 3 (для `ctest`-інтеграційного security-прогону; не потрібен у рантаймі)
- ядро Linux з увімкненим inotify (стандартно на будь-якому сучасному дистрибутиві)

## Збірка (нативна, на самому пристрої)

```sh
git clone <repo-url> wiki && cd wiki

# vcpkg — не вендориться, клонується окремо (див. CLAUDE.md)
git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

# security/E2E-прохід перед тим, як довіряти збірці (auth, CSRF, path
# traversal, session fixation, visibility gating, VaultWatcher — див.
# tests/integration/security_e2e.py)
ctest --test-dir build --output-on-failure
```

На слабкому SBC це може зайняти суттєво довше, ніж на десктопі (Drogon+OpenSSL+trantor
з нуля — не швидко); це одноразова вартість.

## Встановлення

```sh
sudo cmake --install build --prefix /opt/wiki
```

Кладе `bin/wiki-server`, `bin/wiki-mcp`, `static/`, `config.example.toml` та
`share/wiki/systemd/{wiki.service,wiki.env.example}` під `/opt/wiki`. **Не** створює
робочий `config.toml` — тільки приклад (свідомо: реальний конфіг ніколи не має
матеріалізуватись мовчки з дефолтів).

## Перше налаштування

```sh
# виділений непривілейований користувач/група — юніт-файл очікує саме їх
sudo useradd --system --home-dir /opt/wiki --shell /usr/sbin/nologin wiki
sudo chown -R wiki:wiki /opt/wiki

cd /opt/wiki
sudo -u wiki cp config.example.toml config.toml
# відредагуй за потреби: [server].port, [vault].path, [mcp].scope

# створити admin-акаунт (пароль вводиться інтерактивно, echo вимкнено)
sudo -u wiki ./bin/wiki-server --create-admin
```

## systemd

```sh
sudo cp /opt/wiki/share/wiki/systemd/wiki.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wiki.service
sudo systemctl status wiki.service
```

Юніт вже налаштований з хардненням (`ProtectSystem=strict`, `NoNewPrivileges=yes`,
`ReadWritePaths=/opt/wiki/vault_data` — єдине місце, куди сервіс реально пише).
`EnvironmentFile=-/etc/wiki/wiki.env` опціональний — на зараз жодна змінна оточення
застосунком не читається (адмін-креденшли живуть у SQLite, сесії — випадкові токени
без підпису секретом); файл лишається як задокументований гачок під майбутнє
(наприклад, bearer-токен для remote MCP-транспорту у Фазі 2).

## TLS / публічний доступ в інтернет

`wiki-server` сам TLS не термінує. Для доступу поза локальною мережею — реверс-проксі
(nginx/Caddy/traefik) перед ним, що займається TLS і проксує на `127.0.0.1:8080`
(чи що вказано в `config.toml`). Без цього кроку — тримати `listen_addr = "127.0.0.1"`
і не відкривати порт назовні напряму.

## Бекап

Джерело правди — `[vault].path` (директорія з `.md`-файлами та `.trash/`). SQLite-індекс
(`[index].db_path`) повністю одноразовий і перебудовується `wiki-server --reindex` —
бекапити не обов'язково, але й не завадить (швидше при відновленні, ніж повний рескан
з нуля на дуже великому vault). Мінімум: регулярний бекап `[vault].path`.

## Оновлення

```sh
cd wiki && git pull
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # перш ніж рестартити прод
sudo cmake --install build --prefix /opt/wiki
sudo systemctl restart wiki.service
```

## Кросс-компіляція ARM64

Не реалізовано — свідомо відкладено на Фазу 2 (див. план). MVP-шлях — нативна збірка
прямо на цільовому пристрої, як описано вище.
