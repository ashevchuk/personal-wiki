#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wikicore::util {

// One `- [ ] text` / `- [x] text` (GFM task-list) line found in a
// document's body. `lineNo` is 1-based, counting every line of the raw
// body text (blank lines included) -- not currently used for anything
// but display ordering, kept anyway since it's free here and any future
// "toggle this exact item" feature needs a stable way to find the line
// again without re-parsing the whole body to guess which occurrence of
// otherwise-identical text was meant.
struct TodoItem {
  bool checked = false;
  std::string text;
  int lineNo = 0;
};

// Every GFM task-list line in `markdown`, in document order. A hand-rolled
// per-line scan, not <regex> -- same reasoning as WikiLinks.cpp: this is
// the document author's own body text, not adversarial input, and the
// grammar (list marker, "[ ]"/"[x]", one space, the rest is the text) is
// small enough not to need it. Recognizes `-`/`*`/`+` markers and both
// `x`/`X` for "checked", matching what Toast UI Editor's own checkbox
// list actually writes plus the handful of equivalent spellings a human
// editing the raw markdown directly might type. No code-fence awareness,
// same precedent as extractWikiLinkTargets -- a `- [ ]` typed inside a
// fenced example block gets counted too; narrowing that out would need
// tracking fence state across lines for a case nobody has actually hit.
std::vector<TodoItem> extractTodoItems(std::string_view markdown);

}  // namespace wikicore::util
