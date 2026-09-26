#pragma once
#include <string>

namespace agent {

// Windows desktop accessibility (UI Automation) tools. The model reads the
// accessibility tree of open windows as text (no vision needed), locates an
// element by name/role/path, then drives it (click/type/key/scroll) through
// the UIA COM interfaces. All tools are Windows-only; on non-Windows builds
// they report an error.

// List top-level windows: [{hwnd,title,class,pid,rect,focused,visible}].
std::string toolDesktopWindows(std::string const& argsJson);
// Dump the accessibility tree of a window as text: one line per element with
// an index path like "0/2/1", role, name, value, rect and enabled state.
std::string toolDesktopTree(std::string const& argsJson);
// Find an element by path/name/role/automationId and click it (InvokePattern
// or SelectionItemPattern), or click at raw screen coordinates.
std::string toolDesktopClick(std::string const& argsJson);
// Type text into a focused element (ValuePattern.SetValue) or by path/name.
std::string toolDesktopType(std::string const& argsJson);
// Send keyboard chords (e.g. "ctrl+s", "alt+tab", "enter") to the focused
// window or a specific element.
std::string toolDesktopKey(std::string const& argsJson);
// Scroll a scrollable element (ScrollPattern) or the focused window.
std::string toolDesktopScroll(std::string const& argsJson);
// Wait until an element matching name/role appears in a window's tree.
std::string toolDesktopWait(std::string const& argsJson);

}  // namespace agent