// tui studio - a small animated dashboard that exercises the upgraded tui
// library: gradient header, rounded/heavy borders, smooth progress bar,
// animated spinner, markdown streaming, theme switching, and graceful
// fallback for non-unicode / non-color terminals.
//
// Controls:
//   Tab / Shift-Tab   cycle focus between the three panels
//   Arrow keys        navigate the focused list / scroll text view
//   Enter             act on the focused list item (toggles theme / progress)
//   q or Ctrl-C       quit
#include "tui/tui.hpp"
#include "tui/widgets.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tp = tui::platform;
using namespace tui;

static tui::Capabilities gCaps;
static int gCols = 80, gRows = 24;

// ---------------------------------------------------------------------------
// Palette: cycle through a few themes so the user can compare palettes.
// ---------------------------------------------------------------------------
struct Palette {
  tui::Theme theme;
  std::vector<std::pair<float, tui::Color>> gradient;  // for header
  std::string label;
};

static std::vector<Palette> palettes() {
  Palette aurora;
  aurora.label = "Aurora";
  aurora.theme = tui::Theme{};
  aurora.gradient = {{0.f, tui::Color::rgb( 60,  90, 180)},
                     {1.f, tui::Color::rgb(220,  90, 180)}};
  Palette solar;
  solar.label = "Solar";
  solar.theme = tui::Theme{};
  solar.theme.warn = tui::Color::rgb(255, 96, 0);
  solar.gradient = {{0.f, tui::Color::rgb(255, 140,  40)},
                    {1.f, tui::Color::rgb(240, 220,  80)}};
  Palette forest;
  forest.label = "Forest";
  forest.theme = tui::Theme{};
  forest.theme.warn = tui::Color::rgb(220, 180, 60);
  forest.gradient = {{0.f, tui::Color::rgb( 30, 110,  60)},
                     {1.f, tui::Color::rgb(170, 220, 120)}};
  return {aurora, solar, forest};
}

// Cached palette list: palettes() builds fresh vectors; use this to avoid
// rebuilding three themes + gradients every frame.
static std::vector<Palette> const& palsRef() {
  static std::vector<Palette> const pals = palettes();
  return pals;
}

