#include "tui/md.hpp"
#include "tui/utf8.hpp"
#include "wcwidth.hpp"
#include <algorithm>
#include <cstring>

namespace tui {

namespace {

int cwidth(uint32_t cp) {
  int w = (int)tui_ww::wcwidth(cp);
  return w < 0 ? 1 : w;
}

std::string latexToUnicode(std::string const& in);  // defined below (after parseInline)

// ---------------------------------------------------------------------------
// Inline parsing: **bold**, *italic*, `code`, [label](url).
// The whole logical line is `s[lineStart,lineEnd)`; we render into `out`.
// Unclosed markers are emitted verbatim (streaming-friendly: the next tick
// re-parses once the marker closes).
// ---------------------------------------------------------------------------
void parseInline(std::string const& s, size_t start, size_t end, MdTheme const& th, MdLine& out) {
  size_t i = start;
  auto push = [&](size_t b, size_t e, Style st) {
    if (b >= e) return;
    MdRun r;
    r.text.assign(s, b, e - b);
    r.st = st;
    out.push_back(std::move(r));
  };
  auto skip = [&](size_t at, char const* m) {
    size_t n = strlen(m);
    return (at + n <= end) && s.compare(at, n, m) == 0;
  };
  auto find = [&](size_t from, char const* m) -> size_t {
    size_t n = strlen(m);
    size_t p = s.find(m, from, n);
    if (p != std::string::npos && p + n <= end) return p;
    return std::string::npos;
  };

  while (i < end) {
    size_t p = i;
    while (p < end) {
      if (skip(p, "~~") || skip(p, "**") || skip(p, "`") || skip(p, "*") || skip(p, "[") ||
          skip(p, "![") || skip(p, "$")) break;
      size_t ccon = 0;
      utf8Decode(s.data() + p, end - p, ccon);
      if (ccon == 0) ccon = 1;
      p += ccon;
    }
    if (p > i) {
      push(i, p, th.text);
      i = p;
      continue;
    }
    if (skip(i, "$$")) {
      size_t c = find(i + 2, "$$");
      if (c != std::string::npos) {
        MdRun mr;
        mr.text = latexToUnicode(s.substr(i + 2, c - (i + 2)));
        mr.st = th.mathBlock;
        out.push_back(std::move(mr));
        i = c + 2;
        continue;
      }
    }
    if (skip(i, "$")) {
      // A $ pair is math only when it does not look like a currency amount
      // ("$5", "$2.50", "about $ and cents") - those stay literal.
      size_t c = find(i + 1, "$");
      if (c != std::string::npos) {
        size_t inner = c - (i + 1);
        bool priceLike = inner == 0 || (s[i + 1] >= '0' && s[i + 1] <= '9');
        if (!priceLike) {
          MdRun mr;
          mr.text = latexToUnicode(s.substr(i + 1, inner));
          mr.st = th.math;
          out.push_back(std::move(mr));
          i = c + 1;
          continue;
        }
      }
      // lone/currency '$': emit literally
      size_t ccon = 0;
      utf8Decode(s.data() + i, end - i, ccon);
      if (ccon == 0) ccon = 1;
      push(i, i + ccon, th.text);
      i += ccon;
      continue;
    }
    if (skip(i, "~~")) {
      size_t c = find(i + 2, "~~");
      if (c != std::string::npos) {
        push(i + 2, c, th.strike);
        i = c + 2;
        continue;
      }
    }
    if (skip(i, "***")) {
      size_t c = find(i + 3, "***");
      if (c != std::string::npos) {
        Style st = th.bold;
        st.attr(AttrItalic, true);
        push(i + 3, c, st);
        i = c + 3;
        continue;
      }
    }
    if (skip(i, "**")) {
      size_t c = find(i + 2, "**");
      if (c != std::string::npos) {
        push(i + 2, c, th.bold);
        i = c + 2;
        continue;
      }
    }
    if (skip(i, "`")) {
      size_t c = find(i + 1, "`");
      if (c != std::string::npos) {
        push(i + 1, c, th.code);
        i = c + 1;
        continue;
      }
    }
    if (skip(i, "*")) {
      size_t c = find(i + 1, "*");
      if (c != std::string::npos) {
        push(i + 1, c, th.italic);
        i = c + 1;
        continue;
      }
      // lone '*' mid-text: emit literally
      size_t ccon = 0;
      utf8Decode(s.data() + i, end - i, ccon);
      if (ccon == 0) ccon = 1;
      push(i, i + ccon, th.text);
      i += ccon;
      continue;
    }
    if (skip(i, "![")) {
      // Image: ![alt](url) -> "🖼 alt (url)" (alt styled as a link).
      size_t close = find(i + 2, "]");
      if (close != std::string::npos && close + 1 < end && s[close + 1] == '(') {
        size_t closeParen = find(close + 2, ")");
        if (closeParen != std::string::npos) {
          MdRun pre;
          pre.text = "\xF0\x9F\x96\xBC ";  // 🖼
          pre.st = th.link;
          out.push_back(std::move(pre));
          push(i + 2, close, th.link);
          push(close + 1, closeParen + 1, th.linkUrl);
          i = closeParen + 1;
          continue;
        }
      }
    }
    if (skip(i, "[")) {
      size_t close = find(i + 1, "]");
      if (close != std::string::npos && close + 1 < end && s[close + 1] == '(') {
        size_t closeParen = find(close + 2, ")");
        if (closeParen != std::string::npos) {
          push(i + 1, close, th.link);
          push(close + 1, closeParen + 1, th.linkUrl);
          i = closeParen + 1;
          continue;
        }
      }
    }
    // plain char
    size_t con = 0;
    utf8Decode(s.data() + i, end - i, con);
    if (con == 0) con = 1;
    i += con;
  }
}

// Wrap a single logical line (runs) into display lines of at most `width`
// columns, preserving run styles. Word-wrap at spaces, hard-break otherwise.
std::vector<MdLine> wrapRuns(MdLine const& runs, int width) {
  std::vector<MdLine> out;
  if (width <= 0) { out.push_back(runs); return out; }

  struct Cp {
    uint32_t cp;
    size_t run;
    size_t byte;
    size_t con;
  };
  std::vector<Cp> cps;
  for (size_t ri = 0; ri < runs.size(); ri++) {
    size_t bi = 0;
    while (bi < runs[ri].text.size()) {
      size_t con = 0;
      uint32_t cp = utf8Decode(runs[ri].text.data() + bi, runs[ri].text.size() - bi, con);
      if (con == 0) con = 1;
      cps.push_back({cp, ri, bi, con});
      bi += con;
    }
  }

  auto build = [&](size_t start, size_t end) -> MdLine {
    MdLine line;
    for (size_t k = start; k < end; k++) {
      Cp const& c = cps[k];
      MdRun const& src = runs[c.run];
      if (!line.empty() && line.back().st.v == src.st.v && k > start) {
        Cp const& prev = cps[k - 1];
        if (prev.run == c.run && prev.byte + prev.con == c.byte) {
          line.back().text.append(src.text, c.byte, c.con);
          continue;
        }
      }
      MdRun nr;
      nr.text.assign(src.text, c.byte, c.con);
      nr.st = src.st;
      line.push_back(std::move(nr));
    }
    return line;
  };

  size_t lineStart = 0;
  size_t lastSpace = SIZE_MAX;
  int col = 0;
  bool lineHasText = false;

  auto flush = [&](size_t end, bool trimTrailing) {
    size_t e = end;
    if (trimTrailing) {
      while (e > lineStart && cps[e - 1].cp == ' ') e--;
    }
    if (e > lineStart) out.push_back(build(lineStart, e));
    lineStart = end;
    lastSpace = SIZE_MAX;
    col = 0;
    lineHasText = false;
  };

  for (size_t k = 0; k < cps.size(); k++) {
    uint32_t cp = cps[k].cp;
    if (cp == ' ') lastSpace = k;
    int w = cwidth(cp);
    if (col + w > width && lineHasText) {
      if (lastSpace != SIZE_MAX && lastSpace >= lineStart) {
        size_t cut = lastSpace;
        flush(cut, false);
        lineStart = cut + 1;
        col = 0;
        for (size_t m = lineStart; m <= k; m++) col += cwidth(cps[m].cp);
        lineHasText = true;
        continue;
      } else {
        flush(k, false);
        lineStart = k;
        col = 0;
        lineHasText = false;
        k--;
        continue;
      }
    }
    col += w;
    lineHasText = true;
  }
  flush(cps.size(), true);
  return out;
}

// Blockquote prefix ("> " etc). Returns the byte offset after the last '>'
// or 0 if this line is not a quote.
size_t quoteOffset(std::string const& s) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i >= s.size() || s[i] != '>') return 0;
  while (i < s.size() && s[i] == '>') i++;
  if (i < s.size() && s[i] == ' ') i++;
  return i;
}

