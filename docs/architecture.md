# Архітектура

Повний план розробки: `/home/slayer/.claude/plans/zazzy-twirling-sundae.md`. Цей файл —
короткий довідник по прийнятих рішеннях і результатах M0-спайків, щоб не гортати план
щоразу.

## Прийняті рішення (не переглядати без явного запиту користувача)

- **Storage**: Markdown-файли на диску = джерело правди. SQLite — лише вторинний
  індекс (FTS5 + метадані), повністю відновлюваний повним рескануванням vault.
- **MCP**: сервіс сам є MCP-сервером (stdio), не клієнтом. Read/search only у MVP.
- **Auth**: один адмін, argon2id, SQLite-сесії. `visibility: public|private` у
  front-matter, за замовчуванням `private` (fail-safe).
- **Frontend**: Drogon CSP views + htmx, Toast UI Editor точково на сторінці
  редагування. Без SPA build-пайплайна в рантаймі.
- **Деплой**: голий бінарник + systemd. arm64 crosscompile — Фаза 1.5/2; MVP
  збирається нативно на цільовому Raspberry Pi.

## M0 — результати спайків

- **FTS5**: підтверджено доступний (системний `sqlite3` CLI, `CREATE VIRTUAL TABLE
  ... USING fts5(...)` компілюється й працює). У vcpkg-порту `sqlite3` явно
  запитана фіча `fts5` (`vcpkg.json`) — вона не default-feature, без цього її б не
  було в зібраній бібліотеці.
- **Markdown-рендер**: обрано **md4c** замість `cmark-gfm`. Обидва порти є у vcpkg;
  md4c — чистий C, легший, GFM-розширення (tables/strikethrough/tasklists)
  вмикаються прапорцями парсера напряму, без окремого GFM-форку бібліотеки.
- **Front-matter YAML**: обрано **yaml-cpp**, не рукописний парсер. Front-matter
  редагується користувачем напряму в текстовому редакторі (поза Web UI, синхронізується
  через `VaultWatcher`) — довільне квотування/списки/дати мають парситись коректно;
  "Norway problem" (`no`/`yes`/`on`/`off` як булеві) та інші YAML-пастки краще
  делегувати перевіреній бібліотеці, ніж перевинаходити.
- **Package manager**: vcpkg, manifest mode, `builtin-baseline` запінений на конкретний
  commit vcpkg (див. `vcpkg.json`) для відтворюваності збірки на іншій машині/RPi.
  vcpkg **не вендориться в git** (`vcpkg/` у `.gitignore`) — клонується bootstrap-кроком.

## M1 — auth, PathGuard-backed read, результати

- **auth/ і controllers/ — НЕ частина libwikicore.** vault/index/config/util
  лишаються в wikicore (потрібні і wiki-server, і майбутньому wiki-mcp);
  auth/ (sessions, argon2, CSRF, Drogon-фільтри) і controllers/ — суто
  веб-турбота, лінкуються тільки в wiki-server. MCP-інструменти read-only
  й не потребують сесій.
- **Drogon HttpFilter — пастка з іменем.** `registerHandler(..., {Get,
  "AuthFilter"})` шукає фільтр у `DrClassMap` за **повним кваліфікованим,
  демангленим** іменем типу (`__cxa_demangle(typeid(T).name())`), тобто
  `"wikicore::auth::AuthFilter"`, а не голим `"AuthFilter"` — інакше падає
  мовчки в рантаймі з `middleware X not found` (лог, не крах процесу, тож
  легко пропустити). Другий нюанс: `DrObject<T>::alloc_` — static-член
  шаблону, компілятор інстанціює (а отже й реєструє клас) лише якщо його
  реально ODR-юзнути; жоден фільтр ніде більше не конструюється й не
  згадується напряму, тож без явного форс-виклику `AuthFilter::
  classTypeName()`/`CsrfFilter::classTypeName()` у `main()` реєстратор
  ніколи не збирається лінкером. Обидва нюанси задокументовано прямо в
  коді (`main.cpp`, коментар перед реєстрацією роутів) — не приберати
  той виклик, він не "мертвий код".
- **CSRF-токен: двокукі доставка.** Синхронізатор-токен зберігається
  server-side в `sessions.csrf_token`; клієнту він доставляється окремим
  НЕ-HttpOnly cookie (`wiki_csrf_token`), виставленим паралельно з
  сесійним при логіні. `CsrfFilter` звіряє заголовок/форм-поле з
  server-side значенням, cookie — лише канал доставки, не джерело правди.
- **Fail-safe-private підтверджено тестами й E2E**: відсутній/зламаний
  YAML front-matter, відсутнє поле `visibility`, будь-яке значення крім
  рівно `"public"` — усе колапсує в `private`. Приватний документ
  анонімному запиту повертає `404`, не `403` (не розкривати існування).
- **`--create-admin` CLI**: пише/перезаписує єдиний admin-рядок
  (`users`, `id=1` enforced CHECK), пароль вводиться з вимкненим echo
  термінала (termios), не логується.

## Двобінарна структура

`libwikicore` (vault + index + MCP tool logic) — без залежності від Drogon/OpenSSL.
Лінкується в обидва виконувані файли:

- `wiki-server` — HTTP (Drogon), контролери, CSP views.
- `wiki-mcp` — stdio MCP entrypoint, спавниться Claude Desktop/Code напряму, без
  накладних витрат HTTP-стеку.

## Build

```sh
# перший раз: клонувати й забутстрапити vcpkg (не в git)
git clone --depth 1 https://github.com/microsoft/vcpkg.git vcpkg
./vcpkg/bootstrap-vcpkg.sh -disableMetrics

# configure + build (сам встановить залежності з vcpkg.json)
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

# запуск
./build/wiki-server        # слухає 127.0.0.1:8080, GET /healthz
```
