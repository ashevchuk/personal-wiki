# Розгортання

## Статус перевірки на реальному залізі

**Чесно, без прикрас**: повна нативна збірка (`cmake --build` з нуля, не крос-компіляція)
перевірена лише на x86_64 Linux (Arch) — `cmake --install`, запуск встановленого дерева,
повний security/E2E-прохід (`ctest`), живий VaultWatcher. **Нативної збірки на самому
Raspberry Pi (компіляція просто на пристрої, як описано нижче в "Збірка") ще не
виконано** — у середовищі розробки немає часу ганяти багатогодинний білд Drogon+OpenSSL
на слабкому SBC щоразу.

Натомість **крос-компільовані бінарники реально розгорнуто й перевірено на живому
цільовому залізі** — armv7l, Debian 9 (stretch, EOL, glibc 2.24) — через
`arm-linux-musleabihf`+musl+static (див. "Крос-компіляція" нижче): `wiki-server` і
`wiki-mcp` запущені НАТИВНО (не під емуляцією) на реальному пристрої, `unit_tests`
пройшов під `qemu-arm-static` (112 assertions, 52 test cases), повний цикл
login → CSRF → створення документа → атомарний запис на диск → FTS5-пошук з
підсвіткою snippet — перевірено живим HTTP-трафіком проти реального systemd-юніту
(`ProtectSystem=strict` та інше хардненинг з розділу нижче включно), поруч із живими
nginx/Samba/NFS/ProFTPD/mosquitto/munin, без жодного впливу на них. Код не містить
нічого свідомо x86-специфічного, і зараз це емпірично підтверджено, а не лише
"має працювати за конструкцією".

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

## Крос-компіляція (armv7, musl, static) — для старого/слабкого таргета

Коли нативна збірка на самому пристрої непрактична (старий дистрибутив без сучасного
компілятора, або просто шкода часу на багатогодинний білд Drogon+OpenSSL на слабкому
SBC) — крос-компіляція з x86_64 dev-машини через [zig](https://ziglang.org/)
(`zig cc`/`zig c++`) як самодостатній C/C++ крос-компілятор з вбудованим musl libc +
libc++, повністю статичне лінкування (`-static`). Чому musl+static, а не glibc
крос-тулчейн: старий таргет (наприклад, Debian 9 stretch, glibc 2.24 з 2016 року)
зламався б на рантаймі (`GLIBC_2.XX not found`) проти будь-якого сучасного glibc
крос-тулчейну; статичний musl-бінарник узагалі не чіпає glibc таргета.

```sh
# 1. крос-збірка залежностей через vcpkg (classic mode — drogon[ctl] не
#    підтримує cross-таргет, тому ctl-фіча пропускається; вже зібраний
#    x64-linux drogon_ctl передається окремо нижче)
cd vcpkg
./vcpkg install --classic --triplet arm-musl \
  --overlay-triplets=../cross/arm-musl --overlay-ports=../cross/overlay-ports \
  --x-install-root=../vcpkg_installed_arm \
  drogon sqlite3[core,fts5,json1] libargon2 nlohmann-json md4c yaml-cpp \
  tomlplusplus catch2
cd ..

# 2. конфіг+білд проєкту проти крос-встановленого префікса
export PKG_CONFIG_LIBDIR="$PWD/vcpkg_installed_arm/arm-musl/lib/pkgconfig:$PWD/vcpkg_installed_arm/arm-musl/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
cmake -S . -B build-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cross/arm-musl/toolchain.cmake \
  -DCMAKE_PREFIX_PATH=$PWD/vcpkg_installed_arm/arm-musl \
  -DCMAKE_FIND_ROOT_PATH=$PWD/vcpkg_installed_arm/arm-musl \
  -DDROGON_CTL_COMMAND=$PWD/build/vcpkg_installed/x64-linux/tools/drogon/drogon_ctl \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-arm -j"$(nproc)"

# 3. верифікація ПЕРЕД перенесенням на реальне залізо — під qemu-user-mode
qemu-arm-static ./build-arm/tests/unit_tests   # має пройти всі кейси, не лише не впасти
qemu-arm-static ./build-arm/wiki-server --create-admin   # smoke: реальний бінарник, не тільки "скомпілювалось"
```

**Відомий баг zig 0.16.0**: лінк багатьох статичних `.a`-архівів для
`arm-linux-musleabihf` SIGSEGV-ить (`code=139`) у вбудованому lld — але не через
кількість архівів чи паралелізм (обидві гіпотези перевірено й відкинуто), а через
конкретний прапорець: CMake (Ninja-генератор, ≥3.20) автоматично додає
`-Xlinker --dependency-file=...` для трекінгу залежностей на рівні лінкера, і саме
цей прапорець валить lld для цього таргета детерміновано, 100% випадків. Фікс —
`set(CMAKE_LINK_DEPENDS_USE_LINKER OFF)` у `cross/arm-musl/toolchain.cmake`
(CMake сам падає назад на не-лінкерне трекання залежностей). Два menші оверлей-порти
(`cross/overlay-ports/{brotli,libuuid}`) вимикають побудову їхніх CLI/test-бінарників
з тієї самої причини (лінк маленького виконуваного файлу проти одного `.a` теж падав).

**Reverse-proxy nginx впритул до вже живого nginx на таргеті**: `wiki-server`
навмисно слухає лише `127.0.0.1:8080` (див. `config.toml`), TLS сам не термінує —
додавання окремого `server{}`-блоку в існуючий nginx (новий піддомен або `location`)
лишається за адміністратором вручну, воно НЕ чіпає жодну наявну конфігурацію nginx
автоматично.

## Reverse-proxy під підшляхом (наприклад `/wiki`)

Коли застосунок мають розкрити не на окремому (під)домені, а під шляхом на вже
існуючому сайті (`https://example.com/wiki/`) — застосунок сам генерує КОЖЕН
`href`/`action`/`hx-get`/redirect Location/збережений шлях у своєму JS як абсолютний
шлях від кореня (`/login`, `/d/...`, `/css/...`). Без якоїсь домовленості з проксі
про префікс це виглядало б чисто на самій `/wiki/`-сторінці, але зламало б усе, що
на неї посилається (CSS/JS 404, форма логіну постить на `/login` замість
`/wiki/login`, і так далі).

**Правильне рішення — `[server].base_path` в `config.toml`**, не хаки на боці nginx:

```toml
[server]
base_path = "/wiki"
```

Коли встановлено, застосунок сам вшиває префікс у КОЖЕН `href`/`action`/`hx-*`/
redirect/JS-шлях, який генерує (сторінки логіну/перегляду/редагування/пошуку,
`edit.js`, CSP-шаблони `EditPage`/`SearchPage`) — маршрути всередині лишаються
незмінними (`/login`, `/d/{path}`, ...), оскільки проксі знімає префікс ще ДО
переадресації запиту всередину. Це означає, що nginx-сторона — звичайний
prefix-stripping `proxy_pass`, без жодного переписування тіла відповіді чи
заголовків:

```nginx
location = /wiki {
    return 301 /wiki/;
}

location /wiki/ {
    proxy_pass http://127.0.0.1:8080/;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
}
```

Ніякого `sub_filter`, ніякого `proxy_redirect`, ніякого `Accept-Encoding ""`-хака —
усе це були потрібні милиці ДО того, як `base_path` зʼявився в самому застосунку
(історично перший робочий варіант деплою на це залізо якраз через них і пройшов;
`base_path` — прибирає потребу в них повністю й раз назавжди, а не латає симптом
на проксі щоразу заново).