// Build a markdown render theme from a UI Theme. Mirrors the palette mapping
// used by the main agent app so headings/code/links read consistently.
static tui::MdTheme makeMdTheme(tui::Theme const& th) {
  tui::MdTheme md;
  md.text        = th.sDim();
  md.heading     = tui::Style::plain().fg(th.accent2).attr(tui::AttrBold, true);
  md.bold        = tui::Style::plain().fg(th.accent1).attr(tui::AttrBold, true);
  md.italic      = tui::Style::plain().fg(th.accent3).attr(tui::AttrItalic, true);
  md.code        = tui::Style::plain().fg(th.accent1);
  md.codeBlock   = tui::Style::plain().fg(th.fg);
  md.codeFence   = th.sDim();
  md.codeKeyword = tui::Style::plain().fg(th.accent2).attr(tui::AttrBold, true);
  md.codeType    = tui::Style::plain().fg(th.accent1).attr(tui::AttrBold, true);
  md.codeString  = tui::Style::plain().fg(th.accent3);
  md.codeComment = th.sDim();
  md.codeNumber  = tui::Style::plain().fg(th.warn);
  md.codeFunc    = tui::Style::plain().fg(th.accent1);
  md.codePreproc = tui::Style::plain().fg(th.error_);
  md.quote       = th.sDim();
  md.quoteMark   = tui::Style::plain().fg(th.accent2).attr(tui::AttrBold, true);
  md.bullet      = tui::Style::plain().fg(th.accent1).attr(tui::AttrBold, true);
  md.bulletText  = th.sDim();
  md.link        = tui::Style::plain().fg(th.accent1).attr(tui::AttrUnderline, true);
  md.linkUrl     = th.sDim();
  md.hr          = th.sDim();
  md.tableHead   = tui::Style::plain().fg(th.accent2).attr(tui::AttrBold, true)
                              .attr(tui::AttrUnderline, true);
  md.table       = th.sDim();
  md.strike      = th.sDim();
  md.math        = tui::Style::plain().fg(th.accent2).attr(tui::AttrItalic, true);
  md.mathBlock   = tui::Style::plain().fg(th.accent2).attr(tui::AttrItalic, true);
  return md;
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
enum class Focus { Nav, Body, Side };

static Focus       gFocus       = Focus::Nav;
static int         gTheme       = 0;
static tui::Theme  gThemeNow    = palsRef()[0].theme;
static std::vector<std::pair<float, tui::Color>> gGradient = palsRef()[0].gradient;
static tui::List   gNav;
static tui::TextView gBody;
static std::string gBodySample;
static size_t      gBodyPos    = 0;
static int         gPhase      = 0;       // spinner phase
static int         gFrame      = 0;
static float       gProgress   = 0.0f;    // 0..1
static float       gProgressV  = 0.013f;  // per-tick delta (toggles)
static bool        gStreaming  = false;
static bool        gRunning    = true;
static tui::InputParser gParser;   // persists across read() calls so multi-byte
                                    // escape sequences aren't split/lost

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void enableModes() {
  if (!gCaps.ansi) return;
  if (gCaps.mouse) tp::writeAll("\x1b[?1000h\x1b[?1002h\x1b[?1006h", 24);
  if (gCaps.bracketedPaste) tp::writeAll("\x1b[?2004h", 8);
  if (gCaps.focusEvents) tp::writeAll("\x1b[?1004h", 8);
}

static void disableModes() {
  if (!gCaps.ansi) return;
  if (gCaps.focusEvents) tp::writeAll("\x1b[?1004l", 8);
  if (gCaps.bracketedPaste) tp::writeAll("\x1b[?2004l", 8);
  if (gCaps.mouse) tp::writeAll("\x1b[?1000l\x1b[?1002l\x1b[?1006l", 24);
}

static tui::Style focusBorderStyle(tui::Theme const& th, bool focused) {
  return focused ? th.sBorderH() : th.sBorder();
}

static void drawHeader(tui::Screen& s, tui::Rect r) {
  if (r.w <= 0 || r.h <= 0) return;
  tui::drawGradientH(s, r, gGradient);
  // Overlay title (centered)
  std::string title = "tui studio";
  int tx = r.x + (r.w - (int)title.size()) / 2;
  int ty = r.y + r.h / 2;
  Style stTitle = Style::plain().fg(pickContrastingFg(gGradient.front().second))
                            .attr(tui::AttrBold, true);
  if (ty >= 0 && ty < s.rows() && tx > r.x)
    s.putText(tx, ty, title.c_str(), stTitle);
  // Left: small palette chip showing current theme.
  std::string chip = " " + palsRef()[gTheme].label + " ";
  if (r.h >= 1 && r.x + (int)chip.size() + 2 < r.x + r.w) {
    s.putText(r.x + 2, ty, chip.c_str(),
              Style::plain().fg(pickContrastingFg(gGradient.front().second)));
  }
  // Right: terminal label.
  std::string right = std::string(tui::glyphsFor(gCaps).hLine, '_') + std::string("ANSI ") +
                      std::to_string(gCaps.colorBits) + "b";
  if (r.h >= 1) {
    int rx = r.x + r.w - (int)right.size() - 1;
    if (rx > r.x) {
      s.putText(rx, ty, right.c_str(),
                Style::plain().fg(pickContrastingFg(gGradient.front().second)));
    }
  }
}

static void drawNav(tui::Screen& s, tui::Rect r, tui::Theme const& th) {
  bool focused = gFocus == Focus::Nav;
  drawBorder(s, r, tui::BorderStyle::Rounded, focusBorderStyle(th, focused));
  if (r.w < 4 || r.h < 3) return;
  tui::Rect inner{r.x + 1, r.y + 1, r.w - 2, r.h - 2};
  // Title bar inside the border (overwrite the top line of the frame on the
  // inner top): title uses heavy/unicode divider so the rounded border is
  // visually pierced through.
  Glyphs g = tui::glyphsFor(gCaps);
  s.put(inner.x, inner.y - 1, g.hLine, th.sDim());
  std::string t = " themes ";
  for (int i = 0; i < (int)t.size() && inner.x + i < inner.x + inner.w; i++) {
    s.put(inner.x + i, inner.y - 1, (uint32_t)(uint8_t)t[i],
          th.sAccent2());
  }
  s.put(inner.x + (int)t.size(), inner.y - 1, g.hLine, th.sDim());
  gNav.render(s, inner, th.sDim(), th.sAccent1());
}

static void drawSidePanel(tui::Screen& s, tui::Rect r, tui::Theme const& th) {
  bool focused = gFocus == Focus::Side;
  drawBorder(s, r, focused ? tui::BorderStyle::Heavy : tui::BorderStyle::Single,
             focusBorderStyle(th, focused));
  if (r.w < 6 || r.h < 4) return;
  tui::Rect inner{r.x + 2, r.y + 1, r.w - 4, r.h - 2};

  // Spinner + label
  SpinnerSet const& sp = spinnerDots();
  Style spStyle = th.sAccent3();
  char32_t fc = spinnerFrame(sp, gPhase);
  if (inner.w >= 4) {
    s.put(inner.x, inner.y, fc, spStyle);
    s.putText(inner.x + 2, inner.y, "loading", th.sDim());
  }

  // Smooth progress bar (1/3 of remaining height)
  int barY = inner.y + 2;
  if (barY + 1 < inner.y + inner.h) {
    tui::Rect bar{inner.x, barY, inner.w, 1};
    drawProgressSmooth(s, bar, gProgress,
                       th.accent3, tui::Color::index(236));
    // percentage label overlaid on right
    char pct[8];
    std::snprintf(pct, sizeof(pct), "%3d%%", (int)(gProgress * 100.f));
    int labelW = (int)std::strlen(pct);
    int labelX = inner.x + inner.w - labelW;
    Style labelStyle = Style::plain().fg(pickContrastingFg(th.accent3));
    s.putText(labelX, barY, pct, labelStyle);
  }

  // Mini gauge: a 16-segment RLE'd strip
  int gy = barY + 2;
  if (gy + 1 < inner.y + inner.h) {
    char32_t full = tui::glyphsFor(gCaps).blockFull;
    int segs = inner.w;
    for (int i = 0; i < segs; i++) {
      float v = std::sin((gFrame + i * 4) * 0.18f) * 0.5f + 0.5f;
      Color c = i < segs / 2 ? th.accent1 : th.accent2;
      if (v > 0.45f) s.put(inner.x + i, gy, full,
                          Style::plain().fg(c).bg(tui::Color::index(236)));
      else            s.put(inner.x + i, gy, ' ',
                          Style::plain().fg(tui::Color::index(236))
                                         .bg(tui::Color::index(236)));
    }
  }

  // ASCII-style stat block
  int sy = gy + 2;
  if (sy + 4 < inner.y + inner.h) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "frame:%06d", gFrame);
    s.putText(inner.x, sy++, buf, th.sDim());
    std::snprintf(buf, sizeof(buf), "cols:%d  rows:%d", gCols, gRows);
    s.putText(inner.x, sy++, buf, th.sDim());
    std::snprintf(buf, sizeof(buf), "colors:%d bit", gCaps.colorBits);
    s.putText(inner.x, sy++, buf, th.sDim());
    std::snprintf(buf, sizeof(buf), "focus:%s",
                  gFocus == Focus::Nav ? "nav" :
                  gFocus == Focus::Body ? "body" : "side");
    s.putText(inner.x, sy++, buf, th.sAccent2());
  }
}

