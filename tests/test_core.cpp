// Core library tests (no terminal required). Self-contained assertion framework.
#include "tui/tui.hpp"
#include "wcwidth.hpp"
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

static bool contains(std::string const& s, char const* needle) {
  return s.find(needle) != std::string::npos;
}

static void test_screen() {
  tui::Screen s;
  s.resize(10, 4);
  CHECK(s.cols() == 10 && s.rows() == 4);

  s.put(2, 1, 'x', tui::Style::plain());
  CHECK(s.cell(2, 1).ch == 'x');
  CHECK(s.rowDirty(1));

  // wide char occupies continuation cell
  s.clearAll();
  s.put(3, 0, 0x4E00 /* CJK */, tui::Style::plain());
  CHECK(s.cell(3, 0).ch == 0x4E00);
  CHECK(s.cell(4, 0).cont == 1);

  // text with wrap
  tui::Rect box{0, 0, 8, 3};
  s.putTextWrapped(box, "hello world this is a wrap test", tui::Style::plain());
  CHECK(s.cell(0, 1).ch != 0);  // wrapped onto line 1

  // newline inside wrapped text
  s.clearAll();
  s.putTextWrapped(box, "ab\ncd", tui::Style::plain());
  CHECK(s.cell(0, 1).ch == 'c');
}

static void test_renderer() {
  tui::Capabilities caps;
  caps.colorBits = 24;
  caps.ansi = true;
  caps.cols = 20;
  caps.rows = 5;

  tui::Screen s;
  s.resize(20, 5);
  tui::Renderer r(caps);
  tui::Out out;

  s.putText(0, 0, "Hello", tui::Style::plain().fg(tui::Color::rgb(255, 0, 0)));
  r.frame(out, s);
  std::string o = out.str();
  CHECK(contains(o, "\x1b[38;2;255;0;0m"));  // truecolor SGR
  CHECK(contains(o, "Hello"));
  CHECK(contains(o, "\x1b[1;1H"));           // cursor to 0,0 -> 1;1

  // second frame with no changes emits nothing
  out.clear();
  r.frame(out, s);
  CHECK(out.size() == 0);

  // style transition resets
  s.put(5, 0, 'W', tui::Style::plain().fg(tui::Color::rgb(0, 255, 0)));
  out.clear();
  r.frame(out, s);
  CHECK(contains(out.str(), "\x1b[38;2;0;255;0m"));
}

static void test_input() {
  tui::InputParser p;

  // arrows
  char const* seq = "\x1b[A\x1b[1;5C\x1b[1;2D";
  p.feed((uint8_t const*)seq, strlen(seq));
  tui::Event ev;
  CHECK(p.next(ev) && ev.type == tui::EventType::Key && ev.ch == tui::KeyUp);
  CHECK(p.next(ev) && ev.ch == tui::KeyRight && ev.ctrl);
  CHECK(p.next(ev) && ev.ch == tui::KeyLeft && ev.shift);

  // enter, ctrl+c
  seq = "\r\x03";
  p.feed((uint8_t const*)seq, strlen(seq));
  CHECK(p.next(ev) && ev.ch == tui::KeyEnter);
  CHECK(p.next(ev) && ev.ch == tui::KeyCtrlC && ev.ctrl);

  // bracketed paste
  seq = "\x1b[200~some \x1b[31mpasted\x1b[201~";
  p.feed((uint8_t const*)seq, strlen(seq));
  CHECK(p.next(ev) && ev.type == tui::EventType::Paste && ev.text == "some \x1b[31mpasted");

  // SGR mouse click at (3,4) (1-based 4;5). Left press = btn 0 + 32.
  seq = "\x1b[<32;4;5M";
  p.feed((uint8_t const*)seq, strlen(seq));
  CHECK(p.next(ev) && ev.type == tui::EventType::Mouse);
  CHECK(ev.mouse.x == 3 && ev.mouse.y == 4);
  CHECK((ev.mouse.buttons & 1) != 0 && ev.mouse.press);

  // SGR wheel up = 64 + 32
  seq = "\x1b[<96;2;2M";
  p.feed((uint8_t const*)seq, strlen(seq));
  CHECK(p.next(ev) && ev.type == tui::EventType::Mouse && ev.mouse.wheel == 1);

  // alt+key and bare escape via timeout
  p.feed((uint8_t const*)"\x1bX", 2);
  CHECK(p.next(ev) && ev.ch == 'X' && ev.alt);
  p.feed((uint8_t const*)"\x1b", 1);
  p.expireTimeout();
  CHECK(p.next(ev) && ev.ch == tui::KeyEscape);
}

static void test_utf8() {
  size_t con = 0;
  char const* s = "\xe4\xb8\x80";  // U+4E00 (一)
  CHECK(tui::utf8Decode(s, 3, con) == 0x4E00 && con == 3);
  std::string enc;
  tui::utf8Append(enc, 0x4E00);
  CHECK(enc == s);
  CHECK(tui_ww::wcwidth(0x4E00) == 2);
  CHECK(tui_ww::wcwidth('a') == 1);
  CHECK(tui_ww::wcwidth(0x0301) == 0);
}

int main() {
  test_screen();
  test_renderer();
  test_input();
  test_utf8();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