// List marker. Returns the byte offset after the marker and space, or 0.
size_t listOffset(std::string const& s, bool& numbered) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i >= s.size()) return 0;
  char c = s[i];
  if (c == '-' || c == '*' || c == '+') {
    if (i + 1 < s.size() && s[i + 1] == ' ') {
      numbered = false;
      return i + 2;
    }
    return 0;
  }
  size_t j = i;
  while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
  if (j > i && j < s.size() && (s[j] == '.' || s[j] == ')') && j + 1 < s.size() &&
      s[j + 1] == ' ') {
    numbered = true;
    return j + 2;
  }
  return 0;
}

// A plain-text line starting with '|' after trimming leading whitespace.
bool isTableRow(std::string const& s, std::string& trimmed) {
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i >= s.size() || s[i] != '|') return false;
  trimmed.assign(s, i, s.size() - i);
  return true;
}

// A header separator line like "| --- | :---: |". True if the trimmed line
// consists only of '|', '-', ':', spaces and tabs (and includes at least one
// '|'), which also shields real content lines and "---" hr lines from
// false positives.
bool isTableSep(std::string const& s) {
  bool seenPipe = false;
  for (char ch : s) {
    if (ch == '|') { seenPipe = true; continue; }
    if (ch == '-' || ch == ':' || ch == ' ' || ch == '\t') continue;
    return false;
  }
  return seenPipe;
}

// Split a table row line into its cells. Outer pipes/whitespace are trimmed,
// cell boundaries are '|', and interior spaces are preserved.
std::vector<std::string> splitCells(std::string const& trimmed) {
  std::vector<std::string> cells;
  size_t s = 0;
  size_t e = trimmed.size();
  while (s < e && (trimmed[s] == ' ' || trimmed[s] == '\t' || trimmed[s] == '|')) s++;
  while (e > s && (trimmed[e - 1] == ' ' || trimmed[e - 1] == '\t' || trimmed[e - 1] == '|')) e--;
  size_t i = s;
  while (i <= e) {
    size_t j = trimmed.find('|', i);
    if (j == std::string::npos || j > e) j = e;
    size_t a = i, b = j;
    while (a < b && (trimmed[a] == ' ' || trimmed[a] == '\t')) a++;
    while (b > a && (trimmed[b - 1] == ' ' || trimmed[b - 1] == '\t')) b--;
    cells.push_back(trimmed.substr(a, b - a));
    i = j + 1;
    if (j >= e) break;
  }
  return cells;
}

// Display width (in terminal columns) of a UTF-8 string.
int dispWidth(std::string const& s) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t c = 0;
    uint32_t cp = utf8Decode(s.data() + i, s.size() - i, c);
    if (c == 0) c = 1;
    w += cwidth(cp);
    i += c;
  }
  return w;
}

// ---------------------------------------------------------------------------
// LaTeX math -> Unicode approximation.
//
// Follows the approach used by innomd, mdterm and Rich's MathRenderer
// (Textualize/rich PR #3709): substitute common LaTeX commands/Greek letters
// with Unicode glyphs, convert ^{..}/{..} to Unicode super/subscripts and
// \frac/\sqrt to readable forms. Terminals cannot typeset like a LaTeX
// compiler; this makes physics/algebra/engineering notation read cleanly and
// degrades gracefully for exotic constructs.
// ---------------------------------------------------------------------------
char const* mathSup(char32_t c) {
  switch (c) {
    case '0': return "\xE2\x81\xB0";  // ⁰
    case '1': return "\xC2\xB9";      // ¹
    case '2': return "\xC2\xB2";      // ²
    case '3': return "\xC2\xB3";      // ³
    case '4': return "\xE2\x81\xB4";  // ⁴
    case '5': return "\xE2\x81\xB5";  // ⁵
    case '6': return "\xE2\x81\xB6";  // ⁶
    case '7': return "\xE2\x81\xB7";  // ⁷
    case '8': return "\xE2\x81\xB8";  // ⁸
    case '9': return "\xE2\x81\xB9";  // ⁹
    case '+': return "\xE2\x81\xBA";  // ⁺
    case '-': return "\xE2\x81\xBB";  // ⁻
    case '=': return "\xE2\x81\xBC";  // ⁼
    case '(': return "\xE2\x81\xBD";  // ⁽
    case ')': return "\xE2\x81\xBE";  // ⁾
    case 'n': return "\xE2\x81\xBF";  // ⁿ
    case 'a': return "\xE1\xB5\x83";  // ᵃ
    case 'b': return "\xE1\xB5\x87";  // ᵇ
    case 'c': return "\xE1\xB6\x9C";  // ᶜ
    case 'd': return "\xE1\xB5\x88";  // ᵈ
    case 'e': return "\xE1\xB5\x89";  // ᵉ
    case 'f': return "\xE1\xB6\xA0";  // ᶠ
    case 'g': return "\xE1\xB5\x8D";  // ᵍ
    case 'h': return "\xCA\xB0";      // ʰ
    case 'i': return "\xE2\x81\xB1";  // ⁱ
    case 'j': return "\xCA\xB2";      // ʲ
    case 'k': return "\xE1\xB5\x8F";  // ᵏ
    case 'l': return "\xCA\xA1";      // ˡ
    case 'm': return "\xE1\xB5\x90";  // ᵐ
    case 'o': return "\xE1\xB5\x92";  // ᵒ
    case 'p': return "\xE1\xB5\x96";  // ᵖ
    case 'r': return "\xCA\xB3";      // ʳ
    case 's': return "\xCA\xA2";      // ˢ
    case 't': return "\xE1\xB5\x97";  // ᵗ
    case 'u': return "\xE1\xB5\x98";  // ᵘ
    case 'v': return "\xE1\xB5\x9B";  // ᵛ
    case 'w': return "\xCA\xB7";      // ʷ
    case 'x': return "\xCA\xA3";      // ˣ
    case 'y': return "\xCA\xB8";      // ʸ
    case 'z': return "\xE1\xB6\xBB";  // ᶻ
    default: return nullptr;
  }
}

char const* mathSub(char32_t c) {
  switch (c) {
    case '0': return "\xE2\x82\x80";  // ₀
    case '1': return "\xE2\x82\x81";  // ₁
    case '2': return "\xE2\x82\x82";  // ₂
    case '3': return "\xE2\x82\x83";  // ₃
    case '4': return "\xE2\x82\x84";  // ₄
    case '5': return "\xE2\x82\x85";  // ₅
    case '6': return "\xE2\x82\x86";  // ₆
    case '7': return "\xE2\x82\x87";  // ₇
    case '8': return "\xE2\x82\x88";  // ₈
    case '9': return "\xE2\x82\x89";  // ₉
    case '+': return "\xE2\x82\x8A";  // ₊
    case '-': return "\xE2\x82\x8B";  // ₋
    case '=': return "\xE2\x82\x8C";  // ₌
    case '(': return "\xE2\x82\x8D";  // ₍
    case ')': return "\xE2\x82\x8E";  // ₎
    case 'a': return "\xE2\x82\x90";  // ₐ
    case 'e': return "\xE2\x82\x91";  // ₑ
    case 'h': return "\xE2\x82\x95";  // ₕ
    case 'i': return "\xE1\xB5\xA2";  // ᵢ
    case 'j': return "\xE2\xB1\xBC";  // ⱼ
    case 'k': return "\xE2\x82\x96";  // ₖ
    case 'l': return "\xE2\x82\x97";  // ₗ
    case 'm': return "\xE2\x82\x98";  // ₘ
    case 'n': return "\xE2\x82\x99";  // ₙ
    case 'o': return "\xE2\x82\x92";  // ₒ
    case 'p': return "\xE2\x82\x9A";  // ₚ
    case 'r': return "\xE1\xB5\xA3";  // ᵣ
    case 's': return "\xE2\x82\x9B";  // ₛ
    case 't': return "\xE2\x82\x9C";  // ₜ
    case 'u': return "\xE1\xB5\xA4";  // ᵤ
    case 'v': return "\xE1\xB5\xA5";  // ᵥ
    case 'x': return "\xE2\x82\x93";  // ₓ
    default: return nullptr;
  }
}

