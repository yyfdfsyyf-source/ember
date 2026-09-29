#include "agent/app.hpp"
#include "agent/background.hpp"
#include "agent/http.hpp"
#include "agent/mode.hpp"
#include "agent/session.hpp"
#include "agent/usage.hpp"
#include "agent/version.hpp"
#include "tui/utf8.hpp"
#include "wcwidth.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace agent {

namespace {

struct Theme {
  tui::Color green, gold, blue, purple, red;
  tui::Color barBg, barFg, headBg;
  tui::Style user, assistant, reasoning, tool, toolResult, error;
  tui::Style header, headerDim, prompt, inputText, cursor, dim, rule;
  tui::Style barBase, barAccent, barBusy;
  tui::MdTheme md;
};

Theme g_theme;

// ---- headless NDJSON (--serve) --------------------------------------------
// One JSON document per line on stdout; the desktop frontend and scripts read
// this stream, so nothing else may be written to stdout while serve mode runs.
// setStatus() can fire from the worker thread, so the whole line is emitted
// under one lock: a torn JSON line would be dropped by the frontend parser.
std::mutex g_serveOutMu;

void serveEmit(mini::Value const& v) {
  std::string line = mini::dump(v);
  std::lock_guard lk(g_serveOutMu);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

mini::Value serveEvent(char const* type) {
  mini::Value o = mini::Value::makeObject();
  o.set("type", mini::Value::makeString(type));
  return o;
}

namespace {

// Accent palette for the generated dark themes. Each entry is derived from a
// well-known terminal color scheme so the UI blends with the user's terminal.
struct TPalette {
  char const* name;
  tui::Color green, gold, blue, purple, red;
  tui::Color fg, fgDim, bg, headBg, rule;
  tui::Color curFg, curBg;
};

TPalette const kPalettes[] = {
    {"nord", tui::Color::rgb(163, 190, 140), tui::Color::rgb(235, 203, 139),
     tui::Color::rgb(129, 161, 193), tui::Color::rgb(180, 142, 173),
     tui::Color::rgb(191, 97, 106), tui::Color::rgb(216, 222, 233),
     tui::Color::rgb(140, 160, 184), tui::Color::rgb(46, 52, 64),
     tui::Color::rgb(59, 66, 82), tui::Color::rgb(146, 158, 178),
     tui::Color::rgb(46, 52, 64), tui::Color::rgb(216, 222, 233)},
    {"gruvbox", tui::Color::rgb(184, 187, 38), tui::Color::rgb(250, 189, 47),
     tui::Color::rgb(131, 165, 152), tui::Color::rgb(211, 134, 155),
     tui::Color::rgb(251, 73, 52), tui::Color::rgb(235, 219, 178),
     tui::Color::rgb(168, 153, 132), tui::Color::rgb(40, 40, 40),
     tui::Color::rgb(60, 56, 54), tui::Color::rgb(146, 131, 113),
     tui::Color::rgb(40, 40, 40), tui::Color::rgb(235, 219, 178)},
    {"dracula", tui::Color::rgb(80, 250, 123), tui::Color::rgb(241, 250, 140),
     tui::Color::rgb(139, 233, 253), tui::Color::rgb(189, 147, 249),
     tui::Color::rgb(255, 85, 85), tui::Color::rgb(248, 248, 242),
     tui::Color::rgb(110, 126, 172), tui::Color::rgb(40, 42, 54),
     tui::Color::rgb(68, 71, 90), tui::Color::rgb(139, 143, 167),
     tui::Color::rgb(40, 42, 54), tui::Color::rgb(248, 248, 242)},
    {"solarized", tui::Color::rgb(133, 153, 0), tui::Color::rgb(181, 137, 0),
     tui::Color::rgb(38, 139, 210), tui::Color::rgb(108, 113, 196),
     tui::Color::rgb(220, 50, 47), tui::Color::rgb(131, 148, 150),
     tui::Color::rgb(88, 110, 117), tui::Color::rgb(0, 43, 54),
     tui::Color::rgb(7, 54, 66), tui::Color::rgb(93, 128, 131),
     tui::Color::rgb(0, 43, 54), tui::Color::rgb(147, 161, 161)},
};

Theme buildThemeFromPalette(TPalette const& p) {
  Theme t;
  t.green = p.green; t.gold = p.gold; t.blue = p.blue;
  t.purple = p.purple; t.red = p.red;
  t.barBg = p.bg; t.barFg = p.fg; t.headBg = p.headBg;
  t.user = tui::Style::plain().fg(p.green).attr(tui::AttrBold, true);
  t.assistant = tui::Style::plain().fg(p.fg);
  t.reasoning = tui::Style::plain().fg(p.fgDim).attr(tui::AttrItalic, true);
  t.tool = tui::Style::plain().fg(p.purple).attr(tui::AttrItalic, true);
  t.toolResult = tui::Style::plain().fg(p.fgDim);
  t.error = tui::Style::plain().fg(p.red).attr(tui::AttrBold, true);
  t.header = tui::Style::plain().fg(p.gold).bg(p.headBg).attr(tui::AttrBold, true);
  t.headerDim = tui::Style::plain().fg(p.fgDim).bg(p.headBg);
  t.prompt = tui::Style::plain().fg(p.blue);
  t.inputText = tui::Style::plain().fg(p.fg);
  t.cursor = tui::Style::plain().fg(p.curFg).bg(p.curBg);
  t.dim = tui::Style::plain().fg(p.fgDim);
  t.rule = tui::Style::plain().fg(p.rule);
  t.barBase = tui::Style::plain().fg(p.fg).bg(p.bg);
  t.barAccent = tui::Style::plain().fg(p.gold).bg(p.bg).attr(tui::AttrBold, true);
  t.barBusy = tui::Style::plain().fg(p.green).bg(p.bg).attr(tui::AttrBold, true);
  t.md.text = t.assistant;
  t.md.heading = tui::Style::plain().fg(p.gold).attr(tui::AttrBold, true);
  t.md.bold = tui::Style::plain().fg(p.fg).attr(tui::AttrBold, true);
  t.md.italic = tui::Style::plain().fg(p.fg).attr(tui::AttrItalic, true);
  t.md.code = tui::Style::plain().fg(p.blue).bg(p.bg);
  t.md.codeBlock = tui::Style::plain().fg(p.fg);
  t.md.codeFence = tui::Style::plain().fg(p.fgDim);
  t.md.codeKeyword = tui::Style::plain().fg(p.purple).attr(tui::AttrBold, true);
  t.md.codeType = tui::Style::plain().fg(p.blue).attr(tui::AttrBold, true);
  t.md.codeString = tui::Style::plain().fg(p.green);
  t.md.codeComment = tui::Style::plain().fg(p.fgDim).attr(tui::AttrItalic, true);
  t.md.codeNumber = tui::Style::plain().fg(p.gold);
  t.md.codeFunc = tui::Style::plain().fg(p.blue);
  t.md.codePreproc = tui::Style::plain().fg(p.red);
  t.md.quote = tui::Style::plain().fg(p.fgDim).attr(tui::AttrItalic, true);
  t.md.quoteMark = tui::Style::plain().fg(p.gold).attr(tui::AttrBold, true);
  t.md.bullet = tui::Style::plain().fg(p.blue).attr(tui::AttrBold, true);
  t.md.bulletText = t.assistant;
  t.md.link = tui::Style::plain().fg(p.blue).attr(tui::AttrUnderline, true);
  t.md.linkUrl = tui::Style::plain().fg(p.fgDim).attr(tui::AttrDim, true);
  t.md.hr = tui::Style::plain().fg(p.rule);
  t.md.tableHead =
      tui::Style::plain().fg(p.gold).attr(tui::AttrBold, true).attr(tui::AttrUnderline, true);
  t.md.table = t.assistant;
  t.md.strike = tui::Style::plain().fg(p.fgDim).attr(tui::AttrItalic, true);
  t.md.math = tui::Style::plain().fg(p.purple).attr(tui::AttrItalic, true);
  t.md.mathBlock = tui::Style::plain().fg(p.purple).attr(tui::AttrItalic, true);
  return t;
}

}  // namespace

void applyTheme(std::string const& name, ThemeOverride const& ov) {
  for (auto const& p : kPalettes) {
    if (name == p.name) {
      if (ov.active) {
        TPalette pp = p;
#define SET3(dst, src) dst = tui::Color::rgb(src[0], src[1], src[2])
        SET3(pp.green, ov.green); SET3(pp.gold, ov.gold); SET3(pp.blue, ov.blue);
        SET3(pp.purple, ov.purple); SET3(pp.red, ov.red); SET3(pp.fg, ov.fg);
        SET3(pp.fgDim, ov.fgDim); SET3(pp.bg, ov.bg); SET3(pp.headBg, ov.headBg);
        SET3(pp.rule, ov.rule);
        pp.curFg = pp.bg; pp.curBg = pp.fg;
#undef SET3
        g_theme = buildThemeFromPalette(pp);
      } else {
        g_theme = buildThemeFromPalette(p);
      }
      return;
    }
  }
  bool light = (name == "light");
  bool term  = (name == "terminal");

  if (light) {
    g_theme = Theme{};
    g_theme.green = tui::Color::rgb(20, 128, 52);
    g_theme.gold  = tui::Color::rgb(176, 122, 4);
    g_theme.blue  = tui::Color::rgb(20, 96, 180);
    g_theme.purple= tui::Color::rgb(128, 76, 180);
    g_theme.red   = tui::Color::rgb(200, 40, 40);
    g_theme.barBg = tui::Color::rgb(240, 242, 248);
    g_theme.barFg = tui::Color::rgb(70, 76, 90);
    g_theme.headBg= tui::Color::rgb(220, 226, 240);
    g_theme.user  = tui::Style::plain().fg(g_theme.green).attr(tui::AttrBold, true);
    g_theme.assistant = tui::Style::plain().fg(tui::Color::rgb(40, 40, 48));
    g_theme.reasoning = tui::Style::plain().fg(tui::Color::index(22)).attr(tui::AttrItalic, true).attr(tui::AttrDim, true);
    g_theme.tool  = tui::Style::plain().fg(g_theme.purple).attr(tui::AttrItalic, true);
    g_theme.toolResult = tui::Style::plain().fg(tui::Color::rgb(70, 70, 80));
    g_theme.error = tui::Style::plain().fg(g_theme.red).attr(tui::AttrBold, true);
    g_theme.header = tui::Style::plain().fg(g_theme.gold).bg(g_theme.headBg).attr(tui::AttrBold, true);
    g_theme.headerDim = tui::Style::plain().fg(tui::Color::rgb(90, 100, 115)).bg(g_theme.headBg);
    g_theme.prompt = tui::Style::plain().fg(g_theme.blue);
    g_theme.inputText = tui::Style::plain().fg(tui::Color::rgb(30, 30, 36));
    g_theme.cursor = tui::Style::plain().fg(tui::Color::rgb(255, 255, 255)).bg(tui::Color::rgb(40, 40, 48));
    g_theme.dim = tui::Style::plain().fg(tui::Color::index(24));
    g_theme.rule = tui::Style::plain().fg(tui::Color::index(245));
    g_theme.barBase = tui::Style::plain().fg(g_theme.barFg).bg(g_theme.barBg);
    g_theme.barAccent = tui::Style::plain().fg(g_theme.gold).bg(g_theme.barBg).attr(tui::AttrBold, true);
    g_theme.barBusy = tui::Style::plain().fg(g_theme.green).bg(g_theme.barBg).attr(tui::AttrBold, true);
    g_theme.md.text = g_theme.assistant;
    g_theme.md.heading = tui::Style::plain().fg(g_theme.gold).attr(tui::AttrBold, true);
    g_theme.md.bold = tui::Style::plain().fg(tui::Color::rgb(60, 50, 10)).attr(tui::AttrBold, true);
    g_theme.md.italic = tui::Style::plain().fg(tui::Color::rgb(60, 60, 80)).attr(tui::AttrItalic, true);
    g_theme.md.code = tui::Style::plain().fg(tui::Color::rgb(30, 96, 140)).bg(tui::Color::rgb(228, 234, 242));
    g_theme.md.codeBlock = tui::Style::plain().fg(tui::Color::rgb(50, 60, 75));
    g_theme.md.codeFence = tui::Style::plain().fg(tui::Color::rgb(110, 120, 140));
    g_theme.md.codeKeyword = tui::Style::plain().fg(tui::Color::rgb(128, 76, 180)).attr(tui::AttrBold, true);
    g_theme.md.codeType = tui::Style::plain().fg(tui::Color::rgb(20, 96, 180)).attr(tui::AttrBold, true);
    g_theme.md.codeString = tui::Style::plain().fg(tui::Color::rgb(20, 128, 52));
    g_theme.md.codeComment = tui::Style::plain().fg(tui::Color::rgb(120, 125, 135)).attr(tui::AttrItalic, true);
    g_theme.md.codeNumber = tui::Style::plain().fg(tui::Color::rgb(176, 122, 4));
    g_theme.md.codeFunc = tui::Style::plain().fg(tui::Color::rgb(20, 96, 180));
    g_theme.md.codePreproc = tui::Style::plain().fg(tui::Color::rgb(200, 40, 40));
    g_theme.md.quote = tui::Style::plain().fg(tui::Color::index(23)).attr(tui::AttrItalic, true);
    g_theme.md.quoteMark = tui::Style::plain().fg(tui::Color::rgb(180, 110, 30)).attr(tui::AttrBold, true);
    g_theme.md.bullet = tui::Style::plain().fg(g_theme.blue).attr(tui::AttrBold, true);
    g_theme.md.bulletText = g_theme.assistant;
    g_theme.md.link = tui::Style::plain().fg(g_theme.blue).attr(tui::AttrUnderline, true);
    g_theme.md.linkUrl = tui::Style::plain().fg(tui::Color::index(24)).attr(tui::AttrDim, true);
    g_theme.md.hr = tui::Style::plain().fg(tui::Color::index(247));
    g_theme.md.tableHead = tui::Style::plain().fg(tui::Color::rgb(120, 70, 0)).attr(tui::AttrBold, true).attr(tui::AttrUnderline, true);
    g_theme.md.table = g_theme.assistant;
    g_theme.md.strike = tui::Style::plain().fg(tui::Color::index(24)).attr(tui::AttrItalic, true);
    g_theme.md.math = tui::Style::plain().fg(tui::Color::rgb(110, 70, 200)).attr(tui::AttrItalic, true);
    g_theme.md.mathBlock = tui::Style::plain().fg(tui::Color::rgb(130, 100, 200)).attr(tui::AttrItalic, true);
  } else if (term) {
    g_theme = Theme{};
    auto idx = [](int i) { return tui::Color::index(i); };
    g_theme.green = idx(2); g_theme.gold = idx(3);
    g_theme.blue = idx(4); g_theme.purple = idx(5);
    g_theme.red  = idx(1);
    g_theme.barBg = idx(0); g_theme.barFg = idx(7); g_theme.headBg = idx(0);
    g_theme.user  = tui::Style::plain().fg(idx(2)).attr(tui::AttrBold, true);
    g_theme.assistant = tui::Style::plain().fg(idx(7));
    g_theme.reasoning = tui::Style::plain().fg(idx(8)).attr(tui::AttrItalic, true).attr(tui::AttrDim, true);
    g_theme.tool  = tui::Style::plain().fg(idx(5)).attr(tui::AttrItalic, true);
    g_theme.toolResult = tui::Style::plain().fg(idx(8));
    g_theme.error = tui::Style::plain().fg(idx(1)).attr(tui::AttrBold, true);
    g_theme.header = tui::Style::plain().fg(idx(3)).bg(idx(0)).attr(tui::AttrBold, true);
    g_theme.headerDim = tui::Style::plain().fg(idx(7)).bg(idx(0));
    g_theme.prompt = tui::Style::plain().fg(idx(4));
    g_theme.inputText = tui::Style::plain().fg(idx(7));
    g_theme.cursor = tui::Style::plain().fg(idx(0)).bg(idx(7));
    g_theme.dim = tui::Style::plain().fg(idx(8));
    g_theme.rule = tui::Style::plain().fg(idx(0));
    g_theme.barBase = tui::Style::plain().fg(idx(7)).bg(idx(0));
    g_theme.barAccent = tui::Style::plain().fg(idx(3)).bg(idx(0)).attr(tui::AttrBold, true);
    g_theme.barBusy = tui::Style::plain().fg(idx(2)).bg(idx(0)).attr(tui::AttrBold, true);
    g_theme.md.text = g_theme.assistant;
    g_theme.md.heading = tui::Style::plain().fg(idx(3)).attr(tui::AttrBold, true);
    g_theme.md.bold = tui::Style::plain().fg(idx(7)).attr(tui::AttrBold, true);
    g_theme.md.italic = tui::Style::plain().fg(idx(7)).attr(tui::AttrItalic, true);
    g_theme.md.code = tui::Style::plain().fg(idx(4)).bg(idx(0));
    g_theme.md.codeBlock = tui::Style::plain().fg(idx(7));
    g_theme.md.codeFence = tui::Style::plain().fg(idx(8));
    g_theme.md.codeKeyword = tui::Style::plain().fg(idx(5)).attr(tui::AttrBold, true);
    g_theme.md.codeType = tui::Style::plain().fg(idx(4)).attr(tui::AttrBold, true);
    g_theme.md.codeString = tui::Style::plain().fg(idx(2));
    g_theme.md.codeComment = tui::Style::plain().fg(idx(8)).attr(tui::AttrItalic, true);
    g_theme.md.codeNumber = tui::Style::plain().fg(idx(3));
    g_theme.md.codeFunc = tui::Style::plain().fg(idx(4));
    g_theme.md.codePreproc = tui::Style::plain().fg(idx(1));
    g_theme.md.quote = tui::Style::plain().fg(idx(7)).attr(tui::AttrItalic, true);
    g_theme.md.quoteMark = tui::Style::plain().fg(idx(3)).attr(tui::AttrBold, true);
    g_theme.md.bullet = tui::Style::plain().fg(idx(4)).attr(tui::AttrBold, true);
    g_theme.md.bulletText = g_theme.assistant;
    g_theme.md.link = tui::Style::plain().fg(idx(4)).attr(tui::AttrUnderline, true);
    g_theme.md.linkUrl = tui::Style::plain().fg(idx(8)).attr(tui::AttrDim, true);
    g_theme.md.hr = tui::Style::plain().fg(idx(0));
    g_theme.md.tableHead = tui::Style::plain().fg(idx(3)).attr(tui::AttrBold, true).attr(tui::AttrUnderline, true);
    g_theme.md.table = g_theme.assistant;
    g_theme.md.strike = tui::Style::plain().fg(idx(8)).attr(tui::AttrItalic, true);
    g_theme.md.math = tui::Style::plain().fg(idx(5)).attr(tui::AttrItalic, true);
    g_theme.md.mathBlock = tui::Style::plain().fg(idx(5)).attr(tui::AttrItalic, true);
  } else {  // "dark" default
    g_theme = Theme{};
    // Tokyo-Night-inspired palette: soft accents that read well on dark
    // terminals instead of primary-color neon.
    tui::Color const tnGreen = tui::Color::rgb(158, 206, 106);
    tui::Color const tnGold  = tui::Color::rgb(224, 175, 104);
    tui::Color const tnBlue  = tui::Color::rgb(122, 162, 247);
    tui::Color const tnPurple = tui::Color::rgb(187, 154, 247);
    tui::Color const tnRed   = tui::Color::rgb(247, 118, 142);
    g_theme.green = tnGreen; g_theme.gold = tnGold; g_theme.blue = tnBlue;
    g_theme.purple = tnPurple; g_theme.red = tnRed;
    g_theme.barBg = tui::Color::rgb(24, 28, 36);
    g_theme.barFg = tui::Color::rgb(188, 197, 212);
    g_theme.headBg = tui::Color::rgb(38, 44, 58);
    g_theme.user  = tui::Style::plain().fg(tnGreen).attr(tui::AttrBold, true);
    g_theme.assistant = tui::Style::plain().fg(tui::Color::rgb(235, 238, 245));
    g_theme.reasoning = tui::Style::plain().fg(tui::Color::rgb(150, 162, 182))
                            .bg(tui::Color::rgb(29, 34, 45))
                            .attr(tui::AttrItalic, true);
    g_theme.tool  = tui::Style::plain().fg(tui::Color::rgb(182, 166, 236));
    g_theme.toolResult = tui::Style::plain().fg(tui::Color::rgb(150, 160, 178));
    g_theme.error = tui::Style::plain().fg(tnRed).attr(tui::AttrBold, true);
    g_theme.header = tui::Style::plain().fg(tnGold).bg(g_theme.headBg).attr(tui::AttrBold, true);
    g_theme.headerDim = tui::Style::plain().fg(tui::Color::rgb(150, 160, 178)).bg(g_theme.headBg);
    g_theme.prompt = tui::Style::plain().fg(tnBlue);
    g_theme.inputText = tui::Style::plain().fg(tui::Color::rgb(230, 233, 240));
    g_theme.cursor = tui::Style::plain().fg(tui::Color::rgb(10, 12, 16))
                         .bg(tui::Color::rgb(180, 200, 230));
    g_theme.dim = tui::Style::plain().fg(tui::Color::rgb(120, 130, 148));
    g_theme.rule = tui::Style::plain().fg(tui::Color::rgb(58, 66, 82));
    g_theme.barBase = tui::Style::plain().fg(g_theme.barFg).bg(g_theme.barBg);
    g_theme.barAccent = tui::Style::plain().fg(tnGold).bg(g_theme.barBg).attr(tui::AttrBold, true);
    g_theme.barBusy = tui::Style::plain().fg(tnGreen).bg(g_theme.barBg).attr(tui::AttrBold, true);
    g_theme.md.text = g_theme.assistant;
    g_theme.md.heading = tui::Style::plain().fg(tnGold).attr(tui::AttrBold, true);
    g_theme.md.bold = tui::Style::plain().fg(tui::Color::rgb(255, 244, 214)).attr(tui::AttrBold, true);
    g_theme.md.italic = tui::Style::plain().fg(tui::Color::rgb(214, 218, 232)).attr(tui::AttrItalic, true);
    g_theme.md.code = tui::Style::plain().fg(tui::Color::rgb(125, 207, 255)).bg(tui::Color::rgb(30, 36, 48));
    // Code block body: subtle card background lifts the block off the chat
    // background so its extent is obvious at a glance (WCAG-safe contrast).
    g_theme.md.codeBlock = tui::Style::plain().fg(tui::Color::rgb(190, 208, 228))
                               .bg(tui::Color::rgb(29, 34, 44));
    g_theme.md.codeFence = tui::Style::plain().fg(tui::Color::rgb(108, 138, 166))
                               .bg(tui::Color::rgb(29, 34, 44));
    g_theme.md.codeKeyword =
        tui::Style::plain().fg(tui::Color::rgb(187, 154, 247)).attr(tui::AttrBold, true);
    g_theme.md.codeType =
        tui::Style::plain().fg(tui::Color::rgb(122, 162, 247)).attr(tui::AttrBold, true);
    g_theme.md.codeString = tui::Style::plain().fg(tui::Color::rgb(158, 206, 106));
    g_theme.md.codeComment =
        tui::Style::plain().fg(tui::Color::rgb(140, 150, 168)).attr(tui::AttrItalic, true);
    g_theme.md.codeNumber = tui::Style::plain().fg(tui::Color::rgb(224, 175, 104));
    g_theme.md.codeFunc = tui::Style::plain().fg(tui::Color::rgb(125, 207, 255));
    g_theme.md.codePreproc = tui::Style::plain().fg(tui::Color::rgb(247, 118, 142));
    g_theme.md.quote = tui::Style::plain().fg(tui::Color::rgb(180, 192, 210)).attr(tui::AttrItalic, true);
    g_theme.md.quoteMark = tui::Style::plain().fg(tui::Color::rgb(255, 170, 90)).attr(tui::AttrBold, true);
    g_theme.md.bullet = tui::Style::plain().fg(tnBlue).attr(tui::AttrBold, true);
    g_theme.md.bulletText = g_theme.assistant;
    g_theme.md.link = tui::Style::plain().fg(tnBlue).attr(tui::AttrUnderline, true);
    g_theme.md.linkUrl = tui::Style::plain().fg(tui::Color::rgb(120, 132, 150)).attr(tui::AttrDim, true);
    g_theme.md.hr = tui::Style::plain().fg(tui::Color::rgb(58, 66, 82));
    g_theme.md.tableHead = tui::Style::plain().fg(tui::Color::rgb(255, 214, 140)).attr(tui::AttrBold, true)
                .attr(tui::AttrUnderline, true);
    g_theme.md.table = g_theme.assistant;
    g_theme.md.strike = tui::Style::plain().fg(tui::Color::rgb(120, 130, 148)).attr(tui::AttrItalic, true);
    g_theme.md.math = tui::Style::plain().fg(tui::Color::rgb(176, 158, 246)).attr(tui::AttrItalic, true);
    g_theme.md.mathBlock =
        tui::Style::plain().fg(tui::Color::rgb(196, 182, 255)).attr(tui::AttrItalic, true);
  }
}

tui::Color cGold() { return g_theme.gold; }

tui::Style stUser() { return g_theme.user; }
tui::Style stAssistant() { return g_theme.assistant; }
tui::Style stReasoning() { return g_theme.reasoning; }
tui::Style stTool() { return g_theme.tool; }
tui::Style stToolResult() { return g_theme.toolResult; }
tui::Style stError() { return g_theme.error; }
tui::Style stHeader() { return g_theme.header; }
tui::Style stHeaderDim() { return g_theme.headerDim; }
tui::Style stPrompt() { return g_theme.prompt; }
tui::Style stInputText() { return g_theme.inputText; }
tui::Style stCursor() { return g_theme.cursor; }
tui::Style stDim() { return g_theme.dim; }
tui::Style stRule() { return g_theme.rule; }
tui::Style stBarBase() { return g_theme.barBase; }
tui::Style stBarAccent() { return g_theme.barAccent; }
tui::Style stBarBusy() { return g_theme.barBusy; }

tui::MdTheme mdTheme() { return g_theme.md; }

void enableModes(tui::Capabilities const& c) {
  using tui::platform::writeAll;
  if (!c.ansi) return;
#ifndef _WIN32
  // POSIX: input is read as a VT byte stream, so terminal-side tracking modes apply.
  if (c.mouse) writeAll("\x1b[?1000h\x1b[?1002h\x1b[?1006h", 24);
  if (c.bracketedPaste) writeAll("\x1b[?2004h", 8);
  if (c.focusEvents) writeAll("\x1b[?1004h", 8);
#else
  // Windows: input is read as INPUT_RECORDs (ReadConsoleInputW). On the
  // legacy conhost this already delivers MOUSE_EVENT records, so no VT modes
  // are needed (emitting ?1000h/?1006h there would route mouse events into
  // the VT byte queue instead, breaking the wheel and leaking escapes).
  //
  // Windows Terminal, however, enables its "alternate scroll" by default:
  // in the alternate screen buffer it translates the wheel into Up/Down key
  // events instead of mouse records. Requesting real mouse reporting makes
  // it deliver positioned MOUSE_EVENT records so the app can scroll the
  // conversation and step the input history by pointer location.
  char const* wt = std::getenv("WT_SESSION");
  if (wt && *wt) {
    if (c.mouse) writeAll("\x1b[?1000h\x1b[?1006h", 18);
    if (c.bracketedPaste) writeAll("\x1b[?2004h", 8);
    if (c.focusEvents) writeAll("\x1b[?1004h", 8);
  }
#endif
}

std::string excerpt(std::string const& s, size_t max) {
  if (s.size() <= max) return s;
  return s.substr(0, max) + "...";
}

// Truncate `s` to at most `maxCols` terminal columns, never splitting a UTF-8
// sequence (used for single-line trajectory list rows).
std::string truncCols(std::string const& s, int maxCols) {
  if (maxCols <= 0) return "";
  int col = 0;
  size_t i = 0, n = s.size();
  while (i < n) {
    size_t con = 0;
    uint32_t cp = tui::utf8Decode(s.data() + i, n - i, con);
    if (con == 0) con = 1;
    int w = (int)tui_ww::wcwidth(cp);
    if (w < 0) w = 1;
    if (col + w > maxCols) break;
    col += w;
    i += con;
  }
  return s.substr(0, i);
}

// "HH:MM:SS" from epoch ms (portable localtime).
std::string fmtClock(int64_t ms) {
  time_t t = (time_t)(ms / 1000);
  std::tm tm = {};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[16];
  snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

tui::Style trajStyleOf(agent::TrajKind k) {
  switch (k) {
    case agent::TrajKind::TurnStart:
    case agent::TrajKind::TurnEnd:
      return stBarBase();
    case agent::TrajKind::UserMessage:
      return stUser();
    case agent::TrajKind::Reasoning:
      return stReasoning();
    case agent::TrajKind::Assistant:
      return stAssistant();
    case agent::TrajKind::ToolCall:
      return stTool();
    case agent::TrajKind::ToolResult:
      return stToolResult();
    case agent::TrajKind::ContextNote:
      return stPrompt();
    case agent::TrajKind::Error:
      return stError();
  }
  return stAssistant();
}

// Load AGENTS.md / SKILL.md content (if present) and return it capped, ready
// to prepend to the system prompt.
std::string loadRulesText(std::string const& path) {
  std::string out;
  auto readIfExists = [&](std::string const& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string t = ss.str();
    if (!t.empty()) out += t;
  };
  if (!path.empty()) {
    readIfExists(path);
  } else {
    readIfExists("AGENTS.md");
  }
  readIfExists("SKILL.md");
  // Cross-session lessons memory (out/lessons.md).
  if (std::ifstream("out/lessons.md").good()) {
    std::ifstream li("out/lessons.md", std::ios::binary);
    std::stringstream lss;
    lss << li.rdbuf();
    std::string lt = lss.str();
    if (!lt.empty()) {
      if (lt.size() > 6000) {
        lt = "...(older lessons omitted)\n" + lt.substr(lt.size() - 6000);
      }
      out += "\n\nLessons from previous sessions:\n" + lt;
    }
  }
  if (out.size() > 12000) {
    out.resize(12000);
    out += "\n...[truncated]";
  }
  return out.empty() ? out : "Project rules (AGENTS.md/SKILL.md):\n" + out;
}

}  // namespace

App::~App() {
  busy_ = false;
  if (worker_.joinable()) worker_.join();
}

bool App::init(AppConfig const& cfg) {
  cfg_ = cfg;
  // Apply the theme before restoreSession()/fillConvoFromMessages() render
  // markdown runs: otherwise the restore path stamps runs with a zero theme
  // (attrs only, no fg/bg) and previously md-formatted text comes back plain.
  applyTheme(cfg_.theme, cfg_.themeOverride);
  registerBuiltinTools(tools_);
  if (!cfg_.shellEnabled) tools_.setEnabled("shell_exec", false);
  // Workspace wiring: absolutize the file-ish settings paths BEFORE any
  // chdir, then make the active workspace the process CWD and install the
  // out-of-bounds approval guard on the registry (covers every tool,
  // run_code steps and sub-agents alike).
  {
    auto absArg = [](std::string& s) {
      if (s.empty()) return;
      std::error_code ec;
      std::filesystem::path p(s);
      if (p.is_relative()) {
        auto a = std::filesystem::absolute(p, ec);
        if (!ec) s = a.string();
      }
    };
    absArg(cfg_.settingsPath);
    absArg(cfg_.tracePath);
    absArg(cfg_.rulesPath);
    absArg(cfg_.pluginsDir);
  }
  tools_.setGuard([this](std::string const& t, std::string const& a) {
    return workspaceGuard(t, a);
  });
  applyActiveWorkspace();
  if (cfg_.defaultBaseUrl.empty()) {
    cfg_.defaultBaseUrl = cfg_.baseUrl;  // remember the pristine connection
    cfg_.defaultApiKey = cfg_.apiKey;
  }
  applyProviderForModel(cfg_.model);  // grouped model: its provider wins at startup
  if (!client_.configure(cfg_.baseUrl, cfg_.apiKey)) return false;
  setAskUserCallback([this](std::string const& q) { return askUser(q); });
  setSubagentCallback([this](std::string const& d, std::string const& p) {
    return runSubagent(d, p);
  });
  setSubagentParallelCallback([this](std::string const& tasksJson) {
    return runSubagentsParallel(tasksJson);
  });
  // Creator-mode introspection tools (advertised/run only in creator mode).
  tools_.add({.name = "list_tools",
              .description = "List every currently registered tool (name, enabled, "
                             "description). Use in creator mode to inspect the runtime.",
              .parameters = mini::Value::makeObject(),
              .run = [this](std::string const&) {
                mini::Value arr = mini::Value::makeArray();
                for (auto const& nm : tools_.names()) {
                  Tool const* t = tools_.find(nm);
                  mini::Value o = mini::Value::makeObject();
                  o.set("name", mini::Value::makeString(nm));
                  o.set("enabled", mini::Value::makeBool(t && t->enabled));
                  if (t) o.set("description", mini::Value::makeString(t->description));
                  arr.arr.push_back(std::move(o));
                }
                return mini::dump(arr);
              }});
  tools_.add({.name = "get_config",
              .description = "Return the current runtime configuration (model, base URL, "
                             "budget, theme, mode, plugin/session paths, cost estimates). "
                             "Use in creator mode to see how the agent is composed.",
              .parameters = mini::Value::makeObject(),
              .run = [this](std::string const&) {
                mini::Value o = mini::Value::makeObject();
                o.set("model", mini::Value::makeString(cfg_.model));
                o.set("base_url", mini::Value::makeString(cfg_.baseUrl));
                o.set("mode", mini::Value::makeString(cfg_.mode));
                o.set("budget_tokens", mini::Value::makeInt((int64_t)cfg_.budgetTokens));
                o.set("tool_result_cap", mini::Value::makeInt((int64_t)cfg_.toolResultCap));
                o.set("shell_enabled", mini::Value::makeBool(cfg_.shellEnabled));
                o.set("theme", mini::Value::makeString(cfg_.theme));
                o.set("plugins_dir", mini::Value::makeString(cfg_.pluginsDir));
                o.set("mcp_servers", mini::Value::makeInt((int64_t)cfg_.mcpServers.size()));
                o.set("rules_path", mini::Value::makeString(cfg_.rulesPath));
                o.set("session_dir", mini::Value::makeString(cfg_.sessionDir));
                {
                  std::error_code ec;
                  o.set("cwd", mini::Value::makeString(
                                   std::filesystem::current_path(ec).string()));
                  mini::Value ws = mini::Value::makeArray();
                  for (auto const& w : cfg_.workspaces)
                    ws.arr.push_back(mini::Value::makeString(w));
                  o.set("workspaces", std::move(ws));
                  o.set("active_workspace", mini::Value::makeInt((int64_t)cfg_.activeWorkspace));
                  o.set("workspace_guard", mini::Value::makeBool(cfg_.workspaceGuard));
                }
                o.set("cost_in_per_m", mini::Value::makeDouble(cfg_.costInPerM));
                o.set("cost_out_per_m", mini::Value::makeDouble(cfg_.costOutPerM));
                o.set("cost_budget_usd", mini::Value::makeDouble(cfg_.costBudgetUsd));
                return mini::dump(o);
              }});
  backgroundRunner().setNotifier(
      [this](std::string const& id, int exitCode, bool timedOut,
             std::string const& tail) {
        std::string note = exitCode == 0
                               ? "后台任务 " + id + " 已完成 (exit 0)"
                               : "后台任务 " + id + " 已结束 (exit " +
                                     std::to_string(exitCode) +
                                     (timedOut ? ", 超时自动终止" : "") + ")";
        std::string full = note;
        if (!tail.empty()) full += "\n最新输出:\n" + tail;
        StreamEvent ev;
        ev.kind = StreamKind::Background;
        ev.text = full;
        pushEvent(ev);
        std::lock_guard lk(bgMutex_);
        bgNotes_.push_back(full);
      });
  // Snapshot the always-available tool set; plugin/MCP tools registered below
  // are auto-advertised in makeToolSubset from the start.
  builtinNames_ = tools_.names();
  if (!cfg_.pluginsDir.empty()) {
    std::string e = startPluginsFromDir(cfg_.pluginsDir, tools_, plugins_);
    if (!e.empty()) {
      setStatus("plugins: " + e);
    }
  }
  for (auto const& ms : cfg_.mcpServers) {
    if (!ms.enabled || ms.name.empty() || ms.command.empty()) continue;
    auto m = std::make_unique<PluginProcess>();
    std::string e = m->startMcp(ms);
    if (!e.empty()) {
      setStatus("mcp " + ms.name + ": " + e);
      continue;
    }
    e = m->registerTools(tools_, ms.name + "_");
    if (!e.empty()) {
      setStatus("mcp " + ms.name + ": " + e);
      continue;
    }
    mcp_.push_back(std::move(m));
  }
  if (cfg_.sessionPath.empty() && cfg_.sessionDir.empty()) {
    traj_.open(cfg_.tracePath);
    return true;
  }
  restoreSession();
  traj_.open(cfg_.tracePath);
  return true;
}

// Rebuild the conversation pane from `messages_` (used by restore/switch).
void App::fillConvoFromMessages() {
  convo_.clear();
  for (auto const& m : messages_) {
    if (m.role == "user") {
      convo_.append("\xE2\x9D\xAF " + toPlainText(m.content) + "\n", stUser());
    } else if (m.role == "assistant") {
      for (auto const& tc : m.toolCalls) {
        convo_.append("\xE2\x9A\x99 " + tc.name + " " + excerpt(tc.arguments, 80) + "\n", stTool());
      }
      if (!m.content.empty()) convo_.appendMarkdown(m.content, mdTheme());
    } else if (m.role == "tool") {
      convo_.append("\xE2\x86\xB3 " + excerpt(toPlainText(m.content), 120) + "\n", stToolResult());
    }
  }
  scrollAnchor_ = -1;
}

void App::restoreSession() {
  if (cfg_.sessionPath.empty() && cfg_.sessionDir.empty()) return;
  std::vector<Message> msgs;
  std::string model;
  std::vector<agent::SessionInfo> list;
  std::string target;
  if (!cfg_.sessionPath.empty()) {
    // Legacy single-file mode.
    target = cfg_.sessionPath;
  } else {
    // Multi-session dir: resume the most recently saved session, if any.
    agent::listSessions(cfg_.sessionDir, list);
    if (list.empty()) return;
    for (auto const& s : list) {
      if (s.msgCount > 0 && s.savedAt > 0) {
        target = s.path;
        break;
      }
    }
    if (target.empty()) return;
  }
  int lr = loadSession(target, msgs, model);
  if (lr <= 0) {
    std::string why = lr == 0 ? "file not found"
                     : lr == -1 ? "JSON parse failed"
                                : "messages field missing";
    appendError("session resume failed: " + target + " (" + why + ")");
    return;
  }
  {
    std::lock_guard lk(msgsMutex_);
    messages_ = std::move(msgs);
  }
  fillConvoFromMessages();
  if (!model.empty() && model != cfg_.model) cfg_.model = model;
  if (cfg_.sessionDir.empty()) {
    setStatus("resumed session: " + target);
  } else {
    for (auto const& s : list) {
      if (s.path == target) {
        curSessId_ = s.id;
        curSessTitle_ = s.title;
        break;
      }
    }
    std::string name = curSessTitle_.empty() ? curSessId_ : curSessTitle_;
    if (curSessId_.empty()) curSessId_ = agent::makeSessionId();
    setStatus("resumed session: " + name);
  }
}

void App::persistSession(bool force) {
  if (cfg_.sessionPath.empty() && cfg_.sessionDir.empty()) return;
  // Turn-end saves are throttled: rewriting the whole session file every turn
  // is O(n^2) in total bytes for long conversations. Explicit actions
  // (/save, /new, /clear, exit) always persist.
  if (!force) {
    auto now = std::chrono::steady_clock::now();
    if (now - lastPersist_ < std::chrono::seconds(30)) return;
    lastPersist_ = now;
  }
  std::vector<Message> snapshot;
  {
    std::lock_guard lk(msgsMutex_);
    snapshot = messages_;
  }
  std::string title = curSessTitle_;
  if (title.empty()) {
    // Derive a default title from the first user message.
    for (auto const& m : snapshot) {
      if (m.role != "user") continue;
      std::string line = toPlainText(m.content);
      for (auto& ch : line)
        if (ch == '\n' || ch == '\r') ch = ' ';
      title = excerpt(line, 42);
      break;
    }
  }
  if (!cfg_.sessionPath.empty()) {
    if (!saveSession(cfg_.sessionPath, snapshot, cfg_.model, title)) {
      appendError("failed to save session to " + cfg_.sessionPath);
    }
    return;
  }
  // Multi-session dir: place the conversation in its own file.
  if (curSessId_.empty()) curSessId_ = agent::makeSessionId();
  std::string path = cfg_.sessionDir + "/" + curSessId_ + ".json";
  if (!saveSession(path, snapshot, cfg_.model, title)) {
    appendError("failed to save session to " + path);
  }
  curSessTitle_ = title;
}

std::string App::askUser(std::string const& question) {
  {
    std::unique_lock lk(askMutex_);
    askQuestion_ = question;
    askAnswer_.clear();
    askAnswered_ = false;
  }
  if (cfg_.serveMode) {
    // Headless: the question goes out as an event and the frontend answers with
    // {"type":"answer","text":...}; never hang on a frontend that went away.
    mini::Value q = serveEvent("ask");
    q.set("question", mini::Value::makeString(question));
    serveEmit(q);
  }
  std::unique_lock lk(askMutex_);
  if (cfg_.serveMode) {
    if (!askCv_.wait_for(lk, std::chrono::minutes(5), [this] { return askAnswered_; })) {
      askQuestion_.clear();
      return std::string();
    }
  } else {
    askCv_.wait(lk, [this] { return askAnswered_; });
  }
  std::string ans = askAnswer_;
  askQuestion_.clear();  // leave ask mode in the UI thread
  return ans;
}

bool App::asking() const {
  std::lock_guard lk(askMutex_);
  return !askQuestion_.empty() && !askAnswered_;
}

std::string App::runSubagent(std::string const& description, std::string const& prompt,
                             int maxToolLoops) {
  // Own Client per sub-agent: parallel sub-agents must not share request state.
  agent::Client c;
  c.configure(cfg_.baseUrl, cfg_.apiKey);
  std::vector<Message> child;
  child.emplace_back("user", prompt);
  ChatOptions opts;
  opts.model = cfg_.model;
  opts.stream = false;
  opts.jsonMode = false;
  opts.toolResultCap = cfg_.toolResultCap;
  opts.prune = cfg_.pruneBeforeSend;
  applyThinking(opts);
  opts.systemPrompt =
      "You are a subagent completing a delegated task" +
      (description.empty() ? std::string(".")
                           : " (\"" + description + "\").") +
      " Work toward the goal independently: you have your own conversation and "
      "context budget, do not rely on the main session's history. Use the available "
      "tools; when done, reply with the final answer as plain text and make no more "
      "tool calls.";
  opts.systemPrompt += workspaceNote();
  std::vector<std::string> subset = tools_.enabledNames();
  // No nesting: sub-agents cannot spawn further sub-agents, and asking the user
  // from a parallel worker would deadlock the shared input line.
  subset.erase(std::remove(subset.begin(), subset.end(), "subagent"), subset.end());
  subset.erase(std::remove(subset.begin(), subset.end(), "subagent_parallel"), subset.end());
  subset.erase(std::remove(subset.begin(), subset.end(), "ask_user"), subset.end());
  opts.toolsJson = tools_.toolsJson(subset);
  // Runs on the worker thread (the main agent is parked waiting for this tool).
  int rc = 0;
  try {
    rc = runTurn(
        c, child, opts,
        [this](std::string const& name, std::string const& argsJson) {
          return execTool(name, argsJson);
        },
        [](StreamEvent const&) {}, maxToolLoops);
  } catch (std::exception const& e) {
    mini::Value o = mini::Value::makeObject();
    o.set("ok", mini::Value::makeBool(false));
    o.set("error", mini::Value::makeString(std::string("subagent exception: ") + e.what()));
    return mini::dump(o);
  } catch (...) {
    return "{\"ok\":false,\"error\":\"subagent exception (unknown)\"}";
  }
  if (rc != 0) {
    mini::Value o = mini::Value::makeObject();
    o.set("ok", mini::Value::makeBool(false));
    o.set("error",
          mini::Value::makeString("subagent turn failed (rc " + std::to_string(rc) + ")"));
    return mini::dump(o);
  }
  // Compact even on failure so a long sub-agent cannot blow its own context.
  if (cfg_.budgetTokens > 0) {
    ChatOptions copts;
    copts.model = cfg_.model;
    copts.stream = false;
    copts.systemPrompt = opts.systemPrompt;
    copts.toolResultCap = cfg_.toolResultCap;
    copts.prune = cfg_.pruneBeforeSend;
    try {
      compactConversation(c, child, copts, cfg_.budgetTokens,
                          [](std::string const&) {});
    } catch (...) {
    }
  }
  // The final assistant message without tool calls is the answer.
  for (auto it = child.rbegin(); it != child.rend(); ++it) {
    if (it->role == "assistant" && it->toolCalls.empty()) return it->content;
  }
  return "(no answer)";
}

std::string App::runSubagentsParallel(std::string const& tasksJson) {
  mini::Value v;
  if (!mini::tryParse(tasksJson, v)) return "{\"error\":\"invalid tasks JSON\"}";
  mini::Value const* tasks = v.get("tasks");
  if (!tasks || tasks->type != mini::Value::Array) return "{\"error\":\"tasks must be an array\"}";
  int64_t maxPar = v.has("max_parallel") ? v.get("max_parallel")->asInt(4) : 4;
  if (maxPar < 1) maxPar = 1;
  if (maxPar > 12) maxPar = 12;

  struct Item {
    std::string description;
    std::string prompt;
    int maxLoops = 15;
  };
  std::vector<Item> items;
  for (auto const& t : tasks->arr) {
    if (t.type != mini::Value::Object) continue;
    Item it;
    it.description = t.has("description") ? t.get("description")->asString() : "";
    it.prompt = t.has("prompt") ? t.get("prompt")->asString() : "";
    if (it.prompt.empty()) continue;
    if (t.has("max_tool_loops")) {
      it.maxLoops = (int)t.get("max_tool_loops")->asInt(15);
      if (it.maxLoops < 1) it.maxLoops = 1;
      if (it.maxLoops > 60) it.maxLoops = 60;
    }
    items.push_back(std::move(it));
  }
  if (items.empty()) return "{\"error\":\"no tasks to run\"}";

  std::vector<std::string> results(items.size());
  std::atomic<size_t> next{0};
  size_t threads = std::min<size_t>((size_t)maxPar, items.size());
  if (threads < 1) threads = 1;
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (size_t t = 0; t < threads; t++) {
    pool.emplace_back([this, &items, &results, &next]() {
      for (;;) {
        size_t i = next.fetch_add(1);
        if (i >= items.size()) break;
        results[i] =
            runSubagent(items[i].description, items[i].prompt, items[i].maxLoops);
      }
    });
  }
  for (auto& th : pool) th.join();

  mini::Value out = mini::Value::makeArray();
  for (size_t i = 0; i < items.size(); i++) {
    mini::Value one = mini::Value::makeObject();
    if (!items[i].description.empty())
      one.set("description", mini::Value::makeString(items[i].description));
    one.set("result", mini::Value::makeString(results[i]));
    out.arr.push_back(std::move(one));
  }
  mini::Value res = mini::Value::makeObject();
  res.set("ok", mini::Value::makeBool(true));
  res.set("count", mini::Value::makeInt((int64_t)items.size()));
  res.set("results", std::move(out));
  return mini::dump(res);
}

void App::resize(int cols, int rows) {
  // Only the width decides where lines break, so a height-only drag (the
  // common one) costs nothing. Relayout re-wraps the whole transcript.
  bool const widthChanged = cols != cols_;
  cols_ = cols;
  rows_ = rows;
  screen_.resize(cols, rows);
  screen_.reset();
  convo_.setWidth(cols - 2);
  if (widthChanged) scrollAnchor_ = convo_.relayout(scrollAnchor_);
  // The input sits inside a frame: top border, the text rows, then a bottom
  // border that carries the key hints. One row above the status bar.
  input_.setBox(tui::Rect(2, rows - 3, cols - 4, 4));
  forceFull_ = true;
}

int App::convoHeight() const {
  int r = asking() ? 1 : input_.rowCount();
  if (r > 4) r = 4;
  // Must mirror render()'s convoBox, which reserves the frame's rows.
  bool framed = rows_ >= 9 && cols_ >= 24;
  int h = framed ? (rows_ - 6 - r) : (rows_ - 5 - r);
  return h < 1 ? 1 : h;
}

std::string App::sizeTag() const {
  char b[24];
  snprintf(b, sizeof b, "%dx%d", cols_, rows_);
  return b;
}

// The frame's foot and the small-terminal status bar both end with this, so a
// screenshot of any screen identifies both the build and the terminal size.
std::string App::buildTag() const {
  return std::string("v") + kVersion + " " + sizeTag();
}

void App::overlayChrome(std::string const& title, std::string const& rightTag) {
  tui::drawPageHeader(screen_, tui::Rect{0, 0, cols_, 1}, title, rightTag,
                      stHeader(), stHeaderDim());
  tui::drawFadedRule(screen_, 1, cols_, stRule(),
                     tui::Color::rgb(120, 90, 60), tui::Color::rgb(60, 60, 30), 20);
}

void App::beginStream(tui::Style st) {
  bool md = (st == stAssistant());
  bool wantReasoning = !md;
  if (convo_.streaming()) {
    if (streamReasoning_ == wantReasoning) return;  // already streaming in this style
    convo_.endStream();  // commit previous block, then switch style
  }
  if (wantReasoning) {
    // Reasoning block header: visually separates the model's thinking from
    // the answer so the two never blur together in the scrollback.
    convo_.append("\xE2\x9C\xBB thinking \xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\n", stDim());
    convo_.beginStream(st);
  } else if (md) {
    convo_.beginStreamMarkdown(mdTheme());
  } else {
    convo_.beginStream(st);
  }
  streamReasoning_ = wantReasoning;
}

void App::appendBlock(std::string const& text, tui::Style st, bool asLine) {
  if (asLine) {
    if (convo_.streaming()) convo_.endStream();
    convo_.append(text + "\n", st);
  } else {
    if (convo_.streaming()) convo_.cancelStream();
    convo_.append(text + "\n", st);
    convo_.beginStream(st);
    streamReasoning_ = (st == stReasoning());
  }
}

void App::appendError(std::string const& text) { appendBlock("\xE2\x9C\x98 " + text, stError(), true); }

std::string App::execTool(std::string const& name, std::string const& argsJson) {
  if (!modeAllowsTool(modeFromString(cfg_.mode), name)) {
    mini::Value e = mini::Value::makeObject();
    e.set("error", mini::Value::makeString("tool '" + name + "' is not available in mode '" +
                                           cfg_.mode + "'"));
    return mini::dump(e);
  }
  auto t0 = std::chrono::steady_clock::now();
  std::string result = tools_.run(name, argsJson);
  recordToolRun((int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count());
  noteToolUsed(name);
  return result;
}

namespace {
std::string trimStr(std::string const& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}
int widthOf(std::string const& s) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t used = 0;
    uint32_t cp = tui::utf8Decode(s.c_str() + i, s.size() - i, used);
    if (used == 0) { i++; continue; }
    int cw = (cp >= 0x20 && cp < 0x7F) ? 1 : (int)tui_ww::wcwidth(cp);
    w += cw < 0 ? 0 : cw;
    i += used;
  }
  return w;
}
std::string clipToWidth(std::string const& s, int maxW) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t used = 0;
    uint32_t cp = tui::utf8Decode(s.c_str() + i, s.size() - i, used);
    if (used == 0) break;
    int cw = (cp >= 0x20 && cp < 0x7F) ? 1 : (int)tui_ww::wcwidth(cp);
    if (cw < 0) cw = 0;
    if (w + cw > maxW) break;
    w += cw;
    i += used;
  }
  return s.substr(0, i);
}
}  // namespace

