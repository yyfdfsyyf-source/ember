// Markdown renderer tests (tables, streaming-friendly blocks). Standalone.
#include "tui/md.hpp"
#include "wcwidth.hpp"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) {                                                        \
      failures++;                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                     \
  } while (0)

static std::string join(tui::MdTheme const& th, std::vector<tui::MdStyledLine> const& lines,
                        size_t& boldHeaderHead) {
  std::string all;
  boldHeaderHead = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    int w = 0;
    for (auto const& r : lines[i].runs) {
      for (char ch : r.text) {
        (void)ch;
      }
      w = 0;
      (void)w;
    }
    for (auto const& r : lines[i].runs) all += r.text;
    all += "\n";
  }
  (void)th;
  return all;
}

static bool hasBold(tui::MdStyledLine const& l) {
  for (auto const& r : l.runs) {
    if (r.st.v & (1ull << 32)) return true;  // AttrBold bit stored at bits 32..38
  }
  return false;
}

static void test_table() {
  tui::MdTheme th;
  std::string md =
      "| model | cost |\n"
      "| --- | ------ |\n"
      "| agnes-2.5-flash | 0.42 |\n"
      "| gpt-4o | 2.50 |\n"
      "后段\n";
  size_t dummy = 0;
  auto lines = tui::renderMarkdown(md, 60, th);
  std::string all = join(th, lines, dummy);

  CHECK(lines.size() >= 5);           // header + rule + 2 data rows + paragraph
  CHECK(all.find("agnes-2.5-flash") != std::string::npos);
  CHECK(all.find("gpt-4o") != std::string::npos);
  CHECK(hasBold(lines[0]));           // header row is bold + underlined
  CHECK(!hasBold(lines[2]));          // data row is not bold
  CHECK(all.find("后段") != std::string::npos);
}

static void test_lone_pipe_is_plain() {
  tui::MdTheme th;
  size_t dummy = 0;
  {
    auto lines = tui::renderMarkdown("| 说明 | 这是一个句子", 40, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("| 说明") != std::string::npos);  // kept verbatim, no table
    CHECK(lines.size() == 1);
  }
  {
    auto lines = tui::renderMarkdown("a | b", 40, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("a | b") != std::string::npos);
  }
}

static void test_cells_keep_internal_spaces() {
  tui::MdTheme th;
  size_t dummy = 0;
  std::string md = "| model | rate |\n| --- | --- |\n| gpt-4o | $2.50 /M |\n";
  auto lines = tui::renderMarkdown(md, 60, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("$2.50 /M") != std::string::npos);  // space inside a cell kept
  CHECK(all.find("gpt-4o") != std::string::npos);
}

static void test_stray_separator_renders_as_rule() {
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("| --- | --- |\n正文", 40, th);
  std::string all = join(th, lines, dummy);
  (void)all;
  CHECK(lines.size() == 2);  // rule + paragraph, no crash
}

static void test_table_incomplete_stream() {
  // Streaming: header seen, separator not yet arrived -> stays plain.
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("| col | value |", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("col") != std::string::npos);
  CHECK(!hasBold(lines[0]));  // not a table head yet

  // After separator arrives it becomes a table.
  auto lines2 = tui::renderMarkdown("| col | value |\n| --- | --- |", 40, th);
  CHECK(lines2.size() == 2);
  CHECK(hasBold(lines2[0]));
}

static void test_strike() {
  tui::MdTheme th;
  th.strike = tui::Style::plain().fg(tui::Color::index(240));
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("保留 ~~删除的~~ 内容", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("删除的") != std::string::npos);
  bool sawStrike = false;
  for (auto const& l : lines)
    for (auto const& r : l.runs)
      if (r.st == th.strike) sawStrike = true;
  CHECK(sawStrike);
}

