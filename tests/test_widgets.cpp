#include "tui/widgets.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

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

static void test_wrap() {
  auto l = tui::wrapText("hello world", 5);
  CHECK(l.size() == 2);
  CHECK(l[0] == "hello");
  CHECK(l[1] == "world");

  // hard break for long words
  l = tui::wrapText("supercalifragilistic", 6);
  CHECK(l.size() >= 2);
  CHECK(l[0].size() <= 6);

  // explicit newlines
  l = tui::wrapText("ab\ncd\nef", 10);
  CHECK(l.size() == 3 && l[0] == "ab" && l[1] == "cd" && l[2] == "ef");

  // CJK double width
  std::string s = "中文字符测试";
  l = tui::wrapText(s.c_str(), 4);
  CHECK(l.size() >= 2);
}

static void test_textview_streaming() {
  tui::TextView tv;
  tv.setWidth(20);  // "committed line" = 14 cols < 20 -> one line
  tv.append("committed line", tui::Style::plain());
  CHECK(tv.lineCount() == 1);

  tv.beginStream(tui::Style::plain());
  tv.streamAppend("part1 ");
  tv.streamAppend("part2");  // still one line until wrap
  CHECK(tv.lineCount() >= 1);
  tv.streamAppend(" more text that will wrap over width");
  CHECK(tv.lineCount() >= 2);
  tv.endStream();
  int committed = tv.lineCount();
  CHECK(committed >= 2);

  // a second stream appends after committed lines
  tv.beginStream(tui::Style::plain());
  tv.streamAppend("tail");
  tv.endStream();
  CHECK(tv.lineCount() == committed + 1);

  // cancel discards the tail
  tv.beginStream(tui::Style::plain());
  tv.streamAppend("discard me");
  tv.cancelStream();
  CHECK(tv.lineCount() == committed + 1);
}

static void test_input() {
  tui::Input in;
  tui::Event e;
  e.type = tui::EventType::Key;

  e.ch = 'h'; in.handle(e);
  e.ch = 'i'; in.handle(e);
  CHECK(in.text() == "hi" && in.cursor() == 2);

  e.ch = tui::KeyLeft; in.handle(e);
  CHECK(in.cursor() == 1);
  e.ch = 'X'; in.handle(e);
  CHECK(in.text() == "hXi");

  e.ch = tui::KeyBackspace; in.handle(e);
  CHECK(in.text() == "hi");

  e.ch = tui::KeyEnter; in.handle(e);
  CHECK(in.text() == "hi");

  // UTF-8 backspace step
  in.clear();
  in.setText("\xe4\xb8\x80");  // one CJK char
  in.setCursor(3);
  e.ch = tui::KeyBackspace;
  in.handle(e);
  CHECK(in.text().empty());

  // Enter sets the flag
  in.consumeEnter();
  CHECK(!in.enter());
}

