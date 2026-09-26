#include "tui/tui.hpp"
#include <cstring>

static void enableModes(tui::Capabilities const& c) {
  using tui::platform::writeAll;
  if (c.ansi) {
    if (c.mouse) writeAll("\x1b[?1000h\x1b[?1002h\x1b[?1006h", 24);
    if (c.bracketedPaste) writeAll("\x1b[?2004h", 8);
    if (c.focusEvents) writeAll("\x1b[?1004h", 8);
  }
}

int main() {
  if (!tui::platform::enterRaw()) return 1;
  tui::Capabilities caps = tui::detectCapabilities();
  tui::platform::enterAlternateScreen();
  enableModes(caps);

  tui::Screen screen;
  tui::Renderer renderer(caps);
  tui::Out out;
  tui::InputParser parser;

  int cols = caps.cols, rows = caps.rows;
  screen.resize(cols, rows);

  // movable box state
  int bx = 8, by = 5;

  bool running = true;
  while (running) {
    screen.clearAll();

    tui::Style title = tui::Style::plain().fg(tui::Color::rgb(255, 200, 50)).attr(tui::AttrBold, true);
    tui::Style dim = tui::Style::plain().fg(tui::Color::index(244)).attr(tui::AttrDim, true);
    tui::Style accent = tui::Style::plain().fg(tui::Color::rgb(80, 200, 255));
    tui::Style boxStyle = tui::Style::plain().fg(tui::Color::rgb(0, 255, 150)).attr(tui::AttrBold, true);
    tui::Style border = tui::Style::plain().fg(tui::Color::index(240));

    screen.putText(2, 1, "tui demo  -  arrows: move box, j/k: vertical, +/-: resize, q/Esc: quit", title);
    screen.putText(2, 2, "sizes:", dim);
    screen.putText(9, 2, std::to_string(cols).c_str(), accent);
    screen.putText(9 + (int)std::to_string(cols).size(), 2, "x", dim);
    screen.putText(11 + (int)std::to_string(cols).size(), 2, std::to_string(rows).c_str(), accent);
    std::string capsLine = "caps: " + std::to_string(caps.colorBits) + "-bit color, mouse=" +
                           (caps.mouse ? "on" : "off");
    screen.putText(2, 3, capsLine.c_str(), dim);

    // draw a box with border (codepoints: ┌┐└┘─│)
    int bw = 20, bh = 5;
    for (int i = 0; i <= bw; i++) {
      screen.put(bx + i, by, (i == 0) ? 0x250C : (i == bw) ? 0x2510 : 0x2500, border);
      screen.put(bx + i, by + bh, (i == 0) ? 0x2514 : (i == bw) ? 0x2518 : 0x2500, border);
    }
    for (int j = 1; j < bh; j++) {
      screen.put(bx, by + j, 0x2502, border);
      screen.put(bx + bw, by + j, 0x2502, border);
    }
    screen.putText(bx + 2, by + 2, "box (arrows)", boxStyle);

    // fill area with a rainbow strip (█ = 0x2588)
    for (int x = 2; x < 40; x++) {
      tui::Style s = tui::Style::plain().fg(tui::Color::rgb((uint8_t)(x * 6), 100, 255 - (uint8_t)(x * 6)));
      screen.put(x, rows - 2, 0x2588, s);
    }

    renderer.frame(out, screen);
    tui::platform::writeAll(out.data(), out.size());
    tui::platform::flush();
    tui::platform::showCursor();
    tui::platform::writeAll("\x1b[1;1H", 6);

    // input
    uint8_t buf[2048];
    int n = tui::platform::readInput(buf, sizeof buf, 50);
    if (n > 0) {
      parser.feed(buf, (size_t)n);
      tui::Event ev;
      while (parser.next(ev)) {
        if (ev.type == tui::EventType::Key) {
          if (ev.ch == 'q' || ev.ch == tui::KeyEscape) { running = false; break; }
          switch (ev.ch) {
            case tui::KeyUp: by = by > 0 ? by - 1 : 0; break;
            case tui::KeyDown: by = by + 1 < rows - 6 ? by + 1 : by; break;
            case tui::KeyLeft: bx = bx > 0 ? bx - 1 : 0; break;
            case tui::KeyRight: bx = bx + 1 < cols - 22 ? bx + 1 : bx; break;
            default: break;
          }
        }
      }
    } else {
      parser.expireTimeout();
    }

    int nc, nr;
    tui::platform::getSize(nc, nr);
    if (nc != cols || nr != rows) {
      cols = nc;
      rows = nr;
      screen.resize(cols, rows);
    }
  }

  tui::platform::leaveAlternateScreen();
  tui::platform::leaveRaw();
  return 0;
}

