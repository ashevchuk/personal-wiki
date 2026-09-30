#include "util/TodoItems.h"

#include <optional>

namespace wikicore::util {

namespace {

std::optional<TodoItem> parseTodoLine(std::string_view line) {
  size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;

  if (i >= line.size()) return std::nullopt;
  const char marker = line[i];
  if (marker != '-' && marker != '*' && marker != '+') return std::nullopt;
  ++i;

  if (i >= line.size() || line[i] != ' ') return std::nullopt;
  ++i;
  if (i >= line.size() || line[i] != '[') return std::nullopt;
  ++i;

  if (i >= line.size()) return std::nullopt;
  bool checked;
  if (line[i] == ' ') {
    checked = false;
  } else if (line[i] == 'x' || line[i] == 'X') {
    checked = true;
  } else {
    return std::nullopt;
  }
  ++i;

  if (i >= line.size() || line[i] != ']') return std::nullopt;
  ++i;
  if (i >= line.size() || line[i] != ' ') return std::nullopt;
  ++i;

  std::string_view rest = line.substr(i);
  size_t end = rest.size();
  while (end > 0 &&
         (rest[end - 1] == ' ' || rest[end - 1] == '\t' || rest[end - 1] == '\r')) {
    --end;
  }
  if (end == 0) return std::nullopt;  // "- [ ] " with nothing after it

  TodoItem item;
  item.checked = checked;
  item.text = std::string(rest.substr(0, end));
  return item;
}

}  // namespace

std::vector<TodoItem> extractTodoItems(std::string_view markdown) {
  std::vector<TodoItem> items;
  size_t pos = 0;
  int lineNo = 0;
  while (pos <= markdown.size()) {
    ++lineNo;
    const size_t nl = markdown.find('\n', pos);
    const std::string_view line =
        nl == std::string_view::npos ? markdown.substr(pos) : markdown.substr(pos, nl - pos);

    if (auto item = parseTodoLine(line)) {
      item->lineNo = lineNo;
      items.push_back(std::move(*item));
    }

    if (nl == std::string_view::npos) break;
    pos = nl + 1;
  }
  return items;
}

}  // namespace wikicore::util