// ---------------------------------------------------------------------------
// Workspaces: multiple roots, active one is the process CWD; tool access that
// resolves outside every root is blocked unless the user approves it (the
// approval reuses the ask_user input channel from the worker thread).
// ---------------------------------------------------------------------------
void App::applyActiveWorkspace() {
  if (cfg_.workspaces.empty()) return;
  if (cfg_.activeWorkspace < 0 || cfg_.activeWorkspace >= (int)cfg_.workspaces.size())
    cfg_.activeWorkspace = 0;
  std::error_code ec;
  std::filesystem::current_path(cfg_.workspaces[(size_t)cfg_.activeWorkspace], ec);
  if (ec) setStatus("workspace chdir failed: " + ec.message());
}

std::string App::workspaceNote() const {
  if (cfg_.workspaces.empty()) return "";
  int idx = cfg_.activeWorkspace;
  if (idx < 0 || idx >= (int)cfg_.workspaces.size()) idx = 0;
  std::string s = "\n\n# Workspace\n";
  s += "Default working root (relative paths resolve here): " +
       cfg_.workspaces[(size_t)idx] + "\nRegistered roots:";
  for (auto const& w : cfg_.workspaces) s += "\n - " + w;
  if (cfg_.workspaceGuard)
    s += "\nAny file/shell operation touching paths outside these roots triggers an "
         "interactive approval prompt to the user and may be denied. Stay inside the "
         "active root unless the task clearly requires otherwise; never guess an "
         "out-of-workspace path without needing the approval.";
  else
    s += "\n(Workspace guard is currently OFF: outside paths are not prompted.)";
  return s;
}

