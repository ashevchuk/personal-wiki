#include "util/MarkdownRenderer.h"

#include <md4c-html.h>

#include <stdexcept>

namespace wikicore::util {

namespace {

void appendOutput(const MD_CHAR* text, MD_SIZE size, void* userdata) {
  static_cast<std::string*>(userdata)->append(text, size);
}

}  // namespace

std::string renderMarkdownToHtml(std::string_view markdown) {
  std::string html;

  constexpr unsigned kParserFlags =
      MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
      MD_FLAG_PERMISSIVEAUTOLINKS |
      // Raw HTML passthrough is off on purpose: a document written while
      // private shouldn't get to inject arbitrary HTML/JS just because it
      // later gets flipped to public. There's no separate sanitizer in
      // front of this renderer's output — this flag IS the sanitization.
      MD_FLAG_NOHTMLBLOCKS | MD_FLAG_NOHTMLSPANS;
  constexpr unsigned kRendererFlags = 0;

  const int rc =
      md_html(markdown.data(), static_cast<MD_SIZE>(markdown.size()),
              &appendOutput, &html, kParserFlags, kRendererFlags);
  if (rc != 0) {
    throw std::runtime_error("markdown rendering failed");
  }
  return html;
}

}  // namespace wikicore::util