static void test_input_render() {
  tui::Screen sc;
  sc.resize(80, 24);
  tui::Input in;
  in.setBox(tui::Rect(2, 10, 60, 1));
  tui::Event e;
  e.type = tui::EventType::Key;
  e.ch = 'h'; in.handle(e);
  e.ch = 'i'; in.handle(e);
  in.render(sc, "> ", tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
  // regression: all typed chars must be visible, not just the first
  // (prompt "> " occupies 2 cells, so text begins at box.x + 2)
  CHECK(sc.cell(4, 10).ch == 'h');
  CHECK(sc.cell(5, 10).ch == 'i');
}

static void test_multiline_input() {
  tui::Input in;
  in.setBox(tui::Rect(0, 10, 40, 3));
  tui::Event e;
  e.type = tui::EventType::Key;
  e.ch = 'a'; in.handle(e);
  e.ch = 'b'; in.handle(e);
  CHECK(in.rowCount() == 1);
  // Shift+Enter inserts a newline instead of submitting
  e.ch = tui::KeyEnter; e.shift = true; e.alt = false; e.ctrl = false;
  bool submitted = in.handle(e);
  CHECK(!submitted);
  CHECK(!in.enter());
  CHECK(in.rowCount() == 2);
  e.ch = 'c'; e.shift = false; in.handle(e);
  CHECK(in.text() == "ab\nc");
  // Enter still submits on plain Enter
  e.shift = false;
  e.ch = tui::KeyEnter;
  CHECK(in.handle(e));
  CHECK(in.enter());
  // cursor navigation: Home/End move to end/line start
  in.setText("ab\ncd\nef");
  in.setCursor((int)in.text().size());
  CHECK(in.text() == "ab\ncd\nef");
  e.ch = tui::KeyHome; in.handle(e);
  // Home moves to start of the line (after the newline of the row above)
  CHECK(in.cursor() == 6);  // inside "ef"
  in.setText("ab\ncd\nef");
  in.setCursor(1);
  e.ch = tui::KeyEnd; in.handle(e);
  CHECK(in.cursor() == 2);  // end of "ab"
  in.consumeEnter();
}

static void test_list() {
  tui::List l;
  l.setItems({"a", "b", "c"});
  CHECK(l.selected() == 0);
  CHECK(!l.needsScroll());  // empty box: nothing to scroll yet
  tui::Event e;
  e.type = tui::EventType::Key;
  e.ch = tui::KeyDown;
  l.handle(e);
  CHECK(l.selected() == 1);
  e.ch = tui::KeyDown;
  l.handle(e);
  CHECK(l.selected() == 2);
  e.ch = tui::KeyDown;  // clamp
  l.handle(e);
  CHECK(l.selected() == 2);
  tui::Screen sc;
  sc.resize(80, 24);
  tui::Rect r(0, 0, 10, 2);
  l.render(sc, r, tui::Style::plain(), tui::Style::plain());
  CHECK(l.needsScroll());  // 3 items in a 2-row list must scroll
}

static void test_layout() {
  tui::Rect r{0, 0, 20, 10};
  tui::Rect top = tui::splitTop(r, 2);
  CHECK(top.w == 20 && top.h == 2);
  tui::Rect left = tui::splitLeft(r, 5);
  CHECK(left.w == 5 && left.h == 10);
}

// ---------------------------------------------------------------------------
// Reflow: committed content must re-wrap when the terminal width changes.
// ---------------------------------------------------------------------------
static int longestLine(tui::TextView& tv) {
  int m = 0;
  for (int i = 0; i < tv.lineCount(); i++) m = std::max(m, (int)tv.line(i).size());
  return m;
}

static std::string joined(tui::TextView& tv) {
  std::string s;
  for (int i = 0; i < tv.lineCount(); i++) { s += tv.line(i); s += '\n'; }
  return s;
}

// Everything but the line breaks: used to prove a reflow loses no characters.
static std::string squeeze(std::string s) {
  std::string out;
  for (char c : s)
    if (c != ' ' && c != '\n' && c != '\t') out += c;
  return out;
}

static void test_reflow_plain() {
  std::string para;
  for (int i = 0; i < 60; i++) para += "word" + std::to_string(i) + " ";
  tui::TextView tv;
  tv.setWidth(80);
  tv.append(para, tui::Style::plain());
  int n80 = tv.lineCount();
  CHECK(n80 == 6);
  CHECK(longestLine(tv) == 80);

  tv.setWidth(40);
  int mapped = tv.relayout(-1);
  CHECK(mapped == -1);  // still following the bottom
  CHECK(tv.lineCount() > n80);
  CHECK(longestLine(tv) <= 40);

  tv.setWidth(120);
  tv.relayout(-1);
  CHECK(tv.lineCount() < n80);  // widening rejoins the broken rows
  CHECK(squeeze(joined(tv)) == squeeze(para));
}

static void test_reflow_markdown() {
  tui::MdTheme th;
  std::string doc =
      "# Heading\n\nA paragraph long enough to wrap over several display lines "
      "at any reasonable terminal width, with a trailing marker ZZZ.\n\n"
      "```cpp\nint main() { return 0; }\n```\n";
  // The invariant that matters: reflowing to W must land on exactly the layout
  // a fresh build at W produces (same lines, same inline style runs).
  auto sameAsFresh = [&](tui::TextView& tv, int w) {
    tui::TextView fresh;
    fresh.setWidth(w);
    fresh.appendMarkdown(doc, th);
    if (fresh.lineCount() != tv.lineCount()) return false;
    for (int i = 0; i < fresh.lineCount(); i++)
      if (fresh.line(i) != tv.line(i)) return false;
    return true;
  };
  tui::TextView tv;
  tv.setWidth(70);
  tv.appendMarkdown(doc, th);
  int n70 = tv.lineCount();
  CHECK(n70 > 3);

  tv.setWidth(24);
  tv.relayout(-1);
  CHECK(tv.lineCount() > n70);
  CHECK(sameAsFresh(tv, 24));

  tv.setWidth(70);
  tv.relayout(-1);
  CHECK(tv.lineCount() == n70);
  CHECK(sameAsFresh(tv, 70));
  CHECK(joined(tv).find("Heading") != std::string::npos);
  CHECK(joined(tv).find("marker ZZZ") != std::string::npos);
}

static void test_reflow_verbatim() {
  // A box row is the artwork itself: reflow must clip it, never break it.
  std::string rowA = "  +--" + std::string(40, '-') + "--+";
  std::string rowB = "  | text here" + std::string(30, ' ') + "|";
  tui::TextView tv;
  tv.setWidth(70);
  tv.append(rowA, tui::Style::plain(), true);
  tv.append(rowB, tui::Style::plain(), true);
  tv.setWidth(30);
  tv.relayout(-1);
  CHECK(tv.lineCount() == 2);
  CHECK(tv.line(0) == rowA);
  CHECK(tv.line(1) == rowB);
  // Ordinary text in the same view still reflows.
  std::string para(100, 'x');
  for (size_t i = 0; i < para.size(); i += 3) para[i] = ' ';
  tv.append(para, tui::Style::plain());
  int before = tv.lineCount();
  tv.setWidth(20);
  tv.relayout(-1);
  CHECK(tv.lineCount() > before);
  CHECK(tv.line(0) == rowA);
}

static void test_reflow_keeps_scroll_anchor() {
  tui::TextView tv;
  tv.setWidth(60);
  std::string head = "ALPHA " + std::string(200, 'b');  // many lines
  for (size_t i = 6; i < head.size(); i += 7) head[i] = ' ';
  std::string tail = "OMEGA " + std::string(200, 'c');
  for (size_t i = 6; i < tail.size(); i += 7) tail[i] = ' ';
  tv.append(head, tui::Style::plain());
  tv.append(tail, tui::Style::plain());
  int firstOfTail = tv.lineCount() / 2;  // somewhere inside the second block
  CHECK(joined(tv).find("ALPHA") != std::string::npos);
  CHECK(joined(tv).find("OMEGA") == std::string::npos || firstOfTail > 0);

  tv.setWidth(30);
  int a2 = tv.relayout(firstOfTail);
  CHECK(a2 >= 0);
  CHECK(a2 < tv.lineCount());
  // After the reflow the anchored line still belongs to the OMEGA block.
  CHECK(tv.line(a2).find("OMEGA") != std::string::npos ||
        tv.line(a2).find("ccc") != std::string::npos);
}

static void test_reflow_while_streaming() {
  tui::MdTheme th;
  std::string committed = "first block that is long enough to wrap at forty columns or fewer";
  tui::TextView tv;
  tv.setWidth(80);
  tv.appendMarkdown(committed, th);
  tv.beginStreamMarkdown(th);
  tv.streamAppend("# Title\n\nstreamed body that must survive a resize in the "
                  "middle of a turn with plenty of words to wrap.\n");
  tv.setWidth(32);
  tv.relayout(-1);
  std::string mid = joined(tv);
  CHECK(mid.find("Title") != std::string::npos);
  CHECK(mid.find("streamed body") != std::string::npos);
  CHECK(mid.find("first block") != std::string::npos);
  CHECK(longestLine(tv) <= 32);
  // An anchor inside the still-streaming tail maps by line offset, not by block.
  int bodyLine = 0;
  for (int i = 0; i < tv.lineCount(); i++)
    if (tv.line(i).find("streamed body") != std::string::npos) bodyLine = i;
  CHECK(bodyLine > 0);
  tv.setWidth(70);
  int mappedTail = tv.relayout(bodyLine);
  CHECK(mappedTail >= 0 && mappedTail < tv.lineCount());
  CHECK(tv.line(mappedTail).find("streamed body") != std::string::npos);
  tv.setWidth(32);
  tv.relayout(-1);
  // Committing after a mid-stream reflow must keep the tail as one block.
  tv.streamAppend("\nFinal marker QQQ\n");
  tv.endStream();
  std::string done = squeeze(joined(tv));
  CHECK(done.find("FinalmarkerQQQ") != std::string::npos);
  int n = tv.lineCount();
  tv.setWidth(60);
  tv.relayout(-1);
  CHECK(squeeze(joined(tv)) == done);
  CHECK(tv.lineCount() < n);
}

static void test_reflow_after_clear() {
  std::string four = "dddddddddd eeeeeeeeee ffffffffff gggggggggg";  // 4 x 10 cols
  tui::TextView tv;
  tv.setWidth(50);
  tv.append(four, tui::Style::plain());
  CHECK(tv.lineCount() == 1);
  tv.clear();
  CHECK(tv.lineCount() == 0);
  tv.setWidth(24);
  CHECK(tv.relayout(-1) == -1);
  CHECK(tv.lineCount() == 0);  // relayout on an empty view must stay empty
  // A block appended after clear() must be the only thing left.
  tv.append(four, tui::Style::plain());
  CHECK(tv.lineCount() == 2);
  tv.setWidth(11);
  tv.relayout(-1);
  CHECK(tv.lineCount() == 4);
  CHECK(joined(tv).find("dddddddddd") != std::string::npos);
  CHECK(joined(tv).find("eeeeeeeeee") != std::string::npos);
}

static tui::Event dkey(uint32_t ch) {
  tui::Event e;
  e.type = tui::EventType::Key;
  e.ch = ch;
  return e;
}

static void test_dialog() {
  tui::Dialog d;
  CHECK(!d.active());
  d.open("Delete", "are you sure?", {"Yes", "No"}, 1);
  CHECK(d.active());
  CHECK(d.selected() == 1);  // default highlighted = No

  // navigation wraps
  d.handle(dkey(tui::KeyRight));
  CHECK(d.selected() == 0);
  d.handle(dkey(tui::KeyLeft));
  CHECK(d.selected() == 1);

  // Enter activates the highlighted button
  CHECK(d.handle(dkey(tui::KeyEnter)));
  CHECK(!d.active());
  int res = -99;
  CHECK(d.takeResult(res));
  CHECK(res == 1);
  CHECK(!d.takeResult(res));  // consumed

  // Escape = last button (cancel) when a choice exists
  d.open("Q", "m", {"OK", "Cancel"});
  d.handle(dkey(tui::KeyEscape));
  CHECK(!d.active());
  CHECK(d.takeResult(res));
  CHECK(res == 1);

  // single-button dialog: Escape yields -1
  d.open("Note", "hi");
  d.handle(dkey(tui::KeyEscape));
  CHECK(d.takeResult(res));
  CHECK(res == -1);

  // render must not crash and stays a no-op when inactive
  d.close();
  tui::Screen sc;
  sc.resize(40, 12);
  d.render(sc, tui::Rect{0, 0, 40, 12}, tui::Style::plain(), tui::Style::plain(),
           tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
  d.open("Wide message that will certainly need wrapping across many rows of text",
         "line one\nsecond line is also quite long so that it must wrap at this width",
         {"Confirm", "Cancel"});
  d.render(sc, tui::Rect{0, 0, 40, 12}, tui::Style::plain(), tui::Style::plain(),
           tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
  CHECK(true);
}

static void test_markdown_stream_head_preserved() {
  // Regression: the old "incremental tail" rewrap dropped every rendered
  // prefix line on ticks where a new blank-line boundary appeared, so large
  // first appends (or ending right after a boundary) lost the head of the
  // answer: only the last paragraph was visible.
  std::string doc =
      "# Heading one\n\nFirst paragraph that must survive streaming with a "
      "quite long tail of text so that it wraps over multiple lines here.\n\n"
      "```python\nprint('body')\n```\n\nSecond paragraph also long enough to "
      "matter and to contain CJK 中文 for good measure.\n\nFinal marker XYZ";
  tui::MdTheme th;
  // 1) one big append, then end: nothing may be missing
  tui::TextView a;
  a.setWidth(48);
  a.beginStreamMarkdown(th);
  a.streamAppend(doc.c_str());
  a.endStream();
  std::string ga;
  for (int i = 0; i < a.lineCount(); i++) { ga += a.line(i); ga += '\n'; }
  CHECK(ga.find("Heading one") != std::string::npos);
  CHECK(ga.find("First paragraph") != std::string::npos);
  CHECK(ga.find("print('body')") != std::string::npos);
  CHECK(ga.find("Second paragraph") != std::string::npos);
  CHECK(ga.find("Final marker XYZ") != std::string::npos);
  // 2) end immediately after a boundary-producing tick (old bug path)
  tui::TextView b;
  b.setWidth(48);
  b.beginStreamMarkdown(th);
  size_t cut = doc.find("Second paragraph") + 6;  // right after a blank-line
  b.streamAppend(doc.substr(0, cut).c_str());     // tick 1: tail path tick
  b.endStream();                                  // commit while head dropped
  b.beginStreamMarkdown(th);
  b.streamAppend(doc.substr(cut).c_str());
  b.endStream();
  std::string gb;
  for (int i = 0; i < b.lineCount(); i++) { gb += b.line(i); gb += '\n'; }
  CHECK(gb.find("Heading one") != std::string::npos);
  CHECK(gb.find("print('body')") != std::string::npos);
  CHECK(gb.find("Final marker XYZ") != std::string::npos);
  // 3) streamed char-by-char then resized mid-flight keeps full content
  tui::TextView c;
  c.setWidth(48);
  c.beginStreamMarkdown(th);
  for (size_t i = 0; i < doc.size(); i++) {
    c.streamAppend(doc.substr(i, 1).c_str());
    if (i == doc.size() / 2) c.setWidth(30);
  }
  c.endStream();
  std::string gc;
  for (int i = 0; i < c.lineCount(); i++) { gc += c.line(i); gc += '\n'; }
  CHECK(gc.find("Heading one") != std::string::npos);
  CHECK(gc.find("Final marker XYZ") != std::string::npos);
}

static void test_page_chrome() {
  tui::Screen sc;
  sc.resize(40, 10);
  tui::Style st = tui::Style::plain().fg(tui::Color::rgb(10, 20, 30));
  tui::drawPageHeader(sc, tui::Rect{0, 0, 40, 1}, " title", "tag", st, st);
  CHECK(sc.cell(1, 0).ch == 't');                       // title at left
  CHECK(sc.cell(38, 0).ch == 'g');                      // tag flush right
  CHECK(sc.cell(20, 0).ch == 0);                       // middle untouched
  tui::drawRule(sc, 1, 40, st);
  CHECK(sc.cell(0, 1).ch == 0x2500);
  CHECK(sc.cell(39, 1).ch == 0x2500);
  tui::drawFadedRule(sc, 2, 40, st, tui::Color::rgb(200, 200, 200),
                     tui::Color::rgb(20, 20, 20), 10);
  CHECK(sc.cell(0, 2).ch == 0x2500);
  CHECK(sc.cell(39, 2).ch == 0x2500);
  // fade: right edge dims differently from the bright inside of the region
  CHECK(sc.cell(39, 2).st.v != sc.cell(31, 2).st.v);
  tui::drawHintBar(sc, 9, 40, "hints", "40x10", st);
  CHECK(sc.cell(1, 9).ch == 'h');
  CHECK(sc.cell(38, 9).ch == '0');
}

static void test_suggest_rows() {
  std::vector<tui::SuggestRow> rows;
  for (int i = 0; i < 10; i++)
    rows.push_back({"/cmd" + std::to_string(i), "desc" + std::to_string(i), "k"});
  tui::Screen sc;
  sc.resize(40, 6);
  tui::Style st = tui::Style::plain();
  int sel = 9;  // out of range: clamps to last
  int painted = tui::drawSuggestRows(sc, tui::Rect{0, 0, 40, 3}, rows, sel, st, st, st, st, st);
  CHECK(sel == 9);
  CHECK(painted == 3);
  // scrolled: last row visible in the 3-row window => "9" rendered
  bool found = false;
  for (int y = 0; y < 3; y++)
    for (int x = 0; x < 40; x++) {
      std::string line;
      for (int cx = 0; cx < 40; cx++) line += (char)sc.cell(cx, y).ch;
      if (line.find("cmd9") != std::string::npos) found = true;
    }
  CHECK(found);
  // selection follows scroll: sel=0 shows cmd0
  sel = 0;
  tui::drawSuggestRows(sc, tui::Rect{0, 0, 40, 3}, rows, sel, st, st, st, st, st);
  bool found0 = false;
  for (int y = 0; y < 3; y++) {
    std::string line;
    for (int x = 0; x < 40; x++) line += (char)sc.cell(x, y).ch;
    if (line.find("cmd0") != std::string::npos) found0 = true;
  }
  CHECK(found0);
}

int main() {
  test_wrap();
  test_textview_streaming();
  test_markdown_stream_head_preserved();
  test_page_chrome();
  test_suggest_rows();
  test_input();
  test_input_render();
  test_multiline_input();
  test_list();
  test_layout();
  test_dialog();
  test_reflow_plain();
  test_reflow_markdown();
  test_reflow_verbatim();
  test_reflow_keeps_scroll_anchor();
  test_reflow_while_streaming();
  test_reflow_after_clear();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
