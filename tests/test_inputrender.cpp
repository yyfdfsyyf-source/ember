#include "tui/tui.hpp"
#include "tui/widgets.hpp"
#include <cstdio>
#include <string>

static std::string rowText(tui::Screen& sc, int y, int ncols) {
  std::string row;
  for (int x = 0; x < ncols; x++) {
    if (sc.cell(x, y).cont) continue;  // continuation cell of a wide char
    uint32_t ch = sc.cell(x, y).ch;
    if (ch == 0) { row += '.'; continue; }
    char tmp[8];
    size_t k = 0;
    if (ch < 0x80) tmp[k++] = (char)ch;
    else if (ch < 0x800) { tmp[k++] = (char)(0xC0 | (ch >> 6)); tmp[k++] = (char)(0x80 | (ch & 0x3F)); }
    else { tmp[k++] = (char)(0xE0 | (ch >> 12)); tmp[k++] = (char)(0x80 | ((ch >> 6) & 0x3F)); tmp[k++] = (char)(0x80 | (ch & 0x3F)); }
    row.append(tmp, k);
  }
  return row;
}

static tui::Event key(uint32_t ch) {
  tui::Event e;
  e.type = tui::EventType::Key;
  e.ch = ch;
  return e;
}

int main() {
  int failures = 0;
  auto check = [&](bool cond, std::string const& what) {
    if (!cond) { std::printf("FAIL: %s\n", what.c_str()); failures++; }
  };

  // 1. ASCII
  {
    tui::Screen sc; sc.resize(80, 24);
    tui::Input in; in.setBox(tui::Rect(2, 20, 60, 1));
    for (char c : std::string("hello")) in.handle(key((uint32_t)c));
    in.render(sc, "\xE2\x9D\xAF ", tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
    std::string r = rowText(sc, 20, 30);
    check(r.find("hello") != std::string::npos, "ascii hello visible: [" + r + "]");
  }

  // 2. mixed CJK + ascii
  {
    tui::Screen sc; sc.resize(80, 24);
    tui::Input in; in.setBox(tui::Rect(2, 20, 60, 1));
    in.handle(key(0x4F60));  // 你
    in.handle(key(0x597D));  // 好
    for (char c : std::string("hi")) in.handle(key((uint32_t)c));
    in.render(sc, "\xE2\x9D\xAF ", tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
    std::string r = rowText(sc, 20, 30);
    check(r.find("\xe4\xbd\xa0\xe5\xa5\xbdhi") != std::string::npos, "mixed visible: [" + r + "]");
  }

  // 3. long text requiring horizontal scroll (box width 10)
  {
    tui::Screen sc; sc.resize(80, 24);
    tui::Input in; in.setBox(tui::Rect(0, 21, 10, 1));
    std::string msg = "abcdefghijklmnopqrstuvwxyz";
    for (char c : msg) in.handle(key((uint32_t)c));
    in.render(sc, "", tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
    std::string r = rowText(sc, 21, 12);
    check(r.find("stuvwxyz") != std::string::npos, "scroll shows tail: [" + r + "]");
    check(r.find('z') != std::string::npos, "scroll shows last char: [" + r + "]");
  }

  // 4. long CJK text scrolling
  {
    tui::Screen sc; sc.resize(80, 24);
    tui::Input in; in.setBox(tui::Rect(0, 22, 10, 1));
    for (int i = 0; i < 8; i++) in.handle(key(0x4E00 + i));  // 一..万 (wide)
    in.render(sc, "", tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
    std::string r = rowText(sc, 22, 12);
    check(r.find("\xe4\xb8\x87") != std::string::npos, "cjk scroll shows last char 万: [" + r + "]");
  }

  // 5. shortened buffer must not leave ghost cells (Backspace/Delete residue)
  {
    tui::Screen sc; sc.resize(80, 24);
    tui::Input in; in.setBox(tui::Rect(2, 23, 60, 1));
    for (char c : std::string("hello")) in.handle(key((uint32_t)c));
    in.render(sc, "\xE2\x9D\xAF ", tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
    in.handle(key(tui::KeyBackspace));  // "hell"
    in.handle(key(tui::KeyDelete));     // "hel" (cursor at end; no-op) prev
    while (in.cursor() > 0) in.handle(key(tui::KeyBackspace));  // ""
    in.render(sc, "\xE2\x9D\xAF ", tui::Style::plain(), tui::Style::plain(), tui::Style::plain());
    std::string r = rowText(sc, 23, 30);
    check(r.find('h') == std::string::npos, "no ghost after delete-all: [" + r + "]");
    check(r.find('o') == std::string::npos, "no ghost 'o' after delete-all: [" + r + "]");
  }

  std::printf("%s\n", failures ? "SOME FAILED" : "ALL OK");
  return failures ? 1 : 0;
}