bool App::requestOutsideAccess(std::string const& tool, std::string const& absPath) {
  std::string active = "(未设)";
  if (!cfg_.workspaces.empty()) {
    int i = cfg_.activeWorkspace;
    if (i < 0 || i >= (int)cfg_.workspaces.size()) i = 0;
    active = cfg_.workspaces[(size_t)i];
  }
  std::string q = "⚠ 工作区外操作申请\n  工具: " + tool + "\n  目标: " + absPath +
                  "\n  当前工作区: " + active +
                  "\n  回复 y=仅本次允许 · a=本会话允许该目录 · 其它/Esc=拒绝";
  std::string ans = trimStr(askUser(q));
  if (ans.empty()) return false;
  char c0 = (char)std::tolower((unsigned char)ans[0]);
  std::lock_guard lk(wsMu_);
  if (c0 == 'y') {
    wsApproved_.push_back(agent::normalizeWorkspacePath(absPath));
    return true;
  }
  if (c0 == 'a') {
    std::string parent = agent::normalizeWorkspacePath(
        std::filesystem::path(absPath).parent_path().string());
    wsApproved_.push_back(parent.empty() ? absPath : parent);
    return true;
  }
  return false;
}

ToolRegistry::GuardResult App::workspaceGuard(std::string const& tool,
                                              std::string const& argsJson) {
  ToolRegistry::GuardResult allow;
  if (!cfg_.workspaceGuard || cfg_.workspaces.empty()) return allow;
  // run_code sub-steps dispatch through run() and are guarded individually.
  if (tool == "run_code") return allow;
  std::vector<std::string> targets = agent::toolPathTargets(tool, argsJson);
  if (targets.empty()) return allow;
  std::vector<std::string> roots;
  for (auto const& w : cfg_.workspaces) roots.push_back(agent::normalizeWorkspacePath(w));
  for (auto const& raw : targets) {
    std::string n = agent::normalizeWorkspacePath(raw);
    bool ok = false;
    for (auto const& rr : roots)
      if (agent::pathWithinNormalizedRoot(rr, n)) { ok = true; break; }
    if (!ok) {
      std::lock_guard lk(wsMu_);
      for (auto const& ap : wsApproved_)
        if (agent::pathWithinNormalizedRoot(ap, n)) { ok = true; break; }
    }
    if (ok) continue;
    if (!requestOutsideAccess(tool, n)) {
      ToolRegistry::GuardResult deny;
      deny.verdict = ToolRegistry::GuardVerdict::Deny;
      std::string active;
      if (!cfg_.workspaces.empty()) {
        int i = cfg_.activeWorkspace;
        if (i < 0 || i >= (int)cfg_.workspaces.size()) i = 0;
        active = cfg_.workspaces[(size_t)i];
      }
      deny.message =
          "用户拒绝了工作区外访问: " + n + "（当前工作区: " + active +
          "）。请改用工作区内路径；确有需要请先征询用户或用 /ws add 登记该目录。";
      return deny;
    }
  }
  return allow;
}

void App::runWsCommand(std::string const& sub) {
  auto save = [&]() {
    if (!cfg_.settingsPath.empty()) saveSettingsFile(cfg_.settingsPath, cfg_);
  };
  if (sub.empty() || sub == "list") {
    if (convo_.streaming()) convo_.endStream();
    std::string head = "◍ workspace (" + std::to_string(cfg_.workspaces.size()) +
                       ")  guard=" + (cfg_.workspaceGuard ? "on" : "off") + "\n";
    if (cfg_.workspaces.empty())
      convo_.append(head + "  (未登记工作区：当前不限制路径边界)\n", stDim());
    else {
      convo_.append(head, stDim());
      for (size_t i = 0; i < cfg_.workspaces.size(); i++)
        convo_.append("  " + std::to_string(i + 1) + ".  " + cfg_.workspaces[i] +
                          ((int)i == cfg_.activeWorkspace ? "   ← active" : "") + "\n",
                      stAssistant());
      convo_.append("  /ws use N|PATH · /ws add PATH · /ws rm N · /ws on|off\n", stDim());
    }
    setStatus("/ws 管理工作区；文件与命令默认在 active 工作区内执行");
    return;
  }
  if (sub == "on" || sub == "off") {
    cfg_.workspaceGuard = (sub == "on");
    save();
    setStatus(cfg_.workspaceGuard ? "工作区审批: ON（越界操作需批准）"
                                  : "工作区审批: OFF（不再询问）");
    return;
  }
  if (sub.rfind("add ", 0) == 0) {
    std::string p = trimStr(sub.substr(4));
    if (p.empty()) { setStatus("用法: /ws add PATH"); return; }
    std::error_code ec;
    std::string abs = std::filesystem::absolute(p, ec).string();
    if (ec) { setStatus("无法解析路径: " + p); return; }
    std::string n = agent::normalizeWorkspacePath(abs);
    for (auto const& w : cfg_.workspaces)
      if (agent::normalizeWorkspacePath(w) == n) { setStatus("已是工作区: " + abs); return; }
    cfg_.workspaces.push_back(abs);
    bool first = cfg_.workspaces.size() == 1;
    save();
    if (first) applyActiveWorkspace();
    setStatus("已添加工作区: " + abs + " (共 " + std::to_string(cfg_.workspaces.size()) +
              (first ? ", 已切换)" : ")"));
    return;
  }
  if (sub.rfind("rm ", 0) == 0) {
    std::string arg = trimStr(sub.substr(3));
    int idx = -1;
    bool numeric = !arg.empty();
    for (char ch : arg) if (ch < '0' || ch > '9') numeric = false;
    if (numeric) {
      int i = std::atoi(arg.c_str());
      if (i >= 1 && i <= (int)cfg_.workspaces.size()) idx = i - 1;
    } else {
      std::string n = agent::normalizeWorkspacePath(arg);
      for (size_t i = 0; i < cfg_.workspaces.size(); i++)
        if (agent::normalizeWorkspacePath(cfg_.workspaces[i]) == n) { idx = (int)i; break; }
    }
    if (idx < 0) { setStatus("未找到工作区: " + arg + "（/ws 查看列表）"); return; }
    std::string gone = cfg_.workspaces[(size_t)idx];
    cfg_.workspaces.erase(cfg_.workspaces.begin() + idx);
    if (cfg_.activeWorkspace >= (int)cfg_.workspaces.size()) cfg_.activeWorkspace = 0;
    save();
    if (!cfg_.workspaces.empty()) applyActiveWorkspace();
    setStatus("已移除工作区: " + gone + " (剩 " + std::to_string(cfg_.workspaces.size()) + ")");
    return;
  }
  if (sub.rfind("use ", 0) == 0) {
    std::string arg = trimStr(sub.substr(4));
    int idx = -1;
    bool numeric = !arg.empty();
    for (char ch : arg) if (ch < '0' || ch > '9') numeric = false;
    if (numeric) {
      int i = std::atoi(arg.c_str());
      if (i >= 1 && i <= (int)cfg_.workspaces.size()) idx = i - 1;
    } else {
      std::string n = agent::normalizeWorkspacePath(arg);
      for (size_t i = 0; i < cfg_.workspaces.size(); i++)
        if (agent::normalizeWorkspacePath(cfg_.workspaces[i]) == n) { idx = (int)i; break; }
      if (idx < 0) {
        // not registered: adopt it (like add + use) if it exists as a directory
        std::error_code ec;
        auto abs = std::filesystem::absolute(arg, ec);
        if (!ec && std::filesystem::is_directory(abs, ec)) {
          cfg_.workspaces.push_back(abs.string());
          idx = (int)cfg_.workspaces.size() - 1;
        }
      }
    }
    if (idx < 0) { setStatus("未找到工作区: " + arg + "（/ws 查看列表，或 /ws add PATH）"); return; }
    cfg_.activeWorkspace = idx;
    applyActiveWorkspace();
    save();
    setStatus("工作区 → " + cfg_.workspaces[(size_t)idx]);
    return;
  }
  setStatus("用法: /ws [list|use N|PATH|add PATH|rm N|on|off]");
}

std::vector<std::string> App::makeToolSubset() const {
  static const char* kMinimal[] = {"shell_exec", "edit", "ask_user"};
  Mode m = modeFromString(cfg_.mode);
  if (m == Mode::Minimal) {
    std::vector<std::string> out;
    for (auto const* c : kMinimal) {
      Tool const* t = tools_.find(c);
      if (t && t->enabled) out.push_back(c);
    }
    return out;
  }
  static const char* kCore[] = {
      "shell_exec",          "web_fetch",       "web_search",
      "file_read",           "file_write",      "edit",
      "patch",               "file_list",       "grep",
      "glob",                "ask_user",        "verify",
      "remember",            "recall",          "todowrite",
      "subagent",            "subagent_parallel",
      "background_start",    "background_status", "background_kill",
      "background_list",     "desktop_windows",   "desktop_tree",
      "desktop_click",       "desktop_type",      "desktop_key",
      "desktop_scroll",      "desktop_wait"};
  std::vector<std::string> out;
  for (auto const* c : kCore) {
    Tool const* t = tools_.find(c);
    if (t && t->enabled) out.push_back(c);
  }
  {
    std::lock_guard lk(toolsUsedMutex_);
    for (auto const& n : toolsUsed_) {
      if (std::find(out.begin(), out.end(), n) == out.end() &&
          tools_.find(n) && tools_.find(n)->enabled)
        out.push_back(n);
    }
  }
  for (auto const& n : modeExtraTools(m)) {
    if (std::find(out.begin(), out.end(), n) == out.end()) {
      Tool const* t = tools_.find(n);
      if (t && t->enabled) out.push_back(n);
    }
  }
  // Plugin and MCP tools (registered after init's builtin snapshot) are always
  // advertised once enabled, so an external tool needs no prior use to appear.
  for (auto const& n : tools_.names()) {
    if (std::find(out.begin(), out.end(), n) != out.end()) continue;
    if (std::find(builtinNames_.begin(), builtinNames_.end(), n) != builtinNames_.end()) continue;
    Tool const* t = tools_.find(n);
    if (t && t->enabled) out.push_back(n);
  }
  return out;
}

void App::noteToolUsed(std::string const& name) {
  std::lock_guard lk(toolsUsedMutex_);
  if (name.empty()) return;
  if (std::find(toolsUsed_.begin(), toolsUsed_.end(), name) == toolsUsed_.end())
    toolsUsed_.push_back(name);
}

void App::pushEvent(StreamEvent const& ev) {
  std::lock_guard lk(qmutex_);
  events_.push_back(ev);
}

void App::submit(std::string const& text) {
  std::string t = text;
  if (t.empty()) return;
  if (t[0] == '/') { runCommand(t); return; }
  startTurn(t);
}

void App::startTurn(std::string const& userText) {
  if (busy_.exchange(true)) return;
  if (convo_.streaming()) convo_.endStream();
  // Turn separator: a blank line plus a faint rule keeps consecutive turns
  // visually distinct in the scrollback.
  if (turnNum_ > 0) {
    int w = std::max(8, std::min(cols_ - 2, 60));
    std::string rule;
    rule.reserve((size_t)w * 3);
    for (int i = 0; i < w; i++) rule += "\xE2\x94\x80";  // ─
    convo_.append("\n", stDim());
    convo_.append(rule + "\n", stRule(), /*verbatim=*/true);
  }
  convo_.append("\xE2\x9D\xAF " + userText + "\n", stUser());

  turnNum_++;
  {
    agent::TrajEvent ev;
    ev.kind = agent::TrajKind::TurnStart;
    ev.turn = turnNum_;
    ev.model = cfg_.model;
    traj_.append(ev);
  }
  {
    agent::TrajEvent ev;
    ev.kind = agent::TrajKind::UserMessage;
    ev.turn = turnNum_;
    ev.source = "prompt";
    ev.text = userText;
    traj_.append(ev);
  }

  std::vector<Message> snapshot;
  {
    std::lock_guard lk(msgsMutex_);
    messages_.emplace_back("user", userText);
    snapshot = messages_;
  }
  recordTurn();
  turnStartUsage_ = agent::snapshotUsage();

  ChatOptions opts;
  opts.model = cfg_.model;
  opts.stream = true;
  opts.jsonMode = cfg_.jsonMode;
  {
    std::string sys = cfg_.systemPrompt;
    std::string rules = loadRulesText(cfg_.rulesPath);
    if (!rules.empty()) sys += "\n\n" + rules;
    std::string mi = modeInstructions(modeFromString(cfg_.mode));
    if (!mi.empty()) sys += "\n\n" + mi;
    sys += workspaceNote();
    opts.systemPrompt = std::move(sys);
  }
  opts.toolResultCap = cfg_.toolResultCap;
  opts.prune = cfg_.pruneBeforeSend;
  applyThinking(opts);
  if (!cfg_.jsonMode) opts.toolsJson = tools_.toolsJson(makeToolSubset());

  worker_ = std::thread([this, snapshot, opts]() mutable {
    int rc = 0;
    try {
      // Surface background-task completions to the model at the start of this
      // turn, after the user's new message.
      {
        std::vector<std::string> notes;
        {
          std::lock_guard lk(bgMutex_);
          notes.swap(bgNotes_);
        }
        for (auto const& n : notes) {
          Message m;
          m.role = "user";
          m.content = "[后台任务通知] " + n;
          snapshot.push_back(std::move(m));
        }
      }
      rc = runTurn(
          client_, snapshot, opts,
          [this](std::string const& name, std::string const& argsJson) {
            return execTool(name, argsJson);
          },
          [this](StreamEvent const& ev) { pushEvent(ev); });

      // Compact even when the turn failed (e.g. tool-loop limit): skipping it
      // on rc != 0 let the history grow unbounded across failed turns.
      if (cfg_.budgetTokens > 0) {
        ChatOptions copts;
        copts.model = cfg_.model;
        copts.stream = false;
        copts.systemPrompt = cfg_.systemPrompt + workspaceNote();
        copts.toolResultCap = cfg_.toolResultCap;
        copts.prune = cfg_.pruneBeforeSend;
        compactConversation(client_, snapshot, copts, cfg_.budgetTokens,
                            [this](std::string const& note) {
                              StreamEvent ev;
                              ev.kind = StreamKind::ToolResult;
                              ev.toolName = "context";
                              ev.toolResult = note;
                              pushEvent(ev);
                            });
      }
    } catch (std::exception const& e) {
      StreamEvent err;
      err.kind = StreamKind::Error;
      err.error = std::string("worker exception: ") + e.what();
      pushEvent(err);
      rc = 1;
    } catch (...) {
      StreamEvent err;
      err.kind = StreamKind::Error;
      err.error = "worker exception (unknown)";
      pushEvent(err);
      rc = 1;
    }

    StreamEvent done;
    done.kind = StreamKind::TurnComplete;
    done.turnRc = rc;
    done.turnMessages = std::move(snapshot);
    pushEvent(done);
  });
}

void App::appendTurnEndTraj(int turn, int rc) {
  agent::SessionUsage now = agent::snapshotUsage();
  auto d = [](int64_t a, int64_t b) { return b >= 0 && a >= b ? a - b : -1; };
  agent::TrajEvent te;
  te.kind = agent::TrajKind::TurnEnd;
  te.turn = turn;
  te.rc = rc;
  te.text = rc == 0 ? "completed" : "failed";
  te.promptTokens = d(now.promptTokens, turnStartUsage_.promptTokens);
  te.completionTokens = d(now.completionTokens, turnStartUsage_.completionTokens);
  te.cachedTokens = d(now.cachedTokens, turnStartUsage_.cachedTokens);
  traj_.append(te);
  trajDirty_ = true;
}

void App::settleTrajStream() {
  trajDirty_ = true;
  auto isBlank = [](std::string const& s) {
    for (char ch : s)
      if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') return false;
    return true;
  };
  if (!pendingReasoning_.empty() && !isBlank(pendingReasoning_)) {
    agent::TrajEvent ev;
    ev.kind = agent::TrajKind::Reasoning;
    ev.turn = turnNum_;
    ev.text = pendingReasoning_;
    traj_.append(ev);
  }
  pendingReasoning_.clear();
  if (!pendingDelta_.empty() && !isBlank(pendingDelta_)) {
    agent::TrajEvent ev;
    ev.kind = agent::TrajKind::Assistant;
    ev.turn = turnNum_;
    ev.text = pendingDelta_;
    traj_.append(ev);
  }
  pendingDelta_.clear();
}

bool App::drainEvents(std::vector<StreamEvent>* out) {
  std::vector<StreamEvent> evs;
  {
    std::lock_guard lk(qmutex_);
    evs.swap(events_);
  }
  if (!evs.empty()) trajDirty_ = true;
  // Coalesce consecutive Delta/Reasoning chunks: streaming tokens can arrive
  // several per frame, and every convo_.streamAppend re-parses the whole
  // markdown tail. Merging keeps the UI at a true character-by-character
  // cadence without staggering over repeated full re-parses.
  std::vector<StreamEvent> merged;
  merged.reserve(evs.size());
  {
    std::string rbuf, dbuf;
    auto flush = [&]() {
      if (!rbuf.empty()) {
        StreamEvent e;
        e.kind = StreamKind::Reasoning;
        e.text = rbuf;
        merged.push_back(std::move(e));
        rbuf.clear();
      }
      if (!dbuf.empty()) {
        StreamEvent e;
        e.kind = StreamKind::Delta;
        e.text = dbuf;
        merged.push_back(std::move(e));
        dbuf.clear();
      }
    };
    for (auto const& ev : evs) {
      if (ev.kind == StreamKind::Reasoning) {
        rbuf += stripAnsi(ev.text);
        continue;
      }
      if (ev.kind == StreamKind::Delta) {
        dbuf += stripAnsi(ev.text);
        continue;
      }
      flush();
      merged.push_back(ev);
    }
    flush();
  }
  for (auto const& ev : merged) {
    switch (ev.kind) {
      case StreamKind::Reasoning: {
        std::string safe = stripAnsi(ev.text);
        if (safe.empty()) break;
        pendingReasoning_ += safe;
        beginStream(stReasoning());
        convo_.streamAppend(safe.c_str());
        break;
      }
      case StreamKind::Delta: {
std::string safe = stripAnsi(ev.text);
    if (safe.empty()) break;
    pendingDelta_ += safe;
    beginStream(stAssistant());
    convo_.streamAppend(safe.c_str());
    break;
  }
  case StreamKind::ToolCall: {
    settleTrajStream();
    if (convo_.streaming()) convo_.endStream();
    convo_.append("  \xE2\x9A\x99 " + stripAnsi(ev.toolName) + " " +
                      stripAnsi(excerpt(ev.toolArgs, 80)) + "\n",
                  stTool());
    agent::TrajEvent te;
    te.kind = agent::TrajKind::ToolCall;
    te.turn = turnNum_;
    te.toolName = stripAnsi(ev.toolName);
    te.toolArgs = stripAnsi(ev.toolArgs);
    traj_.append(te);
    break;
  }
      case StreamKind::ToolResult: {
        settleTrajStream();
        if (convo_.streaming()) convo_.endStream();
        std::string res = stripAnsi(ev.toolResult);
        bool blank = true;
        for (char ch : res)
          if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            blank = false;
            break;
          }
        if (blank) res = "[no output]";
        if (res.size() > 4096) res = res.substr(0, 4096) + "\n...[truncated]";
        convo_.append("    \xE2\x86\xB3 " + excerpt(res, 120) + "\n", stToolResult());
        agent::TrajEvent te;
        te.kind = ev.toolName == "context" ? agent::TrajKind::ContextNote
                                           : agent::TrajKind::ToolResult;
        te.turn = turnNum_;
        te.toolName = stripAnsi(ev.toolName);
        te.text = res;
        traj_.append(te);
        break;
      }
      case StreamKind::Background: {
        settleTrajStream();
        if (convo_.streaming()) convo_.endStream();
        if (!ev.text.empty()) {
          convo_.append("  \xE2\xA7\x97 " + excerpt(ev.text, 120) + "\n",
                        stToolResult());
          agent::TrajEvent te;
          te.kind = agent::TrajKind::ContextNote;
          te.turn = turnNum_;
          te.source = "background";
          te.text = ev.text;
          traj_.append(te);
        }
        break;
      }
      case StreamKind::Error: {
        settleTrajStream();
        std::string msg = ev.error.empty() ? "request failed" : ev.error;
        appendError(msg);
        agent::TrajEvent te;
        te.kind = agent::TrajKind::Error;
        te.turn = turnNum_;
        te.text = msg;
        traj_.append(te);
        break;
      }
      case StreamKind::Done:
        break;
      case StreamKind::Models: {
        modelFetching_ = false;
        if (ev.turnRc != 0) {
          appendError(ev.turnRc == -1
                          ? "fetching models failed: " + agent::httpLastErrorDetail()
                          : "fetching models failed: HTTP " + std::to_string(ev.turnRc));
          break;
        }
        std::string rest2 = ev.text;
        size_t start = 0;
        while (start <= rest2.size()) {
          size_t nl = rest2.find('\n', start);
          std::string line =
              nl == std::string::npos ? rest2.substr(start) : rest2.substr(start, nl - start);
          if (!line.empty()) modelList_.push_back(line);
          if (nl == std::string::npos) break;
          start = nl + 1;
        }
        if (convo_.streaming()) convo_.endStream();
        convo_.append("\xE2\x97\xBB models (" + std::to_string(modelList_.size()) + ")\n",
                      stDim());
        for (size_t i = 0; i < modelList_.size(); i++)
          convo_.append("  " + std::to_string(i + 1) + ".  " + modelList_[i] + "\n",
                        stAssistant());
        convo_.append("  切换: /model <名称或编号>\n", stDim());
        setStatus("/model NAME 或 /model 编号");
        break;
      }
      case StreamKind::TurnComplete: {
        settleTrajStream();
        if (convo_.streaming()) convo_.endStream();
        {
          std::lock_guard lk(msgsMutex_);
          messages_ = ev.turnMessages;
        }
persistSession(true);  // exit: never lose the last turn to the save throttle
        busy_ = false;
        if (worker_.joinable()) worker_.join();
        if (ev.turnRc != 0) {
          appendError("turn ended with code " + std::to_string(ev.turnRc));
        }
        appendTurnEndTraj(turnNum_, ev.turnRc);
        setStatus("");
        break;
      }
      default:
        break;
    }
  }
  if (out) *out = merged;
  if (trajOpen_ && trajDirty_) {
    rebuildTrajList();
    trajDirty_ = false;
  }
  return !evs.empty();
}