static void drawBody(tui::Screen& s, tui::Rect r, tui::Theme const& th) {
  bool focused = gFocus == Focus::Body;
  drawBorder(s, r, focused ? tui::BorderStyle::Double : tui::BorderStyle::Rounded,
             focusBorderStyle(th, focused));
  if (r.w < 4 || r.h < 3) return;
  tui::Rect inner{r.x + 1, r.y + 1, r.w - 2, r.h - 2};
  tui::Glyphs g = tui::glyphsFor(gCaps);
  std::string t = " markdown stream ";
  s.put(inner.x, inner.y - 1, g.hLine, th.sDim());
  for (int i = 0; i < (int)t.size() && inner.x + i < inner.x + inner.w; i++)
    s.put(inner.x + i, inner.y - 1, (uint32_t)(uint8_t)t[i], th.sAccent1());
  s.put(inner.x + (int)t.size(), inner.y - 1, g.hLine, th.sDim());

  // Show the most recent lines of the text view.
  int total = gBody.lineCount();
  int scroll = std::max(0, total - inner.h);
  tui::drawTextView(s, inner, gBody, scroll, th.sDim());

  // If still streaming, blink a small caret in the corner.
  if (gStreaming && (gFrame / 6) % 2 == 0 && inner.w >= 2) {
    s.put(inner.x + inner.w - 1, inner.y + inner.h - 1,
          tui::glyphsFor(gCaps).blockFull, th.sAccent3());
  }
}

