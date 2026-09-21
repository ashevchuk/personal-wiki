#include "embeddings/EmbeddingChunks.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace wikicore::embeddings {

namespace {

bool isUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

std::size_t utf8Floor(const std::string& s, std::size_t i) {
  if (i >= s.size()) return s.size();
  while (i > 0 && isUtf8Continuation(static_cast<unsigned char>(s[i]))) --i;
  return i;
}

std::size_t utf8Ceil(const std::string& s, std::size_t i) {
  if (i >= s.size()) return s.size();
  while (i < s.size() && isUtf8Continuation(static_cast<unsigned char>(s[i]))) ++i;
  return i;
}

int estimateTokens(std::string_view s) {
  if (s.empty()) return 0;
  return static_cast<int>((s.size() + static_cast<std::size_t>(kEmbeddingChunkBytesPerToken) - 1) /
                          static_cast<std::size_t>(kEmbeddingChunkBytesPerToken));
}

std::size_t findBreak(const std::string& body, std::size_t start, std::size_t end) {
  if (end >= body.size()) return body.size();
  if (end <= start) return end;
  // Prefer a paragraph/sentence boundary in the second half of the window
  // so a break doesn't leave a tiny first piece and dump the rest next.
  const std::size_t minEnd = start + (end - start) / 2;
  std::size_t p = body.rfind("\n\n", end - 1);
  if (p != std::string::npos && p >= minEnd) return p + 2;
  p = body.rfind('\n', end - 1);
  if (p != std::string::npos && p >= minEnd) return p + 1;
  p = body.rfind(". ", end - 1);
  if (p != std::string::npos && p + 2 >= minEnd && p + 2 <= end) return p + 2;
  return utf8Floor(body, end);
}

}  // namespace

std::vector<std::string> chunkForEmbedding(const std::string& title, const std::string& body,
                                           int maxInputTokens) {
  int target = kEmbeddingChunkTargetTokens;
  if (maxInputTokens > 0) {
    target = std::min(target, maxInputTokens - kEmbeddingChunkSpecialTokenReserve);
  }
  if (target < 32) target = 32;

  std::string prefix;
  if (!title.empty()) {
    prefix = title;
    prefix += "\n\n";
  }

  int prefixTok = estimateTokens(prefix);
  int bodyBudgetTok = target - prefixTok;
  if (bodyBudgetTok < 48) {
    // Title alone is eating the window — keep a short prefix so the
    // passage still has room, rather than emitting title-only chunks.
    const int titleBudgetTok = std::max(16, target / 4);
    const std::size_t titleBytes =
        static_cast<std::size_t>(titleBudgetTok) * kEmbeddingChunkBytesPerToken;
    const std::string clipped = title.substr(0, utf8Floor(title, std::min(title.size(), titleBytes)));
    prefix = clipped;
    if (!prefix.empty()) prefix += "\n\n";
    prefixTok = estimateTokens(prefix);
    bodyBudgetTok = std::max(32, target - prefixTok);
  }

  const std::size_t bodyBudgetBytes =
      static_cast<std::size_t>(bodyBudgetTok) * kEmbeddingChunkBytesPerToken;
  std::size_t overlapBytes =
      static_cast<std::size_t>(kEmbeddingChunkOverlapTokens) * kEmbeddingChunkBytesPerToken;
  if (overlapBytes >= bodyBudgetBytes) overlapBytes = bodyBudgetBytes / 4;

  if (body.empty()) {
    return {prefix.empty() ? title : prefix};
  }

  std::vector<std::string> chunks;
  std::size_t pos = 0;
  while (pos < body.size() && static_cast<int>(chunks.size()) < kEmbeddingChunkMaxChunks) {
    std::size_t rawEnd = pos + bodyBudgetBytes;
    if (rawEnd > body.size()) rawEnd = body.size();
    std::size_t end = (rawEnd < body.size()) ? findBreak(body, pos, utf8Floor(body, rawEnd))
                                             : body.size();
    if (end <= pos) {
      end = utf8Ceil(body, std::min(body.size(), pos + std::max<std::size_t>(1, bodyBudgetBytes)));
      if (end <= pos) end = body.size();
    }
    chunks.push_back(prefix + body.substr(pos, end - pos));
    if (end >= body.size()) break;
    std::size_t next = (end > overlapBytes) ? utf8Floor(body, end - overlapBytes) : end;
    if (next <= pos) next = end;
    pos = next;
  }

  // Hard cap: if the body still has unread tail after kMaxChunks windows,
  // replace the last chunk with a window ending at EOF so both the start
  // AND the end of a long document stay searchable (a buried middle
  // section may drop; 32 * ~384 tokens is already a long personal-wiki
  // note).
  if (static_cast<int>(chunks.size()) == kEmbeddingChunkMaxChunks && pos < body.size() &&
      !chunks.empty()) {
    const std::size_t start =
        body.size() > bodyBudgetBytes ? utf8Ceil(body, body.size() - bodyBudgetBytes) : 0;
    chunks.back() = prefix + body.substr(start);
  }

  if (chunks.empty()) chunks.push_back(prefix.empty() ? std::string() : prefix);
  return chunks;
}

}  // namespace wikicore::embeddings