// Read an ASCII word token, returns the substring and advances `i`.
std::string mathWord(std::string const& s, size_t& i) {
  size_t j = i;
  while (j < s.size() && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) j++;
  std::string w(s, i, j - i);
  i = j;
  return w;
}

// Parse the brace group starting at s[pos]=='{'. Returns its content and
// advances `pos` past the closing brace.
std::string mathGroup(std::string const& s, size_t& pos, bool& ok) {
  ok = false;
  if (pos >= s.size() || s[pos] != '{') return "";
  size_t depth = 0;
  size_t start = pos + 1;
  for (size_t k = pos; k < s.size(); k++) {
    if (s[k] == '{') depth++;
    else if (s[k] == '}') {
      depth--;
      if (depth == 0) {
        std::string content(s, start, k - start);
        pos = k + 1;
        ok = true;
        return content;
      }
    }
  }
  return "";
}

struct MathSym { char const* cmd; char const* u; };
static MathSym const kMathSym[] = {
    {"alpha", "α"},      {"beta", "β"},        {"gamma", "γ"},      {"delta", "δ"},
    {"epsilon", "ϵ"},    {"varepsilon", "ε"},  {"zeta", "ζ"},       {"eta", "η"},
    {"theta", "θ"},      {"vartheta", "ϑ"},    {"iota", "ι"},       {"kappa", "κ"},
    {"lambda", "λ"},     {"mu", "μ"},          {"nu", "ν"},         {"xi", "ξ"},
    {"pi", "π"},         {"rho", "ρ"},         {"sigma", "σ"},      {"tau", "τ"},
    {"upsilon", "υ"},    {"phi", "φ"},         {"varphi", "ϕ"},     {"chi", "χ"},
    {"psi", "ψ"},        {"omega", "ω"},       {"Gamma", "Γ"},      {"Delta", "Δ"},
    {"Theta", "Θ"},      {"Lambda", "Λ"},      {"Xi", "Ξ"},         {"Pi", "Π"},
    {"Sigma", "Σ"},      {"Upsilon", "Υ"},     {"Phi", "Φ"},        {"Psi", "Ψ"},
    {"Omega", "Ω"},      {"cdot", "·"},        {"times", "×"},      {"div", "÷"},
    {"pm", "±"},         {"mp", "∓"},          {"leq", "≤"},        {"leqq", "≤"},
    {"le", "≤"},         {"ge", "≥"},          {"geq", "≥"},        {"geqq", "≥"},
    {"neq", "≠"},        {"approx", "≈"},      {"sim", "~"},        {"simeq", "≃"},
    {"equiv", "≡"},      {"propto", "∝"},      {"infty", "∞"},      {"sum", "∑"},
    {"int", "∫"},        {"prod", "∏"},        {"oint", "∮"},       {"nabla", "∇"},
    {"nabla", "∇"},      {"partial", "∂"},     {"forall", "∀"},     {"exists", "∃"},
    {"in", "∈"},         {"notin", "∉"},       {"subset", "⊂"},     {"supset", "⊃"},
    {"subseteq", "⊆"},   {"supseteq", "⊇"},    {"cup", "∪"},        {"cap", "∩"},
    {"land", "∧"},       {"lor", "∨"},         {"lnot", "¬"},       {"to", "→"},
    {"rightarrow", "→"}, {"leftarrow", "←"},   {"mapsto", "↦"},     {"gets", "←"},
    {"therefore", "∴"},  {"because", "∵"},     {"angle", "∠"},      {"parallel", "∥"},
    {"perp", "⊥"},       {"star", "⋆"},        {"bullet", "•"},     {"mid", "∣"},
    {"ldots", "…"},      {"cdots", "⋯"},       {"dots", "…"},       {"oplus", "⊕"},
    {"otimes", "⊗"},     {"circ", "∘"},     {"degree", "°"},     {"ell", "ℓ"},        {"hbar", "ℏ"},        {"Re", "ℜ"},         {"Im", "ℑ"},         {"langle", "⟨"},
    {"rangle", "⟩"},
    {"quad", "  "},      {"qquad", "    "},    {";", " "},          {",", " "},
    {":", " "},          {" ", ""},
    // function-name keywords rendered as plain roman text
    {"sin", "sin"},      {"cos", "cos"},      {"tan", "tan"},      {"cot", "cot"},
    {"sec", "sec"},      {"csc", "csc"},      {"arcsin", "arcsin"}, {"arccos", "arccos"},
    {"arctan", "arctan"}, {"sinh", "sinh"},   {"cosh", "cosh"},    {"tanh", "tanh"},
    {"log", "log"},      {"ln", "ln"},        {"lg", "lg"},        {"exp", "exp"},
    {"min", "min"},      {"max", "max"},      {"lim", "lim"},      {"sup", "sup"},
    {"inf", "inf"},      {"det", "det"},      {"dim", "dim"},      {"ker", "ker"},
    {"rank", "rank"},    {"span", "span"},    {"deg", "deg"},      {"sgn", "sgn"},
    {"arg", "arg"},
};
static int const kMathSymCount = (int)(sizeof(kMathSym) / sizeof(kMathSym[0]));

char const* mathSymLookup(std::string const& w) {
  for (int k = 0; k < kMathSymCount; k++)
    if (w == kMathSym[k].cmd) return kMathSym[k].u;
  return nullptr;
}

// map a char to blackboard bold (e.g. R -> ℝ)
char const* mathBlackboard(char32_t c) {
  switch (c) {
    case 'R': return "\xE2\x84\x9D";  // ℝ
    case 'N': return "\xE2\x84\x95";  // ℕ
    case 'Z': return "\xE2\x84\xA4";  // ℤ
    case 'Q': return "\xE2\x84\x9A";  // ℚ
    case 'C': return "\xE2\x84\x82";  // ℂ
    case 'P': return "\xE2\x84\x99";  // ℙ
    case 'H': return "\xE2\x84\x8D";  // ℍ
    default: return c >= 'A' && c <= 'Z' ? nullptr : nullptr;
  }
}