void App::runCommand(std::string const& line) {
  std::string rest = line.substr(1);
  if (rest == "clear") {
    if (busy_) return;
    {
      std::lock_guard lk(msgsMutex_);
      messages_.clear();
    }
    {
      std::lock_guard lk(toolsUsedMutex_);
      toolsUsed_.clear();
    }
    convo_.clear();
    setStatus("conversation cleared");
    persistSession(true);  // explicit action: bypass the save throttle
    return;
  }
  if (rest == "new") {
    if (busy_) return;
    newSessionNow();
    return;
  }
  if (rest == "sessions" || rest == "ls") {
    if (busy_) {
      setStatus("request in flight; open sessions when it finishes");
      return;
    }
    openSessions();
    return;
  }
  if (rest == "save") {
    // Persist the conversation right now (--session FILE, or the current
    // multi-session file). Safe during a turn: it snapshots under the lock.
    persistSession(true);
    std::string target = cfg_.sessionPath;
    if (target.empty() && !cfg_.sessionDir.empty()) {
      if (curSessId_.empty()) curSessId_ = agent::makeSessionId();
      target = cfg_.sessionDir + "/" + curSessId_ + ".json";
    }
    setStatus(target.empty() ? "no session file configured (--session/--sessions)"
                             : "saved: " + target);
    return;
  }
  if (rest == "compact") {
    if (busy_) return;
    setStatus("compacting...");
    std::vector<Message> snapshot;
    {
      std::lock_guard lk(msgsMutex_);
      snapshot = messages_;
    }
    busy_ = true;
    worker_ = std::thread([this, snapshot]() mutable {
      int rc = 0;
      try {
        ChatOptions copts;
        copts.model = cfg_.model;
        copts.stream = false;
        compactConversation(client_, snapshot, copts, cfg_.budgetTokens,
                            [this](std::string const& note) {
                              StreamEvent ev;
                              ev.kind = StreamKind::ToolResult;
                              ev.toolName = "context";
                              ev.toolResult = note;
                              pushEvent(ev);
                            });
      } catch (std::exception const& e) {
        StreamEvent err;
        err.kind = StreamKind::Error;
        err.error = std::string("compact exception: ") + e.what();
        pushEvent(err);
        rc = 1;
      } catch (...) {
        StreamEvent err;
        err.kind = StreamKind::Error;
        err.error = "compact exception (unknown)";
        pushEvent(err);
        rc = 1;
      }
      StreamEvent done;
      done.kind = StreamKind::TurnComplete;
      done.turnRc = rc;
      done.turnMessages = std::move(snapshot);
      pushEvent(done);
    });
    return;
  }
  if (rest == "json") {
    if (busy_) {
      setStatus("request in flight; toggle JSON mode when it finishes");
      return;
    }
    cfg_.jsonMode = !cfg_.jsonMode;
    setStatus(cfg_.jsonMode ? "JSON mode ON" : "JSON mode OFF");
    return;
  }
  if (rest == "stats") {
    agent::SessionUsage u = agent::snapshotUsage();
    std::string line = agent::formatUsage(u);
    if (convo_.streaming()) convo_.endStream();
    convo_.append("\xE2\x96\x88 " + line + "\n", stBarAccent());
    setStatus("/stats");
    return;
  }
  if (rest == "usage" || rest == "cost") {
    usageOpen_ = true;
    return;
  }
  if (rest == "ws" || rest.rfind("ws ", 0) == 0 || rest == "workspace" ||
      rest.rfind("workspace ", 0) == 0) {
    std::string sub;
    if (rest.rfind("ws ", 0) == 0) sub = trimStr(rest.substr(3));
    else if (rest.rfind("workspace ", 0) == 0) sub = trimStr(rest.substr(10));
    runWsCommand(sub);
    return;
  }
  if (rest == "model" || rest == "models") {
    std::vector<std::string> choices = modelChoices();
    if (!choices.empty()) {
      if (convo_.streaming()) convo_.endStream();
      convo_.append("◍ models (" + std::to_string(choices.size()) + ")\n", stDim());
      int num = 0;
      std::vector<std::string> listed;
      for (auto const& g : cfg_.groups) {
        bool known = g.provider.empty() || g.provider == "default";
        for (auto const& p : cfg_.providers)
          if (p.name == g.provider) known = true;
        convo_.append("  ▸ " + g.name + "  [" +
                          (g.provider.empty() ? std::string("default") : g.provider) +
                          (known ? "" : " 未知服务商!") + "]\n",
                      stBarAccent());
        for (auto const& m : g.models) {
          if (std::find(listed.begin(), listed.end(), m) != listed.end()) continue;
          listed.push_back(m);
          num++;
          std::string row = "    " + std::to_string(num) + ".  " + m;
          if (m == cfg_.model) row += "   ← current";
          convo_.append(row + "\n", stAssistant());
        }
      }
      bool defHeader = false;
      for (auto const& m : cfg_.models) {
        if (std::find(listed.begin(), listed.end(), m) != listed.end()) continue;
        if (!defHeader) {
          convo_.append("  ▸ default  [顶层默认连接]\n", stBarAccent());
          defHeader = true;
        }
        num++;
        std::string row = "    " + std::to_string(num) + ".  " + m;
        if (m == cfg_.model) row += "   ← current";
        convo_.append(row + "\n", stAssistant());
      }
      convo_.append("  切换: /model <编号|名称>（服务商随分组自动切换） · 增删: /model add NAME · /model rm NAME · 服务端列表: /model fetch\n",
                    stDim());
      setStatus("/model NAME 切换 · 前缀 + Tab 补全");
      return;
    }
    fetchModelList();
    return;
  }
  if (rest == "model fetch" || rest == "models fetch") {
    fetchModelList();
    return;
  }
  if (rest.rfind("model add ", 0) == 0) {
    std::string name = trimStr(rest.substr(10));
    if (name.empty()) { setStatus("用法: /model add NAME"); return; }
    for (auto const& m : cfg_.models)
      if (m == name) { setStatus("已在列表中: " + name); return; }
    cfg_.models.push_back(name);
    if (!cfg_.settingsPath.empty()) saveSettingsFile(cfg_.settingsPath, cfg_);
    setStatus("已添加: " + name + " (默认服务商 · 共 " +
              std::to_string(modelChoices().size()) + " 个模型)");
    return;
  }
  if (rest.rfind("model rm ", 0) == 0 || rest.rfind("model remove ", 0) == 0) {
    std::string name = rest.rfind("model rm ", 0) == 0 ? trimStr(rest.substr(9))
                                                      : trimStr(rest.substr(13));
    auto it = std::find(cfg_.models.begin(), cfg_.models.end(), name);
    if (it == cfg_.models.end()) { setStatus("不在列表中: " + name); return; }
    cfg_.models.erase(it);
    if (!cfg_.settingsPath.empty()) saveSettingsFile(cfg_.settingsPath, cfg_);
    setStatus("已移除: " + name + " (剩 " + std::to_string(cfg_.models.size()) + " 个)");
    return;
  }
  if (rest.rfind("model ", 0) == 0) {
    std::string name = trimStr(rest.substr(6));
    if (!name.empty()) {
      bool numeric = true;
      for (char ch : name)
        if (ch < '0' || ch > '9') { numeric = false; break; }
      if (numeric) {
        std::vector<std::string> choices = modelChoices();
        std::vector<std::string> const& lst = !choices.empty() ? choices : modelList_;
        if (!lst.empty()) {
          size_t idx = (size_t)std::atoi(name.c_str());
          if (idx >= 1 && idx <= lst.size()) name = lst[idx - 1];
          else {
            setStatus("无编号 #" + name + " (列表共 " + std::to_string(lst.size()) + " 个)");
            return;
          }
        }
      }
    }
    if (name.empty()) { setStatus("用法: /model NAME 或 /model <编号>"); return; }
    cfg_.model = name;
    std::string prov;
    int prc = applyProviderForModel(name, &prov);
    if (!cfg_.settingsPath.empty()) saveSettingsFile(cfg_.settingsPath, cfg_);
    std::string msg = "model: " + cfg_.model;
    if (prc == 1) msg += "  · 服务商: " + prov;
    else if (prc == 2) msg += "  · 警告: 分组服务商 '" + prov + "' 未定义";
    setStatus(msg +
              (cfg_.settingsPath.empty() ? "  (this session only)" : "  (saved)"));
    return;
  }
  if (rest.rfind("theme ", 0) == 0) {
    std::string name = rest.substr(6);
    if (name != "dark" && name != "light" && name != "terminal" &&
        name != "nord" && name != "gruvbox" && name != "dracula" &&
        name != "solarized") {
      setStatus("theme: dark | light | terminal | nord | gruvbox | dracula | solarized");
      return;
    }
    cfg_.theme = name;
    applyTheme(name, cfg_.themeOverride);
    if (!cfg_.settingsPath.empty()) saveSettingsFile(cfg_.settingsPath, cfg_);
    setStatus("theme: " + name +
              (cfg_.settingsPath.empty() ? "  (this session only)" : "  (saved)"));
    return;
  }
  if (rest == "mode") {
    setStatus("mode: " + cfg_.mode +
              "   (/mode standard | minimal | ptc | creator)");
    return;
  }
  if (rest.rfind("mode ", 0) == 0) {
    std::string name = rest.substr(5);
    Mode m = modeFromString(name);
    if (modeToString(m) != name) {
      setStatus("mode: standard | minimal | ptc | creator");
      return;
    }
    cfg_.mode = name;
    if (!cfg_.settingsPath.empty()) saveSettingsFile(cfg_.settingsPath, cfg_);
    setStatus("mode: " + name +
              (cfg_.settingsPath.empty() ? "  (this session only)" : "  (saved)"));
    return;
  }
  // Thinking strength: one global level, spelled per provider by thinking_style.
  if (rest == "think") {
    ChatOptions probe;
    probe.model = cfg_.model;
    applyThinking(probe);
    setStatus("thinking: " + thinkingLevelFromString(cfg_.thinking) +
              "  写法 " + thinkingStyleFromString(probe.thinkingStyle) +
              "   (/think auto | none | minimal | low | medium | high | max)");
    return;
  }
  if (rest.rfind("think ", 0) == 0) {
    std::string name = trimStr(rest.substr(6));
    std::string lvl = thinkingLevelFromString(name);
    if (lvl != name) {
      setStatus("thinking: auto | none | minimal | low | medium | high | max");
      return;
    }
    cfg_.thinking = lvl;
    if (!cfg_.settingsPath.empty()) saveSettingsFile(cfg_.settingsPath, cfg_);
    std::string msg = "thinking: " + lvl +
                      (cfg_.settingsPath.empty() ? "  (this session only)" : "  (saved)");
    ChatOptions probe;
    probe.model = cfg_.model;
    applyThinking(probe);
    std::string style = thinkingStyleFromString(probe.thinkingStyle);
    if (lvl != "auto" && style == "none")
      msg += "  注意: 当前服务商写法 none，不发参数，档位无效";
    else if (lvl != "auto" && thinkingStyleIsBinary(style))
      msg += "  注意: " + style + " 只分开关，low/high/max 都是 enabled";
    setStatus(msg);
    return;
  }
  if (rest == "mcp") {
    if (mcp_.empty()) {
      setStatus("mcp: no servers configured (add \"mcp_servers\" to settings.json)");
      return;
    }
    std::string out = "MCP servers:";
    for (auto const& m : mcp_) {
      out += "\n  " + m->name() +
             (m->running() ? " [connected, " : " [down, ") +
             std::to_string(m->toolCount()) + " tools]";
    }
    if (convo_.streaming()) convo_.endStream();
    convo_.append(out + "\n", stAssistant());
    return;
  }
  if (rest == "copy" || rest == "copy all") {
    // Copy the last assistant answer to the system clipboard (OSC52 / Win32).
    std::vector<Message> snapshot;
    {
      std::lock_guard lk(msgsMutex_);
      snapshot = messages_;
    }
    std::string out;
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
      if (it->role == "assistant" && !it->content.empty()) {
        out = it->content;
        break;
      }
    }
    if (rest == "copy all") {
      // Whole conversation as a readable transcript.
      out.clear();
      for (auto const& m : snapshot) {
        if (m.role == "user") out += "user: " + toPlainText(m.content) + "\n";
        else if (m.role == "assistant") out += "assistant: " + toPlainText(m.content) + "\n";
        else if (m.role == "tool") out += "tool: " + toPlainText(m.content) + "\n";
      }
    }
    if (out.empty()) {
      setStatus("nothing to copy yet");
      return;
    }
    setStatus(tui::platform::copyClipboard(out)
                  ? "copied " + std::to_string(out.size()) + " chars to clipboard"
                  : "clipboard unavailable (needs a modern terminal)");
    return;
  }
  if (rest == "provider" || rest == "api") {
    if (busy_) {
      setStatus("request in flight; open provider setup when it finishes");
      return;
    }
    openProvider();
    return;
  }
  if (rest == "settings") {
    if (busy_) {
      setStatus("request in flight; open settings when it finishes");
      return;
    }
    openSettings();
    return;
  }
  if (rest == "trajectory" || rest == "trace") {
    openTrajectory();
    return;
  }
  if (rest == "help") {
    if (convo_.streaming()) convo_.endStream();
    printHelp(false);
    return;
  }
  setStatus("unknown command: " + line + "  (/help for list)");
}

void App::handleEvent(tui::Event const& e) {
  if (e.type == tui::EventType::Paste) {
    // bracketed paste: treat as typed text (POSIX). Windows already delivers
    // paste as individual key events, so nothing arrives here.
    if (sessOpen_ || trajOpen_ || settingsOpen_ || usageOpen_) return;
    if (dialog_.active()) return;
    if (providerOpen_) {
      if (providerEditing_) providerInput_.insertText(e.text);
      return;
    }
    if (busy_ || asking()) return;
    input_.insertText(e.text);
    return;
  }
  if (e.type == tui::EventType::Key) {
    if (e.ch == tui::KeyCtrlC) {
      if (busy_) {
        setStatus("request in flight; it will finish. Ctrl+C again to quit");
      } else {
        running_ = false;
      }
      return;
    }
    if (dialog_.active()) {  // modal popup intercepts everything but Ctrl+C
      dialog_.handle(e);
      int r = -1;
      if (dialog_.takeResult(r)) {
        std::function<void(int)> cb;
        cb.swap(dialogCb_);
        forceFull_ = true;
        if (cb) cb(r);
      }
      return;
    }
    if (providerOpen_) {  // provider setup popup: modal field editor
      auto commit = [&]() {
        if (!providerEditing_) return;
        std::string v = providerInput_.text();
        if (providerSel_ == 0) provBase_ = v;
        else if (providerSel_ == 1) provKey_ = v;
        else provModel_ = v;
        providerEditing_ = false;
      };
      auto startEdit = [&]() {
        providerInput_.setText(providerSel_ == 0 ? provBase_
                           : providerSel_ == 1 ? provKey_ : provModel_);
        providerEditing_ = true;
      };
      if (providerEditing_) {
        if (e.ch == tui::KeyEscape) { providerEditing_ = false; return; }
        if (e.ch == tui::KeyEnter) { commit(); return; }
        if (e.ch == tui::KeyTab) { commit(); providerSel_ = (providerSel_ + 1) % 3; startEdit(); return; }
        if (e.ch == tui::KeyCtrlS) { commit(); saveProvider(); return; }
        providerInput_.handle(e);
        return;
      }
      switch (e.ch) {
        case tui::KeyEscape:
          providerOpen_ = false;
          forceFull_ = true;
          setStatus("服务商设置已取消（/provider 重新打开）");
          return;
        case tui::KeyCtrlS:
          saveProvider();
          return;
        case tui::KeyUp:
          providerSel_ = (providerSel_ + 2) % 3;
          return;
        case tui::KeyDown:
        case tui::KeyTab:
          providerSel_ = (providerSel_ + 1) % 3;
          return;
        case tui::KeyEnter:
          startEdit();
          return;
        default:
          return;  // modal: swallow the rest
      }
    }
    if (sessOpen_) {
      if (sessRenaming_) {
        if (e.ch == tui::KeyEscape) {
          sessRenaming_ = false;
          return;
        }
        if (input_.handle(e)) {
          renameSelectedSession(input_.text());
          input_.clear();
          return;
        }
        return;
      }
      if (e.ch == tui::KeyEscape) {
        closeSessions();
        return;
      }
      if (e.ch == 'n') {
        newSessionNow();
        refreshSessList();
        return;
      }
      if (e.ch == 'r') {
        if (sessVisible_.empty()) return;
        sessRenaming_ = true;
        agent::SessionInfo const& s = sessVisible_[sessList_.selected()];
        input_.setText(s.title);
        return;
      }
      if (e.ch == 'd') {
        if (sessVisible_.empty()) return;
        agent::SessionInfo const& s = sessVisible_[sessList_.selected()];
        std::string name = s.title.empty() ? s.id : s.title;
        openDialog("删除会话", "确定删除会话 “" + name + "” ？\n此操作不可恢复。",
                   {"删除", "取消"},
                   [this, name](int r) {
                     if (r == 0) deleteSelectedSession();
                     else setStatus("已取消: " + name);
                   }, 1);
        return;
      }
      if (e.ch == tui::KeyEnter || e.ch == 'v') {
        switchSession(sessList_.selected());
        return;
      }
      {
        int prevSel = sessList_.selected();
        bool nav = (e.ch == tui::KeyUp || e.ch == tui::KeyDown ||
                    e.ch == tui::KeyPageUp || e.ch == tui::KeyPageDown);
        sessList_.handle(e);
        if (nav && sessList_.selected() != prevSel) sessPreviewIdx_ = -1;  // rebuild next frame
      }
      // PgUp/PgDn scroll the preview pane when the list can't scroll further
      if (e.ch == tui::KeyPageDown && !sessList_.needsScroll()) {
        int vis = rows_ - 6;
        sessPreviewScroll_ += vis;
        int pc = sessPreview_.lineCount();
        if (sessPreviewScroll_ > pc - vis) sessPreviewScroll_ = std::max(0, pc - vis);
      } else if (e.ch == tui::KeyPageUp && !sessList_.needsScroll()) {
        int vis = rows_ - 6;
        sessPreviewScroll_ = std::max(0, sessPreviewScroll_ - vis);
      }
      return;
    }
    if (trajOpen_) {
      if (trajDetail_) {
        if (e.ch == tui::KeyEscape || e.ch == 'v' || e.ch == tui::KeyEnter) {
          trajDetail_ = false;
          trajDetailText_.clear();
        } else if (e.ch == tui::KeyUp) {
          trajDetailScroll_ = std::max(0, trajDetailScroll_ - 1);
        } else if (e.ch == tui::KeyDown) {
          trajDetailScroll_++;
        } else if (e.ch == tui::KeyPageUp) {
          int vis = rows_ - 11;
          trajDetailScroll_ = std::max(0, trajDetailScroll_ - vis);
        } else if (e.ch == tui::KeyPageDown) {
          trajDetailScroll_ += (rows_ - 11);
        } else if (e.ch == tui::KeyHome) {
          trajDetailScroll_ = 0;
        } else if (e.ch == tui::KeyEnd) {
          int vis = rows_ - 11;
          trajDetailScroll_ = std::max(0, trajDetailText_.lineCount() - vis);
        }
        return;
      }
      if (e.ch == tui::KeyEscape) {
        closeTrajectory();
        return;
      }
      if (e.ch >= '1' && e.ch <= '5') {
        trajFilter_ = (agent::TrajFilter)((int)agent::TrajFilter::All + (e.ch - '1'));
        rebuildTrajList();
        return;
      }
      if (e.ch == tui::KeyEnter || e.ch == 'v') {
        if (!trajVisible_.empty()) {
          int idx = trajVisible_[trajList_.selected()];
          agent::TrajEvent const& ev = traj_.at(idx);
          int w = cols_ - 2;
          if (w < 10) w = 10;
          trajDetailText_.setWidth(w);
          trajDetailText_.clear();
          std::string head = "#" + std::to_string(ev.seq) + "  " + fmtClock(ev.time) +
                             "  " + agent::trajKindName(ev.kind);
          if (ev.turn > 0) head += "  turn " + std::to_string(ev.turn);
          if (!ev.model.empty()) head += "  model " + ev.model;
          trajDetailText_.append(head, stBarAccent());
          trajDetailText_.append("", stDim());
          if (ev.kind == agent::TrajKind::ToolCall) {
            trajDetailText_.append("tool: " + ev.toolName + "  callId: " + std::to_string(ev.seq), stDim());
            trajDetailText_.append("arguments:", stDim());
            trajDetailText_.append(ev.toolArgs, stAssistant());
          } else if (ev.kind == agent::TrajKind::ToolResult) {
            trajDetailText_.append("tool: " + ev.toolName, stDim());
            trajDetailText_.append(ev.text, stAssistant());
          } else if (ev.kind == agent::TrajKind::TurnEnd) {
            trajDetailText_.append("rc: " + std::to_string(ev.rc), stDim());
            trajDetailText_.append(ev.text, stAssistant());
          } else if (ev.kind == agent::TrajKind::UserMessage && !ev.source.empty()) {
            trajDetailText_.append("source: " + ev.source, stDim());
            trajDetailText_.append(ev.text, stAssistant());
          } else {
            trajDetailText_.append(ev.text, stAssistant());
          }
          trajDetailScroll_ = 0;
          trajDetail_ = true;
        }
        return;
      }
      trajList_.handle(e);
      return;
    }
    if (e.ch == tui::KeyCtrlS) {
      if (busy_) {
        setStatus("request in flight; open settings when it finishes");
      } else {
        openSettings();
      }
      return;
    }
    if (e.ch == tui::KeyCtrlT) {
      openTrajectory();
      return;
    }
    if (e.ch == tui::KeyCtrlO) {
      openSessions();
      return;
    }
    if (e.ch == tui::KeyCtrlG) {
      cycleSession(+1);
      return;
    }
    if (e.ch == tui::KeyCtrlU) {
      usageOpen_ = true;
      return;
    }
    if (e.ch == tui::KeyCtrlP) {
      if (busy_) {
        setStatus("request in flight; palette available when it finishes");
      } else if (asking()) {
        setStatus("answer the question first");
      } else {
        openCommandPalette();
      }
      return;
    }
    if (usageOpen_) {
      if (e.ch == tui::KeyEscape) {
        usageOpen_ = false;
        setStatus("usage closed");
      }
      return;
    }
    if (cmdOpen_) {
      if (e.ch == tui::KeyEscape) {
        cmdFilter_ = input_.text();
        input_.setText(savedInput_);
        cmdOpen_ = false;
        return;
      }
      if (e.ch == tui::KeyEnter) {
        execCommandPalette();
        return;
      }
      if (e.ch == tui::KeyUp) {
        if (cmdSel_ > 0) cmdSel_--;
        return;
      }
      if (e.ch == tui::KeyDown || e.ch == tui::KeyTab) {
        cmdSel_++;
        return;
      }
      input_.handle(e);
      cmdSel_ = 0;  // any edit resets the selection
      return;
    }
    if (settingsOpen_) {
      if (e.ch == tui::KeyEscape) {
        if (settingsForm_.editing()) {
          settingsForm_.cancelEditing();
        } else {
          settingsOpen_ = false;
          setStatus("settings closed (changes discarded)");
        }
        return;
      }
      if (e.ch == tui::KeyCtrlS) {
        saveSettings();
        return;
      }
      settingsForm_.handle(e);
      return;
    }
    if (e.ch == tui::KeyEscape) {
      if (convo_.streaming()) { convo_.cancelStream(); }
      input_.setText("");
      return;
    }
    if (e.ch == tui::KeyPageUp) {
      int h = convoHeight();
      int count = convo_.lineCount();
      int cur = scrollAnchor_ >= 0 ? scrollAnchor_ : std::max(0, count - h);
      scrollAnchor_ = std::max(0, cur - h);
      return;
    }
    if (e.ch == tui::KeyPageDown) {
      int h = convoHeight();
      int count = convo_.lineCount();
      int bottom = std::max(0, count - h);
      int cur = scrollAnchor_ >= 0 ? scrollAnchor_ : bottom;
      int next = std::min(bottom, cur + h);
      scrollAnchor_ = next >= bottom ? -1 : next;
      return;
    }
    if (e.ch == tui::KeyEnd) {
      scrollAnchor_ = -1;
      return;
    }
    if (asking()) {
      // The worker thread is parked waiting for an answer: route typing into
      // the input line; Enter delivers the reply, Escape cancels.
      if (e.ch == tui::KeyEscape) {
        std::unique_lock lk(askMutex_);
        askAnswer_ = "";
        askAnswered_ = true;
        askCv_.notify_all();
        input_.clear();
        return;
      }
      if (input_.handle(e)) {
        std::string ans = input_.text();
        input_.clear();
        agent::TrajEvent te;
        te.kind = agent::TrajKind::UserMessage;
        te.turn = turnNum_;
        te.source = "ask";
        te.text = ans;
        traj_.append(te);
        {
          std::unique_lock lk(askMutex_);
          askAnswer_ = ans;
          askAnswered_ = true;
        }
        askCv_.notify_all();
      }
      return;
    }
    if (busy_) return;  // ignore edits while a turn is running
    if (cmdSuggestActive()) {
      std::string const& tt = input_.text();
      auto sugIdxs = commandMatches(tt.substr(1), true);
      if (!sugIdxs.empty()) {
        if (e.ch == tui::KeyEscape) {
          cmdSug_ = false;
          forceFull_ = true;
          return;
        }
        if (e.ch == tui::KeyTab || e.ch == tui::KeyDown) {
          cmdSugSel_ = (cmdSugSel_ + 1) % (int)sugIdxs.size();
          return;
        }
        if (e.ch == tui::KeyUp) {
          cmdSugSel_ = (cmdSugSel_ + (int)sugIdxs.size() - 1) % (int)sugIdxs.size();
          return;
        }
        if (e.ch == tui::KeyEnter) {
          execSuggest();
          return;
        }
      }
    } else if (cmdArgSuggestActive()) {
      // "/model <prefix>", "/theme <prefix>", "/mode <prefix>": Tab/arrows walk
      // the candidate list; Enter completes (a second Enter submits).
      ArgSug as = argSuggest(input_.text());
      int n = (int)as.cands.size();
      if (n > 0) {
        if (e.ch == tui::KeyEscape) {
          cmdSug_ = false;
          forceFull_ = true;
          return;
        }
        if (e.ch == tui::KeyTab || e.ch == tui::KeyDown) {
          cmdArgSel_ = (cmdArgSel_ + 1) % n;
          return;
        }
        if (e.ch == tui::KeyUp) {
          cmdArgSel_ = (cmdArgSel_ + n - 1) % n;
          return;
        }
        if (e.ch == tui::KeyEnter) {
          if (cmdArgSel_ < 0 || cmdArgSel_ >= n) cmdArgSel_ = 0;
          std::string filled = "/" + as.cmd + " " + as.cands[cmdArgSel_];
          if (input_.text() != filled) {
            input_.setText(filled);
            cmdArgSel_ = 0;
            return;
          }
          // Already exactly the candidate: let Enter submit normally below.
        }
      }
    }
    cmdArgSel_ = 0;  // any plain edit re-derives the arg popup from scratch
    if (input_.handle(e)) {
      std::string t = input_.text();
      input_.clear();
      submit(t);
      return;
    }
    return;
  }
  if (e.type == tui::EventType::Mouse && e.mouse.wheel != 0) {
    if (settingsOpen_ || providerOpen_ || dialog_.active()) return;  // overlays own the wheel
    if (sessOpen_) {
      tui::Event ke;
      ke.type = tui::EventType::Key;
      ke.ch = e.mouse.wheel > 0 ? tui::KeyUp : tui::KeyDown;
      sessList_.handle(ke);
      return;
    }
    if (trajOpen_) {
      if (trajDetail_) {
        int step = e.mouse.wheel > 0 ? 3 : -3;
        trajDetailScroll_ += step;
        int vis = rows_ - 11;
        if (trajDetailScroll_ > trajDetailText_.lineCount() - vis)
          trajDetailScroll_ = std::max(0, trajDetailText_.lineCount() - vis);
        if (trajDetailScroll_ < 0) trajDetailScroll_ = 0;
      } else {
        tui::Event ke;
        ke.type = tui::EventType::Key;
        ke.ch = e.mouse.wheel > 0 ? tui::KeyUp : tui::KeyDown;
        trajList_.handle(ke);
      }
      return;
    }
    // Position-aware wheel: over the input line it recalls history, over the
    // conversation it scrolls. Uses the event's mouse coordinates (Windows
    // Terminal and SGR mouse both carry them).
    int inputY = rows_ - 2;
    if (e.mouse.y == inputY && !asking() && !busy_) {
      input_.historyRecall(e.mouse.wheel > 0 ? 1 : -1);
      return;
    }
    int h = convoHeight();
    int count = convo_.lineCount();
    int bottom = std::max(0, count - h);
    int cur = scrollAnchor_ >= 0 ? scrollAnchor_ : bottom;
    int step = std::max(3, rows_ / 6);
    int next = cur + (e.mouse.wheel > 0 ? -step : step);
    if (next < 0) next = 0;
    if (next > bottom) next = bottom;
    scrollAnchor_ = next >= bottom ? -1 : next;
    return;
  }
  if (busy_) return;
}

