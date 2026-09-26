#pragma once
#include <cstdint>

namespace tui {

// ---------------------------------------------------------------------------
// Color. Packed into 16 bits:
//   bits[15:14] mode  00=Default, 01=Index(256), 10=TrueColor(15-bit RGB)
//   bits[13:10] r (5-bit)   bits[9:5] g   bits[4:0] b     (TrueColor)
//   bits[7:0]  palette index                              (Index)
// ---------------------------------------------------------------------------
struct Color {
  uint16_t v;

  static constexpr Color def() { return Color{0}; }
  static constexpr Color index(uint8_t i) { return Color{(uint16_t)(0x4000u | i)}; }
  static constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return Color{(uint16_t)(0x8000u | ((uint16_t)(r >> 3) << 10) |
                            ((uint16_t)(g >> 3) << 5) | (uint16_t)(b >> 3))};
  }

  static constexpr Color rgb5(uint8_t r5, uint8_t g5, uint8_t b5) {
    return Color{(uint16_t)(0x8000u | ((uint16_t)(r5 & 0x1F) << 10) |
                            ((uint16_t)(g5 & 0x1F) << 5) | (uint16_t)(b5 & 0x1F))};
  }

  constexpr uint8_t mode() const { return (uint8_t)(v >> 14); }  // 0 default,1 index,2 true
  constexpr uint8_t r5() const { return (uint8_t)((v >> 10) & 0x1F); }
  constexpr uint8_t g5() const { return (uint8_t)((v >> 5) & 0x1F); }
  constexpr uint8_t b5() const { return (uint8_t)(v & 0x1F); }
  constexpr uint8_t r() const { return (uint8_t)((r5() << 3) | (r5() >> 2)); }
  constexpr uint8_t g() const { return (uint8_t)((g5() << 3) | (g5() >> 2)); }
  constexpr uint8_t b() const { return (uint8_t)((b5() << 3) | (b5() >> 2)); }
  constexpr uint8_t index() const { return (uint8_t)(v & 0xFF); }

  constexpr bool operator==(Color const& o) const { return v == o.v; }
  constexpr bool operator!=(Color const& o) const { return v != o.v; }
};

// ---------------------------------------------------------------------------
// Style. Packed into 64 bits:
//   bits[0:15]  foreground color
//   bits[16:31] background color
//   bits[32:38] attributes
// Equal to an integer => one compare for dirty/style-change checks.
// ---------------------------------------------------------------------------
enum : uint8_t {
  AttrBold      = 1u << 0,
  AttrDim       = 1u << 1,
  AttrItalic    = 1u << 2,
  AttrUnderline = 1u << 3,
  AttrBlink     = 1u << 4,
  AttrReverse   = 1u << 5,
  AttrStrike    = 1u << 6,
};

struct Style {
  uint64_t v = 0;

  static constexpr Style plain() { return Style{0}; }

  constexpr Style& fg(Color c) {
    v = (v & ~0xFFFFull) | (uint64_t)c.v;
    return *this;
  }
  constexpr Style& bg(Color c) {
    v = (v & ~(0xFFFFull << 16)) | ((uint64_t)c.v << 16);
    return *this;
  }
  constexpr Style& attr(uint8_t a, bool on) {
    if (on) v |= (uint64_t)a << 32;
    else v &= ~((uint64_t)a << 32);
    return *this;
  }
  constexpr Style& allAttr(uint8_t a) {
    v = (v & ~(0x7Full << 32)) | ((uint64_t)a << 32);
    return *this;
  }

  constexpr Color fgColor() const { return Color{(uint16_t)(v & 0xFFFF)}; }
  constexpr Color bgColor() const { return Color{(uint16_t)((v >> 16) & 0xFFFF)}; }
  constexpr uint8_t attrs() const { return (uint8_t)(v >> 32); }

  constexpr bool operator==(Style const& o) const { return v == o.v; }
  constexpr bool operator!=(Style const& o) const { return v != o.v; }
};

}  // namespace tui