std::string latexToUnicode(std::string const& in) {
  std::string out;
  size_t i = 0;
  auto appendSupOrSub = [&](std::string const& content, bool sup, bool& any) {
    std::string conv = latexToUnicode(content);  // resolve \infty etc. first
    std::string mapped;
    bool all = conv.size() > 0;
    for (size_t k = 0; k < conv.size();) {
      size_t c = 0;
      uint32_t cp = utf8Decode(conv.data() + k, conv.size() - k, c);
      if (c == 0) c = 1;
      char const* m = sup ? mathSup(cp) : mathSub(cp);
      if (!m) { all = false; break; }
      mapped += m;
      k += c;
    }
    if (all) {
      out += mapped;
      any = true;
    } else {
      out += (sup ? "^" : "_");
      out += conv;
    }
  };
  while (i < in.size()) {
    if (in[i] == '\\') {
      i++;
      if (i >= in.size()) break;
      if (in[i] == ' ' || in[i] == '~') { out += ' '; i++; continue; }
      if (in[i] == '{' || in[i] == '}') { i++; continue; }
      // escaped punctuation: \% \$ \& \# \_ \{ \}
      if (in[i] == '%' || in[i] == '$' || in[i] == '&' || in[i] == '#' ||
          in[i] == '_' || in[i] == '{' || in[i] == '}') { out += in[i]; i++; continue; }
      if (in[i] == '|') { out += "\xE2\x80\x96"; i++; continue; }  // \| -> ‖
      std::string w = mathWord(in, i);
      if (w == "pmod") {
        bool ok = false;
        std::string inner = mathGroup(in, i, ok);
        out += "(mod ";
        out += latexToUnicode(inner);
        out += ")";
        continue;
      }
      if (w == "mod" || w == "bmod") { out += " mod "; continue; }
      if (w == "frac") {
        bool ok = false, ok2 = false;
        std::string num = mathGroup(in, i, ok);
        std::string den = mathGroup(in, i, ok2);
        out += ok ? latexToUnicode(num) : ("{" + num + "}");
        out += "/";
        out += ok2 ? latexToUnicode(den) : ("{" + den + "}");
        continue;
      }
      if (w == "sqrt" || w == "text") {
        if (w == "sqrt" && i < in.size() && in[i] == '[') {
          // sqrt[n]{x}
          size_t e = in.find(']', i);
          if (e == std::string::npos) { out += "√"; i++; continue; }
          std::string n(in, i + 1, e - i - 1);
          i = e + 1;
          bool ok = false;
          std::string inner = mathGroup(in, i, ok);
          out += latexToUnicode(n) + "√(";
          out += latexToUnicode(inner);
          out += ")";
          continue;
        }
        bool ok = false;
        std::string inner = mathGroup(in, i, ok);
        if (w == "sqrt") {
          out += "√(";
          out += latexToUnicode(inner);
          out += ")";
        } else {
          out += latexToUnicode(inner);  // \text{...}
        }
        continue;
      }
      if (w == "mathbb") {
        bool ok = false;
        std::string inner = mathGroup(in, i, ok);
        for (size_t k = 0; k < inner.size();) {
          size_t c = 0;
          uint32_t cp = utf8Decode(inner.data() + k, inner.size() - k, c);
          if (c == 0) c = 1;
          char const* m = mathBlackboard(cp);
          out += m ? m : inner.substr(k, c);
          k += c;
        }
        continue;
      }
      if (w == "mathbf" || w == "boldsymbol") {
        bool ok = false;
        std::string inner = mathGroup(in, i, ok);
        for (size_t k = 0; k < inner.size();) {
          size_t c = 0;
          uint32_t cp = utf8Decode(inner.data() + k, inner.size() - k, c);
          if (c == 0) c = 1;
          if (cp >= 'A' && cp <= 'Z') {
            uint32_t bold = 0x1D400 + (cp - 'A');
            char b[4] = {char(0xF0 | (bold >> 18)), char(0x80 | ((bold >> 12) & 0x3F)),
                         char(0x80 | ((bold >> 6) & 0x3F)), char(0x80 | (bold & 0x3F))};
            out.append(b, 4);
          } else if (cp >= 'a' && cp <= 'z') {
            uint32_t bold = 0x1D41A + (cp - 'a');
            char b[4] = {char(0xF0 | (bold >> 18)), char(0x80 | ((bold >> 12) & 0x3F)),
                         char(0x80 | ((bold >> 6) & 0x3F)), char(0x80 | (bold & 0x3F))};
            out.append(b, 4);
          } else {
            out.append(inner, k, c);
          }
          k += c;
        }
        continue;
      }
      if (w == "tilde" || w == "hat" || w == "bar" || w == "dot" || w == "vec") {
        char const* acc = w == "tilde" ? "\xCC\x83" : w == "hat" ? "\xCC\x82"
                          : w == "bar" ? "\xCC\x84"
                          : w == "dot" ? "\xCC\x87"
                                       : "\xE2\x83\x97";  // vec: combining arrow
        bool ok = false;
        std::string inner = mathGroup(in, i, ok);
        if (!ok) {
          // accent a single following non-space char, e.g. \tilde U
          size_t save = i;
          while (i < in.size() && (in[i] == ' ' || in[i] == '\t')) i++;
          if (i < in.size() && in[i] != '\\' && in[i] != '{' && in[i] != '}') {
            size_t c = 0;
            utf8Decode(in.data() + i, in.size() - i, c);
            if (c == 0) c = 1;
            inner.assign(in, i, c);
            i += c;
          } else {
            i = save;
          }
        }
        std::string conv = latexToUnicode(inner);
        if (!conv.empty()) {
          size_t c = 0;
          utf8Decode(conv.data(), conv.size(), c);
          if (c == 0) c = 1;
          out.append(conv, 0, c);
          out += acc;
          out.append(conv, c, conv.size() - c);
        }
        continue;
      }
      if (w == "left" || w == "right" || w == "big" || w == "Big" || w == "bigg" ||
          w == "Bigg" || w == "mid") {
        // drop auto-sizing fences, keep the following char (e.g. ( ))
        continue;
      }
      char const* u = mathSymLookup(w);
      if (u) out += u;
      else { out += "\\"; out += w; }
      continue;
    }
    if (in[i] == '{') { i++; continue; }
    if (in[i] == '}') { i++; continue; }
    if (in[i] == '^' || in[i] == '_') {
      bool sup = in[i] == '^';
      i++;
      bool ok = false;
      std::string content;
      if (i < in.size() && in[i] == '{') content = mathGroup(in, i, ok);
      if (!ok) {
        // capture a single token: either one char or a \command
        if (i < in.size() && in[i] == '\\') {
          size_t t = i + 1;
          while (t < in.size() &&
                 ((in[t] >= 'a' && in[t] <= 'z') || (in[t] >= 'A' && in[t] <= 'Z')))
            t++;
          if (t > i + 1) {
            content.assign(in, i, t - i);
            i = t;
          } else {
            content = "\\";
            i++;
          }
        } else if (i < in.size()) {
          size_t c = 0;
          utf8Decode(in.data() + i, in.size() - i, c);
          if (c == 0) c = 1;
          content.assign(in, i, c);
          i += c;
        }
      }
      bool any = false;
      appendSupOrSub(content, sup, any);
      continue;
    }
    if (in[i] == '~') { out += ' '; i++; continue; }
    size_t c = 0;
    utf8Decode(in.data() + i, in.size() - i, c);
    if (c == 0) c = 1;
    out.append(in, i, c);
    i += c;
  }
  return out;
}

void pushLine(std::vector<MdStyledLine>& out, MdLine runs, Style base) {
  MdStyledLine sl;
  sl.runs = std::move(runs);
  sl.base = base;
  out.push_back(std::move(sl));
}

// Append `n` copies of a UTF-8 encoded character (e.g. "─") to `s`.
void appendChar(std::string& s, char const* utf8, size_t n) {
  size_t len = std::strlen(utf8);
  for (size_t k = 0; k < n; k++) s.append(utf8, len);
}