void App::openSettings() {
  settingsForm_.setFields(buildSettingsFields(cfg_, tools_));
  settingsForm_.setGroup("connection", 0, 6);
  settingsForm_.setGroup("behavior", 6, 4);
  settingsForm_.setGroup("tools", 10, 0);
  settingsOpen_ = true;
  setStatus("settings: \xE2\x86\x91\xE2\x86\x93 move, Enter edit/toggle, Ctrl+S save, Esc close");
}

void App::saveSettings() {
  settingsForm_.commitEditing();
  std::vector<tui::Field> fields = settingsForm_.fieldsSnapshot();
  applySettingsFields(cfg_, fields, tools_);
  applyTheme(cfg_.theme, cfg_.themeOverride);
  // reconnect the client if base url or key changed
  if (!client_.configure(cfg_.baseUrl, cfg_.apiKey)) {
    setStatus("warning: client credentials no longer valid");
    return;
  }
  if (!cfg_.settingsPath.empty()) {
    if (saveSettingsFile(cfg_.settingsPath, cfg_)) {
      setStatus("settings saved to " + cfg_.settingsPath);
    } else {
      setStatus("failed to write " + cfg_.settingsPath);
    }
  } else {
    setStatus("settings applied for this session (no settings file configured)");
  }
}

void App::openProvider() {
  provBase_ = cfg_.baseUrl;
  provKey_ = cfg_.apiKey;
  provModel_ = cfg_.model;
  providerSel_ = 0;
  providerEditing_ = false;
  providerOpen_ = true;
  forceFull_ = true;
  setStatus("服务商设置: ↑↓/Tab 选择 · Enter 编辑 · Ctrl+S 保存 · Esc 取消");
}

void App::saveProvider() {
  if (busy_) { setStatus("request in flight; save when the turn finishes"); return; }
  std::string base = trimStr(provBase_);
  if (base.empty()) { setStatus("base url 不能为空"); return; }
  provBase_ = base;
  cfg_.baseUrl = base;
  cfg_.defaultBaseUrl = base;  // the popup always edits the default connection
  std::string key = trimStr(provKey_);
  if (key != cfg_.apiKey) {
    cfg_.apiKey = key;
    cfg_.defaultApiKey = key;
    cfg_.apiKeyEnv.clear();  // a literal key now wins over any env var
  } else {
    cfg_.defaultApiKey = key;
  }
  std::string model = trimStr(provModel_);
  if (!model.empty()) {
    cfg_.model = model;
    bool known = false;
    for (auto const& m : cfg_.models)
      if (m == model) { known = true; break; }
    if (!known) cfg_.models.push_back(model);  // keep /model list in sync
  }
  providerOpen_ = false;
  providerEditing_ = false;
  forceFull_ = true;
  if (!client_.configure(cfg_.baseUrl, cfg_.apiKey))
    setStatus("warning: client credentials no longer valid");
  else if (!cfg_.settingsPath.empty()) {
    if (saveSettingsFile(cfg_.settingsPath, cfg_))
      setStatus("服务商已保存: " + cfg_.baseUrl + " · " + cfg_.model);
    else
      setStatus("failed to write " + cfg_.settingsPath);
  } else {
    setStatus("服务商已应用（本会话，无 settings 文件）: " + cfg_.model);
  }
}

void App::openTrajectory() {
  if (settingsOpen_) return;
  rebuildTrajList();
  trajOpen_ = true;
  trajDetail_ = false;
  trajDetailText_.clear();
  setStatus("trajectory: " + (traj_.path().empty() ? "in-memory (no --trace file)" : traj_.path()));
}

void App::closeTrajectory() {
  trajOpen_ = false;
  trajDetail_ = false;
  trajDetailText_.clear();
  setStatus("");
}

void App::rebuildTrajList() {
  trajVisible_.clear();
  for (size_t i = 0; i < traj_.size(); i++) {
    if (agent::trajFilterMatch(trajFilter_, traj_.at(i).kind)) trajVisible_.push_back((int)i);
  }
  int prevSel = trajList_.selected();
  int prevCount = trajList_.count();
  bool atTail = prevSel < 0 || prevSel >= prevCount - 1;

  std::vector<std::string> items;
  std::vector<tui::Style> styles;
  items.reserve(trajVisible_.size());
  styles.reserve(trajVisible_.size());
  int width = std::max(10, cols_ - 2);
  for (int idx : trajVisible_) {
    agent::TrajEvent const& ev = traj_.at(idx);
    std::string line = "#" + std::to_string(ev.seq) + " " + agent::trajKindIcon(ev.kind) + " ";
    switch (ev.kind) {
      case agent::TrajKind::TurnStart:
        line += "turn " + std::to_string(ev.turn);
        if (!ev.model.empty()) line += "  model " + ev.model;
        break;
      case agent::TrajKind::TurnEnd:
        line += "turn " + std::to_string(ev.turn) + " end  rc=" + std::to_string(ev.rc);
        break;
      case agent::TrajKind::UserMessage:
        line += "user";
        if (!ev.source.empty() && ev.source != "prompt") line += "[" + ev.source + "]";
        line += ": " + ev.text;
        break;
      case agent::TrajKind::Reasoning:
        line += "think: " + ev.text;
        break;
      case agent::TrajKind::Assistant:
        line += ev.text;
        break;
      case agent::TrajKind::ToolCall:
        line += ev.toolName + " " + excerpt(ev.toolArgs, 60);
        break;
      case agent::TrajKind::ToolResult:
        line += ev.toolName + "  " + excerpt(ev.text, 120);
        break;
      case agent::TrajKind::ContextNote:
        line += "context: " + excerpt(ev.text, 120);
        break;
      case agent::TrajKind::Error:
        line += ev.text;
        break;
    }
    std::replace(line.begin(), line.end(), '\n', ' ');
    items.push_back(truncCols(line, width));
    styles.push_back(trajStyleOf(ev.kind));
  }
  int sel = prevSel;
  if (atTail || sel >= (int)items.size()) sel = (int)items.size() - 1;
  if (sel < 0) sel = 0;
  trajList_.setItems(std::move(items));
  trajList_.setItemStyles(std::move(styles));
  trajList_.setSelected(sel);
}

void App::renderTrajectory() {
  // header
  std::string title = " \xE2\xA7\x89 trajectory";
  screen_.putText(0, 0, title.c_str(), stHeader());
  if (traj_.isOpen()) {
    screen_.putText((int)title.size(), 0, (" " + traj_.path()).c_str(), stHeaderDim());
  }
  tui::drawRule(screen_, 1, cols_, stRule());

  // filter row
  int fx = 1;
  for (int i = 0; i < 5; i++) {
    agent::TrajFilter f = (agent::TrajFilter)i;
    bool active = f == trajFilter_;
    std::string tag = std::to_string(i + 1);
    std::string open = active ? "[" : " ";
    std::string close = active ? "] " : "  ";
    std::string label = agent::trajFilterName(f);
    if (i > 0) {
      screen_.putText(fx, 2, "  ", stDim());
      fx += 2;
    }
    screen_.putText(fx, 2, open.c_str(), stDim());
    fx += (int)open.size();
    screen_.putText(fx, 2, tag.c_str(), active ? stBarAccent() : stDim());
    fx += (int)tag.size();
    screen_.putText(fx, 2, close.c_str(), stDim());
    fx += (int)close.size();
    screen_.putText(fx, 2, label.c_str(), active ? stBarAccent() : stDim());
    fx += (int)label.size();
  }
  // stats
  char stats[64];
  snprintf(stats, sizeof stats, "%d shown / %zu events", (int)trajVisible_.size(), traj_.size());
  screen_.putText(cols_ - (int)strlen(stats) - 1, 2, stats, stDim());

  // body
  if (trajDetail_) {
    int const dw = std::max(10, cols_ - 2);
    if (trajDetailText_.width() != dw) {
      trajDetailText_.setWidth(dw);
      trajDetailScroll_ = trajDetailText_.relayout(trajDetailScroll_);
    }
    tui::Rect box(1, 3, cols_ - 2, rows_ - 6);
    drawTextView(screen_, box, trajDetailText_, trajDetailScroll_, stAssistant());
  } else {
    tui::Rect box(1, 3, cols_ - 2, rows_ - 6);
    tui::Style selSt = tui::Style::plain().fg(tui::Color::rgb(20, 20, 24)).bg(cGold()).attr(tui::AttrBold, true);
    trajList_.render(screen_, box, stAssistant(), selSt);
  }

  // footer
  int fy = rows_ - 1;
  if (fy >= 0) {
    std::string hints = trajDetail_
                            ? "\xE2\x86\x91\xE2\x86\x93/PgUp/PgDn scroll  Esc back  Home/End top/bottom"
                            : "1-5 filter  \xE2\x86\x91\xE2\x86\x93 move  Enter detail  Esc close";
    tui::drawHintBar(screen_, fy, cols_, hints, sizeTag(), stBarBase());
  }

  tui::platform::hideCursor();
  drawModal();
  renderer_.frame(out_, screen_);
  tui::platform::writeAll(out_.data(), out_.size());
  tui::platform::flush();
}

// ---- multi-session browser ------------------------------------------------

void App::openSessions() {
  if (cfg_.sessionDir.empty()) {
    setStatus("multi-session disabled (no session dir; set AGENT_SESSIONS or --sessions)");
    return;
  }
  refreshSessList();
  sessPreviewIdx_ = -1;
  sessPreviewScroll_ = 0;
  input_.clear();
  sessOpen_ = true;
  sessRenaming_ = false;
  setStatus("sessions: " + cfg_.sessionDir);
}

void App::closeSessions() {
  sessOpen_ = false;
  sessRenaming_ = false;
  input_.setBox(tui::Rect(2, rows_ - 3, cols_ - 4, 4));  // restore main input line
}

void App::refreshSessList() {
  sessVisible_.clear();
  agent::listSessions(cfg_.sessionDir, sessVisible_);
  int prevSel = sessList_.selected();
  int width = std::max(10, cols_ - 2);
  std::vector<std::string> items;
  std::vector<tui::Style> styles;
  items.reserve(sessVisible_.size());
  styles.reserve(sessVisible_.size());
  for (size_t i = 0; i < sessVisible_.size(); i++) {
    agent::SessionInfo& s = sessVisible_[i];
    bool cur = s.id == curSessId_;
    s.current = cur;
    std::string line = cur ? "\xE2\x96\xB8 " : "   ";  // ▶ marker
    if (s.title.empty()) {
      line += "untitled";
    } else {
      line += s.title;
    }
    line += "  \xE2\x9A\xAB " + std::to_string(s.msgCount) + " msgs";
    std::time_t t = (std::time_t)s.savedAt;
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char tb[24];
    std::snprintf(tb, sizeof tb, "%02d-%02d %02d:%02d", tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min);
    line += "  " + std::string(tb);
    if (!s.model.empty()) line += "  " + s.model;
    items.push_back(truncCols(line, width));
    styles.push_back(cur ? stBarAccent() : stDim());
  }
  if (items.empty()) {
    items.push_back(truncCols("(no sessions yet - press n to start one)", width));
    styles.push_back(stDim());
  }
  int sel = prevSel;
  if (sel < 0 || sel >= (int)items.size()) sel = 0;
  sessList_.setItems(std::move(items));
  sessList_.setItemStyles(std::move(styles));
  sessList_.setSelected(sel);
  sessPreviewIdx_ = -1;  // force preview rebuild on next render
}

void App::updateSessPreview() {
  int sel = sessList_.selected();
  if (sessVisible_.empty() || sel < 0 || sel >= (int)sessVisible_.size()) {
    sessPreview_.clear();
    sessPreviewIdx_ = -1;
    return;
  }
  if (sel == sessPreviewIdx_) return;  // unchanged
  agent::SessionInfo const& s = sessVisible_[sel];
  std::vector<Message> msgs;
  std::string model;
  sessPreview_.clear();
  int w = std::max(10, cols_ - 2);
  sessPreview_.setWidth(w);
  std::string head = s.title.empty() ? "(untitled)" : s.title;
  sessPreview_.append("  " + head, stHeader());
  sessPreview_.append("  id: " + s.id + "   " + std::to_string(s.msgCount) + " messages", stHeaderDim());
  sessPreview_.append("", stDim());
  if (s.id == curSessId_) {
    sessPreview_.append("  \xE2\x96\xB8 current session  (shown live in the chat)", stBarBusy());
    sessPreview_.append("", stDim());
  }
  if (agent::loadSession(s.path, msgs, model) <= 0) {
    sessPreview_.append("  (unreadable session file)", stError());
    sessPreviewIdx_ = sel;
    return;
  }
  if (!model.empty()) {
    sessPreview_.append("  model: " + model, stHeaderDim());
    sessPreview_.append("", stDim());
  }
  std::string promptText = "  \xE2\x9D\xAF ";
  std::string replyPrefix = "  \xE2\x97\x8E ";
  std::string toolPrefix = "    \xE2\x86\xB3 ";
  for (auto const& m : msgs) {
    if (m.role == "user") {
      sessPreview_.append(promptText + compressNewlines(toPlainText(m.content)), stUser());
    } else if (m.role == "assistant") {
      sessPreview_.append(replyPrefix + compressNewlines(toPlainText(m.content)), stAssistant());
    } else if (m.role == "tool") {
      std::string name = m.toolCallId;
      sessPreview_.append(toolPrefix + "tool " + (name.empty() ? "?" : name), stTool());
    }
    sessPreview_.append("", stDim());
  }
  sessPreviewIdx_ = sel;
  sessPreviewScroll_ = 0;
}

std::string App::compressNewlines(std::string s) const {
  size_t pos = 0;
  while ((pos = s.find('\n')) != std::string::npos) {
    size_t i = pos;
    while (i < s.size() && s[i] == '\n') i++;
    s.replace(pos, i - pos, " ");
  }
  return s;
}

void App::switchSession(int idx) {
  if (busy_) return;  // never swap the conversation mid-turn
  if (idx < 0 || idx >= (int)sessVisible_.size()) return;
  agent::SessionInfo const& s = sessVisible_[idx];
  std::vector<Message> msgs;
  std::string model;
  if (agent::loadSession(s.path, msgs, model) <= 0) {
    setStatus("session load failed: " + s.path);
    return;
  }
  // Save the current conversation before switching away.
  persistSession(true);
  {
    std::lock_guard lk(msgsMutex_);
    messages_ = std::move(msgs);
  }
  fillConvoFromMessages();
  curSessId_ = s.id;
  curSessTitle_ = s.title;
  setStatus("switched to session: " + (s.title.empty() ? s.id : s.title));
  closeSessions();
}

void App::cycleSession(int delta) {
  if (busy_) {
    setStatus("请求进行中,等它结束再切会话");
    return;
  }
  if (cfg_.sessionDir.empty()) {
    setStatus("未启用多会话(启动时加 --sessions DIR)");
    return;
  }
  persistSession(true);  // the live conversation must be resumable once we leave
  std::vector<agent::SessionInfo> list;
  agent::listSessions(cfg_.sessionDir, list);
  if (list.size() < 2) {
    setStatus("会话目录里只有当前这一个会话");
    return;
  }
  int cur = 0;
  for (size_t i = 0; i < list.size(); i++) {
    if (list[i].id == curSessId_) {
      cur = (int)i;
      break;
    }
  }
  int nxt = (cur + delta + (int)list.size()) % (int)list.size();
  sessVisible_ = list;  // switchSession indexes this
  switchSession(nxt);
}

void App::newSessionNow() {
  // Persist any in-progress conversation to its own file first.
  persistSession(true);
  {
    std::lock_guard lk(msgsMutex_);
    messages_.clear();
  }
  convo_.clear();
  curSessId_.clear();
  curSessTitle_.clear();
  setStatus("new session started");
  if (sessOpen_) {
    refreshSessList();
  }
}

void App::renameSelectedSession(std::string const& newTitle) {
  if (sessVisible_.empty()) return;
  agent::SessionInfo const& s = sessVisible_[sessList_.selected()];
  std::vector<Message> msgs;
  std::string model;
  if (agent::loadSession(s.path, msgs, model) <= 0) {
    setStatus("rename failed: cannot read " + s.path);
    return;
  }
  if (!saveSession(s.path, msgs, model, newTitle)) {
    setStatus("rename failed: cannot write " + s.path);
    return;
  }
  if (s.id == curSessId_) curSessTitle_ = newTitle;
  setStatus("renamed session: " + newTitle);
  refreshSessList();
  sessRenaming_ = false;
}

void App::openDialog(std::string const& title, std::string const& message,
                     std::vector<std::string> const& buttons,
                     std::function<void(int)> onResult, int defaultBtn) {
  dialog_.open(title, message, buttons, defaultBtn);
  dialogCb_ = std::move(onResult);
  forceFull_ = true;
}

void App::drawModal() {
  if (dialog_.active()) {
    tui::Style canvas = tui::Style::plain().bg(tui::Color::rgb(24, 28, 38));
    tui::Style selSt = tui::Style::plain()
                           .fg(tui::Color::rgb(16, 22, 34))
                           .bg(tui::Color::rgb(122, 162, 247))
                           .attr(tui::AttrBold, true);
    tui::Rect full{0, 0, cols_, rows_};
    dialog_.render(screen_, full, stRule(), stBarAccent(), stAssistant(), canvas, selSt);
    return;
  }
  if (!providerOpen_) return;
  // Provider setup popup: framed box, three labeled field rows, hint line.
  const char* labels[3] = {"base url", "api key", "model"};
  std::string vals[3] = {provBase_, provKey_, provModel_};
  bool masked[3] = {false, true, false};
  int labelW = 9;
  int inner = widthOf("服务商设置") + 2;
  for (int i = 0; i < 3; i++) {
    std::string v = masked[i] && !vals[i].empty() && i != providerSel_
                        ? std::string(vals[i].size() > 24 ? 24 : vals[i].size(), '*')
                        : vals[i];
    inner = std::max(inner, labelW + 2 + widthOf(v));
  }
  const char* hint = "↑↓/Tab 选择  Enter 编辑  Ctrl+S 保存  Esc 取消";
  inner = std::max(inner, widthOf(hint));
  int cap = cols_ - 8;
  if (cap < 24) cap = 24;
  if (inner > cap) inner = cap;
  int boxW = inner + 4;
  int boxH = 9;  // border + title + gap + 3 fields + gap + hint + border
  int bx = (cols_ - boxW) / 2;
  int by = (rows_ - boxH) / 2;
  if (bx < 0) bx = 0;
  if (by < 0) by = 0;
  tui::Style canvas = tui::Style::plain().bg(tui::Color::rgb(24, 28, 38));
  tui::Style valueSt = tui::Style::plain()
                           .fg(tui::Color::rgb(230, 233, 240))
                           .bg(tui::Color::rgb(24, 28, 38));
  tui::Style selRowSt = tui::Style::plain()
                           .fg(tui::Color::rgb(16, 22, 34))
                           .bg(tui::Color::rgb(122, 162, 247))
                           .attr(tui::AttrBold, true);
  tui::Rect box{bx, by, boxW, boxH};
  screen_.fill(box, tui::Cell{0, canvas});
  tui::drawBorder(screen_, box, stRule());
  screen_.putText(bx + 2, by + 1, "服务商设置", stBarAccent());
  for (int i = 0; i < 3; i++) {
    int y = by + 3 + i;
    bool sel = i == providerSel_;
    tui::Style ls = sel ? selRowSt : stDim();
    for (int x = bx + 1; x < bx + boxW - 1; x++) screen_.put(x, y, ' ', sel ? selRowSt : canvas);
    screen_.putText(bx + 2, y, labels[i], ls);
    int vx = bx + 2 + labelW;
    int vw = boxW - (vx - bx) - 2;
    if (vw < 2) continue;
    if (sel && providerEditing_) {
      providerInput_.setBox(tui::Rect{vx, y, vw, 1});
      providerInput_.render(screen_, "", valueSt, sel ? selRowSt : valueSt, stCursor());
    } else {
      std::string v = masked[i] && !(sel && providerEditing_) && !vals[i].empty()
                          ? std::string(vals[i].size() > 24 ? 24 : vals[i].size(), '*')
                          : vals[i];
      screen_.putText(vx, y, clipToWidth(v, vw).c_str(), sel ? selRowSt : valueSt);
    }
  }
  screen_.putText(bx + 2, by + boxH - 2, hint, stDim());
}

