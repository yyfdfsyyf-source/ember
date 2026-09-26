#include "tui/platform.hpp"
#include "tui/screen.hpp"
#include "tui/terminal.hpp"
#include "tui/utf8.hpp"
#include "wcwidth.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <csignal>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace tui::platform {

namespace {

bool g_forceDumb = false;
bool g_raw = false;

#ifdef _WIN32
HANDLE g_in = INVALID_HANDLE_VALUE;
HANDLE g_out = INVALID_HANDLE_VALUE;
DWORD g_oldInMode = 0;
DWORD g_oldOutMode = 0;
UINT g_oldOutputCP = 0;
bool g_vt = false;
bool g_legacy = false;

// --- Legacy console ANSI emulation (Windows without VT) -------------------
namespace legacy {

Screen legacyScreen;  // what we believe the console shows
int cx = 0, cy = 0;
Style cur;

enum St : uint8_t { LText, LEsc, LCSI, LOSC };
St st = LText;
std::string csiBuf;
std::string oscBuf;
int params[16] = {0};
int nParams = 0;
bool oscEsc = false;
std::string utf8buf;

void reset() {
  st = LText;
  cx = 0;
  cy = 0;
  cur = Style::plain();
  legacyScreen.reset();
}

// Map a Color to a Windows console attribute index 0..15.
WORD attrIndex(Color c) {
  if (c.mode() == 1) {
    unsigned idx = c.index();
    return (WORD)((idx & 7) | (idx >= 8 ? 8 : 0));
  }
  if (c.mode() == 2) {
    static const WORD table[16][3] = {
        {0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0},
        {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
        {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
        {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
    };
    int r = c.r(), g = c.g(), b = c.b();
    int best = 0;
    long bestD = 0x7FFFFFFF;
    for (int i = 0; i < 16; i++) {
      long dr = r - table[i][0], dg = g - table[i][1], db = b - table[i][2];
      long d = dr * dr + dg * dg + db * db;
      if (d < bestD) { bestD = d; best = i; }
    }
    return (WORD)best;
  }
  return 7;  // default light gray on black
}

WORD styleAttr(Style s) {
  WORD fg = attrIndex(s.fgColor());
  WORD bg = attrIndex(s.bgColor());
  bool fgDefault = s.fgColor().mode() == 0;
  bool bgDefault = s.bgColor().mode() == 0;
  WORD a = 0;
  if (fgDefault) fg = 7;
  if (bgDefault) bg = 0;
  if (s.attrs() & AttrBold) fg |= 8;
  if (s.attrs() & AttrReverse) { WORD t = fg; fg = bg; bg = t; }
  a = (WORD)(fg | (bg << 4));
  if (s.attrs() & AttrUnderline) a |= 0x8000;  // COMMON_LVB_UNDERSCORE
  return a;
}

void flushRow(HANDLE h, int y, int cols) {
  Screen const& scr = legacyScreen;
  int rowCols = cols;
  std::vector<CHAR_INFO> buf((size_t)rowCols);
  int x = 0;
  while (x < rowCols) {
    Cell const& c = scr.cell(x, y);
    if (c.cont) {
      buf[x].Char.UnicodeChar = L' ';
      buf[x].Attributes = styleAttr(c.st);
      x++;
      continue;
    }
    if (c.ch == 0) {
      buf[x].Char.UnicodeChar = L' ';
      buf[x].Attributes = styleAttr(c.st);
      x++;
      continue;
    }
    uint32_t cp = c.ch;
    if (cp < 0x10000) {
      buf[x].Char.UnicodeChar = (wchar_t)cp;
      buf[x].Attributes = styleAttr(c.st);
      x++;
      if ((int)tui_ww::wcwidth(cp) == 2 && x < rowCols) {
        buf[x].Char.UnicodeChar = L' ';
        buf[x].Attributes = styleAttr(c.st);
        x++;
      }
    } else {
      // supplementary plane: encode as UTF-16 pair across two CHAR_INFO slots
      cp -= 0x10000;
      buf[x].Char.UnicodeChar = (wchar_t)(0xD800 + (cp >> 10));
      buf[x].Attributes = styleAttr(c.st);
      if (x + 1 < rowCols) {
        buf[x + 1].Char.UnicodeChar = (wchar_t)(0xDC00 + (cp & 0x3FF));
        buf[x + 1].Attributes = styleAttr(c.st);
      }
      x += 2;
    }
  }
  COORD size = {(SHORT)rowCols, 1};
  COORD at = {0, (SHORT)y};
  SMALL_RECT r = {(SHORT)0, (SHORT)y, (SHORT)(rowCols - 1), (SHORT)y};
  WriteConsoleOutputW(h, buf.data(), size, at, &r);
}

void flush(HANDLE h, int cols, int rows) {
  for (int y = 0; y < rows; y++) {
    if (legacyScreen.rowDirty(y)) flushRow(h, y, cols);
  }
  legacyScreen.clearDirty();
}

void putChar(uint32_t cp) {
  if (!legacyScreen.inBounds(cx, cy)) return;
  legacyScreen.put(cx, cy, cp, cur);
  int w = (int)tui_ww::wcwidth(cp);
  if (w < 0) w = 1;
  cx += w;
}

void applySgr() {
  int i = 0;
  int maxP = nParams;
  if (maxP == 0) { params[0] = 0; maxP = 1; }
  for (i = 0; i < maxP; i++) {
    int p = params[i];
    if (p == 0) cur = Style::plain();
    else if (p == 1) cur.attr(AttrBold, true);
    else if (p == 2) cur.attr(AttrDim, true);
    else if (p == 3) cur.attr(AttrItalic, true);
    else if (p == 4) cur.attr(AttrUnderline, true);
    else if (p == 5) cur.attr(AttrBlink, true);
    else if (p == 7) cur.attr(AttrReverse, true);
    else if (p == 9) cur.attr(AttrStrike, true);
    else if (p == 22) { cur.attr(AttrBold, false); cur.attr(AttrDim, false); }
    else if (p == 23) cur.attr(AttrItalic, false);
    else if (p == 24) cur.attr(AttrUnderline, false);
    else if (p == 25) cur.attr(AttrBlink, false);
    else if (p == 27) cur.attr(AttrReverse, false);
    else if (p == 29) cur.attr(AttrStrike, false);
    else if (p >= 30 && p <= 37) cur.fg(Color::index((uint8_t)(p - 30)));
    else if (p >= 40 && p <= 47) cur.bg(Color::index((uint8_t)(p - 40)));
    else if (p >= 90 && p <= 97) cur.fg(Color::index((uint8_t)(p - 90 + 8)));
    else if (p >= 100 && p <= 107) cur.bg(Color::index((uint8_t)(p - 100 + 8)));
    else if (p == 39) cur.fg(Color::def());
    else if (p == 49) cur.bg(Color::def());
    else if (p == 38 || p == 48) {
      // 38;5;N or 38;2;r;g;b
      int k = i + 1;
      if (k < maxP && params[k] == 5 && k + 1 < maxP) {
        if (p == 38) cur.fg(Color::index((uint8_t)params[k + 1]));
        else cur.bg(Color::index((uint8_t)params[k + 1]));
        i = k + 1;
      } else if (k < maxP && params[k] == 2 && k + 3 < maxP) {
        if (p == 38) cur.fg(Color::rgb((uint8_t)params[k + 1], (uint8_t)params[k + 2], (uint8_t)params[k + 3]));
        else cur.bg(Color::rgb((uint8_t)params[k + 1], (uint8_t)params[k + 2], (uint8_t)params[k + 3]));
        i = k + 3;
      }
    }
  }
}

void feedByte(unsigned char b, int cols, int rows) {
  switch (st) {
    case LText: {
      if (b == 0x1B) { st = LEsc; return; }
      if (b == 0x0A) { cy++; if (cy >= rows) cy = rows - 1; return; }
      if (b == 0x0D) { cx = 0; return; }
      if (b == 0x08) { if (cx > 0) cx--; return; }
      if (b < 0x20 || b == 0x7F) return;
      putChar(b);
      break;
    }
    case LEsc: {
      if (b == '[') { st = LCSI; csiBuf.clear(); nParams = 0; memset(params, 0, sizeof(params)); return; }
      if (b == ']') { st = LOSC; oscBuf.clear(); oscEsc = false; return; }
      st = LText;
      return;
    }
    case LCSI: {
      if (b >= '0' && b <= '9') {
        params[nParams] = params[nParams] * 10 + (b - '0');
        return;
      }
      if (b == ';') { nParams++; if (nParams >= 16) nParams = 15; return; }
      if (b >= 0x20 && b <= 0x2F) { csiBuf.push_back((char)b); return; }
      if (b >= 0x40 && b <= 0x7E) {
        csiBuf.push_back((char)b);
        char f = (char)b;
        if (nParams == 0) { params[0] = 1; nParams = 1; }
        if (f == 'H' || f == 'f') {
          cy = (params[0] - 1); if (cy < 0) cy = 0; if (cy >= rows) cy = rows - 1;
          cx = (params[1] - 1); if (cx < 0) cx = 0; if (cx >= cols) cx = cols - 1;
        } else if (f == 'A') { cy -= params[0]; if (cy < 0) cy = 0; }
        else if (f == 'B') { cy += params[0]; if (cy >= rows) cy = rows - 1; }
        else if (f == 'C') { cx += params[0]; if (cx >= cols) cx = cols - 1; }
        else if (f == 'D') { cx -= params[0]; if (cx < 0) cx = 0; }
        else if (f == 'G') { cx = params[0] - 1; if (cx < 0) cx = 0; if (cx >= cols) cx = cols - 1; }
        else if (f == 'J') {
          if (params[0] == 2) { legacyScreen.clearAll(); }
          else if (params[0] == 0) {
            for (int x = cx; x < cols; x++) legacyScreen.put(x, cy, 0, cur);
          }
        }
        else if (f == 'K') {
          for (int x = cx; x < cols; x++) legacyScreen.put(x, cy, 0, cur);
        }
        else if (f == 'm') applySgr();
        else if (f == 's') { /* ignore save */ }
        else if (f == 'u') { /* ignore restore */ }
        else if (f == 'h' || f == 'l') {
          // private modes: ?25h/l cursor visibility handled separately; ignore others
        }
        st = LText;
        return;
      }
      st = LText;
      return;
    }
    case LOSC: {
      if (b == 0x07) { st = LText; return; }
      if (b == 0x1B) { oscEsc = true; return; }
      if (oscEsc && b == '\\') { oscEsc = false; st = LText; return; }
      oscEsc = false;
      return;
    }
  }
}

void emit(char const* data, size_t n, int cols, int rows) {
  // A multi-byte UTF-8 sequence may straddle two writes. Keep any incomplete
  // tail and prepend it to the next call instead of silently dropping it
  // (previously a split glyph vanished from the legacy console).
  static thread_local std::string pend;
  std::string combined;
  if (!pend.empty()) {
    combined = pend;
    combined.append(data, n);
    data = combined.data();
    n = combined.size();
    pend.clear();
  }
  for (size_t i = 0; i < n; i++) {
    unsigned char b = (unsigned char)data[i];
    if (st == LText && b >= 0x80) {
      // gather a full UTF-8 sequence
      int len = utf8SeqLen(b);
      if (i + (size_t)len > n) {
        pend.assign(data + i, n - i);
        break;
      }
      size_t consumed = 0;
      uint32_t cp = utf8Decode(data + i, (size_t)len, consumed);
      putChar(cp);
      i += (size_t)len - 1;
    } else {
      feedByte(b, cols, rows);
    }
  }
}

}  // namespace legacy

// --- Windows legacy input: console events -> ANSI byte stream --------------
void translateKey(WORD vk, DWORD ctrlState, wchar_t unicode, bool keyDown, std::string& out) {
  if (!keyDown) return;
  bool ctrl = (ctrlState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
  bool shift = (ctrlState & SHIFT_PRESSED) != 0;
  bool alt = (ctrlState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
  int mod = (ctrl ? 5 : (alt ? 3 : (shift ? 2 : 1)));

  auto seq = [&](char const* s) { out += s; };
  switch (vk) {
    case VK_RETURN: seq("\r"); return;
    case VK_TAB: if (shift) seq("\x1b[Z"); else seq("\t"); return;
    case VK_ESCAPE: seq("\x1b"); return;
    case VK_BACK: seq("\x7f"); return;
    case VK_INSERT: seq("\x1b[2~"); return;
    case VK_DELETE: seq("\x1b[3~"); return;
    case VK_HOME: seq("\x1b[1;"); out += (char)('0' + mod); seq("H"); return;
    case VK_END: seq("\x1b[1;"); out += (char)('0' + mod); seq("F"); return;
    case VK_PRIOR: seq("\x1b[5~"); return;
    case VK_NEXT: seq("\x1b[6~"); return;
    case VK_UP: seq("\x1b[1;"); out += (char)('0' + mod); seq("A"); return;
    case VK_DOWN: seq("\x1b[1;"); out += (char)('0' + mod); seq("B"); return;
    case VK_RIGHT: seq("\x1b[1;"); out += (char)('0' + mod); seq("C"); return;
    case VK_LEFT: seq("\x1b[1;"); out += (char)('0' + mod); seq("D"); return;
    default: break;
  }
  if (vk >= VK_F1 && vk <= VK_F4) {
    char const* map[4] = {"\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS"};
    seq(map[vk - VK_F1]);
    return;
  }
  if (vk >= VK_F5 && vk <= VK_F12) {
    static char const* map[8] = {"\x1b[15~", "\x1b[17~", "\x1b[18~", "\x1b[19~",
                                  "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~"};
    seq(map[vk - VK_F5]);
    return;
  }
  if (unicode == 0) return;
  if (ctrl && unicode >= 'a' && unicode <= 'z') {
    out += (char)(unicode - 'a' + 1);
    return;
  }
  if (ctrl && unicode >= 'A' && unicode <= 'Z') {
    out += (char)(unicode - 'A' + 1);
    return;
  }
  // Emit UTF-8
  uint32_t cp = (uint32_t)unicode;
  char buf[8];
  size_t n = 0;
  if (cp < 0x80) { buf[n++] = (char)cp; }
  else if (cp < 0x800) { buf[n++] = (char)(0xC0 | (cp >> 6)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
  else { buf[n++] = (char)(0xE0 | (cp >> 12)); buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
  out.append(buf, n);
}

void translateMouse(MOUSE_EVENT_RECORD const& m, std::string& out) {
  int btn = 0;
  DWORD fl = m.dwButtonState;
  if (m.dwEventFlags == MOUSE_WHEELED) {
    btn = ((SHORT)HIWORD(m.dwButtonState) > 0) ? 64 : 65;
  } else if (m.dwEventFlags == MOUSE_HWHEELED) {
    btn = ((SHORT)HIWORD(m.dwButtonState) > 0) ? 66 : 67;
  } else {
    if (fl & FROM_LEFT_1ST_BUTTON_PRESSED) btn = 0;
    else if (fl & RIGHTMOST_BUTTON_PRESSED) btn = 2;
    else if (fl & FROM_LEFT_2ND_BUTTON_PRESSED) btn = 1;
    else return;  // button released
  }
  DWORD ks = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) ks |= 4;
  if (GetKeyState(VK_MENU) & 0x8000) ks |= 8;
  if (GetKeyState(VK_CONTROL) & 0x8000) ks |= 16;
  int code = btn + 32 + ks;
  char tmp[64];
  int n = snprintf(tmp, sizeof(tmp), "\x1b[<%d;%d;%dM", code, m.dwMousePosition.X + 1, m.dwMousePosition.Y + 1);
  out.append(tmp, (size_t)n);
}

int legacyRead(HANDLE h, uint8_t* buf, int cap, int timeoutMs) {
  // Serve any overflow buffered from a previous read first.
  static std::string pending;
  if (!pending.empty()) {
    size_t sz = pending.size();
    if (sz > (size_t)cap) sz = (size_t)cap;
    memcpy(buf, pending.data(), sz);
    pending.erase(0, sz);
    return (int)sz;
  }
  DWORD wait = (timeoutMs < 0) ? INFINITE : (DWORD)timeoutMs;
  DWORD r = WaitForSingleObject(h, wait);
  if (r == WAIT_TIMEOUT) return 0;
  std::string out;
  INPUT_RECORD recs[64];
  DWORD n = 0;
  if (!ReadConsoleInputW(h, recs, 64, &n)) return 0;
  for (DWORD i = 0; i < n; i++) {
    switch (recs[i].EventType) {
      case KEY_EVENT:
        translateKey(recs[i].Event.KeyEvent.wVirtualKeyCode, recs[i].Event.KeyEvent.dwControlKeyState,
                     recs[i].Event.KeyEvent.uChar.UnicodeChar, recs[i].Event.KeyEvent.bKeyDown != 0, out);
        break;
      case MOUSE_EVENT:
        translateMouse(recs[i].Event.MouseEvent, out);
        break;
      case WINDOW_BUFFER_SIZE_EVENT: {
        // Convert to a CSI 8;<rows>;<cols>t resize event the parser turns
        // into EventType::Resize, so the app reacts without waiting for the
        // next size poll.
        auto const& sr = recs[i].Event.WindowBufferSizeEvent.dwSize;
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "\x1b[8;%u;%ut", sr.Y, sr.X);
        out.append(tmp, (size_t)n);
        break;
      }
      default:
        break;
    }
  }
  size_t sz = out.size();
  if (sz > (size_t)cap) {
    memcpy(buf, out.data(), (size_t)cap);
    pending = out.substr(cap);
    return cap;
  }
  memcpy(buf, out.data(), sz);
  return (int)sz;
}

#endif  // _WIN32

}  // namespace

// --- Public API -------------------------------------------------------------

#ifdef _WIN32

bool vtSupported() {
  if (g_forceDumb) return false;
  if (!g_raw) {
    // probe output handle directly
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    DWORD test = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (SetConsoleMode(h, test)) { SetConsoleMode(h, mode); return true; }
    return false;
  }
  return g_vt;
}

bool enterRaw() {
  if (g_raw) return true;
  g_in = GetStdHandle(STD_INPUT_HANDLE);
  g_out = GetStdHandle(STD_OUTPUT_HANDLE);
  // A console is required: stdin/stdout must be a real terminal (a pipe or
  // redirected file cannot drive the TUI and would crash the legacy path).
  if (g_in == INVALID_HANDLE_VALUE || g_out == INVALID_HANDLE_VALUE ||
      (GetFileType(g_in) & 0x7F) != FILE_TYPE_CHAR ||
      (GetFileType(g_out) & 0x7F) != FILE_TYPE_CHAR) {
    return false;
  }
  GetConsoleMode(g_in, &g_oldInMode);
  GetConsoleMode(g_out, &g_oldOutMode);

  // Switch the console OUTPUT to UTF-8 so raw UTF-8 output (box drawing,
  // CJK) is decoded correctly. Required on GBK/GB2312 consoles (chcp 936).
  // The INPUT codepage is deliberately left at the system default: changing
  // it to UTF-8 breaks the Windows console IME (no composition window for
  // Chinese/Japanese input on many conhost builds). Input is read via
  // ReadConsoleInputW, which delivers UTF-16 and is codepage-independent.
  g_oldOutputCP = GetConsoleOutputCP();
  SetConsoleOutputCP(CP_UTF8);

  DWORD inMode = ENABLE_EXTENDED_FLAGS;
  // Input is read as INPUT_RECORDs via ReadConsoleInputW (legacyRead), which
  // already delivers KEY_EVENT and MOUSE_EVENT records. ENABLE_VIRTUAL_TERMINAL_INPUT
  // is deliberately NOT set: it would make conhost also buffer VT byte sequences
  // (leaking arrow/mouse escapes to the screen) and steal mouse events from the
  // record stream.
  inMode |= ENABLE_MOUSE_INPUT;
  inMode &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
              ENABLE_WINDOW_INPUT | ENABLE_INSERT_MODE);
  SetConsoleMode(g_in, inMode);

  DWORD outMode = g_oldOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  if (SetConsoleMode(g_out, outMode)) {
    g_vt = true;
  } else {
    g_vt = false;
    SetConsoleMode(g_out, g_oldOutMode);
  }

  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);

  g_legacy = !g_vt && !g_forceDumb;
  if (g_legacy) {
    legacy::reset();
    // enable mouse capture for legacy console
    DWORD inMode2 = g_oldInMode;
    inMode2 |= ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT;
    inMode2 &= ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    SetConsoleMode(g_in, inMode2);
    CONSOLE_CURSOR_INFO ci = {1, TRUE};
    SetConsoleCursorInfo(g_out, &ci);
  }
  g_raw = true;
  return true;
}

void leaveRaw() {
  if (!g_raw) return;
  SetConsoleMode(g_in, g_oldInMode);
  SetConsoleMode(g_out, g_oldOutMode);
  if (g_oldOutputCP) SetConsoleOutputCP(g_oldOutputCP);
  g_raw = false;
}

bool rawActive() { return g_raw; }

bool getSize(int& cols, int& rows) {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  CONSOLE_SCREEN_BUFFER_INFO bi;
  if (!GetConsoleScreenBufferInfo(h, &bi)) {
    cols = 80;
    rows = 24;
    return false;
  }
  cols = bi.srWindow.Right - bi.srWindow.Left + 1;
  rows = bi.srWindow.Bottom - bi.srWindow.Top + 1;
  return true;
}

bool resizePending() { return false; }  // Windows surfaces resizes as events

bool writeAll(char const* data, size_t n) {
  if (g_legacy) {
    int cols = 80, rows = 24;
    getSize(cols, rows);
    legacy::emit(data, n, cols, rows);
    legacy::flush(g_out, cols, rows);
    return true;
  }
  HANDLE h = g_out != INVALID_HANDLE_VALUE ? g_out : GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD written = 0;
  return WriteFile(h, data, (DWORD)n, &written, nullptr) != 0;
}

bool flush() { return true; }  // WriteFile is unbuffered

int readInput(uint8_t* buf, int cap, int timeoutMs) {
  HANDLE h = g_in != INVALID_HANDLE_VALUE ? g_in : GetStdHandle(STD_INPUT_HANDLE);
  // Always read via INPUT_RECORDs (ReadConsoleInputW): the ReadFile+VT-input
  // stream is unreliable on conhost (drops keys after the first read, mangles
  // IME/UTF-8). translateKey/translateMouse convert records to the same ANSI
  // byte stream the parser expects.
  return legacyRead(h, buf, cap, timeoutMs);
}

void setForceDumb(bool b) { g_forceDumb = b; }

bool envDumb() {
  char const* term = getenv("TERM");
  if (term && strstr(term, "dumb")) return true;
  return false;
}

#else  // POSIX

namespace {
termios g_oldTerm;
bool g_haveTerm = false;
volatile sig_atomic_t g_resized = 0;

extern "C" void onWinch(int) { g_resized = 1; }
}

bool resizePending() {
  bool r = g_resized != 0;
  g_resized = 0;
  return r;
}

bool vtSupported() {
  if (g_forceDumb) return false;
  return !envDumb();
}

bool enterRaw() {
  if (g_raw) return true;
  if (tcgetattr(0, &g_oldTerm) == 0) g_haveTerm = true;
  termios t = g_oldTerm;
  t.c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  t.c_oflag &= (tcflag_t)~(OPOST);
  t.c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t.c_cflag |= CS8;
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(0, TCSANOW, &t);
  // notify the event loop of window size changes instead of ignoring them
  struct sigaction sa{};
  sa.sa_handler = onWinch;
  sigaction(SIGWINCH, &sa, nullptr);
  g_raw = true;
  return true;
}

void leaveRaw() {
  if (!g_raw) return;
  if (g_haveTerm) tcsetattr(0, TCSANOW, &g_oldTerm);
  g_raw = false;
}

bool rawActive() { return g_raw; }

bool getSize(int& cols, int& rows) {
  winsize ws{};
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    cols = ws.ws_col;
    rows = ws.ws_row;
    return true;
  }
  char const* c = getenv("COLUMNS");
  if (c) cols = atoi(c);
  else cols = 80;
  c = getenv("LINES");
  if (c) rows = atoi(c);
  else rows = 24;
  return false;
}

bool writeAll(char const* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(1, data + off, n - off);
    if (w < 0) return false;
    off += (size_t)w;
  }
  return true;
}

bool flush() { return fflush(stdout) == 0; }

int readInput(uint8_t* buf, int cap, int timeoutMs) {
  pollfd p{0, POLLIN, 0};
  int pr = poll(&p, 1, timeoutMs);
  if (pr <= 0) return 0;
  ssize_t n = ::read(0, buf, (size_t)cap);
  if (n <= 0) return 0;
  return (int)n;
}

void setForceDumb(bool b) { g_forceDumb = b; }

bool envDumb() {
  char const* term = getenv("TERM");
  if (term && strstr(term, "dumb")) return true;
  return false;
}

#endif

void enterAlternateScreen() {
  // NOTE: both strings are 15 bytes; the previous hardcoded 14 truncated the
  // final byte ('h'/'l'), leaving the private-mode sequence unterminated.
  static char const kEnter[] = "\x1b[?1049h\x1b[?25l";
  if (!g_forceDumb && vtSupported()) writeAll(kEnter, sizeof(kEnter) - 1);
  else writeAll("", 0);
}

void leaveAlternateScreen() {
  static char const kLeave[] = "\x1b[?25h\x1b[?1049l";
  if (!g_forceDumb && vtSupported()) writeAll(kLeave, sizeof(kLeave) - 1);
  else writeAll("", 0);
}

void hideCursor() {
  static char const kHide[] = "\x1b[?25l";
  if (!g_forceDumb && vtSupported()) writeAll(kHide, sizeof(kHide) - 1);
}

void showCursor() {
  static char const kShow[] = "\x1b[?25h";
  if (!g_forceDumb && vtSupported()) writeAll(kShow, sizeof(kShow) - 1);
}

bool copyClipboard(std::string const& text) {
#ifdef _WIN32
  // Win32 clipboard: open, empty, set as a UTF-16 string.
  if (!OpenClipboard(nullptr)) return false;
  bool ok = false;
  if (EmptyClipboard()) {
    std::wstring w;
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (n > 0) {
      w.resize((size_t)n);
      MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &w[0], n);
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
      if (h) {
        void* p = GlobalLock(h);
        if (p) {
          memcpy(p, w.data(), (w.size() + 1) * sizeof(wchar_t));
          GlobalUnlock(h);
          ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
      }
    }
  }
  CloseClipboard();
  return ok;
#else
  // OSC52: ESC ] 52 ; c ; <base64> BEL. Works in tmux, most terminals and
  // SSH clients even over a remote connection.
  if (g_forceDumb || !vtSupported()) return false;
  static char const* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string enc;
  enc.reserve((text.size() + 2) / 3 * 4);
  for (size_t i = 0; i < text.size(); i += 3) {
    unsigned v = (unsigned char)text[i] << 16;
    if (i + 1 < text.size()) v |= (unsigned char)text[i + 1] << 8;
    if (i + 2 < text.size()) v |= (unsigned char)text[i + 2];
    enc += b64[(v >> 18) & 63];
    enc += b64[(v >> 12) & 63];
    enc += b64[(v >> 6) & 63];
    enc += b64[v & 63];
  }
  size_t rem = text.size() % 3;
  if (rem == 1) { enc[enc.size() - 2] = '='; enc[enc.size() - 1] = '='; }
  else if (rem == 2) { enc[enc.size() - 1] = '='; }
  std::string osc = "\x1b]52;c;" + enc + "\x07";
  return writeAll(osc.data(), osc.size());
#endif
}

}  // namespace tui::platform

namespace tui {

Capabilities detectCapabilities() {
  Capabilities c;
  c.ansi = platform::vtSupported() && !platform::envDumb();
  c.color = c.ansi;

  // NO_COLOR (https://no-color.org): any value (including empty) disables color.
  bool noColor = getenv("NO_COLOR") != nullptr;
  // CI / build environments usually have no useful color.
  bool isCI = getenv("CI") != nullptr;

  char const* ct = getenv("COLORTERM");
  if (!noColor && !isCI) {
    if (ct && (strstr(ct, "24bit") || strstr(ct, "truecolor"))) c.colorBits = 24;
    else if (ct) c.colorBits = 8;
    else {
      char const* term = getenv("TERM");
      if (term && strstr(term, "256color")) c.colorBits = 8;
      else if (term && (strstr(term, "xterm") || strstr(term, "vt100"))) c.colorBits = 8;
      else if (term) c.colorBits = 4;
      else c.colorBits = 4;
    }
  }
#ifdef _WIN32
  if (c.ansi && !noColor && !isCI) c.colorBits = 24;  // modern Win console: VT 24-bit
#endif
  if (!c.ansi || noColor || isCI) c.colorBits = 0;
  if (c.colorBits == 0) c.color = false;

  c.mouse = c.ansi && !isCI;  // no mouse events in most CI environments
  c.bracketedPaste = c.ansi;
  c.focusEvents = c.ansi;

  // Unicode by default; ASCII fallback only when an obviously dumb terminal
  // asks for one. Windows legacy (GetACP returns CP_ACP) typically renders
  // unicode glyphs correctly, so keep unicode=true there.
  c.unicode = true;
  if (getenv("LANG")) {
    char const* lang = getenv("LANG");
    // POSIX locales without UTF-8 suffix historically can't show BMP glyphs.
    // Only downgrade for an explicit non-UTF-8 C locale; otherwise default
    // to unicode (modern terminals all encode as UTF-8 regardless of LANG).
    if (lang[0] == 'C' && lang[1] == '\0') c.unicode = false;
  }

  int cols = 80, rows = 24;
  platform::getSize(cols, rows);
  c.cols = cols;
  c.rows = rows;
  return c;
}

}  // namespace tui