static void drawStatus(tui::Screen& s, tui::Rect r, tui::Theme const& th) {
  drawBorder(s, r, tui::BorderStyle::None, tui::Style::plain());
  using Seg = std::pair<std::string, tui::Style>;
  std::vector<Seg> segs;
  segs.emplace_back(" tab ", th.sAccent2());
  segs.emplace_back("switch focus  ", th.sDim());
  segs.emplace_back(" enter ", th.sAccent1());
  segs.emplace_back("toggle theme  ", th.sDim());
  segs.emplace_back(" space ", th.sAccent3());
  segs.emplace_back("pause/resume  ", th.sDim());
  segs.emplace_back(" q ", th.sError());
  segs.emplace_back("quit", th.sDim());
  tui::drawStatusBar(s, r, segs, th.sDim());
}

static void streamNextChar() {
  if (!gStreaming) return;
  if (gBodyPos >= gBodySample.size()) {
    gStreaming = false;
    gBody.endStream();
    return;
  }
  // Feed a few characters at a time so visible feedback feels lively.
  int chunk = 3;
  std::string chunkStr;
  for (int i = 0; i < chunk && gBodyPos < gBodySample.size(); ++i, ++gBodyPos) {
    chunkStr.push_back(gBodySample[gBodyPos]);
  }
  gBody.streamAppend(chunkStr.c_str());
}

static void resetStream() {
  gBody.clear();
  gBody.setWidth(std::max(20, gCols - 6));  // matches the body inner width
  // A tiny markdown document showcasing inline formatting, lists, code,
  // and a quote; the text view breaks at width automatically.
  gBodySample =
    "## welcome to *tui studio*\n"
    "\n"
    "this window streams a tiny markdown document in real time so you can\n"
    "watch the renderer work.\n"
    "\n"
    "- **widgets**: gradient, spinner, progress, status, list\n"
    "- **compat**: falls back to ascii on dumb terminals\n"
    "- **perf**: dirty-row + per-row column diff, sync updates\n"
    "\n"
    "> the bar at the bottom hints at the keyboard map.\n"
    "\n"
    "    while (running) render();\n"
    "\n"
    "_end of stream_\n";
  gBodyPos = 0;
  gBody.beginStreamMarkdown(makeMdTheme(gThemeNow));
  gStreaming = true;
}

static void cycleFocus(int dir) {
  int f = (int)gFocus;
  int n = 3;
  f = (f + dir + n) % n;
  gFocus = (Focus)f;
}