void App::deleteSelectedSession() {
  if (sessVisible_.empty()) return;
  agent::SessionInfo const& s = sessVisible_[sessList_.selected()];
  std::error_code ec;
  std::filesystem::remove(s.path, ec);
  if (ec) {
    setStatus("delete failed: " + ec.message());
    return;
  }
  bool wasCurrent = s.id == curSessId_;
  refreshSessList();
  if (wasCurrent) {
    // The active conversation just disappeared: fall back to a fresh session.
    {
      std::lock_guard lk(msgsMutex_);
      messages_.clear();
    }
    convo_.clear();
    curSessId_.clear();
    curSessTitle_.clear();
    setStatus("deleted active session; started a fresh session");
  } else {
    setStatus("deleted session: " + (s.title.empty() ? s.id : s.title));
  }
}

void App::renderSessions() {
  std::string title = " \xE2\x9C\x85 sessions";
  if (cfg_.sessionDir.empty()) title += "  (no session dir)";
  overlayChrome(title, cfg_.sessionDir.empty() ? "" : " " + cfg_.sessionDir);

  int splitX = cols_ * 2 / 5;  // list occupies the left ~40%
  if (splitX < 24) splitX = cols_ / 2;
  if (splitX > cols_ - 8) splitX = cols_ - 8;

  // left: session list
  tui::Rect box(1, 2, splitX - 2, rows_ - 6);
  tui::Style selSt = tui::Style::plain().fg(tui::Color::rgb(20, 20, 24)).bg(cGold()).attr(tui::AttrBold, true);
  sessList_.render(screen_, box, stAssistant(), selSt);

  // vertical divider
  for (int r = 2; r < rows_ - 2; r++) screen_.put(splitX, r, 0x2502, stRule());

  // right: preview of the selected session
  updateSessPreview();
  if (sessVisible_.empty()) {
    screen_.putText(splitX + 2, 3, "Select a session on the left to preview it.", stDim());
  } else {
    // header + scrollable preview text
    int const pw = std::max(10, cols_ - splitX - 3);
    if (sessPreview_.width() != pw) {
      sessPreview_.setWidth(pw);
      sessPreviewScroll_ = std::max(0, sessPreview_.relayout(sessPreviewScroll_));
    }
    tui::Rect pbox(splitX + 1, 2, cols_ - splitX - 2, rows_ - 4);
    int pc = sessPreview_.lineCount();
    int vis = pbox.h;
    int first = sessPreviewScroll_;
    if (first + vis > pc) first = std::max(0, pc - vis);
    if (first < 0) first = 0;
    for (int rr = 0; rr < vis; rr++) {
      int li = first + rr;
      if (li >= pc) break;
      tui::Style st = sessPreview_.styleAt((size_t)li);
      std::string ln = sessPreview_.line((size_t)li);
      // truncate to pane width
      int w = pbox.w;
      std::string out = truncCols(ln, w);
      screen_.putText(pbox.x, pbox.y + rr, out.c_str(), st);
    }
  }

  // rename prompt line
  int ry = rows_ - 4;
  if (ry >= 2) {
    if (sessRenaming_) {
      std::string prompt = "\xE2\x9C\x8F rename to: ";
      int pcols = (int)prompt.size();
      screen_.putText(1, ry, prompt.c_str(), stPrompt());
      input_.setBox(tui::Rect(1 + pcols, ry, std::max(10, cols_ - 2 - pcols), 1));
      input_.render(screen_, "", stPrompt(), stInputText(), stCursor());
    } else {
      std::string note = "Enter switch  \xC2\xB7  n new  \xC2\xB7  r rename  \xC2\xB7  d delete  \xC2\xB7  Esc close";
      screen_.putText(1, ry, note.c_str(), stDim());
    }
  }

  // footer
  int fy = rows_ - 1;
  if (fy >= 0) {
    std::string hints;
    if (sessRenaming_) {
      hints = "Enter commit  Esc cancel";
    } else {
      hints = "\xE2\x86\x91\xE2\x86\x93 move  ";
      if (sessVisible_.empty()) hints += "n to create";
      else hints += "Enter switch  n new  r rename  d delete  PgUp/PgDn preview scroll";
      hints += "  Esc close";
    }
    tui::drawHintBar(screen_, fy, cols_, hints, sizeTag(), stBarBase());
  }

  tui::platform::hideCursor();
  drawModal();
  renderer_.frame(out_, screen_);
  tui::platform::writeAll(out_.data(), out_.size());
  tui::platform::flush();
  if (sessRenaming_) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "\x1b[%d;%dH", input_.cursorScreenY() + 1,
                     input_.cursorScreenX() + 1);
    tui::platform::writeAll(tmp, (size_t)n);
    tui::platform::showCursor();
  }
}

namespace {

// A 12-cell ring around (cx, cy) (radius 2), filled clockwise from the top in
// 4 steps per cell (48 steps total). `pct` is clamped to 0..100. Center text
// is written by the caller onto rows cy-1/cy.
void drawRing(tui::Screen& sc, int cx, int cy, double pct, tui::Style lit,
              tui::Style dim) {
  static std::pair<int, int> const ring[12] = {
      {0, -2}, {1, -2}, {2, -1}, {2, 0}, {2, 1}, {1, 2},
      {0, 2},  {-1, 2}, {-2, 1}, {-2, 0}, {-2, -1}, {-1, -2}};
  static char const* const frac[4] = {"\xE2\x97\x90", "\xE2\x97\x93",
                                      "\xE2\x97\x91", "\xE2\x97\x92"};  // ◐◓◑◒
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int n = (int)(pct * 48.0 / 100.0 + 0.5);
  if (n > 48) n = 48;
  for (int i = 0; i < 12; i++) {
    int k = n - i * 4;
    if (k > 4) k = 4;
    if (k < 0) k = 0;
    char const* glyph;
    tui::Style st;
    if (k == 0) {
      glyph = "\xC2\xB7";  // ·
      st = dim;
    } else if (k == 4) {
      glyph = "\xE2\x97\x8F";  // ●
      st = lit;
    } else {
      glyph = frac[k - 1];
      st = lit;
    }
    sc.putText(cx + ring[i].first, cy + ring[i].second, glyph, st);
  }
}

// Display width in terminal cells. Decoding first matters: the old byte
// heuristic counted a 3-byte CJK character as 6 columns, which mis-sized the
// input frame's borders and pushed the right-hand label on top of the hints.
int displayWidth(std::string const& s) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t con = 0;
    uint32_t cp = tui::utf8Decode(s.data() + i, s.size() - i, con);
    if (con == 0) con = 1;  // undecodable byte: count one column, advance one
    int cw = (int)tui_ww::wcwidth(cp);
    if (cw < 0) cw = 1;
    w += cw;
    i += con;
  }
  return w;
}

// Shorten `s` until it fits `max` display columns. Trims whole UTF-8
// sequences: pop_back() would cut a 3-byte CJK character in half and put a
// stray continuation byte on screen.
std::string trimDisplay(std::string s, int max) {
  while (displayWidth(s) > max && !s.empty()) {
    size_t k = s.size() - 1;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    s.erase(k);
  }
  return s;
}

}  // namespace

void App::renderUsage() {
  tui::Style dim = tui::Style::plain().fg(tui::Color::index(237));

  std::string title = " \xE2\x97\x88 usage";
  overlayChrome(title, "  Ctrl+U / /usage  \xC2\xB7  Esc close");

  agent::SessionUsage u = agent::snapshotUsage();
  double cost = agent::estimateCostUsd(u, cfg_.costInPerM, cfg_.costOutPerM);
  int cache = agent::cacheHitPct(u);
  double costPct =
      cfg_.costBudgetUsd > 0 ? cost / cfg_.costBudgetUsd * 100.0 : 0.0;

  int cy = 6;
  int cx1 = 12;
  int cx2 = cols_ / 2 + 6;

  auto center = [&](int cx, std::string const& l1, std::string const& l2,
                    tui::Style st1, tui::Style st2) {
    screen_.putText(cx - displayWidth(l1) / 2, cy - 1, l1.c_str(), st1);
    screen_.putText(cx - displayWidth(l2) / 2, cy, l2.c_str(), st2);
  };

  tui::Style costSt = cfg_.costBudgetUsd > 0 && cost >= cfg_.costBudgetUsd
                          ? stError()
                          : stBarAccent();
  drawRing(screen_, cx1, cy, costPct, costSt, dim);
  center(cx1, agent::formatUsd(cost),
         cfg_.costBudgetUsd > 0 ? "成本 / 预算" : "成本 (无预算)", costSt,
         stHeaderDim());

  tui::Style cacheSt = cache < 0 ? stHeaderDim()
                       : cache >= 90 ? stBarBusy()
                       : cache >= 50 ? stBarAccent()
                                     : stError();
  drawRing(screen_, cx2, cy, cache < 0 ? 0.0 : (double)cache, cacheSt, dim);
  center(cx2, cache < 0 ? "--" : std::to_string(cache) + "%", "缓存命中率",
         cacheSt, stHeaderDim());

  // labels under the rings
  std::string costLabel = cfg_.costBudgetUsd > 0
                              ? "  $/M in " + std::to_string(cfg_.costInPerM) +
                                    "  \xC2\xB7  out " +
                                    std::to_string(cfg_.costOutPerM) +
                                    "  \xC2\xB7  budget $" +
                                    std::to_string((long long)cfg_.costBudgetUsd)
                              : "  no budget configured";
  screen_.putText(cx1 - displayWidth(costLabel) / 2, cy + 3, costLabel.c_str(),
                  stDim());
  std::string cacheLabel =
      u.promptTokens > 0
          ? "  " + agent::formatTokens(u.cachedTokens) + " / " +
                agent::formatTokens(u.promptTokens) + " prompt tokens cached"
          : "  no usage reported by the API yet";
  screen_.putText(cx2 - displayWidth(cacheLabel) / 2, cy + 3,
                  cacheLabel.c_str(), stDim());

  // details
  int dy = cy + 6;
  if (dy >= rows_ - 1) dy = 3;
  screen_.putText(1, dy, "  session", stHeader());
  for (int c = 1; c < cols_ - 1; c++) screen_.put(c, dy + 1, 0x2500, stRule());
  auto putRow = [&](int y, std::string const& s, tui::Style st) {
    std::string t = s;
    if (displayWidth(t) > cols_ - 4) {
      while (displayWidth(t) > cols_ - 4) t.pop_back();
    }
    screen_.putText(2, y, t.c_str(), st);
  };
  putRow(dy + 2, agent::formatUsage(u), stAssistant());
  putRow(dy + 4,
         "  输入 " + agent::formatTokens(u.promptTokens) + "  \xC2\xB7  输出 " +
             agent::formatTokens(u.completionTokens) + "  \xC2\xB7  " +
             std::to_string(u.calls) + " 次调用  \xC2\xB7  平均首 token " +
             agent::formatDuration(u.calls > 0 ? u.firstTokenSumMs / u.calls : 0),
         stDim());
  putRow(dy + 5,
         "  估算成本 " + agent::formatUsd(cost) + "  (tokens \xC3\x97 $/M, 可在 /settings 调整)",
         stDim());

  // footer
  int fy = rows_ - 1;
  if (fy >= 0) {
    std::string hints = "Esc close  \xC2\xB7  /settings 调价格/预算  \xC2\xB7  /stats 一行进对话";
    tui::drawHintBar(screen_, fy, cols_, hints, sizeTag(), stBarBase());
  }

  tui::platform::hideCursor();
  drawModal();
  renderer_.frame(out_, screen_);
  tui::platform::writeAll(out_.data(), out_.size());
  tui::platform::flush();
}

namespace {

struct CmdDef {
  char const* name;      // command name after '/'
  char const* desc;      // palette description
  char const* key;       // shortcut hint ("" when none)
  bool needsArg;         // requires a parameter (palette fills the input line)
};

CmdDef const kCommands[] = {
    {"help", "本指引(命令与快捷键一览)", "", false},
    {"clear", "清空当前对话", "", false},
    {"new", "开始新会话", "", false},
    {"compact", "压缩上下文(减少 token 用量)", "", false},
    {"model", "切换模型(服务商随分组自动切换) NAME|编号|add|rm|fetch", "", true},
    {"theme", "切换主题 dark|light|terminal|nord|gruvbox|dracula|solarized", "", true},
    {"mode", "切换运行模式 standard|minimal|ptc|creator", "", true},
    {"think", "模型思考强度 auto|none|minimal|low|medium|high|max", "", true},
    {"mcp", "MCP 服务器连接状态", "", false},
    {"ws", "工作区: /ws list|use N|add PATH|rm N|on|off", "", true},
    {"json", "切换 JSON 输出模式", "", false},
    {"provider", "弹窗配置服务商 base_url / key / 模型", "", false},
    {"settings", "设置(价格/预算/工具开关)", "Ctrl+S", false},
    {"usage", "用量与成本环", "Ctrl+U", false},
    {"stats", "一行用量统计(写入对话)", "", false},
    {"trajectory", "事件轨迹查看", "Ctrl+T", false},
    {"sessions", "会话列表", "Ctrl+O", false},
    {"save", "立即保存会话到文件", "", false},
    {"copy", "复制最近回答到剪贴板(/copy all 复制全场)", "", false},
};

std::string lowerAscii(std::string const& s) {
  std::string r = s;
  for (auto& c : r)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return r;
}

// --serve: the desktop palette lists the TUI's own command table rather than
// keeping a second copy that can drift out of sync with kCommands.
void serveEmitCommands() {
  mini::Value list = mini::Value::makeArray();
  for (auto const& c : kCommands) {
    mini::Value row = mini::Value::makeObject();
    row.set("name", mini::Value::makeString(std::string("/") + c.name));
    row.set("desc", mini::Value::makeString(c.desc));
    row.set("key", mini::Value::makeString(c.key));
    row.set("needsArg", mini::Value::makeBool(c.needsArg));
    list.arr.push_back(row);
  }
  mini::Value o = serveEvent("commands");
  o.set("list", list);
  serveEmit(o);
}

}  // namespace

// The empty-screen card shown instead of a wall of text: wordmark, what it is
// connected to, and the handful of keys worth knowing. Only shown when the
// conversation has nothing in it, so resuming a session stays clean.
void App::printWelcome() {
  if (convoHeight() < 6) return;  // too short for chrome: leave every row for content
  tui::Glyphs const g = tui::glyphsFor(caps_);
  bool const uni = caps_.unicode;
  auto utf8 = [](char32_t cp) {
    std::string s;
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) {
      s += (char)(0xC0 | (cp >> 6));
      s += (char)(0x80 | (cp & 0x3F));
    } else {
      s += (char)(0xE0 | (cp >> 12));
      s += (char)(0x80 | ((cp >> 6) & 0x3F));
      s += (char)(0x80 | (cp & 0x3F));
    }
    return s;
  };
  std::string const hLine = utf8(g.hLine), vLine = utf8(g.vLine);
  std::string const tl = utf8(g.tlR), tr = utf8(g.trR), bl = utf8(g.blR), br = utf8(g.brR);

  int const usable = std::max(10, cols_ - 2);  // the pane starts at column 1
  int boxW = usable - 8;
  if (boxW > 56) boxW = 56;
  if (boxW < 24) boxW = 24;
  if (boxW > usable - 2) boxW = usable - 2;
  int const left = std::max(0, (usable - (boxW + 4)) / 2);
  auto fit = [](std::string s, int max) { return trimDisplay(s, max); };
  auto row = [&](std::string const& s, tui::Style st = tui::Style::plain()) {
    std::string body = fit(s, boxW);
    int pad = boxW - displayWidth(body);
    if (pad < 0) pad = 0;
    // No trailing '\n': TextView::append wraps its argument, and a final
    // newline would become an extra empty line that doubles the card height.
    // verbatim: the padded row IS the artwork, so reflow must not break it.
    convo_.append(std::string(left, ' ') + vLine + " " + body + std::string(pad, ' ') + " " + vLine,
                  st, true);
  };
  auto center = [&](std::string const& s) {
    int off = (boxW - displayWidth(s)) / 2;
    if (off < 0) off = 0;
    return std::string(off, ' ') + s;
  };
  auto border = [&](std::string const& l, std::string const& r) {
    std::string s = std::string(left, ' ') + l;
    for (int i = 0; i < boxW + 2; i++) s += hLine;
    return s + r;
  };

  tui::Style logo = tui::Style::plain().fg(cGold()).attr(tui::AttrBold, true);
  bool const tiny = convoHeight() < 12;  // short terminal: card without the details
  convo_.append(border(tl, tr), stRule(), true);
  row(center("E M B E R"), logo);
  row(center(std::string("v") + kVersion), stDim());  // build id, tiny card too
  if (!tiny) {
    row(center(uni ? "终端里的软件工程 agent" : "terminal software-engineering agent"),
        stAssistant());
    row("");
    row(std::string(uni ? "端点   " : "endpoint  ") + cfg_.baseUrl, stDim());
    // No model line here: the input frame's top edge already carries it, and
    // the card is on screen at the same time.
    row("");
    row(uni ? "Enter 发送      /help 全部命令" : "Enter send      /help all commands", stPrompt());
    row(uni ? "Ctrl+P 面板   Ctrl+O 会话   Ctrl+G 下一个"
            : "Ctrl+P palette  Ctrl+O sessions  Ctrl+G next",
        stPrompt());
  }
  convo_.append(border(bl, br), stRule(), true);
}

void App::printHelp(bool firstRun) {
  auto head = [this](std::string const& t) {
    convo_.append("  " + t + "\n", stBarAccent());
  };
  auto line = [this](std::string const& t) {
    convo_.append("  " + t + "\n", stAssistant());
  };
  auto cmd = [this](std::string const& t) {
    convo_.append("  " + t + "\n", stPrompt());
  };
  auto rule = [this] {
    std::string bar = "\xE2\x94\x80";
    for (int i = 1; i < 30; i++) bar += "\xE2\x94\x80";
    convo_.append("  " + bar + "\n", stRule(), true);
  };

  if (firstRun) {
    head(std::string("\xE2\x96\x88 Ember v") + kVersion + " \xE2\x80\x94 欢迎 / 使用指引");
    line("首次启动:直接输入文字并按 Enter 即可与模型对话。下面是常用功能;");
    line("需要长任务时用 background_start 放到后台,模型完成时会收到通知。");
  } else {
    head(std::string("\xE2\x96\x88 Ember v") + kVersion + " \xE2\x80\x94 命令与提示");
  }
  rule();
  head("输入");
  line("  输入内容 + Enter 发送    Esc 清空输入");
  head("命令");
  cmd("  /help        本指引");
  cmd("  /clear       清空对话    /compact 压缩上下文    /new 新会话");
  cmd("  /json        切换 JSON 模式    /model NAME 切换模型");
  cmd("  /model       分组模型列表(settings.json 配 providers/groups)  /model add|rm NAME");
  cmd("  /theme NAME  主题 dark|light|terminal|nord|gruvbox|dracula|solarized");
  cmd("  /mode NAME   模式 standard|minimal|ptc|creator");
  cmd("  /think NAME  思考强度 auto|none|minimal|low|medium|high|max(auto=不发参数)");
  cmd("  /think NAME  模型思考强度 auto|none|minimal|low|medium|high|max (auto=不发参数)");
  cmd("                  standard=全部工具 ptc=run_code批量 minimal=shell+edit creator=运行时检查");
  cmd("  /mcp         MCP 服务器连接状态(工具以 <服务器名>_ 前缀注册)");
  cmd("  /ws          工作区列表  /ws add PATH|use N|rm N|on|off（越界操作需批准）");
  cmd("  /provider    弹窗填写服务商(base_url/key/model)，同首次启动界面");
  cmd("  /settings    设置(价格/预算/工具开关)");
  cmd("  /usage       用量与成本环(快捷键 Ctrl+U)    /stats 一行统计");
  cmd("  /trajectory  事件轨迹(快捷键 Ctrl+T)");
  cmd("  /sessions    会话列表(快捷键 Ctrl+O)      — 需会话目录");
  cmd("  /save        立即保存会话(--session 文件 / 当前会话)");
  cmd("  /copy        复制最近一次回答到剪贴板    /copy all 复制整场对话");
  head("CLI 对应参数");
  line("  /sessions     ↔  --sessions DIR / AGENT_SESSIONS   (多会话目录)");
  line("  /save         ↔  --session FILE / AGENT_SESSION    (单文件会话)");
  line("  /model NAME   ↔  --model        /json ↔ --json");
  line("  /trajectory   ↔  --trace(别名 /trace 即 CLI 名)");
  line("  /settings     ↔  --config PATH(设置文件;其余 --budget --rules 等见 --help)");
  head("快捷键");
  line("  Ctrl+T 轨迹 \xC2\xB7 Ctrl+U 用量 \xC2\xB7 Ctrl+O 会话 \xC2\xB7 Ctrl+P 命令面板");
  line("  / + 前缀自动联想命令,↑↓ 选择 Tab / Enter 补全或执行");
  line("  /model /theme /mode /think 参数同样支持 Tab / ↑↓ 前缀补全");
  line("  PgUp/PgDn 或滚轮滚动 \xC2\xB7 Ctrl+C 退出(请求中再按一次)");
  head("模型可用工具");
  line("  shell_exec \xC2\xB7 file_read/write/edit \xC2\xB7 grep \xC2\xB7 glob \xC2\xB7 web_fetch \xC2\xB7 search");
  line("  browser_* \xC2\xB7 rag \xC2\xB7 subagent(_parallel) \xC2\xB7 ask_user \xC2\xB7 git_* \xC2\xB7 background_*");
  line("  verify(构建+测试闭环) \xC2\xB7 remember/recall(跨会话记忆) \xC2\xB7 todo");
  head("提示");
  line("  \xC2\xB7 长命令:background_start 后台运行,完成后模型自动收到通知,可用");
  line("    background_status / background_kill 轮询或中止。");
  line("  \xC2\xB7 /usage 里的成本按 tokens \xC3\x97 /settings 中 $/M 价格估算。");
  line("  \xC2\xB7 --session 持久化会话;/save 手动保存;--trace 记录事件日志。");
  rule();
}

// -------------------------------------------------------------------------
// Command palette (Ctrl+P) and "/" suggest popup over the input line.
// -------------------------------------------------------------------------

std::vector<int> App::commandMatches(std::string const& filter, bool prefix) const {
  std::string f = lowerAscii(filter);
  int n = (int)(sizeof kCommands / sizeof kCommands[0]);
  std::vector<int> out;
  for (int i = 0; i < n; i++) {
    std::string nm = lowerAscii(kCommands[i].name);
    if (f.empty() ||
        (prefix ? nm.rfind(f, 0) == 0 : nm.find(f) != std::string::npos))
      out.push_back(i);
  }
  return out;
}

bool App::cmdSuggestActive() const {
  if (cmdOpen_ || busy_ || asking()) return false;
  std::string const& t = input_.text();
  return t.size() > 1 && t[0] == '/' &&
         !commandMatches(t.substr(1), true).empty();
}

