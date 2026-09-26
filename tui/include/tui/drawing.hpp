#pragma once
#include "tui/screen.hpp"
#include "tui/style.hpp"
#include "tui/theme.hpp"

namespace tui {

// ---------------------------------------------------------------------------
// Border drawing. Each occupies the rect entirely (so width/height >= 2).
// ---------------------------------------------------------------------------
enum class BorderStyle {
  None,
  Ascii,    // +-|
  Single,   // ─│┌┐└┘
  Double,   // ═║╔╗╚╝
  Rounded,  // ─│╭╮╰╯
  Heavy,    // ━┃┏┓┗┛
};

struct BorderSet {
  char32_t h, v, tl, tr, bl, br;
};

// Look up the glyphs for a given border style.
BorderSet borderSet(BorderStyle bs, Glyphs const& g = Glyphs::default_());

// Draw a border frame around `r` with the chosen style.
void drawBorder(Screen& s, Rect r, BorderStyle bs, Style st);

// Convenience: existing single-line border (definition lives in widgets.cpp
// for back-compat). Forwarded via the same name; new code should prefer the
// BorderStyle overload.
void drawBorder(Screen& s, Rect r, Style st);

// Drop a 1-cell "shadow" along the bottom + right edge of `r` in `shadow`.
// Gives panels a soft depth feel without using real background fill.
void drawShadow(Screen& s, Rect r, Style shadow);

// ---------------------------------------------------------------------------
// Gradient: paint a horizontal gradient strip across `r` using two colors.
// `stops` controls mid-points: stops[i] in [0..1] gives a color at that
// fraction of the row. Smoothly fades between consecutive stops; falls back
// to per-cell 256-color quantization on non-truecolor terminals.
// ---------------------------------------------------------------------------
void drawGradientH(Screen& s, Rect r, std::vector<std::pair<float, Color>> stops);

// ---------------------------------------------------------------------------
// Progress bar: width `barW`, fills `frac` (0..1). Optional title on the left.
// ---------------------------------------------------------------------------
void drawProgress(Screen& s, Rect r, float frac, Color fill, Color bg, Style base);

// ---------------------------------------------------------------------------
// Spinner: animated braille/dot sequence. `phase` is 0..N-1, advanced each
// tick by the host. Returns the unicode frame glyph (or ascii fallback).
// ---------------------------------------------------------------------------
struct SpinnerSet {
  std::vector<char32_t> frames;
  char32_t asciiFrame = '*';
};
const SpinnerSet& spinnerBraille();
const SpinnerSet& spinnerDots();
const SpinnerSet& spinnerArrows();

char32_t spinnerFrame(SpinnerSet const& sp, int phase);

// ---------------------------------------------------------------------------
// Filled horizontal bar with smooth sub-cell fill using block elements.
// Allows progress bars at any width to look continuous (not 1-char stepped).
// ---------------------------------------------------------------------------
void drawProgressSmooth(Screen& s, Rect r, float frac, Color fill, Color bg);

// Choose fg color that contrasts with `bg` (W3C-ish luma check). Returns
// either near-black or near-white so labels stay readable on any fill.
Color pickContrastingFg(Color bg);

}  // namespace tui