// Render a table whose header is given as a trimmed line plus its data rows.
// Layout is a clean borderless table: bold underlined header, one thin rule
// under it, and left-aligned columns padded to the widest cell. This avoids
// the box-drawing misalignment that vertical │ separators suffer from with
// mixed-width (CJK) cells.
void renderTable(std::string const& header, std::vector<std::string> const& rows,
                 int width, MdTheme const& theme, std::vector<MdStyledLine>& out) {
  size_t const MAXCOLS = 12;
  size_t const MAXROWS = 100;
  std::vector<std::vector<std::string>> cells;
  cells.push_back(splitCells(header));
  size_t ncols = cells[0].size();
  if (ncols == 0) return;
  int nrows = 0;
  for (auto const& line : rows) {
    if (++nrows > (int)MAXROWS) break;
    auto cs = splitCells(line);
    if (cs.size() > MAXCOLS) cs.resize(MAXCOLS);
    if (cs.size() < ncols) cs.resize(ncols);
    cells.push_back(std::move(cs));
  }
  ncols = std::min(ncols, MAXCOLS);
  for (auto& cs : cells) if (cs.size() > ncols) cs.resize(ncols);

  // Column widths (display columns), capped so a wide column cannot eat the
  // whole line.
  std::vector<int> widths(ncols, 0);
  for (auto const& cs : cells) {
    for (size_t c = 0; c < ncols; c++) {
      int w = dispWidth(cs[c]);
      if (w > widths[c]) widths[c] = std::min(w, std::max(4, width / (int)ncols));
    }
  }
  int total = (int)(ncols - 1);  // one │ separator between columns
  for (int w : widths) total += w;
  bool aligned = total <= width;

  auto renderRow = [&](std::vector<std::string> const& cs, Style cellSt, bool isHead) {
    MdLine runs;
    for (size_t c = 0; c < ncols; c++) {
      if (c > 0) {
        MdRun sp;
        sp.text = "\xE2\x94\x82";  // │
        sp.st = theme.hr;
        runs.push_back(std::move(sp));
      }
      // Cells keep inline markdown (**bold**, `code`, [link](url)).
      size_t before = runs.size();
      parseInline(cs[c], 0, cs[c].size(), theme, runs);
      for (size_t r = before; r < runs.size(); r++) {
        if (runs[r].st.v == 0) runs[r].st = cellSt;  // plain text inherits cell
        if (isHead) runs[r].st.attr(AttrBold, true);  // header emphasizes all
      }
      if (aligned) {
        int pad = widths[c] - dispWidth(cs[c]);
        if (pad > 0) {
          MdRun p;
          p.text.assign((size_t)pad, ' ');
          p.st = cellSt;
          runs.push_back(std::move(p));
        }
      }
    }
    pushLine(out, std::move(runs), cellSt);
  };

  renderRow(cells[0], theme.tableHead, true);
  // thin rule under the header spanning the full table width
  {
    MdLine runs;
    MdRun r;
    r.text.assign((size_t)std::max(total, 1), '-');
    r.st = theme.hr;
    runs.push_back(std::move(r));
    pushLine(out, std::move(runs), theme.hr);
  }
  for (size_t i = 1; i < cells.size(); i++) renderRow(cells[i], theme.table, false);
}

// ---------------------------------------------------------------------------
// Lightweight syntax highlighting for fenced code blocks.
//
// This is a deliberately small, streaming-friendly tokenizer: it classifies
// each source line into style runs (keywords, strings, comments, numbers,
// types, function calls, preprocessor) instead of a full parser. State that
// must span lines (block comments, multi-line strings) is carried in
// CodeTokState across lines of a fence. Unknown tokens fall back to the plain
// code-block style.
// ---------------------------------------------------------------------------

struct CodeLang {
  bool cppDirective;   // C-family '#...' preprocessor lines
  bool hashComment;    // '#' line comments (python, shell, ...)
  bool dashComment;    // '--' line comments (sql)
  bool backtick;       // '`...`' strings (js template, go, shell)
  bool tripleDq;       // Python """..."""
  bool tripleSq;       // Python '''...'''
  bool ci;             // case-insensitive keyword matching (SQL)
  char const* const* kw;
  int kwN;
  char const* const* ty;
  int tyN;

  bool isKeyword(std::string const& w) const {
    std::string t = w;
    if (ci)
      for (char& c : t)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (int i = 0; i < kwN; i++)
      if (t == kw[i]) return true;
    return false;
  }
  bool isType(std::string const& w) const {
    std::string t = w;
    if (ci)
      for (char& c : t)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (int i = 0; i < tyN; i++)
      if (t == ty[i]) return true;
    return false;
  }
};

static char const* const kCppKw[] = {
    "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool",
    "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
    "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "not", "nullptr", "operator", "or", "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return", "short", "signed",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
    "template", "this", "thread_local", "throw", "true", "try", "typedef",
    "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "xor",
};
static char const* const kCppTy[] = {
    "string", "vector", "map", "set", "unordered_map", "unordered_set",
    "list", "deque", "queue", "stack", "shared_ptr", "unique_ptr", "weak_ptr",
    "function", "optional", "variant", "pair", "tuple", "size_t", "ssize_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "std", "cout", "cin", "cerr",
};
static char const* const kJavaCsKw[] = {
    "abstract", "as", "base", "boolean", "break", "byte", "case", "catch",
    "char", "checked", "class", "const", "continue", "decimal", "default",
    "delegate", "do", "double", "else", "enum", "event", "explicit", "extern",
    "false", "finally", "fixed", "float", "for", "foreach", "goto", "if",
    "implements", "implicit", "import", "in", "instanceof", "int", "interface",
    "internal", "is", "lock", "long", "namespace", "new", "null", "object",
    "operator", "out", "override", "package", "params", "private", "protected",
    "public", "readonly", "ref", "return", "sbyte", "sealed", "short",
    "sizeof", "stackalloc", "static", "string", "struct", "super", "switch",
    "synchronized", "this", "throw", "throws", "transient", "true", "try",
    "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using",
    "virtual", "void", "volatile", "while",
};
static char const* const kJavaCsTy[] = {
    "String", "Integer", "Boolean", "Long", "Double", "Float", "Object",
    "List", "Map", "Set", "ArrayList", "HashMap", "HashSet", "LinkedList",
    "System", "Math", "Arrays", "Collectors", "stream", "Stream",
};
static char const* const kJsKw[] = {
    "abstract", "arguments", "async", "await", "break", "case", "catch",
    "class", "const", "continue", "debugger", "default", "delete", "do",
    "else", "enum", "eval", "export", "extends", "false", "finally", "for",
    "function", "get", "if", "implements", "import", "in", "instanceof",
    "interface", "let", "new", "null", "of", "package", "private", "protected",
    "public", "return", "set", "static", "super", "switch", "this", "throw",
    "true", "try", "typeof", "undefined", "var", "void", "while", "with",
    "yield", "require", "module", "exports", "process",
};
static char const* const kJsTy[] = {
    "string", "number", "boolean", "object", "symbol", "bigint", "any",
    "never", "unknown", "void", "Array", "Promise", "Object", "String",
    "Number", "Function", "console", "JSON", "Math", "document", "window",
    "global", "Map", "Set", "Buffer",
};
static char const* const kPyKw[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await", "break",
    "class", "continue", "def", "del", "elif", "else", "except", "finally",
    "for", "from", "global", "if", "import", "in", "is", "lambda", "match",
    "case", "nonlocal", "not", "or", "pass", "raise", "return", "try",
    "while", "with", "yield", "self",
};
static char const* const kPyTy[] = {
    "int", "float", "str", "bool", "bytes", "bytearray", "list", "dict",
    "set", "frozenset", "tuple", "complex", "range", "object", "type",
    "print", "len", "enumerate", "zip", "map", "filter", "sorted", "sum",
    "min", "max", "abs", "open", "input", "isinstance", "super",
};
static char const* const kRsKw[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn",
    "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let",
    "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self",
    "Self", "static", "struct", "super", "trait", "true", "type", "unsafe",
    "use", "where", "while",
};
static char const* const kRsTy[] = {
    "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64",
    "u128", "usize", "f32", "f64", "bool", "char", "str", "String", "Vec",
    "Option", "Result", "Box", "HashMap", "BTreeMap", "HashSet", "BTreeSet",
    "Rc", "Arc", "std",
};
static char const* const kGoKw[] = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else",
    "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
    "map", "package", "range", "return", "select", "struct", "switch", "type",
    "var",
};
static char const* const kGoTy[] = {
    "bool", "byte", "complex64", "complex128", "error", "float32", "float64",
    "int", "int8", "int16", "int32", "int64", "rune", "string", "uint",
    "uint8", "uint16", "uint32", "uint64", "uintptr", "nil", "true", "false",
    "make", "len", "cap", "new", "append", "copy", "delete", "print",
    "println", "panic", "recover",
};
static char const* const kShKw[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
    "case", "esac", "function", "in", "select", "time", "coproc", "break",
    "continue", "return", "exit", "export", "local", "readonly", "declare",
    "typeset", "set", "unset", "shift", "source", "alias", "unalias", "echo",
    "printf", "read", "cd", "pwd", "ls", "mkdir", "rm", "cp", "mv", "touch",
    "cat", "grep", "sed", "awk", "find", "xargs", "test", "true", "false",
    "eval", "exec", "trap", "wait", "jobs", "fg", "bg", "kill", "pushd",
    "popd",
};
static char const* const kSqlKw[] = {
    "select", "from", "where", "insert", "into", "values", "update", "delete",
    "create", "table", "drop", "alter", "add", "column", "index", "view",
    "join", "inner", "left", "right", "outer", "full", "on", "group", "by",
    "order", "having", "limit", "offset", "distinct", "as", "and", "or",
    "not", "null", "is", "in", "between", "like", "exists", "union", "all",
    "primary", "key", "foreign", "references", "constraint", "default",
    "unique", "check", "collate", "asc", "desc", "case", "when", "then",
    "else", "end", "begin", "commit", "rollback", "transaction", "savepoint",
    "grant", "revoke", "use", "show", "describe", "explain", "analyze",
    "replace",
};
static char const* const kSqlTy[] = {
    "int", "integer", "text", "varchar", "char", "boolean", "bool", "date",
    "datetime", "timestamp", "numeric", "decimal", "real", "double", "float",
    "bigint", "serial", "json", "uuid", "blob",
};