// Candidates for "/model <prefix>" "/theme <prefix>" "/mode <prefix>"
// "/think <prefix>".
// Model pool: the configured models list, or the last server fetch.
App::ArgSug App::argSuggest(std::string const& text) const {
  ArgSug r;
  if (text.size() < 3 || text[0] != '/') return r;
  size_t sp = text.find(' ');
  if (sp == std::string::npos) return r;
  std::string cmd = text.substr(1, sp - 1);
  std::string pref = text.substr(sp + 1);
  size_t p0 = pref.find_first_not_of(" \t");
  if (p0 == std::string::npos) pref.clear();
  else pref = pref.substr(p0);
  std::vector<std::string> pool;
  if (cmd == "model") {
    pool = modelChoices();
    if (pool.empty()) pool = modelList_;
  } else if (cmd == "theme") {
    pool = {"dark", "light", "terminal", "nord", "gruvbox", "dracula", "solarized"};
  } else if (cmd == "mode") {
    pool = {"standard", "minimal", "ptc", "creator"};
  } else if (cmd == "think") {
    pool = thinkingLevels();
  } else {
    return r;
  }
  std::string lp = lowerAscii(pref);
  for (auto const& m : pool) {
    if (lp.empty() || lowerAscii(m).rfind(lp, 0) == 0) r.cands.push_back(m);
  }
  if (!r.cands.empty()) r.cmd = cmd;
  return r;
}

bool App::cmdArgSuggestActive() const {
  if (cmdOpen_ || busy_ || asking()) return false;
  std::string const& t = input_.text();
  if (cmdSuggestActive()) return false;  // command-name popup has priority
  return !argSuggest(t).cands.empty();
}

void App::fetchModelList() {
  if (modelFetching_) {
    setStatus("fetching models already in progress...");
    return;
  }
  if (busy_) {
    setStatus("request in flight; list models when it finishes");
    return;
  }
  modelFetching_ = true;
  modelList_.clear();
  // Fetch on a detached thread: the network call must not stall the UI.
  std::string bu = cfg_.baseUrl;
  std::string ak = cfg_.apiKey;
  std::thread([this, bu, ak]() {
    agent::Client c;
    c.configure(bu, ak);
    std::vector<std::string> names;
    int rc = c.listModels(names);
    StreamEvent ev;
    ev.kind = StreamKind::Models;
    ev.turnRc = rc;
    if (rc == 0) {
      for (size_t i = 0; i < names.size(); i++) {
        if (i) ev.text += '\n';
        ev.text += names[i];
      }
    }
    pushEvent(ev);
  }).detach();
  setStatus("fetching models...");
}

// Flattened /model picker pool: grouped models first (in group order), then
// any ungrouped cfg_.models entries. This exact order is what /model <N> and
// the grouped listing use, so numbers always line up.
std::vector<std::string> App::modelChoices() const {
  std::vector<std::string> out;
  for (auto const& g : cfg_.groups)
    for (auto const& m : g.models)
      if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  for (auto const& m : cfg_.models)
    if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
  return out;
}

int App::applyProviderForModel(std::string const& model, std::string* providerName) {
  for (auto const& g : cfg_.groups) {
    if (std::find(g.models.begin(), g.models.end(), model) == g.models.end())
      continue;
    std::string want = g.provider;  // "" = the top-level default connection
    for (auto const& p : cfg_.providers) {
      if (p.name != want) continue;
      cfg_.baseUrl = p.baseUrl;
      std::string key = p.apiKey;
      if (!p.apiKeyEnv.empty())
        if (char const* e = std::getenv(p.apiKeyEnv.c_str())) key = e;
      cfg_.apiKey = key;
      client_.configure(cfg_.baseUrl, cfg_.apiKey);
      if (providerName) *providerName = p.name;
      return 1;
    }
    if (want.empty() || want == "default") {
      if (!cfg_.defaultBaseUrl.empty()) cfg_.baseUrl = cfg_.defaultBaseUrl;
      cfg_.apiKey = cfg_.defaultApiKey;
      client_.configure(cfg_.baseUrl, cfg_.apiKey);
      if (providerName) *providerName = "default";
      return 1;
    }
    if (providerName) *providerName = want;
    return 2;  // group references an unknown provider
  }
  return 0;  // not in any group: current connection stays
}

// Thinking strength for a request: the global level, spelled the way the
// provider serving this model wants it (its group's provider, else the
// default connection).
void App::applyThinking(ChatOptions& opts) const {
  opts.thinking = cfg_.thinking;
  opts.thinkingStyle = cfg_.thinkingStyle;
  for (auto const& g : cfg_.groups) {
    if (std::find(g.models.begin(), g.models.end(), opts.model) == g.models.end())
      continue;
    for (auto const& p : cfg_.providers)
      if (p.name == g.provider) opts.thinkingStyle = p.thinkingStyle;
    break;
  }
}

void App::openCommandPalette() {
  if (busy_ || asking()) return;
  savedInput_ = input_.text();
  // Pre-fill the filter with the current "/cmd" draft if there is one.
  if (savedInput_.size() > 1 && savedInput_[0] == '/')
    input_.setText(savedInput_.substr(1));
  else
    input_.setText(cmdFilter_);
  cmdSel_ = 0;
  cmdOpen_ = true;
}

void App::execCommandPalette() {
  auto idxs = commandMatches(input_.text());
  if (idxs.empty()) return;
  int sel = cmdSel_;
  if (sel < 0) sel = 0;
  if (sel >= (int)idxs.size()) sel = (int)idxs.size() - 1;
  CmdDef const& c = kCommands[idxs[sel]];
  cmdFilter_.clear();
  cmdOpen_ = false;
  input_.setText("/" + std::string(c.name) + (c.needsArg ? " " : ""));
  if (!c.needsArg) {
    submit(input_.text());  // clears the input, starts the turn/command
    input_.setText("");
  }
}

void App::execSuggest() {
  std::string const& t = input_.text();
  if (t.size() < 2 || t[0] != '/') return;
  auto idxs = commandMatches(t.substr(1), true);
  if (idxs.empty()) return;
  if (cmdSugSel_ < 0) cmdSugSel_ = 0;
  if (cmdSugSel_ >= (int)idxs.size()) cmdSugSel_ = (int)idxs.size() - 1;
  CmdDef const& c = kCommands[idxs[cmdSugSel_]];
  std::string full = "/" + std::string(c.name);
  if (c.needsArg) {
    input_.setText(full + " ");  // keep the popup, let the user type the argument
    return;
  }
  submit(full);
  input_.setText("");  // the chat path clears on submit; do it here too
}

void App::renderCommandPalette() {
  tui::platform::hideCursor();
  overlayChrome(" \xE2\x9A\x99 command palette",
                " Ctrl+P  \xC2\xB7  Enter 执行  Esc 关闭");

  // filter line (palette borrows the Input widget; the prompt shows "/")
  int fx = 2;
  screen_.putText(fx, 2, "\xE2\x9D\xAF /", stPrompt());
  fx += 4;
  std::string filt = input_.text();
  screen_.putText(fx, 2, filt.c_str(), stInputText());
  {
    int tx = fx + (int)displayWidth(filt);
    for (int c = tx; c < cols_; c++) screen_.put(c, 2, ' ', stInputText());
    screen_.put(tx, 2, ' ', stCursor());
  }

  auto idxs = commandMatches(filt);
  int listTop = 4;
  int listH = rows_ - 2 - listTop;
  if (listH > 0) {
    std::vector<tui::SuggestRow> rows;
    rows.reserve(idxs.size());
    for (int i : idxs) {
      CmdDef const& c = kCommands[i];
      tui::SuggestRow r;
      r.text = "/" + std::string(c.name);
      r.desc = c.desc;
      r.right = c.key;
      rows.push_back(std::move(r));
    }
    tui::Style selSt = tui::Style::plain()
                           .fg(tui::Color::rgb(16, 22, 34))
                           .bg(tui::Color::rgb(122, 162, 247))
                           .attr(tui::AttrBold, true);
    tui::Style rowSt = tui::Style::plain().bg(tui::Color::rgb(24, 28, 38));
    tui::Style nameSt = rowSt.fg(tui::Color::rgb(169, 177, 214));
    tui::Style descSt = rowSt.fg(tui::Color::rgb(120, 130, 155));
    tui::Style keySt = rowSt.fg(tui::Color::rgb(224, 175, 104));
    tui::drawSuggestRows(screen_, tui::Rect{0, listTop, cols_, listH}, rows,
                         cmdSel_, selSt, rowSt, nameSt, descSt, keySt);
  }

  int fy = rows_ - 1;
  if (fy >= 0) {
    std::string hints = "\xE2\x86\x91\xE2\x86\x93/ \xE2\x87\xA5 select  Enter run  Esc close  ";
    hints += std::to_string(idxs.size()) + " 项   过滤:输入子串(如 \"mo\")";
    tui::drawHintBar(screen_, fy, cols_, hints, sizeTag(), stBarBase());
  }

  drawModal();
  renderer_.frame(out_, screen_);
  tui::platform::writeAll(out_.data(), out_.size());
  tui::platform::flush();
}

void App::drawCommandSuggest() {
  std::string const& t = input_.text();
  bool wCmd = cmdSuggestActive();
  ArgSug as;
  bool wArg = false;
  if (!wCmd) {
    as = argSuggest(t);
    wArg = !as.cands.empty() && !cmdOpen_ && !busy_ && !asking();
  }
  bool wants = wCmd || wArg;
  if (!wants) {
    if (cmdSug_) forceFull_ = true;  // popup gone: repaint the covered rows
    cmdSug_ = false;
    return;
  }
  std::vector<int> idxs;
  int total = 0;
  if (wCmd) {
    idxs = commandMatches(t.substr(1), true);
    total = (int)idxs.size();
  } else {
    total = (int)as.cands.size();
  }
  int n = total > 6 ? 6 : total;
  if (n == 0) {
    if (cmdSug_) forceFull_ = true;
    cmdSug_ = false;
    return;
  }
  if (!cmdSug_ || cmdSugRows_ != n) forceFull_ = true;
  cmdSug_ = true;
  cmdSugRows_ = n;
  int& selRef = wCmd ? cmdSugSel_ : cmdArgSel_;
  if (selRef < 0) selRef = 0;
  if (selRef >= total) selRef = total - 1;

  int inpRows = asking() ? 1 : input_.rowCount();
  if (inpRows > 4) inpRows = 4;
  int bottom = rows_ - 3 - inpRows;  // divider row; popup sits above it
  int top = bottom - n;
  if (top < 2) { n -= 2 - top; top = 2; }  // shrink if the window is short
  if (n <= 0) { cmdSug_ = false; return; }
  cmdSugRows_ = n;

  std::vector<tui::SuggestRow> rows;
  rows.reserve((size_t)total);
  for (int i = 0; i < total; i++) {
    tui::SuggestRow r;
    if (wCmd) {
      CmdDef const& c = kCommands[idxs[i]];
      r.text = "/" + std::string(c.name);
      r.desc = c.desc;
    } else {
      r.text = as.cands[i];
      if (as.cmd == "model" && as.cands[i] == cfg_.model) r.desc = "current";
    }
    rows.push_back(std::move(r));
  }
  tui::Style selSt = tui::Style::plain()
                         .fg(tui::Color::rgb(16, 22, 34))
                         .bg(tui::Color::rgb(122, 162, 247))
                         .attr(tui::AttrBold, true);
  tui::Style rowSt = tui::Style::plain().bg(tui::Color::rgb(24, 28, 38));
  tui::Style nameSt = rowSt.fg(tui::Color::rgb(169, 177, 214));
  tui::Style descSt = rowSt.fg(tui::Color::rgb(120, 130, 155));
  tui::drawSuggestRows(screen_, tui::Rect{0, top, cols_, n}, rows, selRef,
                       selSt, rowSt, nameSt, descSt, descSt);
}

void App::render() {
  frame_++;
  // Structural changes (resize, overlay toggles, input-area height changes)
  // require wiping the whole canvas; steady-state frames rely on the screen
  // diff so unchanged cells emit nothing.
  uint32_t sig = (uint32_t)(sessOpen_ | (trajOpen_ << 1) | (usageOpen_ << 2) |
                             (settingsOpen_ << 3) | (providerOpen_ << 4) | (asking() << 5) |
                            ((uint32_t)input_.rowCount() << 6) | (cmdOpen_ << 9));
  sig = sig * 100003 + (uint32_t)cols_ * 9511 + (uint32_t)rows_;
  // The input frame's border carries the session title and model, so a session
  // switch or /model change has to force a full repaint of those cells.
  uint32_t label = 2166136261u;
  for (char ch : curSessTitle_) label = (label ^ (unsigned char)ch) * 16777619u;
  for (char ch : cfg_.model) label = (label ^ (unsigned char)ch) * 16777619u;
  sig += label;
  if (sig != lastLayoutSig_) {
    forceFull_ = true;
    lastLayoutSig_ = sig;
  }
  if (forceFull_) {
    screen_.clearAll();
    forceFull_ = false;
  }

  // ---- session browser page (full-screen overlay) --------------------------
  if (sessOpen_) {
    renderSessions();
    return;
  }

  // ---- trajectory page (full-screen overlay) ------------------------------
  if (trajOpen_) {
    renderTrajectory();
    return;
  }

  // ---- usage page (full-screen overlay) ------------------------------------
  if (usageOpen_) {
    renderUsage();
    return;
  }

  // ---- command palette (full-screen overlay) --------------------------------
  if (cmdOpen_) {
    renderCommandPalette();
    return;
  }

  // ---- settings page (full-screen overlay) ---------------------------------
  if (settingsOpen_) {
    std::string title = " \xE2\x9A\x99 settings";
    std::string fileTag = cfg_.settingsPath.empty() ? " (no settings file)" : " " + cfg_.settingsPath;
    overlayChrome(title, fileTag);

    tui::Rect formBox(1, 2, cols_ - 2, rows_ - 3);
    settingsForm_.render(screen_, formBox, stDim(), stAssistant(), stBarAccent(),
                         tui::Style::plain().fg(cGold()).attr(tui::AttrBold, true),
                         tui::Style::plain().fg(tui::Color::index(240)),
                         stInputText(), stCursor());

    // footer hints
    int fy = rows_ - 1;
    if (fy >= 0) {
      std::string hints = "\xE2\x86\x91\xE2\x86\x93 move  Enter edit/toggle  Tab next  Ctrl+S save  Esc close";
      tui::drawHintBar(screen_, fy, cols_, hints, sizeTag(), stBarBase());
    }

    tui::platform::hideCursor();
    drawModal();
    renderer_.frame(out_, screen_);
    tui::platform::writeAll(out_.data(), out_.size());
    tui::platform::flush();
    return;  // keep the terminal cursor hidden; the Form draws its own
  }

  // ---- header (title bar) ----
  // Identity and run state only: the model, session name and usage each live
  // in exactly one place (the input frame), so nothing is printed twice.
  std::string title = " \xE2\x97\x88 Ember";
  std::string mid;
  std::string right = busy_ ? "\xE2\x97\x8F working" : "\xE2\x97\x8F idle";
  int rightW = 1 + (int)right.size();
  int x = 0;
  screen_.putText(0, 0, title.c_str(), stHeader());
  x += (int)title.size();
  screen_.putText(x, 0, mid.c_str(), stHeaderDim());
  x += (int)mid.size();
  screen_.putText(x, 0, std::string(std::max(0, cols_ - x - rightW - 1), ' ').c_str(), stHeader());
  screen_.putText(cols_ - rightW, 0, right.c_str(), busy_ ? stBarBusy() : stHeaderDim());

  // header underline
  tui::drawFadedRule(screen_, 1, cols_, stRule(), tui::Color::rgb(120, 90, 60),
                     tui::Color::rgb(60, 60, 30), 20);

  // ---- conversation ----
  int inpRows = asking() ? 1 : input_.rowCount();
  if (inpRows > 4) inpRows = 4;
  // Geometry of the input area, needed before the conversation box so the two
  // never share a row.
  tui::Glyphs const g = tui::glyphsFor(caps_);
  bool const uni = caps_.unicode;
  bool const framed = rows_ >= 9 && cols_ >= 24;
  // When framed, the frame's bottom border doubles as the status line, so the
  // input sits one row lower than the old layout and no separate bar is drawn.
  int const inBotY = rows_ - 2;
  int const frameTopY = inBotY - (inpRows - 1) - 1;
  int const frameBotY = rows_ - 1;
  input_.setBox(tui::Rect(2, inBotY, framed ? cols_ - 4 : cols_ - 3, 4));
  int const convoH = framed ? (frameTopY - 2) : (rows_ - 5 - inpRows);
  tui::Rect convoBox(1, 2, cols_ - 2, std::max(1, convoH));
  int count = convo_.lineCount();
  int h = convoBox.h;
  int first = count - h;
  if (scrollAnchor_ >= 0) first = scrollAnchor_;
  int bottom = count - h;
  if (first > bottom) first = bottom;
  if (first < 0) first = 0;
  drawTextView(screen_, convoBox, convo_, first, stAssistant());

  // ---- status pieces (drawn into the frame's bottom border, or into a bar
  //      when the terminal is too small for a frame) ----
  std::string status = status_;
  if (status.empty() && busy_) {
    static const char* spin = "|/-\\";
    status = "working " + std::string(1, spin[frame_ % 4]);
  }
  std::string scrollTag;
  if (scrollAnchor_ >= 0)
    scrollTag = " \xE2\x86\x91" + std::to_string(count - h - first) + " ";
  std::string usageTag;
  {
    agent::SessionUsage su = agent::snapshotUsage();
    if (su.turns > 0)
      usageTag = "  " + std::to_string(su.turns) + (uni ? "轮 " : "t ") +
                 std::to_string(su.steps) + (uni ? "步 " : "s ") +
                 agent::formatDuration(su.llmMs) + " " +
                 agent::formatTokens(su.promptTokens + su.completionTokens);
  }
  std::string ctxTag;
  {
    // Throttled: scanning the whole transcript every frame (and holding
    // msgsMutex_) is wasteful during 60 fps streaming.
    std::lock_guard lk(msgsMutex_);
    auto nowMs = std::chrono::steady_clock::now();
    if (nowMs - lastCtxAt_ >= std::chrono::milliseconds(500)) {
      lastCtxEst_ = agent::estimateMessagesTokens(messages_);
      lastCtxAt_ = nowMs;
    }
    char cb[32];
    snprintf(cb, sizeof cb, " ctx %s", agent::formatTokens((int64_t)lastCtxEst_).c_str());
    ctxTag = cb;
  }

  // ---- input frame ---------------------------------------------------------
  //   ╭─ 会话: xxx ───────────────────── model ─╮
  //   │ ❯ typed text                            │
  //   ╰─ Enter 发送 · Ctrl+O 会话 · …──── 92x24─╯
  // Glyphs come from capabilities so a non-UTF8 terminal still gets '+--+'
  // rather than mojibake, and the labels switch to ASCII there too.
  auto borderRow = [&](int y, uint32_t lch, uint32_t rch) {
    if (y < 0 || y >= rows_ || cols_ < 10) return;
    screen_.put(0, y, lch, stRule());
    screen_.put(cols_ - 1, y, rch, stRule());
    for (int c = 1; c < cols_ - 1; c++) screen_.put(c, y, (uint32_t)g.hLine, stRule());
  };
  auto borderLabel = [&](int y, std::string left, std::string right) {
    int room = cols_ - 4;
    if (!left.empty()) {
      left = trimDisplay(" " + left, room);
      screen_.putText(1, y, left.c_str(), stBarAccent());
      room -= displayWidth(left) + 1;
    }
    if (!right.empty() && room > 3) {
      right = trimDisplay(" " + right, room);
      screen_.putText(cols_ - 2 - (int)displayWidth(right), y, right.c_str(), stHeaderDim());
    }
  };
  // Key/description pairs so the shortcut reads brighter than its explanation.
  auto hintPairs = [&](int y, std::vector<std::pair<std::string, std::string>> const& items,
                       int xMax) {
    int x = 2;  // one column of breathing room after the corner glyph
    int const sep = 3;  // " · " or " | "
    for (size_t i = 0; i < items.size(); i++) {
      std::string const& k = items[i].first;
      std::string const& d = items[i].second;
      int need = displayWidth(k) + displayWidth(d) + (i + 1 < items.size() ? sep : 0);
      if (x + need > xMax) {
        std::string more = uni ? "…" : ">";
        if (x + displayWidth(more) <= xMax) screen_.putText(x, y, more.c_str(), stDim());
        return;
      }
      screen_.putText(x, y, k.c_str(), stPrompt());
      x += displayWidth(k);
      screen_.putText(x, y, d.c_str(), stDim());
      x += displayWidth(d);
      if (i + 1 < items.size()) {
        std::string s = uni ? " \xC2\xB7 " : " | ";
        screen_.putText(x, y, s.c_str(), stRule());
        x += displayWidth(s);
      }
    }
  };

  if (framed) {
    borderRow(frameTopY, g.tlR, g.trR);
    for (int r = frameTopY + 1; r <= inBotY; r++) {
      if (r < 0 || r >= rows_) continue;
      screen_.put(0, r, (uint32_t)g.vLine, stRule());
      screen_.put(cols_ - 1, r, (uint32_t)g.vLine, stRule());
    }
    std::string sessTag;
    if (asking()) {
      std::lock_guard lk(askMutex_);
      sessTag = askQuestion_;
    } else if (!curSessTitle_.empty()) {
      sessTag = curSessTitle_;
    } else if (!curSessId_.empty()) {
      sessTag = curSessId_;
    } else {
      sessTag = uni ? "新会话" : "new session";
    }
    // The model gets exactly one home: the right end of the frame's top edge.
    // The thinking level rides along there, and only when it is not auto.
    std::string thinkTag = cfg_.thinking == "auto"
                               ? std::string()
                               : " [think:" + cfg_.thinking + "]";
    borderLabel(frameTopY, sessTag,
                "\xE2\x97\x8F " + cfg_.model + thinkTag +
                    (cfg_.jsonMode ? " [json]" : "") + ctxTag);
    borderRow(frameBotY, g.blR, g.brR);
    std::string const foot = " " + buildTag();
    // A transient message (or scrollback/usage) outranks the key hints; the
    // hints are always there, the news is not.
    std::string announce = status + scrollTag + usageTag;
    if (announce.empty()) {
      hintPairs(frameBotY,
                asking()
                    ? std::vector<std::pair<std::string, std::string>>{
                          {"Enter", uni ? " 回答" : " answer"},
                          {"Esc", uni ? " 取消" : " cancel"}}
                    : std::vector<std::pair<std::string, std::string>>{
                          {"Enter", uni ? " 发送" : " send"},
                          {"/help", uni ? " 命令" : " cmds"},
                          {"Ctrl+P", uni ? " 面板" : " palette"},
                          {"Ctrl+O", uni ? " 会话" : " sessions"},
                          {"Ctrl+G", uni ? " 下一个" : " next"},
                          {"Esc", uni ? " 清空" : " clear"}},
                cols_ - 2 - displayWidth(foot) - 1);
      borderLabel(frameBotY, "", buildTag());
    } else {
      borderLabel(frameBotY, announce, buildTag());
    }
  } else {
    // Small terminal: spend the rows on content, not chrome.
    int divideY = rows_ - 3 - inpRows;
    if (divideY >= 2 && divideY < rows_) {
      for (int c = 0; c < cols_; c++) screen_.put(c, divideY, (uint32_t)g.hLine, stRule());
    }
    int askY = rows_ - 2 - inpRows;
    if (asking()) {
      std::lock_guard lk(askMutex_);
      std::string line = trimDisplay(askQuestion_, cols_ - 4);
      screen_.putText(1, askY, line.c_str(), stPrompt());
    } else {
      std::string hint = uni ? "Enter 发送 · /help · Ctrl+P · Ctrl+O · Ctrl+G"
                             : "Enter send /help Ctrl+P Ctrl+O Ctrl+G";
      screen_.putText(1, askY, trimDisplay(hint, cols_ - 2).c_str(), stDim());
    }
  }

  input_.render(screen_, "\xE2\x9D\xAF ", stPrompt(), stInputText(), stCursor());
  drawCommandSuggest();

  // ---- status bar (only when there is no frame to carry it) -----------
  if (!framed) {
    int sy = rows_ - 1;
    if (sy >= 0) {
      for (int c = 0; c < cols_; c++) screen_.put(c, sy, 0, stBarBase());
      int sxx = 1;
      auto put = [&](std::string const& t, tui::Style st) {
        if (sxx >= cols_ - 1) return;
        screen_.putText(sxx, sy, t.c_str(), st);
        sxx += (int)t.size();
      };
      put(" \xE2\x97\x8F " + cfg_.model + (cfg_.jsonMode ? " [json]" : "") + " ",
          busy_ ? stBarBusy() : stBarAccent());
      put(status + scrollTag + usageTag, stBarBase());
      put(ctxTag, stBarBase());
      std::string const right = buildTag();
      put(std::string(std::max(0, cols_ - sxx - (int)right.size() - 2), ' '), stBarBase());
      put(right, stBarBase());
    }
  }


  tui::platform::hideCursor();
  drawModal();
  renderer_.frame(out_, screen_);
  tui::platform::writeAll(out_.data(), out_.size());
  tui::platform::flush();
  // Park the terminal cursor on the input line, then reveal it. Painting a
  // frame jumps the cursor across every dirty row, so it must stay hidden
  // until the frame is complete or the block cursor visibly runs up/down the
  // window on every redraw.
  {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "\x1b[%d;%dH", input_.cursorScreenY() + 1,
                     input_.cursorScreenX() + 1);
    tui::platform::writeAll(tmp, (size_t)n);
  }
  tui::platform::showCursor();
}

