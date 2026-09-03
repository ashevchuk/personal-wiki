# MCP-сервер

`wiki-mcp` — окремий бінарник, stdio-транспорт (JSON-RPC 2.0), спавниться MCP-клієнтом
(Claude Desktop, Claude Code) напряму. Read-only у MVP: жодних write-інструментів.

## Підключення в Claude Desktop

Додай у `claude_desktop_config.json` (macOS: `~/Library/Application Support/Claude/`,
Linux: `~/.config/Claude/`):

```json
{
  "mcpServers": {
    "personal-wiki": {
      "command": "/opt/wiki/bin/wiki-mcp",
      "args": [],
      "cwd": "/opt/wiki"
    }
  }
}
```

`cwd` має вказувати на директорію, де лежить `config.toml` (той самий, яким користується
`wiki-server`) — `wiki-mcp` шукає його відносно поточної робочої директорії, як і
`wiki-server`. Якщо `config.toml` відсутній — падає назад на `config.example.toml`
(дефолти).

**Важливо**: `wiki-mcp` НЕ рескание vault при кожному старті (на відміну від
`wiki-server`) — повний обхід на кожен спавн MCP-клієнта суперечив би вимозі
"стартує миттєво". Він довіряє наявному індексу. Переконайся, що `wiki-server`
хоч раз запускався (він рескание при старті) або виконай `wiki-server --reindex`
вручну перед першим підключенням MCP-клієнта.

## Інструменти (read-only)

- **search_documents**(query: string, tags?: string[], type?: string, limit?: number) —
  повнотекстовий пошук (FTS5, ранжування bm25), сніпет з підсвіткою збігів
  (`**термін**`, markdown bold — не HTML, MCP-клієнт читає текст, не рендерить сторінку).
- **get_document**(id_or_path: string) — повне тіло документа + метадані. Приймає і
  vault-relative шлях (`notes/foo.md`), і `id` (uuid) — резолвиться через індекс.
- **list_tags**() — усі теги з лічильниками документів.
- **list_documents**(tag?: string, type?: string, folder?: string, limit?: number,
  offset?: number) — перегляд без пошукового запиту, з пагінацією. `folder` — префікс
  шляху (`"notes/"` знаходить `notes/foo.md`, `notes/sub/bar.md`).

Усі чотири visibility-aware: private-документ не потрапляє в жоден результат, поки
`[mcp].scope` у `config.toml` не `"admin"` (дефолт). `scope = "public"` обмежує MCP-клієнта
лише публічним контентом — той самий fail-safe-private принцип, що й у HTTP-шарі, застосований
до іншої межі довіри (локальний spawn процесу власником машини, а не анонімний веб-візит).

## Реалізація

- Протокольний шар: [hkr04/cpp-mcp](https://github.com/hkr04/cpp-mcp), вендориться через
  CMake `FetchContent`, запінений на конкретний commit (немає порту у vcpkg). Spike під
  GCC 16.2.1/C++20 пройшов чисто з першої спроби — жодного фолбеку на ручний JSON-RPC не
  знадобилось (порівняно з планом, де він був заявлений як запасний варіант).
- `src/mcp/McpServer.cpp` — реєстрація 4 tools + обгортка над `index::FtsSearch`,
  `index::NavQueries`, `index::IndexUpdater::findPathByUuid`, `vault::DocumentService::get` —
  усе це вже існує в `libwikicore` з M2/M3, MCP-шар лише перекладає результати в `mcp::json`.
- `mcp::json` = `nlohmann::ordered_json`, вендорений окремою копією всередині cpp-mcp
  (`common/json.hpp`) — НЕ той самий `nlohmann_json`, що з vcpkg для решти проєкту.
  `McpServer.cpp` свідомо ніколи не включає обидва в одній єдиній точці компіляції
  (ризик ODR при двох копіях того самого хедера).

## Верифікація

Живого Claude Desktop у середовищі розробки нема (headless sandbox, без GUI) — тому
E2E-перевірка це скриптований JSON-RPC клієнт (`/tmp/mcp_e2e_test.py` під час розробки,
не в репо), що жене справжні MCP-повідомлення (`initialize` → `notifications/initialized`
→ `tools/list` → `tools/call` ×N) у реальний `wiki-mcp` процес через stdin/stdout і
звіряє відповіді. Прогнано двічі проти одного й того ж індексу — з `scope=admin` і
`scope=public` — 17/17 перевірок в обох режимах: усі 4 tools зареєстровані, пошук з
підсвіткою, `get_document` і по шляху, і по uuid, path traversal і "не знайдено" коректно
дають `isError:true` замість протокольного краху, а головне — visibility-gating реально
перемикається разом зі scope, а не просто "виглядає підключеним".