static void test_task_list() {
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("- [x] 已完成\n- [ ] 待办\n- 普通项", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("\xE2\x98\x91") != std::string::npos);  // ☑
  CHECK(all.find("\xE2\x98\x90") != std::string::npos);  // ☐
  CHECK(all.find("已完成") != std::string::npos);
  CHECK(all.find("待办") != std::string::npos);
  CHECK(all.find("普通项") != std::string::npos);
}

static void test_nested_list_indent() {
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("顶级\n  - 子项A\n    - 孙项B", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("  \xE2\x80\xA2") != std::string::npos);     // 2-space bullet
  CHECK(all.find("    \xE2\x80\xA2") != std::string::npos);    // 4-space bullet
  CHECK(lines.size() == 3);
}

static void test_heading_trailing_hash() {
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("## 标题 ##", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("标题") != std::string::npos);
  CHECK(all.find("标题 ##") == std::string::npos);  // trailing hashes stripped
}

static void test_bold_italic() {
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("词 ***很强*** 结尾", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("很强") != std::string::npos);
}

static void test_cell_inline_markdown() {
  tui::MdTheme th;
  th.bold = tui::Style::plain().fg(tui::Color::rgb(255, 200, 120));
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("| item | value |\n| --- | --- |\n| **hot** | `code` ok |\n", 60, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("hot") != std::string::npos);
  CHECK(all.find("code") != std::string::npos);
  // **hot** must render inline (bold style), not literal asterisks
  CHECK(all.find("**hot**") == std::string::npos);
  bool sawBold = false;
  for (auto const& l : lines)
    for (auto const& r : l.runs)
      if (r.st == th.bold) sawBold = true;
  CHECK(sawBold);
}

static bool hasAttr(tui::Style st, uint64_t a) { return (st.v >> 32) & a; }

static void test_syntax_highlight() {
  tui::MdTheme th;
  th.codeKeyword = tui::Style::plain().fg(tui::Color::index(1));
  th.codeString = tui::Style::plain().fg(tui::Color::index(2));
  th.codeComment = tui::Style::plain().fg(tui::Color::index(3));
  th.codeNumber = tui::Style::plain().fg(tui::Color::index(4));
  th.codeType = tui::Style::plain().fg(tui::Color::index(5));
  th.codeFunc = tui::Style::plain().fg(tui::Color::index(6));
  th.codePreproc = tui::Style::plain().fg(tui::Color::index(7));

  auto saw = [&](tui::MdStyledLine const& l, tui::Style s) {
    for (auto const& r : l.runs)
      if (r.st.v == s.v) return true;
    return false;
  };

  {  // cpp: keyword, type, number, string, comment
    auto lines = tui::renderMarkdown(
        "```cpp\n#include <vector>\nint main() {\n  std::string s = \"hi\"; // note\n  return 0;\n}\n```",
        60, th);
    std::string all;
    for (auto const& l : lines)
      for (auto const& r : l.runs) all += r.text;
    CHECK(all.find("int main") != std::string::npos);
    CHECK(saw(lines[1], th.codePreproc));   // #include line
    CHECK(saw(lines[2], th.codeKeyword));   // int / return
    CHECK(saw(lines[3], th.codeString));    // "hi"
    CHECK(saw(lines[3], th.codeComment));   // // note
    CHECK(saw(lines[3], th.codeType));      // std::string
    CHECK(saw(lines[4], th.codeNumber));    // 0
  }
  {  // python: hash comment, triple string spans lines, def keyword
    auto lines = tui::renderMarkdown(
        "```python\n# header\ns = \"\"\"multi\nline\"\"\"\ndef f(x):\n    return x + 1\n```", 60, th);
    CHECK(saw(lines[1], th.codeComment));   // # header
    CHECK(saw(lines[2], th.codeString));    // """multi
    CHECK(saw(lines[3], th.codeString));    // line"""
    CHECK(saw(lines[4], th.codeKeyword));   // def
  }
  {  // unknown language: no keyword coloring, text still present
    auto lines = tui::renderMarkdown("```\nplain word\n```", 40, th);
    std::string all;
    for (auto const& l : lines)
      for (auto const& r : l.runs) all += r.text;
    CHECK(all.find("plain word") != std::string::npos);
  }
  {  // sql: case-insensitive keywords
    auto lines = tui::renderMarkdown("```sql\nSELECT id FROM users;\n```", 40, th);
    CHECK(saw(lines[1], th.codeKeyword));  // SELECT
  }
}

