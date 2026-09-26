// tui showcase - walks through every feature of the library:
//   colors (truecolor/256/16 with automatic downgrade), box drawing,
//   TextView scrollback + streaming, Input with history, List menu,
//   mouse/focus events, resize, diff-rendered status bar.
#include "tui/tui.hpp"
#include "tui/widgets.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static tui::Capabilities gCaps;
static int gCols = 80, gRows = 24;

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
enum PageId { PageColors, PageBoxes, PageScroll, PageStream, PageInput, PageMouse, PageCount };

static char const* kPageNames[] = {"colors", "boxes", "scroll text", "streaming", "input", "mouse"};

static int gPage = PageColors;
static bool gFocusList = true;  // true = list navigation, false = input line

static tui::List gList;
static tui::TextView gScrollTV;
static int gScroll = 0;
static tui::TextView gStreamTV;
static std::string gStreamSrc;
static size_t gStreamPos = 0;
static bool gStreaming = false;
static bool gStreamDone = false;
static tui::TextView gInputLog;
static tui::Input gInput;
static std::string gMouseLine = "no mouse events yet";
static uint64_t gFrame = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void enableModes() {
  using tui::platform::writeAll;
  if (!gCaps.ansi) return;
  if (gCaps.mouse) writeAll("\x1b[?1000h\x1b[?1002h\x1b[?1006h", 24);
  if (gCaps.bracketedPaste) writeAll("\x1b[?2004h", 8);
  if (gCaps.focusEvents) writeAll("\x1b[?1004h", 8);
}

static void drawDoubleBorder(tui::Screen& s, tui::Rect r, tui::Style st) {
  if (r.w <= 1 || r.h <= 1) return;
  int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
  for (int x = r.x + 1; x < x1; x++) {
    s.put(x, r.y, 0x2550, st);
    s.put(x, y1, 0x2550, st);
  }
  for (int y = r.y + 1; y < y1; y++) {
    s.put(r.x, y, 0x2551, st);
    s.put(x1, y, 0x2551, st);
  }
  s.put(r.x, r.y, 0x2554, st);
  s.put(x1, r.y, 0x2557, st);
  s.put(r.x, y1, 0x255A, st);
  s.put(x1, y1, 0x255D, st);
}

static void drawRoundedBorder(tui::Screen& s, tui::Rect r, tui::Style st) {
  if (r.w <= 1 || r.h <= 1) return;
  uint32_t h = gCaps.unicode ? 0x2500 : '-', v = gCaps.unicode ? 0x2502 : '|';
  uint32_t tl = gCaps.unicode ? 0x256D : '+', tr = gCaps.unicode ? 0x256E : '+';
  uint32_t bl = gCaps.unicode ? 0x2570 : '+', br = gCaps.unicode ? 0x256F : '+';
  int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
  for (int x = r.x + 1; x < x1; x++) {
    s.put(x, r.y, h, st);
    s.put(x, y1, h, st);
  }
  for (int y = r.y + 1; y < y1; y++) {
    s.put(r.x, y, v, st);
    s.put(x1, y, v, st);
  }
  s.put(r.x, r.y, tl, st);
  s.put(x1, r.y, tr, st);
  s.put(r.x, y1, bl, st);
  s.put(x1, y1, br, st);
}