static int const kCppKwN = (int)(sizeof(kCppKw) / sizeof(kCppKw[0]));
static int const kCppTyN = (int)(sizeof(kCppTy) / sizeof(kCppTy[0]));
static int const kJavaCsKwN = (int)(sizeof(kJavaCsKw) / sizeof(kJavaCsKw[0]));
static int const kJavaCsTyN = (int)(sizeof(kJavaCsTy) / sizeof(kJavaCsTy[0]));
static int const kJsKwN = (int)(sizeof(kJsKw) / sizeof(kJsKw[0]));
static int const kJsTyN = (int)(sizeof(kJsTy) / sizeof(kJsTy[0]));
static int const kPyKwN = (int)(sizeof(kPyKw) / sizeof(kPyKw[0]));
static int const kPyTyN = (int)(sizeof(kPyTy) / sizeof(kPyTy[0]));
static int const kRsKwN = (int)(sizeof(kRsKw) / sizeof(kRsKw[0]));
static int const kRsTyN = (int)(sizeof(kRsTy) / sizeof(kRsTy[0]));
static int const kGoKwN = (int)(sizeof(kGoKw) / sizeof(kGoKw[0]));
static int const kGoTyN = (int)(sizeof(kGoTy) / sizeof(kGoTy[0]));
static int const kShKwN = (int)(sizeof(kShKw) / sizeof(kShKw[0]));
static int const kSqlKwN = (int)(sizeof(kSqlKw) / sizeof(kSqlKw[0]));
static int const kSqlTyN = (int)(sizeof(kSqlTy) / sizeof(kSqlTy[0]));

static CodeLang const kLangC = {true, false, false, false, false, false, false,
                                kCppKw, kCppKwN, kCppTy, kCppTyN};
static CodeLang const kLangJavaCs = {false, false, false, false, false, false, false,
                                     kJavaCsKw, kJavaCsKwN, kJavaCsTy, kJavaCsTyN};
static CodeLang const kLangJs = {false, false, false, true, false, false, false,
                                 kJsKw, kJsKwN, kJsTy, kJsTyN};
static CodeLang const kLangPy = {false, true, false, false, true, true, false,
                                 kPyKw, kPyKwN, kPyTy, kPyTyN};
static CodeLang const kLangRs = {false, false, false, false, false, false, false,
                                 kRsKw, kRsKwN, kRsTy, kRsTyN};
static CodeLang const kLangGo = {false, false, false, true, false, false, false,
                                 kGoKw, kGoKwN, kGoTy, kGoTyN};
static CodeLang const kLangSh = {false, true, false, true, false, false, false,
                                 kShKw, kShKwN, nullptr, 0};
static CodeLang const kLangSql = {false, false, true, false, false, false, true,
                                  kSqlKw, kSqlKwN, kSqlTy, kSqlTyN};
static CodeLang const kLangGeneric = {false, true, false, false, false, false, false,
                                      nullptr, 0, nullptr, 0};

CodeLang const* codeLangOf(std::string const& label) {
  std::string l;
  for (char c : label) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == ' ' || c == '\t' || c == '\r') continue;
    l += c;
  }
  if (l == "c" || l == "cc" || l == "c++" || l == "cpp" || l == "cxx" ||
      l == "h" || l == "hpp" || l == "hxx")
    return &kLangC;
  if (l == "java" || l == "cs" || l == "csharp") return &kLangJavaCs;
  if (l == "js" || l == "javascript" || l == "mjs" || l == "cjs" || l == "jsx" ||
      l == "ts" || l == "typescript" || l == "tsx")
    return &kLangJs;
  if (l == "py" || l == "python") return &kLangPy;
  if (l == "rs" || l == "rust") return &kLangRs;
  if (l == "go" || l == "golang") return &kLangGo;
  if (l == "sh" || l == "bash" || l == "zsh" || l == "ksh" || l == "shell" ||
      l == "ps1" || l == "powershell")
    return &kLangSh;
  if (l == "sql") return &kLangSql;
  return &kLangGeneric;
}

// State that persists across the lines of one fenced block.
struct CodeTokState {
  bool blockComment = false;  // inside /* ... */ continuing from a prior line
  bool inString = false;      // inside a multi-line string
  char strCh = 0;
  bool triple = false;
};

// Tokenize one source line into styled runs. `st` may carry state from the
// previous line of the same block; `out` accumulates runs (gutter added by
// the caller). Empty lines and lines without tokens are fine (produce no runs).
void highlightCodeLine(std::string const& line, CodeLang const& lang,
                       MdTheme const& th, CodeTokState& st, MdLine& out) {
  auto push = [&](size_t b, size_t e, Style style) {
    if (b >= e) return;
    if (!out.empty() && out.back().st.v == style.v) {
      out.back().text.append(line, b, e - b);
      return;
    }
    MdRun r;
    r.text.assign(line, b, e - b);
    r.st = style;
    out.push_back(std::move(r));
  };
  auto isAlpha = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
  auto isHex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  auto isAlnum = [&](char c) { return isAlpha(c) || isDigit(c); };

  size_t n = line.size();
  size_t i = 0;

  // Continue a block comment opened on a previous line.
  if (st.blockComment) {
    size_t c = line.find("*/", 0);
    if (c == std::string::npos) { push(0, n, th.codeComment); return; }
    push(0, c + 2, th.codeComment);
    st.blockComment = false;
    i = c + 2;
  } else if (st.inString) {
    char q = st.strCh;
    size_t k = 0;
    bool closed = false;
    while (k < n) {
      if (line[k] == '\\' && k + 1 < n) { k += 2; continue; }
      if (line[k] == q) {
        if (st.triple) {
          if (k + 2 < n && line[k + 1] == q && line[k + 2] == q) {
            closed = true;
            k += 3;
            break;
          }
        } else {
          closed = true;
          k += 1;
          break;
        }
      }
      k++;
    }
    if (!closed) { push(0, n, th.codeString); return; }
    push(0, k, th.codeString);
    st.inString = false;
    i = k;
  }

  while (i < n) {
    char c = line[i];
    // C-family preprocessor directive: '#' at the start of a line.
    if (lang.cppDirective && c == '#') {
      size_t ws = 0;
      while (ws < n && (line[ws] == ' ' || line[ws] == '\t')) ws++;
      if (i == ws) { push(i, n, th.codePreproc); return; }
    }
    // Line comments.
    if (c == '/' && i + 1 < n && line[i + 1] == '/') {
      push(i, n, th.codeComment);
      return;
    }
    if (lang.hashComment && c == '#') { push(i, n, th.codeComment); return; }
    if (lang.dashComment && c == '-' && i + 1 < n && line[i + 1] == '-') {
      push(i, n, th.codeComment);
      return;
    }
    // Block comment start.
    if (c == '/' && i + 1 < n && line[i + 1] == '*') {
      size_t e = line.find("*/", i + 2);
      if (e == std::string::npos) {
        push(i, n, th.codeComment);
        st.blockComment = true;
        return;
      }
      push(i, e + 2, th.codeComment);
      i = e + 2;
      continue;
    }
    // String literals.
    if (c == '"' || c == '\'' || (lang.backtick && c == '`')) {
      char q = c;
      bool triple = false;
      if ((lang.tripleDq && q == '"') || (lang.tripleSq && q == '\'')) {
        if (i + 2 < n && line[i + 1] == q && line[i + 2] == q) triple = true;
      }
      if (triple) {
        std::string term(3, q);
        size_t e = line.find(term, i + 3);
        if (e == std::string::npos) {
          push(i, n, th.codeString);
          st.inString = true;
          st.strCh = q;
          st.triple = true;
          return;
        }
        push(i, e + 3, th.codeString);
        i = e + 3;
        continue;
      }
      size_t k = i + 1;
      bool closed = false;
      while (k < n) {
        if (line[k] == '\\' && k + 1 < n) { k += 2; continue; }
        if (line[k] == q) { closed = true; k += 1; break; }
        k++;
      }
      if (!closed) {
        push(i, n, th.codeString);
        st.inString = true;
        st.strCh = q;
        st.triple = false;
        return;
      }
      push(i, k, th.codeString);
      i = k;
      continue;
    }
    // Numbers.
    if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(line[i + 1]))) {
      size_t k = i;
      if (c == '0' && i + 1 < n && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
        k += 2;
        while (k < n && isHex(line[k])) k++;
      } else if (c == '0' && i + 1 < n && (line[i + 1] == 'b' || line[i + 1] == 'B')) {
        k += 2;
        while (k < n && (line[k] == '0' || line[k] == '1')) k++;
      } else {
        while (k < n && (isDigit(line[k]) || line[k] == '_' || line[k] == '.')) k++;
        if (k < n && (line[k] == 'e' || line[k] == 'E')) {
          size_t j = k + 1;
          if (j < n && (line[j] == '+' || line[j] == '-')) j++;
          if (j < n && isDigit(line[j])) {
            k = j;
            while (k < n && isDigit(line[k])) k++;
          }
        }
      }
      while (k < n && (line[k] == 'u' || line[k] == 'U' || line[k] == 'l' ||
                       line[k] == 'L' || line[k] == 'f' || line[k] == 'F'))
        k++;
      push(i, k, th.codeNumber);
      i = k;
      continue;
    }
    // Identifiers.
    if (isAlpha(c)) {
      size_t k = i + 1;
      while (k < n && isAlnum(line[k])) k++;
      std::string w = line.substr(i, k - i);
      size_t j = k;
      while (j < n && line[j] == ' ') j++;
      bool isFunc = j < n && line[j] == '(';
      if (lang.isKeyword(w)) push(i, k, th.codeKeyword);
      else if (lang.isType(w)) push(i, k, th.codeType);
      else if (isFunc) push(i, k, th.codeFunc);
      else push(i, k, th.codeBlock);
      i = k;
      continue;
    }
    // Operators / punctuation / whitespace / any other character.
    size_t ccon = 0;
    utf8Decode(line.data() + i, n - i, ccon);
    if (ccon == 0) ccon = 1;
    push(i, i + ccon, th.codeBlock);
    i += ccon;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// renderMarkdown: block-level + inline -> styled, wrapped lines.
