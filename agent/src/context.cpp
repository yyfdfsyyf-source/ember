#include "agent/context.hpp"
#include "minijson.hpp"
#include <cctype>
#include <cstring>
#include <functional>

namespace agent {

std::string stripAnsi(std::string const& in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0, n = in.size();
  while (i < n) {
    unsigned char c = (unsigned char)in[i];
    if (c == 0x1B && i + 1 < n && in[i + 1] == '[') {
      i += 2;
      while (i < n && !(in[i] >= '@' && in[i] <= '~')) i++;
      i++;
      continue;
    }
    if (c == 0x1B) { i++; continue; }        // lone ESC
    if (c < 0x20 && c != '\n' && c != '\t') { i++; continue; }  // other C0
    if (c == 0x7F) { i++; continue; }        // DEL
    out += in[i];
    i++;
  }
  return out;
}

size_t estimateTokens(std::string const& text) {
  size_t cjk = 0, latin = 0;
  unsigned char prev = 0;
  for (unsigned char c : text) {
    if (c == 0 || (c & 0x80) == 0) {
      // ASCII: single-byte in this pass; whitespace/punct are cheap.
      if (std::isalnum(c)) latin++;
      prev = 0;
    } else if ((c & 0xE0) == 0xC0) {
      prev = c;  // 2-byte lead
    } else if ((c & 0xF0) == 0xE0) {
      prev = c;  // 3-byte lead (most CJK)
      // one 3-byte sequence == one CJK-ish token
      cjk++;
    } else if ((c & 0xF8) == 0xF0) {
      prev = c;
      cjk += 2;  // 4-byte codepoint, rare
    } else {
      if (prev) {
        // continuation byte completes a 2-byte sequence
        cjk++;
        prev = 0;
      }
    }
  }
  return cjk + latin / 4 + 1;
}

namespace {

// Filler words/tokens that carry no information and are stripped from
// assistant prose before sending. Matching is boundary-safe: a token only
// counts when it is a whole line or sits at a line start followed by a
// separator (comma/space/。 etc.), so meaningful words ("好的做法",
// "experience", "well-known") are never damaged.
struct Filler { char const* word; };
const Filler kAsianFiller[] = {
    {"好的好的"}, {"嗯嗯"}, {"哦哦"}, {"嗯啊"}, {"诶诶"},
    {"好的"}, {"好吧"}, {"好呀"}, {"好嘞"}, {"好哒"}, {"好的呢"},
    {"嗯"}, {"啊"}, {"哦"}, {"呃"}, {"诶"}, {"哎"}, {"呀"}, {"哈"}, {"嘿"},
};
const Filler kLatinFiller[] = {
    {"okay"}, {"alright"}, {"hmm"}, {"umm"}, {"uhh"}, {"yeah"}, {"yep"},
    {"ok"}, {"um"}, {"uh"}, {"hm"}, {"well"},
};

bool asciiWordEnd(char c) {
  return c == '\0' || c == ' ' || c == '\t' || c == ',' || c == '.' || c == '!' ||
         c == '?' || c == ';' || c == ':' || c == '(' || c == ')' || c == '\n';
}

// ASCII case-insensitive prefix compare for latin fillers.
bool latinPrefixEq(char const* s, size_t n, char const* t) {
  for (size_t i = 0; i < n; i++) {
    char a = s[i], b = t[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

bool utf8StartsWith(char const* s, size_t len, char const* t) {
  size_t tn = 0;
  while (t[tn]) tn++;
  return len >= tn && std::memcmp(s, t, tn) == 0;
}

// After skipping a filler token at line head, optionally eat one separator
// (comma / space / 。 / ! 等) so "好的,我们开始" -> "我们开始".
// Separators may be ASCII (1 byte) or CJK punctuation (3 bytes UTF-8).
bool isSepAt(char const* line, size_t len, size_t pos) {
  if (pos >= len) return false;
  unsigned char c = (unsigned char)line[pos];
  if (c == ' ' || c == '\t' || c == ',' || c == '.' || c == '!' || c == '?' ||
      c == ';' || c == ':' || c == '\n')
    return true;
  if (c == 0xEF && pos + 2 < len) {  // ，（EF BC 8C）！（EF BC 81）？（EF BC 9F）
    return true;
  }
  if (c == 0xE3 && pos + 2 < len) {  // 。（E3 80 82）
    return true;
  }
  return false;
}

size_t sepLenAt(char const* line, size_t len, size_t pos) {
  if (!isSepAt(line, len, pos)) return 0;
  unsigned char c = (unsigned char)line[pos];
  return (c >= 0xE0) ? 3 : 1;  // CJK separators are 3-byte UTF-8
}

// Strip one leading filler token + up to one separator from `line`.
// Returns the new start offset, or `start` if nothing was stripped.
size_t stripLeadingFiller(char const* line, size_t len, size_t start) {
  while (start < len && (line[start] == ' ' || line[start] == '\t')) start++;
  size_t best = start;
  bool stripped = false;
  for (auto const& f : kAsianFiller) {
    size_t wl = 0;
    while (f.word[wl]) wl += 3;  // CJK filler words are 3 bytes per char
    if (len - start >= wl && utf8StartsWith(line + start, len - start, f.word)) {
      size_t after = start + wl;
      // must be followed by a separator or end of line to count as filler
      if (after >= len || isSepAt(line, len, after)) {
        after += sepLenAt(line, len, after);
        if (after < len && (line[after] == ' ' || line[after] == '\t')) after++;
        best = after;
        stripped = true;
        break;
      }
    }
  }
  if (!stripped) {
    for (auto const& f : kLatinFiller) {
      size_t wl = 0;
      while (f.word[wl]) wl++;
      if (len - start >= wl && latinPrefixEq(line + start, wl, f.word)) {
        size_t after = start + wl;
        if (after >= len || asciiWordEnd(line[after])) {
          after += sepLenAt(line, len, after);  // eat trailing ","/"."/space
          if (after < len && (line[after] == ' ' || line[after] == '\t')) after++;
          best = after;
          stripped = true;
          break;
        }
      }
    }
  }
  return best;
}

// Whole line is nothing but filler (optionally with trailing separators)?
bool lineIsPureFiller(std::string const& line) {
  std::string trimmed = line;
  while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' ||
                              trimmed.back() == ',' || trimmed.back() == '!' ||
                              trimmed.back() == '?' || trimmed.back() == '.'))
    trimmed.pop_back();
  if (trimmed.empty()) return false;
  size_t s = 0;
  size_t t = stripLeadingFiller(trimmed.data(), trimmed.size(), s);
  if (t == 0) return false;
  // remaining must be empty or separators only
  for (size_t i = t; i < trimmed.size(); i++) {
    unsigned char c = (unsigned char)trimmed[i];
    if (!(c == ' ' || c == '\t' || c == '\n')) {
      // allow 的/了/吧 trailing particles? keep simple: non-sep => not pure
      return false;
    }
  }
  return true;
}

}  // namespace

std::string stripFillerWords(std::string const& text) {
  std::string out;
  out.reserve(text.size());
  bool inFence = false;
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    size_t eol = text.find('\n', i);
    if (eol == std::string::npos) eol = n;
    std::string line = text.substr(i, eol - i);
    // toggle fence on lines starting with ```
    if (line.rfind("```", 0) == 0) inFence = !inFence;
    if (!inFence) {
      size_t stripped = stripLeadingFiller(line.data(), line.size(), 0);
      if (stripped > 0) line = line.substr(stripped);
      if (lineIsPureFiller(line)) line.clear();
    }
    out += line;
    if (eol < n) out += '\n';
    i = eol + 1;
  }
  return out;
}

bool pruneBeforeSend(std::vector<Message>& messages, bool enabled) {
  if (!enabled) return false;
  bool changed = false;
  // 1. Drop assistant frames that carry neither text nor tool calls (empty
  //    acks some models emit when told to continue).
  for (size_t i = 0; i < messages.size();) {
    Message const& m = messages[i];
    if (m.role == "assistant" && m.content.empty() && m.toolCalls.empty()) {
      messages.erase(messages.begin() + (ptrdiff_t)i);
      changed = true;
      continue;
    }
    i++;
  }
  // 2. Collapse consecutive identical tool results (same id, same content):
  //    the model already has the first copy; later repeats add nothing.
  for (size_t i = 1; i < messages.size();) {
    Message const& cur = messages[i];
    if (cur.role == "tool" && i > 0) {
      Message const& prev = messages[i - 1];
      if (prev.role == "tool" && prev.toolCallId == cur.toolCallId &&
          prev.content == cur.content) {
        messages.erase(messages.begin() + (ptrdiff_t)i);
        changed = true;
        continue;
      }
    }
    i++;
  }
  // 3. Cap runaway payloads so a single huge result cannot blow the budget.
  for (auto& m : messages) {
    if (m.role == "user" && m.content.size() > 8192) {
      m.content.resize(8192);
      m.content += "\n...[truncated]";
    }
    if (m.role == "tool" && m.content.size() > 65536) {
      m.content.resize(65536);
      m.content += "\n...[truncated]";
    }
  }
  // 4. Strip conversational filler words ("好的", "嗯", "um", "well", ...)
  //    from assistant prose to shrink the request. Code fences are preserved
  //    verbatim; user messages and tool payloads are never touched.
  for (auto& m : messages) {
    if (m.role != "assistant" || m.content.empty() || !m.toolCalls.empty()) continue;
    std::string cleaned = stripFillerWords(m.content);
    if (cleaned != m.content) {
      m.content = std::move(cleaned);
      changed = true;
    }
  }
  return changed;
}

size_t estimateMessagesTokens(std::vector<Message> const& messages) {
  size_t t = 0;
  for (auto const& m : messages) {
    t += estimateTokens(m.role) + 2;
    t += estimateTokens(m.content);
    if (!m.toolCallId.empty()) t += estimateTokens(m.toolCallId) + 2;
    for (auto const& tc : m.toolCalls)
      t += estimateTokens(tc.name) + estimateTokens(tc.arguments) + 4;
  }
  return t;
}

std::string capToolResult(std::string result, size_t cap) {
  if (cap == 0 || result.size() <= cap) return result;
  // Structured results (JSON) are truncated per string field so the payload
  // stays parseable for downstream tools and the model's JSON checks.
  mini::Value v;
  if (mini::tryParse(result, v) && v.type == mini::Value::Object) {
    size_t target = cap / 2;  // leave room for escaping + framing overhead
    if (target < 64) target = 64;
    std::function<void(mini::Value&)> trunc = [&](mini::Value& node) {
      switch (node.type) {
        case mini::Value::String:
          if (node.s.size() > target) {
            node.s.resize(target);
            node.s += "\n...[truncated]";
          }
          break;
        case mini::Value::Array:
          for (auto& el : node.arr) trunc(el);
          break;
        case mini::Value::Object:
          for (auto& kv : node.obj) trunc(kv.second);
          break;
        default:
          break;
      }
    };
    trunc(v);
    std::string out = mini::dump(v);
    if (out.size() > cap) {  // oversized after all: hard tail cut
      out.resize(cap);
      out = out.substr(0, out.size() - 24);
      out += "\n...[truncated]";
    }
    return out;
  }
  // Plain-text fallback: tail truncation with a marker.
  result.resize(cap);
  result += "\n...[truncated >" + std::to_string(cap) + " bytes]";
  return result;
}

std::string toPlainText(std::string const& in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0, n = in.size();

  auto skipTo = [&](char end) {
    while (i < n && in[i] != end) {
      unsigned char c = (unsigned char)in[i];
      if ((c & 0x80) == 0) {
        if (c == '\n') break;
      }
      out += in[i];
      i++;
    }
  };

  bool inFence = false;
  std::string fenceLang;
  while (i < n) {
    unsigned char c = (unsigned char)in[i];

    // strip ANSI escapes
    if (c == 0x1B && i + 1 < n && in[i + 1] == '[') {
      i += 2;
      while (i < n && !(in[i] >= '@' && in[i] <= '~')) i++;
      i++;
      continue;
    }
    // fenced code block
    if (c == '`' && i + 2 < n && in[i + 1] == '`' && in[i + 2] == '`') {
      if (inFence) {
        out += "\n";
        inFence = false;
      } else {
        inFence = true;
        fenceLang.clear();
        i += 3;
        while (i < n && in[i] != '\n') { fenceLang += in[i]; i++; }
        out += "\n[code";
        if (!fenceLang.empty()) { out += ":"; out += fenceLang; }
        out += "]\n";
      }
      i++;
      continue;
    }
    if (inFence) {
      if (c == '\n') { out += '\n'; i++; continue; }
      out += in[i];
      i++;
      continue;
    }
    // heading
    if (c == '#' && (i == 0 || in[i - 1] == '\n')) {
      int run = 0;
      while (i < n && in[i] == '#') { run++; i++; }
      if (i < n && in[i] == ' ') i++;
      out += "[";
      out += std::string((size_t)run, '#');
      out += "] ";
      continue;
    }
    // emphasis markers: ** * __ _  -> keep text, drop marker
    if ((c == '*' || c == '_') && i + 1 < n && in[i + 1] == (char)c) {
      i += 2;  // bold
      continue;
    }
    if (c == '*' || c == '_') {
      i++;
      continue;
    }
    // inline code: `x` -> x
    if (c == '`') {
      i++;
      skipTo('`');
      if (i < n && in[i] == '`') i++;
      continue;
    }
    // link [text](url) -> text (url)
    if (c == '[') {
      i++;
      std::string label;
      while (i < n && in[i] != ']' && in[i] != '\n') { label += in[i]; i++; }
      if (i < n && in[i] == ']' && i + 1 < n && in[i + 1] == '(') {
        i += 2;
        std::string url;
        while (i < n && in[i] != ')' && in[i] != '\n') { url += in[i]; i++; }
        if (i < n && in[i] == ')') i++;
        out += label;
        if (!url.empty() && url != label) {
          out += " (";
          out += url;
          out += ")";
        }
        continue;
      }
      // not a link: emit label text we consumed, skip the closing bracket
      out += label;
      if (i < n && in[i] == ']') i++;
      continue;
    }
    // escaped char
    if (c == '\\' && i + 1 < n) {
      i++;
      out += in[i];
      i++;
      continue;
    }
    out += in[i];
    i++;
  }
  return out;
}

namespace {

// Find a safe compaction boundary in [start, messages.size()).
// Returns the largest index `end` such that no tool-call chain is split:
//   messages[end-1] must not be an assistant with pending tool_calls, and
//   messages[end]   must not be a tool response.
size_t safeBoundary(std::vector<Message> const& messages, size_t start) {
  size_t end = messages.size();
  while (end > start + 1) {
    bool ok = true;
    if (messages[end - 1].role == "assistant" && !messages[end - 1].toolCalls.empty()) ok = false;
    if (end < messages.size() && messages[end].role == "tool") ok = false;
    if (messages[end - 1].role == "tool") ok = false;  // would orphan a tool result
    if (ok) break;
    end--;
  }
  return end;
}

}  // namespace

bool compactConversation(Client& client, std::vector<Message>& messages, ChatOptions const& opts,
                         size_t budgetTokens, std::function<void(std::string const&)> onNote) {
  size_t total = estimateMessagesTokens(messages);
  if (budgetTokens == 0 || total <= budgetTokens) return false;

  size_t start = (messages.empty() || messages[0].role == "system") ? 1 : 0;
  size_t end = safeBoundary(messages, start);

  size_t excess = total - (budgetTokens * 3) / 4;  // leave headroom
  size_t acc = 0;
  size_t cut = start;
  size_t keepAtLeast = 6;
  size_t maxCut = messages.size() > keepAtLeast ? messages.size() - keepAtLeast : start + 1;
  while (cut < end && cut < maxCut) {
    acc += estimateTokens(messages[cut].content) + 4;
    cut++;
    if (acc >= excess) break;
  }
  if (cut <= start + 1) return false;
  // never split a tool chain at the final cut
  while (cut > start + 1 &&
         ((messages[cut - 1].role == "assistant" && !messages[cut - 1].toolCalls.empty()) ||
          (cut < messages.size() && messages[cut].role == "tool") ||
          messages[cut - 1].role == "tool")) {
    cut--;
  }
  if (cut <= start + 1) return false;

  // Build a transcript of the range to compress.
  std::string transcript;
  for (size_t k = start; k < cut; k++) {
    Message const& m = messages[k];
    if (m.role == "tool") {
      transcript += "tool(" + m.toolCallId + "): " + m.content + "\n";
    } else if (m.role == "assistant" && !m.toolCalls.empty()) {
      transcript += "assistant(tool_calls): " + m.content + "\n";
      for (auto const& tc : m.toolCalls) transcript += "  -> " + tc.name + " " + tc.arguments + "\n";
    } else {
      transcript += m.role + ": " + m.content + "\n";
    }
  }

  std::vector<Message> summaryMsgs;
  summaryMsgs.push_back({"system",
                         "You are a conversation summarizer. Condense the transcript below into a "
                         "single concise paragraph. Keep all concrete facts, decisions, commands "
                         "run, file paths, error messages and numeric results. The summary will "
                         "replace the original messages, so do not lose important details."});
  summaryMsgs.push_back({"user", transcript});

  ChatOptions sopts;
  sopts.model = opts.model;
  sopts.stream = false;
  sopts.maxTokens = 600;

  int rc = client.chat(summaryMsgs, sopts, nullptr);
  if (rc != 0 || summaryMsgs.empty() || summaryMsgs.back().content.empty()) return false;

  std::string summary = summaryMsgs.back().content;
  if (summary.size() > 4000) summary.resize(4000);

  Message note;
  note.role = "system";
  note.content = "[Summarized earlier conversation]\n" + summary;
  messages.erase(messages.begin() + (ptrdiff_t)start, messages.begin() + (ptrdiff_t)cut);
  messages.insert(messages.begin() + (ptrdiff_t)start, std::move(note));

  if (onNote) onNote("compressed " + std::to_string(cut - start) + " messages");
  return true;
}

int runTurnStructured(
    Client& client, std::vector<Message>& messages, ChatOptions const& opts,
    std::function<std::string(std::string const&, std::string const&)> executor,
    std::function<void(StreamEvent const&)> onEvent,
    std::function<bool(mini::Value const&)> validator, int maxRetries) {
  ChatOptions local = opts;
  local.jsonMode = true;

  for (int attempt = 0; attempt <= maxRetries; attempt++) {
    size_t before = messages.size();
    int rc = runTurn(client, messages, local, executor, onEvent);
    if (rc != 0) return rc;

    if (messages.size() > before) {
      Message const& last = messages.back();
      if (last.role == "assistant" && !last.toolCalls.empty()) continue;  // still executing tools
      mini::Value v;
      if (mini::tryParse(last.content, v) && (!validator || validator(v))) return 0;

      std::string why = "The response is not valid JSON.";
      messages.push_back({"user", why + " Respond again with a single valid JSON value, no prose."});
    }
  }
  StreamEvent ev;
  ev.kind = StreamKind::Error;
  ev.error = "structured output: retry limit reached";
  if (onEvent) onEvent(ev);
  return -3;
}

}  // namespace agent
