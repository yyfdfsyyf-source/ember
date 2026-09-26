#pragma once
#include <cstdint>

namespace tui {

// Terminal capabilities detected at startup. Everything a downstream layer
// may rely on is gathered here so rendering/input can adapt automatically.
struct Capabilities {
  bool color = true;       // any color support at all
  int  colorBits = 24;     // 24 = truecolor, 8 = 256 colors, 4 = 16 colors, 0 = none
  bool unicode = true;     // terminal handles non-ASCII
  bool mouse = true;       // SGR (1006) mouse reporting
  bool bracketedPaste = true;
  bool focusEvents = true;
  bool ansi = true;        // accepts ANSI/VT control sequences
  int  cols = 80;
  int  rows = 24;

  bool isDumb() const { return !ansi; }
};

// Detect terminal capabilities from environment + platform probe.
Capabilities detectCapabilities();

}  // namespace tui