// ---------------------------------------------------------------------------
std::vector<MdStyledLine> renderMarkdown(std::string const& text, int width,
                                         MdTheme const& theme) {
  std::vector<MdStyledLine> out;
  if (width <= 0) width = 80;

  // split into logical lines
  std::vector<std::pair<size_t, size_t>> lines;  // [start,end)
  size_t pos = 0;
  size_t n = text.size();
  while (pos <= n) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) {
      lines.emplace_back(pos, n);
      break;
    }
    lines.emplace_back(pos, nl);
    pos = nl + 1;
  }

  bool inFence = false;
  bool inMath = false;
  char fenceOpener = 0;
  bool lastBlank = false;
  size_t fenceStart = 0;  // out index of the most recent fence-start line
  CodeLang const* curLang = &kLangGeneric;
  CodeTokState tok;  // comment/string state across the lines of one fence
  for (size_t li = 0; li < lines.size(); li++) {
    auto [ls, le] = lines[li];
    std::string line(text, ls, le - ls);

    // standalone "$$" line toggles a math block
    size_t ltrim = 0;
    while (ltrim < line.size() && (line[ltrim] == ' ' || line[ltrim] == '\t')) ltrim++;
    if (inMath) {
      MdStyledLine sl;
      MdRun r;
      if (line.substr(ltrim) == "$$") inMath = false;
      r.text = latexToUnicode(line);
      r.st = theme.mathBlock;
      sl.runs.push_back(std::move(r));
      sl.base = theme.mathBlock;
      out.push_back(std::move(sl));
      continue;
    }
    if (line.substr(ltrim) == "$$") {
      inMath = true;
      MdStyledLine sl;
      MdRun r;
      r.text = line;
      r.st = theme.mathBlock;
      sl.runs.push_back(std::move(r));
      sl.base = theme.mathBlock;
      out.push_back(std::move(sl));
      continue;
    }

    // ----- fenced code blocks -----------------------------------------
    size_t firstNs = 0;
    while (firstNs < line.size() && (line[firstNs] == ' ' || line[firstNs] == '\t')) firstNs++;
    char fmark = 0;
    if (firstNs < line.size() &&
        (line.compare(firstNs, 3, "```") == 0 || line.compare(firstNs, 3, "~~~") == 0))
      fmark = line[firstNs];
    bool fend = false;
    if (fmark) {
      size_t rest = firstNs + 3;
      size_t trail = rest;
      while (trail < line.size() && (line[trail] == ' ' || line[trail] == '\t')) trail++;
      fend = (trail >= line.size());
    }

    if (inFence) {
      if (fmark == fenceOpener && fend) {
        // closing fence: bottom rule
        inFence = false;
        MdStyledLine sl;
        MdRun r;
        std::string bar = "\xE2\x95\xB0";  // ╰
        int b = width - 1;
        if (b < 0) b = 0;
        appendChar(bar, "\xE2\x94\x80", (size_t)b);  // ─
        r.text = bar;
        r.st = theme.codeFence;
        sl.runs.push_back(std::move(r));
        sl.base = theme.codeFence;
        out.push_back(std::move(sl));
        continue;
      }
      // body line, with a gutter so the block reads as one container
      MdStyledLine sl;
      MdRun g;
      g.text = "\xE2\x94\x82 ";  // │
      g.st = theme.codeFence;
      MdLine runs;
      runs.push_back(std::move(g));
      highlightCodeLine(line, *curLang, theme, tok, runs);
      sl.runs = std::move(runs);
      sl.base = theme.codeBlock;
      out.push_back(std::move(sl));
      continue;
    }
    if (fmark) {
      // opening fence: top rule with language label
      inFence = true;
      fenceOpener = fmark;
      fenceStart = out.size();
      tok = CodeTokState{};  // fresh lexer state for this block
      std::string lang = line.substr(firstNs + 3);
      size_t lb = 0;
      while (lb < lang.size() && (lang[lb] == ' ' || lang[lb] == '\t')) lb++;
      size_t le = lang.size();
      while (le > lb && (lang[le - 1] == ' ' || lang[le - 1] == '\t')) le--;
      lang = lang.substr(lb, le - lb);
      curLang = codeLangOf(lang);
      std::string bar = "\xE2\x95\xAD";  // ╭
      bar += "\xE2\x94\x80";             // ─
      if (!lang.empty()) bar += " " + lang + " ";
      int fill = width - dispWidth(bar);
      if (fill < 1) fill = 1;
      appendChar(bar, "\xE2\x94\x80", (size_t)fill);
      MdStyledLine sl;
      MdRun r;
      r.text = bar;
      r.st = theme.codeFence;
      sl.runs.push_back(std::move(r));
      sl.base = theme.codeFence;
      out.push_back(std::move(sl));
      continue;
    }

    // ----- blank-line spacing (one spacer between blocks, no trailing gap) ----
    {
      size_t t = 0;
      while (t < line.size() && (line[t] == ' ' || line[t] == '\t')) t++;
      if (t == line.size()) {
        if (lastBlank || li + 1 == lines.size()) continue;  // collapse / trailing
        lastBlank = true;
        MdStyledLine sl;  // one blank spacer line
        sl.base = theme.text;
        out.push_back(std::move(sl));
        continue;
      }
      lastBlank = false;
    }

    // ----- table (header + separator, then aligned data rows) ----
    // One-line lookahead: a row is a "header" only if the next line is a
    // separator. Otherwise it renders immediately as plain text, so streaming
    // output never holds a line back waiting for a separator that may not
    // come.
    {
      std::string trimmed;
      bool row = isTableRow(line, trimmed);
      bool sep = isTableSep(line);
      if (row && !sep) {
        bool head = false;
        if (li + 1 < lines.size()) {
          std::string nxt(text, lines[li + 1].first, lines[li + 1].second - lines[li + 1].first);
          head = isTableSep(nxt);
        }
        if (head) {
          std::vector<std::string> trows;
          size_t j = li + 2;
          while (j < lines.size()) {
            std::string t;
            if (isTableRow(text.substr(lines[j].first, lines[j].second - lines[j].first), t))
              trows.push_back(std::move(t));
            else break;
            j++;
          }
          renderTable(trimmed, trows, width, theme, out);
          li = j - 1;
          continue;
        }
        // no separator follows -> falls through to plain paragraph below
      } else if (sep) {
        // stray separator without a header: render it as a dim rule
        MdStyledLine sl;
        MdRun r;
        std::string bar(width > 2 ? (size_t)width : 1, '-');
        r.text = "── " + bar;
        r.st = theme.hr;
        sl.runs.push_back(std::move(r));
        sl.base = theme.hr;
        out.push_back(std::move(sl));
        continue;
      }
    }

    // ----- thematic break -----
    {
      size_t i = firstNs;
      if (i < line.size() && (line[i] == '-' || line[i] == '*' || line[i] == '_')) {
        char c = line[i];
        size_t j = i;
        size_t cnt = 0;
        bool only = true;
        while (j < line.size()) {
          if (line[j] == c) { cnt++; j++; }
          else if (line[j] == ' ' || line[j] == '\t') j++;
          else { only = false; break; }
        }
        size_t t = j;
        while (t < line.size() && (line[t] == ' ' || line[t] == '\t')) t++;
        if (cnt >= 3 && only && t >= line.size()) {
          MdStyledLine sl;
          MdRun r;
          std::string hr(width > 2 ? (size_t)width : 1, '-');
          hr = "── " + hr;
          r.text = hr;
          r.st = theme.hr;
          sl.runs.push_back(std::move(r));
          sl.base = theme.hr;
          out.push_back(std::move(sl));
          continue;
        }
      }
    }

    // ----- heading -----
    if (firstNs < line.size() && line[firstNs] == '#') {
      size_t i = firstNs;
      int level = 0;
      while (i < line.size() && line[i] == '#' && level < 6) { level++; i++; }
      if (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        i++;
        // Drop trailing closing '#' markers ("## foo ##").
        size_t contEnd = line.size();
        while (contEnd > i) {
          char ch = line[contEnd - 1];
          if (ch == '#' || ch == ' ' || ch == '\t') { contEnd--; continue; }
          break;
        }
        MdLine runs;
        MdRun h;
        h.text = "▍ ";
        h.st = theme.heading;
        h.st.attr(AttrBold, true);
        runs.push_back(std::move(h));
        parseInline(line, i, contEnd, theme, runs);
        // topic value: emphasize by level (1 = biggest, 4-6 = quiet)
        Style hs = theme.heading;
        if (level <= 1) {
          hs.attr(AttrBold, true);
          hs.attr(AttrUnderline, true);
        } else if (level <= 3) {
          hs.attr(AttrBold, true);
        } else {
          hs.attr(AttrItalic, true);
        }
        for (auto& r : runs) r.st = hs;
        for (auto& wrapped : wrapRuns(runs, width)) {
          MdStyledLine sl;
          sl.runs = std::move(wrapped);
          sl.base = theme.heading;
          out.push_back(std::move(sl));
        }
        // GitHub-style setext underline for H1/H2 sections.
        if (level <= 2) {
          int hw = dispWidth(line.substr(i, contEnd - i)) + 2;  // + ▍ prefix
          if (hw > width) hw = width;
          if (hw > 0) {
            MdStyledLine sl;
            MdRun r;
            appendChar(r.text, "\xE2\x94\x80", (size_t)hw);  // ─
            r.st = theme.hr;
            sl.runs.push_back(std::move(r));
            sl.base = theme.hr;
            out.push_back(std::move(sl));
          }
        }
        continue;
      }
    }

    // ----- blockquote -----
    {
      size_t qo = quoteOffset(line);
      if (qo > 0) {
        MdLine runs;
        MdRun mark;
        mark.text = "▍";
        mark.st = theme.quoteMark;
        runs.push_back(std::move(mark));
        MdRun sp;
        sp.text = " ";
        sp.st = theme.quote;
        runs.push_back(std::move(sp));
        parseInline(line, qo, line.size(), theme, runs);
        for (auto& wrapped : wrapRuns(runs, width)) {
          MdStyledLine sl;
          sl.runs = std::move(wrapped);
          sl.base = theme.quote;
          out.push_back(std::move(sl));
        }
        continue;
      }
    }

    // ----- list -----
    {
      // Nesting depth from leading indentation (2/4/8 spaces).
      size_t indent = 0;
      while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) indent++;
      if (indent > 8) indent = 8;

      bool numbered = false;
      size_t lo = listOffset(line, numbered);
      if (lo > 0) {
        // Task list "- [ ]" / "- [x]".
        bool checked = false, unchecked = false;
        size_t contentLo = lo;
        if (line.compare(lo, 3, "[ ]") == 0 &&
            (lo + 3 >= line.size() || line[lo + 3] == ' ' || line[lo + 3] == '\t')) {
          unchecked = true;
          contentLo = lo + 3;
          while (contentLo < line.size() && (line[contentLo] == ' ' || line[contentLo] == '\t'))
            contentLo++;
        } else if ((line.compare(lo, 3, "[x]") == 0 || line.compare(lo, 3, "[X]") == 0) &&
                   (lo + 3 >= line.size() || line[lo + 3] == ' ' || line[lo + 3] == '\t')) {
          checked = true;
          contentLo = lo + 3;
          while (contentLo < line.size() && (line[contentLo] == ' ' || line[contentLo] == '\t'))
            contentLo++;
        }

        MdLine runs;
        MdRun mark;
        std::string prefix(indent, ' ');
        if (checked) mark.text = prefix + "\xE2\x98\x91 ";       // ☑
        else if (unchecked) mark.text = prefix + "\xE2\x98\x90 ";  // ☐
        else if (numbered) mark.text = prefix + line.substr(0, lo);
        else mark.text = prefix + "\xE2\x80\xA2 ";                // •
        mark.st = checked ? theme.hr : theme.bullet;
        runs.push_back(std::move(mark));
        parseInline(line, contentLo, line.size(), theme, runs);
        tui::Style base = theme.bulletText;
        if (checked) base = theme.hr;
        for (auto& r : runs) {
          if (r.st.v == 0) r.st = base;
        }
        for (auto& wrapped : wrapRuns(runs, width)) {
          MdStyledLine sl;
          sl.runs = std::move(wrapped);
          sl.base = base;
          out.push_back(std::move(sl));
        }
        continue;
      }
    }

    // ----- plain paragraph -----
    {
      MdLine runs;
      parseInline(line, 0, line.size(), theme, runs);
      for (auto& wrapped : wrapRuns(runs, width)) {
        MdStyledLine sl;
        sl.runs = std::move(wrapped);
        sl.base = theme.text;
        out.push_back(std::move(sl));
      }
    }
  }

  // Fence opened but never closed (still streaming, or a genuinely malformed
  // final block): don't paint the whole tail as code. Keep the fence-start
  // marker styled as a code fence, but render the body as plain text so
  // streaming output reads naturally until the closing ``` arrives.
  if (inFence && fenceStart < out.size()) {
    for (size_t i = fenceStart + 1; i < out.size(); i++) {
      MdStyledLine& sl = out[i];
      sl.base = theme.text;
      for (auto& r : sl.runs) r.st = theme.text;
    }
  }

  return out;
}

}  // namespace tui