// ---------------------------------------------------------------------------
// Page renderers
// ---------------------------------------------------------------------------
static void renderColors(tui::Screen& s, tui::Rect box) {
  tui::Style dim = tui::Style::plain().fg(tui::Color::index(244));
  tui::Style header = tui::Style::plain().fg(tui::Color::rgb(255, 200, 50)).attr(tui::AttrBold, true);
  int x = box.x, y = box.y;

  s.putText(x, y++, "truecolor gradient (renderer downgrades if needed)", header);
  for (int row = 0; row < 4; row++) {
    for (int c = 0; c < box.w && c < 48; c++) {
      tui::Style st = tui::Style::plain().bg(tui::Color::rgb((uint8_t)(c * 5), (uint8_t)(40 + row * 40),
                                                             (uint8_t)(255 - c * 5)));
      s.put(x + c, y, ' ', st);
    }
    y++;
  }
  y++;

  s.putText(x, y++, "256-color ramp", header);
  for (int row = 0; row < 4; row++) {
    for (int c = 0; c < 16; c++) {
      s.put(x + c, y, ' ', tui::Style::plain().bg(tui::Color::index((uint8_t)(row * 16 + c))));
    }
    y++;
  }
  y++;

  s.putText(x, y++, "16-color set", header);
  for (int i = 0; i < 8; i++) {
    s.put(x + i, y, ' ', tui::Style::plain().bg(tui::Color::index((uint8_t)i)));
    s.put(x + i, y + 1, ' ', tui::Style::plain().bg(tui::Color::index((uint8_t)(i + 8))));
  }
  y += 3;

  s.putText(x, y, "color depth detected: ", dim);
  s.putText(x + (int)std::string("color depth detected: ").size(), y,
            (std::to_string(gCaps.colorBits) + "-bit").c_str(),
            tui::Style::plain().fg(tui::Color::rgb(80, 200, 255)));
}

static void renderBoxes(tui::Screen& s, tui::Rect box) {
  tui::Style dim = tui::Style::plain().fg(tui::Color::index(244));
  tui::Style border = tui::Style::plain().fg(tui::Color::rgb(0, 255, 150));
  tui::Style accent = tui::Style::plain().fg(tui::Color::rgb(80, 200, 255));
  int x = box.x, y = box.y;

  s.putText(x, y++, "single border  (drawBorder)", dim);
  tui::drawBorder(s, tui::Rect{x, y, 16, 4}, border);
  y += 5;

  s.putText(x, y++, "double border", dim);
  drawDoubleBorder(s, tui::Rect{x, y, 16, 4}, border);
  y += 5;

  s.putText(x, y++, "rounded corners", dim);
  drawRoundedBorder(s, tui::Rect{x, y, 16, 4}, border);
  y += 5;

  s.putText(x, y++, "block bar chart", dim);
  int vals[] = {3, 7, 2, 9, 5, 4, 8, 6};
  for (int i = 0; i < 8; i++) {
    for (int b = 0; b < vals[i]; b++) {
      s.put(x + i * 2, y + vals[i] - b - 1, 0x2588, accent);
    }
  }
  for (int i = 0; i < 8; i++) s.put(x + i * 2, y + 9, 0x2500, dim);
}

static void renderScroll(tui::Screen& s, tui::Rect box) {
  tui::Style dim = tui::Style::plain().fg(tui::Color::index(244));
  int maxScroll = gScrollTV.lineCount() - box.h;
  if (maxScroll < 0) maxScroll = 0;
  gScroll = std::max(0, std::min(gScroll, maxScroll));
  tui::drawTextView(s, box, gScrollTV, gScroll, tui::Style::plain());
  std::string info = "line " + std::to_string(gScroll) + " / " +
                     std::to_string(std::max(0, gScrollTV.lineCount() - box.h)) +
                     "   (PgUp/PgDn or mouse wheel)";
  s.putText(box.x, box.y + box.h - 1, info.c_str(), dim);
}

static void renderStream(tui::Screen& s, tui::Rect box) {
  tui::Style dim = tui::Style::plain().fg(tui::Color::index(244));
  int maxScroll = gStreamTV.lineCount() - box.h;
  if (maxScroll < 0) maxScroll = 0;
  tui::drawTextView(s, box, gStreamTV, maxScroll, tui::Style::plain());  // auto-scroll
  std::string state = gStreamDone ? "[done]  s = replay, c = clear"
                                  : (gStreaming ? "[streaming...]  s = stop" : "[paused]  s = start");
  s.putText(box.x, box.y + box.h - 1, state.c_str(), dim);
}

static void renderInputPage(tui::Screen& s, tui::Rect box) {
  tui::Style dim = tui::Style::plain().fg(tui::Color::index(244));
  int maxScroll = gInputLog.lineCount() - box.h;
  if (maxScroll < 0) maxScroll = 0;
  tui::drawTextView(s, box, gInputLog, maxScroll, tui::Style::plain());
  s.putText(box.x, box.y + box.h - 1, "type on the input line below, Enter submits, Up/Down recalls history",
            dim);
}