static void test_math() {
  tui::MdTheme th;
  th.math = tui::Style::plain().fg(tui::Color::rgb(170, 150, 240));
  th.mathBlock = tui::Style::plain().fg(tui::Color::rgb(200, 180, 255));
  size_t dummy = 0;

  {  // inline math -> Unicode substitutes, no literal $ left behind
    auto lines = tui::renderMarkdown("计算 $a^2+b^2$ 的值", 40, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("a\xC2\xB2") != std::string::npos);  // a²
    CHECK(all.find("$a^2") == std::string::npos);
    bool sawMath = false;
    for (auto const& l : lines)
      for (auto const& r : l.runs)
        if (r.st == th.math) sawMath = true;
    CHECK(sawMath);
  }
  {  // Greek, frac, sqrt, subscript
    auto lines = tui::renderMarkdown("$\\lambda_{peak}=\\frac{b}{T}$", 60, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("λ") != std::string::npos);
    CHECK(all.find("b/T") != std::string::npos);
    auto lines2 = tui::renderMarkdown("$\\sqrt{16}=4$", 40, th);
    std::string all2 = join(th, lines2, dummy);
    CHECK(all2.find("√(16)") != std::string::npos);
    auto lines3 = tui::renderMarkdown("$x_i$", 40, th);
    std::string all3 = join(th, lines3, dummy);
    CHECK(all3.find("x\xE1\xB5\xA2") != std::string::npos);  // xᵢ
  }
  {  // currency stays literal
    auto lines = tui::renderMarkdown("价格 $2.50 和 ￥5", 60, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("$2.50") != std::string::npos);
  }
  {  // block math -> Unicode
    auto lines = tui::renderMarkdown("$$\nx = \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}\n$$", 60, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("-b ± √(") != std::string::npos);
  }
  {  // \mathbb -> blackboard bold
    auto lines = tui::renderMarkdown("$\\mathbb{R}$ 全体实数", 40, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("\xE2\x84\x9D") != std::string::npos);  // ℝ
  }
  {  // norms, accents, bold, mod, function names, superscripted commands
    auto lines =
        tui::renderMarkdown(
            "$\\|v\\|$, $\\vec{a}$, $\\mathbf{x}$, $x \\equiv y \\pmod{n}$, "
            "$\\lim_{x\\to0} \\sin x/x$, $\\int_0^\\infty e^{-x^2}$", 90, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("\xE2\x80\x96") != std::string::npos);        // ‖
    CHECK(all.find("a\xE2\x83\x97") != std::string::npos);       // a⃗
    CHECK(all.find("\xF0\x9D\x90\xB1") != std::string::npos);    // 𝐱
    CHECK(all.find("(mod n)") != std::string::npos);
    CHECK(all.find("\\pmod") == std::string::npos);
    CHECK(all.find("sin x/x") != std::string::npos);
    CHECK(all.find("lim_") != std::string::npos);
    CHECK(all.find("\xE2\x88\xAB\xE2\x82\x80") != std::string::npos);  // ∫₀
    CHECK(all.find("\xE2\x88\x9E") != std::string::npos);              // ∞
  }
}

static void test_heading_levels() {
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("# 大标题\n#### 四级", 50, th);
  std::string all = join(th, lines, dummy);
  (void)all;
  CHECK(hasAttr(lines[0].runs[0].st, tui::AttrBold));
  CHECK(hasAttr(lines[0].runs[0].st, tui::AttrUnderline));  // H1 underlined
  CHECK(lines[1].runs.size() == 1 && lines[1].runs[0].st == th.hr);  // setext rule
  CHECK(hasAttr(lines[2].runs[0].st, tui::AttrItalic));    // H4 quiet/italic
  CHECK(!hasAttr(lines[2].runs[0].st, tui::AttrUnderline));
}