static void applyTheme(int idx) {
  auto const& pals = palsRef();
  gTheme = (idx + (int)pals.size()) % (int)pals.size();
  gThemeNow = pals[gTheme].theme;
  gGradient = pals[gTheme].gradient;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
  gCaps = tui::detectCapabilities();
  if (!tp::enterRaw()) return 1;
  tp::enterAlternateScreen();
  tp::hideCursor();
  enableModes();

  // initial size
  tp::getSize(gCols, gRows);

  // populate nav
  gNav.setItems({"Aurora", "Solar", "Forest"});
  gNav.setSelected(gTheme);

  resetStream();

  tui::Screen screen;
  screen.resize(gCols, gRows);
  tui::Renderer renderer(gCaps);
  tui::Out out;

  auto lastTick = std::chrono::steady_clock::now();
  while (gRunning) {
    // ---- input ----
    uint8_t buf[256];
    int n = tp::readInput(buf, sizeof(buf), 16);
    if (n > 0) {
      gParser.feed(buf, (size_t)n);
      tui::Event ev;
      while (gParser.next(ev)) {
        if (ev.type == tui::EventType::Key) {
          uint32_t k = ev.ch;
          if (k == 'q' || k == 'Q' || k == tui::KeyCtrlC) gRunning = false;
          else if (k == tui::KeyTab) cycleFocus(1);
          else if (k == tui::KeyBackTab) cycleFocus(-1);
          else if (k == tui::KeyEnter) {
            if (gFocus == Focus::Nav) applyTheme(gNav.selected());
          }
          else if (k == ' ') {
            gStreaming = !gStreaming;
            if (gStreaming) {
              if (gBodyPos >= gBodySample.size()) resetStream();
              else gBody.streamAppend("");
            }
          }
          else if (k == tui::KeyEscape) {
            // no-op but swallow the key
          }
          else {
            if (gFocus == Focus::Nav) gNav.handle(ev);
          }
        }
        else if (ev.type == tui::EventType::Resize) {
          gCols = ev.cols;
          gRows = ev.rows;
          screen.resize(gCols, gRows);
        }
      }
    }

    // ---- advance animation / streaming ----
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count();
    if (ms >= 80) {
      lastTick = now;
      gPhase++;
      gFrame++;
      // Stream a small chunk.
      streamNextChar();
      // Bouncing progress.
      gProgress += gProgressV;
      if (gProgress >= 1.0f) { gProgress = 1.0f; gProgressV = -std::abs(gProgressV); }
      if (gProgress <= 0.0f) { gProgress = 0.0f; gProgressV = std::abs(gProgressV); }
    }

    // ---- layout ----
    int W = screen.cols(), H = screen.rows();
    int headerH = 3;
    int statusH = 1;
    int bodyTop = headerH;
    int bodyBot = H - statusH;
    int navW = std::max(18, W / 6);
    int sideW = std::max(20, W / 5);
    int mainW = W - navW - sideW;
    if (mainW < 10) {
      // Too narrow: collapse side panel into nav.
      sideW = 0;
      mainW = W - navW;
      if (mainW < 10) {
        navW = 0;
        mainW = W;
      }
    }

    tui::Rect rHeader{0, 0, W, headerH};
    tui::Rect rNav   {0, bodyTop, navW, bodyBot - bodyTop};
    tui::Rect rMain  {navW, bodyTop, mainW, bodyBot - bodyTop};
    tui::Rect rSide  {navW + mainW, bodyTop, sideW, bodyBot - bodyTop};
    tui::Rect rStatus{0, bodyBot, W, statusH};

    // Clear unfocused empty rects so leftover glyphs don't leak across resize.
    screen.clearAll();

    drawHeader(screen, rHeader);
    if (navW > 0)   drawNav(screen, rNav, gThemeNow);
    if (mainW > 0)  drawBody(screen, rMain, gThemeNow);
    if (sideW > 0)  drawSidePanel(screen, rSide, gThemeNow);
    drawStatus(screen, rStatus, gThemeNow);

    // ---- present ----
    renderer.frame(out, screen);
    tp::writeAll(out.data(), out.size());
    tp::flush();
  }

  tp::showCursor();
  disableModes();
  tp::leaveAlternateScreen();
  tp::leaveRaw();
  return 0;
}