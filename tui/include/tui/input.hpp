#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tui {

enum class EventType : uint8_t {
  None,
  Key,
  Mouse,
  Paste,
  Resize,
  FocusIn,
  FocusOut,
  Unknown,
};

// Special (non-printable) key codes live above the Unicode range.
enum : int {
  KeyNone = 0,
  KeyCtrlA = 0x1,
  KeyCtrlB = 0x2,
  KeyCtrlC = 0x3,
  KeyCtrlD = 0x4,
  KeyCtrlE = 0x5,
  KeyCtrlF = 0x6,
  KeyCtrlG = 0x7,
  KeyCtrlH = 0x8,
  KeyCtrlI = 0x9,
  KeyCtrlJ = 0xA,
  KeyCtrlK = 0xB,
  KeyCtrlL = 0xC,
  KeyCtrlM = 0xD,
  KeyCtrlN = 0xE,
  KeyCtrlO = 0xF,
  KeyCtrlP = 0x10,
  KeyCtrlQ = 0x11,
  KeyCtrlR = 0x12,
  KeyCtrlS = 0x13,
  KeyCtrlT = 0x14,
  KeyCtrlU = 0x15,
  KeyCtrlV = 0x16,
  KeyCtrlW = 0x17,
  KeyCtrlX = 0x18,
  KeyCtrlY = 0x19,
  KeyCtrlZ = 0x1A,
  KeyEnter = 0x100001,
  KeyTab,
  KeyBackTab,
  KeyBackspace,
  KeyEscape,
  KeyInsert,
  KeyDelete,
  KeyHome,
  KeyEnd,
  KeyPageUp,
  KeyPageDown,
  KeyUp,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyF1,
  KeyF2,
  KeyF3,
  KeyF4,
  KeyF5,
  KeyF6,
  KeyF7,
  KeyF8,
  KeyF9,
  KeyF10,
  KeyF11,
  KeyF12,
  KeyMenu,
  KeyMediaPlay,
  KeyMediaPause,
  KeyMediaPrev,
  KeyMediaNext,
  KeyPrintScreen,
  KeyPause,
};

struct MouseEvent {
  int x = 0;
  int y = 0;
  uint8_t buttons = 0;  // bit0=left bit1=middle bit2=right
  bool shift = false;
  bool alt = false;
  bool ctrl = false;
  bool motion = false;  // drag/held motion
  int wheel = 0;        // +1 up, -1 down, +2 right, -2 left
  bool press = false;
  bool release = false;
};

struct Event {
  EventType type = EventType::None;
  // Key
  uint32_t ch = 0;  // Unicode codepoint or special key code
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  // Mouse
  MouseEvent mouse;
  // Paste
  std::string text;
  // Resize
  int cols = 0;
  int rows = 0;
};

// Incremental byte-stream -> Event parser. Handles CSI/SS3/OSC, SGR mouse,
// bracketed paste, kitty protocol basics, and legacy alt-key disambiguation.
class InputParser {
 public:
  void feed(uint8_t const* data, size_t n);
  // Pop next event; returns false if queue is empty.
  bool next(Event& ev);
  // Converts a pending trailing ESC into a real Escape key event.
  void expireTimeout();
  void reset();

  void setBracketedPasteEnabled(bool on) { pasteEnabled_ = on; }
  void setMouseEnabled(bool on) { mouseEnabled_ = on; }

 private:
  void push(Event ev) { queue_.push_back(ev); }
  void pushKey(uint32_t ch, bool ctrl, bool alt, bool shift);
  void dispatchCsi();
  void dispatchSs3();
  void decodeKitty(int code, int mod);
  void parseSgrMouse();

  std::vector<Event> queue_;
  size_t queuePos_ = 0;

  // parser state
  enum class St : uint8_t { Text, Esc, Alt, CSI, SS3, OSC, Paste, PasteEsc };
  St st_ = St::Text;

  std::string buf_;      // CSI params / OSC payload
  int params_[16] = {0};
  int nParams_ = 0;
  bool haveSemi_ = false;
  bool sgr_ = false;     // CSI private/param prefix byte (<) seen
  bool oscBell_ = false;
  int pasteMatch_ = 0;   // bytes matched of the "\x1b[201~" paste terminator
  std::string altBuf_;
  std::string pasteBuf_;
  std::string utfBuf_;
  bool pendingEsc_ = false;
  bool pasteEnabled_ = true;
  bool mouseEnabled_ = true;
};

}  // namespace tui