static void test_fence_box() {
  tui::MdTheme th;
  th.codeFence = tui::Style::plain().fg(tui::Color::index(240));
  th.codeBlock = tui::Style::plain().fg(tui::Color::index(252));
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("```cpp\nint x = 1;\n\nline2\n```\n后文", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("\xE2\x95\xAD") != std::string::npos);      // ╭ top rule
  CHECK(all.find("cpp") != std::string::npos);               // language label
  CHECK(all.find("\xE2\x94\x82 int x = 1;") != std::string::npos);  // │ gutter + code
  CHECK(all.find("\xE2\x94\x82 line2") != std::string::npos);       // blank body line keeps gutter
  CHECK(all.find("\xE2\x95\xB0") != std::string::npos);      // ╰ bottom rule
  CHECK(all.find("后文") != std::string::npos);
}

static void test_fence_bare_and_tilde() {
  tui::MdTheme th;
  size_t dummy = 0;
  {  // a bare ``` must still open a block
    auto lines = tui::renderMarkdown("```\nhello\n```", 40, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("\xE2\x94\x82 hello") != std::string::npos);
  }
  {  // ~~~ fences work too, and must close with ~~~
    auto lines = tui::renderMarkdown("~~~\nbody\n~~~", 40, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("\xE2\x94\x82 body") != std::string::npos);
    CHECK(all.find("\xE2\x95\xB0") != std::string::npos);
  }
  {  // mismatched close is just body content
    auto lines = tui::renderMarkdown("```\nx\n~~~", 40, th);
    std::string all = join(th, lines, dummy);
    CHECK(all.find("x") != std::string::npos);
    CHECK(all.find("~~~") != std::string::npos);
  }
}

static void test_image() {
  tui::MdTheme th;
  th.link = tui::Style::plain().fg(tui::Color::rgb(120, 200, 255));
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("看图 ![logo](http://x/logo.png)", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("\xF0\x9F\x96\xBC") != std::string::npos);  // 🖼
  CHECK(all.find("logo") != std::string::npos);
  CHECK(all.find("![") == std::string::npos);                // no literal marker
  bool sawLink = false;
  for (auto const& l : lines)
    for (auto const& r : l.runs)
      if (r.st == th.link) sawLink = true;
  CHECK(sawLink);
}

static void test_blank_collapse() {
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("a\n\n\n\nb", 40, th);
  std::string all = join(th, lines, dummy);
  (void)all;
  CHECK(lines.size() == 3);  // a, one blank, b
}

static void test_table_separator() {
  tui::MdTheme th;
  size_t dummy = 0;
  auto lines = tui::renderMarkdown("| a | b |\n| --- | --- |\n| 1 | 2 |", 40, th);
  std::string all = join(th, lines, dummy);
  CHECK(all.find("\xE2\x94\x82") != std::string::npos);  // │ between columns
  CHECK(lines.size() == 3);                              // header + rule + data
}

int main() {
  test_table();
  test_lone_pipe_is_plain();
  test_table_incomplete_stream();
  test_cells_keep_internal_spaces();
  test_stray_separator_renders_as_rule();
  test_strike();
  test_task_list();
  test_nested_list_indent();
  test_heading_trailing_hash();
  test_bold_italic();
  test_cell_inline_markdown();
  test_math();
  test_heading_levels();
  test_fence_box();
  test_fence_bare_and_tilde();
  test_image();
  test_blank_collapse();
  test_table_separator();
  test_syntax_highlight();
  if (failures == 0) {
    std::printf("test_md: OK (%d checks)\n", checks);
    return 0;
  }
  std::printf("test_md: %d/%d checks failed\n", failures, checks);
  return 1;
}