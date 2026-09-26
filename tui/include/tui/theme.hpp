#pragma once
#include "tui/style.hpp"
#include "tui/terminal.hpp"

namespace tui {

// ---------------------------------------------------------------------------
// Theme: a small palette used by widgets that don't paint their own colors.
// Apps can substitute their own; defaults are chosen to read well on both
// dark and light backgrounds (mid-luma accent colors).
// ---------------------------------------------------------------------------
struct Theme {
  Color bg      = {0};                         // default background (terminal default)
  Color fg      = {0};                         // default foreground (terminal default)
  Color dim     = Color::index(244);           // subtle / borders / hints
  Color accent1 = Color::rgb( 90, 170, 255);   // primary highlight (blue)
  Color accent2 = Color::rgb(255, 110, 200);   // secondary highlight (pink)
  Color accent3 = Color::rgb(120, 220, 160);   // tertiary (green)
  Color warn    = Color::rgb(255, 196,  64);
  Color error_  = Color::rgb(255,  96,  96);

  Style sDim()      const { return Style::plain().fg(dim); }
  Style sAccent1()  const { return Style::plain().fg(accent1).attr(AttrBold, true); }
  Style sAccent2()  const { return Style::plain().fg(accent2).attr(AttrBold, true); }
  Style sAccent3()  const { return Style::plain().fg(accent3); }
  Style sWarn()     const { return Style::plain().fg(warn).attr(AttrBold, true); }
  Style sError()    const { return Style::plain().fg(error_).attr(AttrBold, true); }
  Style sBorder()   const { return Style::plain().fg(dim); }
  Style sBorderH()  const { return Style::plain().fg(accent1); }   // focused / active panel
  Style sHeader()   const { return sAccent1(); }
};

// ---------------------------------------------------------------------------
// Glyph set: unicode vs ascii fallback, so a UI keeps working on dumb/legacy
// terminals. Selected by capabilities and overridable per widget.
// ---------------------------------------------------------------------------
struct Glyphs {
  // Box-drawing
  char32_t hLine   = U'\u2500';   // ─
  char32_t vLine   = U'\u2502';   // │
  char32_t tl      = U'\u250C';   // ┌
  char32_t tr      = U'\u2510';   // ┐
  char32_t bl      = U'\u2514';   // └
  char32_t br      = U'\u2518';   // ┘
  char32_t cross   = U'\u253C';   // ┼
  // Heavy
  char32_t hLineH  = U'\u2501';
  char32_t vLineH  = U'\u2503';
  char32_t tlH     = U'\u250F';
  char32_t trH     = U'\u2513';
  char32_t blH     = U'\u2517';
  char32_t brH     = U'\u251B';
  // Rounded
  char32_t tlR     = U'\u256D';   // ╭
  char32_t trR     = U'\u256E';   // ╮
  char32_t blR     = U'\u2570';   // ╰
  char32_t brR     = U'\u256F';   // ╯
  // Double
  char32_t hLineD  = U'\u2550';
  char32_t vLineD  = U'\u2551';
  char32_t tlD     = U'\u2554';
  char32_t trD     = U'\u2557';
  char32_t blD     = U'\u255A';
  char32_t brD     = U'\u255D';
  // Block / progress / spinner
  char32_t blockFull  = U'\u2588';
  char32_t block7     = U'\u2589';
  char32_t block6     = U'\u258A';
  char32_t block5     = U'\u258B';
  char32_t block4     = U'\u258C';
  char32_t block3     = U'\u258D';
  char32_t block2     = U'\u258E';
  char32_t block1     = U'\u258F';
  char32_t blockLight = U'\u2591';
  char32_t blockMed   = U'\u2592';
  char32_t arrow      = U'\u25B6';   // ▶
  char32_t arrowRight = U'\u25B8';   // ▸

  static Glyphs ascii();   // fall back to ASCII for non-unicode terminals
  static Glyphs default_();  // unicode variant
};

// Pick the glyph set appropriate for `caps`.
inline Glyphs glyphsFor(Capabilities const& caps) {
  return caps.unicode ? Glyphs::default_() : Glyphs::ascii();
}

}  // namespace tui