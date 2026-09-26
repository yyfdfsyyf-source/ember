#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace tui::platform {

// --- Terminal mode --------------------------------------------------------
bool enterRaw();
void leaveRaw();
bool rawActive();

// --- Sizing ---------------------------------------------------------------
bool getSize(int& cols, int& rows);
// Whether the terminal reported a resize since the last call (POSIX signals).
bool resizePending();

// --- I/O ------------------------------------------------------------------
// Write all bytes (routed through legacy emulation when ANSI is unavailable).
bool writeAll(char const* data, size_t n);
// Flush buffered output to the terminal (POSIX stdio buffering).
bool flush();
// Read raw bytes; blocks up to timeoutMs (0 = non-blocking). Returns count.
int readInput(uint8_t* buf, int cap, int timeoutMs);

// --- Capability probes ----------------------------------------------------
// Whether the OS console natively accepts ANSI/VT sequences.
bool vtSupported();
// Force pure-text (dumb) mode regardless of probe results.
void setForceDumb(bool b);
// Env/terminal-derived hints (pure text mode, etc.).
bool envDumb();

// --- App lifecycle helpers -------------------------------------------------
void enterAlternateScreen();
void leaveAlternateScreen();
void hideCursor();
void showCursor();

// --- Clipboard -------------------------------------------------------------
// Copy `text` to the system clipboard. Windows path uses the Win32 clipboard;
// POSIX writes an OSC52 escape (supported by most modern terminals/SSH).
// Returns true if the copy was issued.
bool copyClipboard(std::string const& text);

}  // namespace tui::platform