static void renderMouse(tui::Screen& s, tui::Rect box) {
  tui::Style dim = tui::Style::plain().fg(tui::Color::index(244));
  tui::Style accent = tui::Style::plain().fg(tui::Color::rgb(80, 200, 255));
  s.putText(box.x, box.y, gMouseLine.c_str(), accent);
  s.putText(box.x, box.y + 2, "click a list item to navigate pages; wheel scrolls.", dim);
  s.putText(box.x, box.y + 3, "focus events (terminal entered/left) also fire on the left panel.", dim);
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void advanceStream() {
  if (!gStreaming || gStreamDone) return;
  if (gStreamPos >= gStreamSrc.size()) {
    gStreamTV.endStream();
    gStreaming = false;
    gStreamDone = true;
    return;
  }
  size_t take = 1 + (gFrame % 3);
  size_t n = std::min(take, gStreamSrc.size() - gStreamPos);
  gStreamTV.streamAppend(gStreamSrc.substr(gStreamPos, n).c_str());
  gStreamPos += n;
}

static void handleEvent(tui::Event const& e) {
  if (e.type == tui::EventType::Mouse) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "mouse  x=%d y=%d  buttons=%d  wheel=%+d  %s%s",
                  e.mouse.x, e.mouse.y, e.mouse.buttons, e.mouse.wheel,
                  e.mouse.press ? "press " : "", e.mouse.release ? "release" : "");
    gMouseLine = buf;
    // click on the list column selects a page
    if (e.mouse.press && e.mouse.x >= 2 && e.mouse.x < 20 && e.mouse.y >= 3 && e.mouse.y < gRows - 3) {
      int idx = e.mouse.y - 3;
      if (idx >= 0 && idx < PageCount) {
        gList.setSelected(idx);
        gPage = gList.selected();
      }
    }
    if (gPage == PageScroll && e.mouse.wheel != 0) {
      gScroll += (e.mouse.wheel > 0) ? -3 : 3;
    }
    return;
  }
  if (e.type == tui::EventType::FocusIn) gMouseLine = "focus in (terminal entered)";
  if (e.type == tui::EventType::FocusOut) gMouseLine = "focus out (terminal left)";
  if (e.type != tui::EventType::Key) return;

  if (e.ch == tui::KeyEscape || e.ch == tui::KeyCtrlC) {
    // quitting is handled by 'q'; keep Escape harmless here
    return;
  }
  if (e.ch == '\t' || e.ch == tui::KeyTab) {
    gFocusList = !gFocusList;
    return;
  }

  // when the input line has focus it consumes keys
  if (!gFocusList) {
    if (gInput.handle(e)) {  // Enter
      std::string t = gInput.text();
      gInputLog.append("> " + t, tui::Style::plain().fg(tui::Color::rgb(230, 230, 230)));
      gInput.clear();
    }
    return;
  }

  // list focus: navigation + activation
  if (gList.handle(e)) {
    gPage = gList.selected();
    return;
  }
  if (e.ch == '\n' || e.ch == tui::KeyEnter || e.ch == ' ') {
    gPage = gList.selected();
    return;
  }
  if (e.ch >= '1' && e.ch <= '6') {
    gPage = (int)(e.ch - '1');
    gList.setSelected(gPage);
    return;
  }
  switch (e.ch) {
    case 'q':
      // handled at loop level via running flag; keep simple: fall through
      break;
    case 's':
      if (gPage == PageStream) {
        if (gStreamDone) {
          gStreamTV.clear();
          gStreamPos = 0;
          gStreamDone = false;
        }
        if (!gStreaming) {
          gStreamTV.beginStream(tui::Style::plain().fg(tui::Color::rgb(230, 230, 230)));
          gStreaming = true;
        } else {
          gStreamTV.endStream();
          gStreaming = false;
        }
      }
      break;
    case 'c':
      if (gPage == PageStream) {
        gStreamTV.clear();
        gStreamPos = 0;
        gStreamDone = false;
      }
      break;
    case tui::KeyPageUp:
      if (gPage == PageScroll) gScroll -= gRows / 2;
      break;
    case tui::KeyPageDown:
      if (gPage == PageScroll) gScroll += gRows / 2;
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Frame render
// ---------------------------------------------------------------------------
static void render(tui::Screen& screen, tui::Renderer& renderer, tui::Out& out) {
  screen.clearAll();

  tui::Style header = tui::Style::plain().fg(tui::Color::rgb(255, 200, 50)).attr(tui::AttrBold, true);
  tui::Style dim = tui::Style::plain().fg(tui::Color::index(244));
  tui::Style accent = tui::Style::plain().fg(tui::Color::rgb(80, 200, 255));
  tui::Style border = tui::Style::plain().fg(tui::Color::index(240));
  tui::Style hi = tui::Style::plain().fg(tui::Color::rgb(0, 0, 0)).bg(tui::Color::rgb(80, 200, 255));
  tui::Style no = tui::Style::plain().fg(tui::Color::rgb(230, 230, 230));

  // header
  screen.putText(1, 0, "tui showcase", header);
  std::string capsStr = "   colors=" + std::to_string(gCaps.colorBits) + "bit  unicode=" +
                        (gCaps.unicode ? "on" : "off") + "  mouse=" + (gCaps.mouse ? "on" : "off") +
                        "  paste=" + (gCaps.bracketedPaste ? "on" : "off");
  screen.putText(1 + 12, 0, capsStr.c_str(), dim);

  // panels
  tui::Rect mainBox(0, 1, gCols, gRows - 3);
  tui::Rect leftBox = tui::splitLeft(mainBox, 20);
  tui::Rect rightBox(leftBox.x + leftBox.w, 1, gCols - leftBox.w, gRows - 3);

  tui::drawBorder(screen, leftBox, border);
  screen.putText(leftBox.x + 2, leftBox.y + 1, "pages", header);
  tui::Rect listBox(leftBox.x + 2, leftBox.y + 2, leftBox.w - 3, leftBox.h - 3);
  gList.render(screen, listBox, no, hi);

  tui::drawBorder(screen, rightBox, border);
  screen.putText(rightBox.x + 2, rightBox.y + 1, kPageNames[gPage], header);
  tui::Rect content(rightBox.x + 2, rightBox.y + 2, rightBox.w - 3, rightBox.h - 3);

  switch (gPage) {
    case PageColors: renderColors(screen, content); break;
    case PageBoxes: renderBoxes(screen, content); break;
    case PageScroll: renderScroll(screen, content); break;
    case PageStream: renderStream(screen, content); break;
    case PageInput: renderInputPage(screen, content); break;
    case PageMouse: renderMouse(screen, content); break;
    default: break;
  }

  // input line (always visible; highlight when focused)
  tui::Style promptSt = gFocusList ? dim : accent;
  tui::Style textSt = tui::Style::plain().fg(tui::Color::rgb(230, 230, 230));
  tui::Style cursorSt = tui::Style::plain().fg(tui::Color::rgb(0, 0, 0)).bg(tui::Color::rgb(230, 230, 230));
  gInput.setBox(tui::Rect(2, gRows - 2, gCols - 3, 1));
  gInput.render(screen, "\xE2\x9D\xAF ", promptSt, textSt, cursorSt);

  // status bar
  std::vector<std::pair<std::string, tui::Style>> segs;
  segs.push_back({" [Tab] focus:" + std::string(gFocusList ? "list" : "input"), dim});
  segs.push_back({" [1-6] page:" + std::to_string(gPage + 1) + "/" + std::to_string(PageCount), accent});
  segs.push_back({" [q] quit", dim});
  if (gPage == PageScroll) segs.push_back({" [PgUp/PgDn] scroll", dim});
  if (gPage == PageStream)
    segs.push_back({std::string(" [s] stream:") + (gStreaming ? "on" : "off"), accent});
  tui::drawStatusBar(screen, tui::Rect(0, gRows - 1, gCols, 1), segs,
                     tui::Style::plain().bg(tui::Color::rgb(25, 25, 25)));

  renderer.frame(out, screen);
  tui::platform::writeAll(out.data(), out.size());
  tui::platform::flush();
  tui::platform::showCursor();
  // park the real terminal cursor on the input line
  std::string mv = "\x1b[" + std::to_string(gRows - 1) + ";1H";
  tui::platform::writeAll(mv.data(), mv.size());
}

// ---------------------------------------------------------------------------
int main() {
  if (!tui::platform::enterRaw()) {
    std::fprintf(stderr, "raw mode failed\n");    return 1;
  }
  gCaps = tui::detectCapabilities();
  tui::platform::enterAlternateScreen();
  enableModes();

  tui::Screen screen;
  tui::Renderer renderer(gCaps);
  tui::Out out;
  tui::InputParser parser;

  std::vector<std::string> items;
  for (int i = 0; i < PageCount; i++) items.push_back(kPageNames[i]);
  gList.setItems(items);
  gList.setSelected(0);

  tui::platform::getSize(gCols, gRows);
  screen.resize(gCols, gRows);
  gScrollTV.setWidth(gCols - 26);
  gStreamTV.setWidth(gCols - 26);
  gInputLog.setWidth(gCols - 26);

  // seed the scrollback page with a long mixed-language text
  for (int p = 0; p < 6; p++) {
    gScrollTV.append(
        "Paragraph " + std::to_string(p + 1) +
            ": the tui library is dependency-free, fast and small. 瀹冧娇鐢ㄥ弻缂撳啿灞忓箷涓庡樊鍒嗘覆鏌擄紝"
            "鍙噸鐢诲彂鐢熷彉鍖栫殑琛屻€俉ord wrapping, wide characters 鍜?Emoji 鍧囨纭鐞嗐€俓n\n",
        tui::Style::plain().fg(tui::Color::rgb(230, 230, 230)));
  }
  gScrollTV.append(
      "Scrolling: use PgUp / PgDn or the mouse wheel to move through this "
      "scrollback. The offset is managed by the app, the widget only wraps lines. "
      "涓枃鏂囨湰涔熻兘姝ｅ父鎹㈣鍜屽洖鐪嬨€俓n\n",
      tui::Style::plain().fg(tui::Color::rgb(230, 230, 230)));

  // streaming demo source
  gStreamSrc =
      "**Streaming demo** 娣峰悎涓嫳鏂囩殑瀹炴椂娓叉煋銆俓n\n- 琛屽唴浠ｇ爜 `streamAppend` 姣忔鍙拷鍔犺嫢骞插瓧绗n"
      "- 灏鹃儴浼氬疄鏃堕噸鏂版崲琛?(rewrap)\n\n```\ndef hello():\n    return \"world\"\n```\n\n"
      "杩欐槸鏈€鍚庝竴娈点€傛祦寮忚緭鍑哄湪娓叉煋鐨勫悓鏃朵繚鎸?UI 鍝嶅簲锛屾瘡绉掑埛鏂扮害 30 甯э紝"
      "宸垎娓叉煋鍙緭鍑哄彉鏇磋銆俓n\nThanks for watching!";

  bool running = true;
  while (running) {
    gFrame++;

    uint8_t buf[2048];
    int n = tui::platform::readInput(buf, sizeof buf, 30);
    if (n > 0) {
      parser.feed(buf, (size_t)n);
      tui::Event ev;
      while (parser.next(ev)) handleEvent(ev);
    } else {
      parser.expireTimeout();
    }

    int nc, nr;
    tui::platform::getSize(nc, nr);
    if (nc != gCols || nr != gRows) {
      gCols = nc;
      gRows = nr;
      screen.resize(nc, nr);
    }
    gScrollTV.setWidth(gCols - 26);
    gStreamTV.setWidth(gCols - 26);
    gInputLog.setWidth(gCols - 26);

    advanceStream();
    render(screen, renderer, out);
  }

  tui::platform::leaveAlternateScreen();
  tui::platform::leaveRaw();
  return 0;
}