// ---------------------------------------------------------------------------
// --serve: headless NDJSON stdio loop.
//
// Commands (stdin, one JSON per line):
//   {"type":"prompt","text":"..."}    start a turn
//   {"type":"answer","text":"..."}    answer a pending "ask" event
//   {"type":"command","text":"/x .."} run a slash command, reply via "notice"
//   {"type":"set","key":K,"value":V}  K = thinking|model|mode|workspace|json
//   {"type":"state"}                  re-emit the shell state
//   {"type":"models"}                 model picker list
//   {"type":"sessions"}               session list (also refreshes it)
//   {"type":"session","id":"..."}     switch to a session, then state + history
//   {"type":"history"}                replay of the live conversation
//   {"type":"new_session"}            /new, then sessions + state
//   {"type":"workspaces"}             registered workspace roots + active index
//   {"type":"commands"}               the TUI's own kCommands table
//   {"type":"usage"}                  re-emit the usage event
//   {"type":"compact"}                /compact
//   {"type":"clear"}                  /clear, then {"type":"clear"}
//   {"type":"shutdown"}               leave the loop
//
// "command" carries any slash command; the three that open TUI popups in the
// terminal (/sessions, /usage, /model) answer with the matching list event
// here, because a popup has nothing to draw into.
//
// Events (stdout, one JSON per line): ready / state / delta / reasoning /
// tool_call / tool_result / ask / models / model_list / sessions / history /
// workspaces / commands / notice / background / error / turn_end / usage / clear.
// The turn machinery, session persistence, trajectory and tool registry are
// exactly the TUI's; only the presentation differs (drainEvents serializes
// instead of drawing).
// ---------------------------------------------------------------------------
namespace {

struct ServeInbox {
  std::mutex mutex;
  std::vector<std::string> lines;
  std::atomic<bool> eof{false};
};

void serveEmitStreamEvent(StreamEvent const& ev) {
  switch (ev.kind) {
    case StreamKind::Delta:
      serveEmit([&] {
        mini::Value o = serveEvent("delta");
        o.set("text", mini::Value::makeString(ev.text));
        return o;
      }());
      break;
    case StreamKind::Reasoning:
      serveEmit([&] {
        mini::Value o = serveEvent("reasoning");
        o.set("text", mini::Value::makeString(ev.text));
        return o;
      }());
      break;
    case StreamKind::ToolCall:
      serveEmit([&] {
        mini::Value o = serveEvent("tool_call");
        o.set("name", mini::Value::makeString(ev.toolName));
        o.set("args", mini::Value::makeString(ev.toolArgs));
        return o;
      }());
      break;
    case StreamKind::ToolResult:
      serveEmit([&] {
        mini::Value o = serveEvent("tool_result");
        o.set("name", mini::Value::makeString(ev.toolName));
        o.set("result", mini::Value::makeString(ev.toolResult));
        return o;
      }());
      break;
    case StreamKind::Error:
      serveEmit([&] {
        mini::Value o = serveEvent("error");
        o.set("message", mini::Value::makeString(ev.error.empty() ? "request failed" : ev.error));
        return o;
      }());
      break;
    case StreamKind::Models:
      serveEmit([&] {
        mini::Value o = serveEvent("models");
        o.set("text", mini::Value::makeString(ev.text));
        o.set("rc", mini::Value::makeInt(ev.turnRc));
        return o;
      }());
      break;
    case StreamKind::Background:
      serveEmit([&] {
        mini::Value o = serveEvent("background");
        o.set("text", mini::Value::makeString(ev.text));
        return o;
      }());
      break;
    case StreamKind::TurnComplete: {
      serveEmit([&] {
        mini::Value o = serveEvent("turn_end");
        o.set("rc", mini::Value::makeInt(ev.turnRc));
        return o;
      }());
      // the usage event follows from runServe(), which can see cfg_ / messages_
      break;
    }
    case StreamKind::Done:
      break;
  }
}

std::string serveStringField(mini::Value const& v, char const* key) {
  mini::Value const* f = v.get(key);
  return (f && f->type == mini::Value::String) ? f->s : std::string();
}

void serveEmitError(std::string const& message) {
  mini::Value o = serveEvent("error");
  o.set("message", mini::Value::makeString(message));
  serveEmit(o);
}

}  // namespace

// Usage event for --serve. Lives here (not with the other serve emitters) because
// the context occupancy and the cost need cfg_ and messages_, which only App sees.
// Money crosses the wire as integer micro-USD: no float noise in the NDJSON.
void App::serveEmitUsage() {
  agent::SessionUsage const u = agent::snapshotUsage();
  double const cost = agent::estimateCostUsd(u, cfg_.costInPerM, cfg_.costOutPerM);
  mini::Value o = serveEvent("usage");
  o.set("turns", mini::Value::makeInt(u.turns));
  o.set("steps", mini::Value::makeInt(u.steps));
  o.set("llmMs", mini::Value::makeInt(u.llmMs));
  o.set("toolMs", mini::Value::makeInt(u.toolMs));
  o.set("promptTokens", mini::Value::makeInt(u.promptTokens));
  o.set("completionTokens", mini::Value::makeInt(u.completionTokens));
  o.set("cachedTokens", mini::Value::makeInt(u.cachedTokens));
  o.set("ctxTokens", mini::Value::makeInt((int64_t)agent::estimateMessagesTokens(messages_)));
  o.set("ctxBudget", mini::Value::makeInt((int64_t)cfg_.budgetTokens));
  o.set("costMicroUsd", mini::Value::makeInt((int64_t)(cost * 1e6 + 0.5)));
  o.set("costBudgetMicroUsd", mini::Value::makeInt((int64_t)(cfg_.costBudgetUsd * 1e6 + 0.5)));
  serveEmit(o);
}

// The status line is the TUI's only feedback channel; in serve mode it becomes
// a "notice" event, which is what makes every existing slash command speak back
// through --serve without reimplementing its message text here.
void App::setStatus(std::string const& s) {
  status_ = s;
  if (!cfg_.serveMode || s.empty()) return;
  mini::Value o = serveEvent("notice");
  o.set("text", mini::Value::makeString(s));
  serveEmit(o);
}

// Everything the shell chrome shows: which model/provider connection is live,
// how thinking is spelled on the wire, where files resolve, session identity.
void App::serveEmitState() {
  ChatOptions probe;
  probe.model = cfg_.model;
  applyThinking(probe);
  std::string const style = thinkingStyleFromString(probe.thinkingStyle);
  std::string workspace;
  if (cfg_.activeWorkspace >= 0 &&
      (size_t)cfg_.activeWorkspace < cfg_.workspaces.size())
    workspace = cfg_.workspaces[(size_t)cfg_.activeWorkspace];
  std::error_code ec;
  mini::Value o = serveEvent("state");
  o.set("version", mini::Value::makeString(kVersion));
  o.set("model", mini::Value::makeString(cfg_.model));
  o.set("thinking", mini::Value::makeString(cfg_.thinking));
  o.set("thinkingStyle", mini::Value::makeString(style));
  o.set("thinkingBinary", mini::Value::makeBool(thinkingStyleIsBinary(style)));
  o.set("mode", mini::Value::makeString(cfg_.mode));
  o.set("jsonMode", mini::Value::makeBool(cfg_.jsonMode));
  o.set("cwd", mini::Value::makeString(std::filesystem::current_path(ec).string()));
  o.set("sessionDir", mini::Value::makeString(cfg_.sessionDir));
  o.set("sessionId", mini::Value::makeString(curSessId_));
  o.set("sessionTitle", mini::Value::makeString(curSessTitle_));
  o.set("ctxBudget", mini::Value::makeInt((int64_t)cfg_.budgetTokens));
  o.set("costBudgetMicroUsd",
        mini::Value::makeInt((int64_t)(cfg_.costBudgetUsd * 1e6 + 0.5)));
  o.set("workspace", mini::Value::makeString(workspace));
  o.set("workspaceGuard", mini::Value::makeBool(cfg_.workspaceGuard));
  o.set("busy", mini::Value::makeBool(busy_.load()));
  serveEmit(o);
}

void App::serveEmitSessions() {
  if (cfg_.sessionDir.empty()) {
    serveEmitError("multi-session disabled (no session dir)");
    return;
  }
  refreshSessList();
  mini::Value list = mini::Value::makeArray();
  for (auto const& s : sessVisible_) {
    mini::Value row = mini::Value::makeObject();
    row.set("id", mini::Value::makeString(s.id));
    row.set("title", mini::Value::makeString(s.title));
    row.set("model", mini::Value::makeString(s.model));
    row.set("msgCount", mini::Value::makeInt(s.msgCount));
    row.set("savedAt", mini::Value::makeInt(s.savedAt));
    row.set("current", mini::Value::makeBool(s.id == curSessId_));
    list.arr.push_back(row);
  }
  mini::Value o = serveEvent("sessions");
  o.set("dir", mini::Value::makeString(cfg_.sessionDir));
  o.set("list", list);
  serveEmit(o);
}

void App::serveEmitModelList() {
  std::vector<std::string> const choices = modelChoices();
  // A /model fetch from the endpoint is not grouped, so it carries no group name.
  std::vector<std::string> const pool =
      !choices.empty() ? choices : modelList_;
  mini::Value list = mini::Value::makeArray();
  for (auto const& m : pool) {
    mini::Value row = mini::Value::makeObject();
    std::string group, provider = "default";
    for (auto const& g : cfg_.groups) {
      if (std::find(g.models.begin(), g.models.end(), m) == g.models.end()) continue;
      group = g.name;
      provider = g.provider.empty() ? "default" : g.provider;
      break;
    }
    row.set("name", mini::Value::makeString(m));
    row.set("group", mini::Value::makeString(group));
    row.set("provider", mini::Value::makeString(provider));
    row.set("current", mini::Value::makeBool(m == cfg_.model));
    list.arr.push_back(row);
  }
  mini::Value o = serveEvent("model_list");
  o.set("list", list);
  o.set("source",
        mini::Value::makeString(choices.empty() ? "fetched" : "settings"));
  serveEmit(o);
}

void App::serveEmitWorkspaces() {
  mini::Value list = mini::Value::makeArray();
  for (size_t i = 0; i < cfg_.workspaces.size(); i++) {
    mini::Value row = mini::Value::makeObject();
    row.set("index", mini::Value::makeInt((int64_t)i));
    row.set("path", mini::Value::makeString(cfg_.workspaces[i]));
    row.set("current", mini::Value::makeBool((int)i == cfg_.activeWorkspace));
    list.arr.push_back(row);
  }
  mini::Value o = serveEvent("workspaces");
  o.set("list", list);
  o.set("active", mini::Value::makeInt(cfg_.activeWorkspace));
  o.set("guard", mini::Value::makeBool(cfg_.workspaceGuard));
  serveEmit(o);
}

// Replay of the live conversation, so switching sessions in the GUI shows what
// is actually in messages_ rather than an empty transcript.
void App::serveEmitHistory() {
  std::vector<Message> snapshot;
  {
    std::lock_guard lk(msgsMutex_);
    snapshot = messages_;
  }
  mini::Value list = mini::Value::makeArray();
  for (auto const& m : snapshot) {
    if (m.role == "user" && !m.content.empty()) {
      mini::Value row = mini::Value::makeObject();
      row.set("kind", mini::Value::makeString("user"));
      row.set("text", mini::Value::makeString(m.content));
      list.arr.push_back(row);
      continue;
    }
    if (m.role == "assistant") {
      if (!m.content.empty()) {
        mini::Value row = mini::Value::makeObject();
        row.set("kind", mini::Value::makeString("assistant"));
        row.set("text", mini::Value::makeString(m.content));
        list.arr.push_back(row);
      }
      for (auto const& tc : m.toolCalls) {
        mini::Value row = mini::Value::makeObject();
        row.set("kind", mini::Value::makeString("tool"));
        row.set("id", mini::Value::makeString(tc.id));
        row.set("name", mini::Value::makeString(tc.name));
        row.set("args", mini::Value::makeString(tc.arguments));
        list.arr.push_back(row);
      }
      continue;
    }
    if (m.role == "tool") {
      mini::Value row = mini::Value::makeObject();
      row.set("kind", mini::Value::makeString("tool_result"));
      row.set("id", mini::Value::makeString(m.toolCallId));
      row.set("result", mini::Value::makeString(m.content));
      list.arr.push_back(row);
    }
  }
  mini::Value o = serveEvent("history");
  o.set("list", list);
  serveEmit(o);
}

// /new: start a fresh session and tell the frontend to drop its transcript.
void App::serveNewSession() {
  if (busy_) {
    serveEmitError("a turn is running; start a new session when it finishes");
    return;
  }
  newSessionNow();
  serveEmitSessions();
  serveEmitState();
  serveEmit(serveEvent("clear"));
}

void App::serveClearTranscript() {
  runCommand("/clear");
  serveEmit(serveEvent("clear"));
}

// Every key here maps onto the slash command that already owns the logic:
// validation, settings save and the provider switch all stay single-sourced.
void App::serveSet(std::string const& key, std::string const& value) {
  if (key == "thinking") {
    runCommand("/think " + value);
  } else if (key == "model") {
    runCommand("/model " + value);
  } else if (key == "mode") {
    runCommand("/mode " + value);
  } else if (key == "workspace") {
    runCommand("/ws use " + value);
    serveEmitWorkspaces();  // the list carries which row is active
  } else if (key == "json") {
    bool const want = (value == "on" || value == "true" || value == "1");
    if (want != cfg_.jsonMode) runCommand("/json");  // /json only toggles
  } else {
    serveEmitError("unknown set key: " + key);
    return;
  }
  serveEmitState();
}

int App::runServe() {
  auto inbox = std::make_shared<ServeInbox>();
  // Blocking stdin reads must not stall the event pump, so lines go through a
  // queue. Detached: getline cannot be interrupted portably, and the process
  // exits right after the loop, which reclaims it.
  std::thread reader([inbox] {
    std::string line;
    while (std::getline(std::cin, line)) {
      std::lock_guard lk(inbox->mutex);
      if (inbox->lines.size() >= 1024) continue;  // runaway frontend: drop, don't grow
      inbox->lines.push_back(std::move(line));
    }
    inbox->eof = true;
  });
  reader.detach();

  {
    std::error_code ec;
    mini::Value hello = serveEvent("ready");
    hello.set("version", mini::Value::makeString(kVersion));
    hello.set("model", mini::Value::makeString(cfg_.model));
    hello.set("mode", mini::Value::makeString(cfg_.mode));
    hello.set("cwd", mini::Value::makeString(std::filesystem::current_path(ec).string()));
    hello.set("session", mini::Value::makeString(
                             cfg_.sessionPath.empty() ? cfg_.sessionDir : cfg_.sessionPath));
    // budgets travel with the handshake so the frontend can draw the rings
    // before the first turn finishes
    hello.set("ctxBudget", mini::Value::makeInt((int64_t)cfg_.budgetTokens));
    hello.set("costBudgetMicroUsd", mini::Value::makeInt((int64_t)(cfg_.costBudgetUsd * 1e6 + 0.5)));
    serveEmit(hello);
    // "ready" carries the model but not how thinking is spelled on the wire,
    // the live session or the workspace, so the full state follows immediately.
    // The resumed conversation follows too: init() may have loaded a session,
    // and an empty transcript would misrepresent that to the frontend.
    serveEmitState();
    serveEmitHistory();
  }

  for (;;) {
    std::vector<StreamEvent> evs;
    drainEvents(&evs);
    for (auto const& ev : evs) {
      serveEmitStreamEvent(ev);
      if (ev.kind == StreamKind::TurnComplete) serveEmitUsage();
    }

    std::string line;
    bool haveLine = false;
    {
      std::lock_guard lk(inbox->mutex);
      if (!inbox->lines.empty()) {
        line = std::move(inbox->lines.front());
        inbox->lines.erase(inbox->lines.begin());
        haveLine = true;
      }
    }

    bool quit = false;
    if (haveLine) {
      mini::Value cmd = mini::parse(line);
      std::string const type = serveStringField(cmd, "type");
      if (type == "prompt") {
        std::string const text = serveStringField(cmd, "text");
        if (text.empty()) {
          serveEmitError("prompt without text");
        } else if (busy_) {
          serveEmitError("a turn is already running");
        } else {
          startTurn(text);
        }
      } else if (type == "answer") {
        {
          std::lock_guard lk(askMutex_);
          askAnswer_ = serveStringField(cmd, "text");
          askAnswered_ = true;
        }
        askCv_.notify_all();
      } else if (type == "shutdown") {
        quit = true;
      } else if (type == "command") {
        std::string const text = serveStringField(cmd, "text");
        if (text.size() < 2 || text[0] != '/') {
          serveEmitError("command must start with / (got: " + text + ")");
        } else if (text == "/sessions" || text == "/ls") {
          serveEmitSessions();  // the TUI opens a browser; the GUI wants the list
        } else if (text == "/usage" || text == "/cost") {
          serveEmitUsage();
        } else if (text == "/model" || text == "/models") {
          serveEmitModelList();
        } else if (text == "/new") {
          serveNewSession();
        } else if (text == "/clear") {
          serveClearTranscript();
        } else {
          submit(text);
          // Any command may move model / thinking / mode / workspace, so the
          // shell re-reads state after each one; /ws also refreshes the list.
          if (text.rfind("/ws", 0) == 0) serveEmitWorkspaces();
          serveEmitState();
        }
      } else if (type == "commands") {
        serveEmitCommands();
      } else if (type == "set") {
        serveSet(serveStringField(cmd, "key"), serveStringField(cmd, "value"));
      } else if (type == "state") {
        serveEmitState();
      } else if (type == "models") {
        serveEmitModelList();
      } else if (type == "sessions") {
        serveEmitSessions();
      } else if (type == "session") {
        std::string const id = serveStringField(cmd, "id");
        if (busy_) {
          serveEmitError("a turn is running; switch sessions when it finishes");
        } else if (id.empty()) {
          serveEmitError("session without id");
        } else {
          serveEmitSessions();  // refills sessVisible_, which switchSession indexes
          int idx = -1;
          for (size_t i = 0; i < sessVisible_.size(); i++) {
            if (sessVisible_[i].id == id) { idx = (int)i; break; }
          }
          if (idx < 0) {
            serveEmitError("no such session: " + id);
          } else {
            switchSession(idx);
            serveEmitState();
            serveEmitHistory();
          }
        }
      } else if (type == "new_session") {
        serveNewSession();
      } else if (type == "workspaces") {
        serveEmitWorkspaces();
      } else if (type == "commands") {
        serveEmitCommands();
      } else if (type == "history") {
        serveEmitHistory();
      } else if (type == "usage") {
        serveEmitUsage();
      } else if (type == "compact") {
        runCommand("/compact");
      } else if (type == "clear") {
        serveClearTranscript();
      } else if (!type.empty()) {
        serveEmitError("unknown command: " + type);
      } else {
        serveEmitError("command without a type field");
      }
    }

    bool inboxEmpty = false;
    {
      std::lock_guard lk(inbox->mutex);
      inboxEmpty = inbox->lines.empty();
    }
    // Exit on an explicit shutdown, or when the host closed stdin and no turn
    // is in flight.
    if (quit || (inbox->eof && inboxEmpty && !busy_)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }

  // A shutdown mid-turn must not hang the exit: the process teardown reclaims
  // the worker, but a finished turn still gets its session saved.
  if (!busy_ && worker_.joinable()) worker_.join();
  persistSession(true);
  return 0;
}

int App::run() {
  if (!tui::platform::enterRaw()) return 1;
  applyTheme(cfg_.theme, cfg_.themeOverride);
  caps_ = tui::detectCapabilities();
  renderer_ = tui::Renderer(caps_);
  tui::platform::enterAlternateScreen();
  enableModes(caps_);

  tui::platform::getSize(cols_, rows_);
  resize(cols_, rows_);
  input_.clear();

  setStatus("");

  // An empty screen gets the logo card; a resumed session keeps its transcript
  // instead of being greeted by chrome it has already seen.
  if (convo_.lineCount() == 0) printWelcome();

  // First run: drop the marker and open the provider picker.
  {
    const char* flag = ".agent_first_run";
    std::ifstream f(flag);
    if (!f.good()) {
      std::ofstream out(flag);
      out << "1\n";
      openProvider();
    }
  }

  auto const frameInterval = std::chrono::milliseconds(16);  // 60fps for animations/spinner
  auto lastFrame = std::chrono::steady_clock::now();
  // Render throttle: the frame loop only repaints when there is something new
  // (events drained) or an animation must advance (spinner while busy). Idle
  // frames are skipped entirely so a quiet session costs almost no CPU.
  auto renderDue = [&]() {
    auto now = std::chrono::steady_clock::now();
    if (now - lastFrame < frameInterval) return;
    bool busyWas = busy_;
    bool hadEvents = drainEvents();
    bool animating = busy_ || busyWas;
    if (hadEvents || animating) {
      render();
      lastFrame = now;
    }
  };
  std::string crashMsg;
  // 首帧得自己画。renderDue() 只在"有事件"或"有动画"时重绘，而静默启动（新会话、
  // 没有 notice、也不忙）两者都没有 —— POSIX 上就停在 alt-screen 空屏，直到用户
  // 按下第一个键才亮。Windows 的输入路径开机先递来记录，把这个问题掩盖了。
  render();
  lastFrame = std::chrono::steady_clock::now();
  try {
    while (running_) {
      uint8_t buf[2048];
      // Long idle timeout when there is nothing to animate, short when busy so
      // the spinner and streaming tail repaint at ~60fps.
      int idleMs = busy_ ? 16 : 100;
      int n = tui::platform::readInput(buf, sizeof buf, idleMs);
      if (n > 0) {
        parser_.feed(buf, (size_t)n);
        tui::Event ev;
        while (parser_.next(ev)) {
          if (ev.type == tui::EventType::Resize) {
            int nc, nr;
            tui::platform::getSize(nc, nr);
            if (nc != cols_ || nr != rows_) resize(nc, nr);
            continue;
          }
          handleEvent(ev);
        }
        // Interactive input is drawn immediately; streaming frames coalesce.
        drainEvents();
        render();
        lastFrame = std::chrono::steady_clock::now();
        continue;
      }

      parser_.expireTimeout();

      if (tui::platform::resizePending()) {
        int nc, nr;
        tui::platform::getSize(nc, nr);
        if (nc != cols_ || nr != rows_) resize(nc, nr);
      }

      int nc, nr;
      tui::platform::getSize(nc, nr);
      if (nc != cols_ || nr != rows_) resize(nc, nr);

      renderDue();
    }
  } catch (std::exception const& e) {
    crashMsg = e.what();
  } catch (...) {
    crashMsg = "(unknown exception)";
  }

  if (!crashMsg.empty()) {
    std::string log = "agent_crash.log";
    {
      std::ofstream f(log, std::ios::app);
      if (f) {
        std::time_t t = std::time(nullptr);
        char tb[32];
        std::strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        f << tb << "  unhandled in UI loop: " << crashMsg << "\n";
      }
    }
    tui::platform::leaveAlternateScreen();
    tui::platform::leaveRaw();
    std::fprintf(stderr,
                 "\nunhandled exception in UI loop: %s\n(see %s; the terminal "
                 "was restored)\n",
                 crashMsg.c_str(), log.c_str());
    std::fflush(stderr);
    return 2;
  }

    persistSession();
  // restore terminal modes (drop mouse tracking so the shell's wheel works)
  {
    using tui::platform::writeAll;
    if (caps_.ansi) {
#ifndef _WIN32
      if (caps_.mouse) writeAll("\x1b[?1000l\x1b[?1002l\x1b[?1006l", 24);
      if (caps_.bracketedPaste) writeAll("\x1b[?2004l", 8);
      if (caps_.focusEvents) writeAll("\x1b[?1004l", 8);
#else
      char const* wt = std::getenv("WT_SESSION");
      if (wt && *wt && caps_.mouse) {
        writeAll("\x1b[?1000l\x1b[?1006l", 18);
        writeAll("\x1b[?2004l", 8);
      }
#endif
    }
  }
  tui::platform::leaveAlternateScreen();
  tui::platform::leaveRaw();
  if (cfg_.tokenSummary) {
    agent::SessionUsage u = agent::snapshotUsage();
    int cachePct = agent::cacheHitPct(u);
    std::printf("TOKLEDGER\tmodel=%s\tturns=%lld\tcalls=%lld\tprompt=%lld\tcompletion=%lld\tcached=%lld\tcache_hit_pct=%s\n",
                cfg_.model.c_str(), (long long)u.turns, (long long)u.calls,
                (long long)u.promptTokens, (long long)u.completionTokens,
                (long long)u.cachedTokens,
                cachePct < 0 ? "-" : std::to_string(cachePct).c_str());
    std::fflush(stdout);
  }
  return 0;
}

}  // namespace agent
