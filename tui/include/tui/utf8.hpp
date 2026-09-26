#pragma once
#include <cstdint>
#include <string>

namespace tui {

// Length of the UTF-8 sequence that starts with `lead` byte.
inline int utf8SeqLen(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

// Decode one codepoint from `s` (at most `n` bytes). `consumed` receives bytes used.
inline uint32_t utf8Decode(char const* s, size_t n, size_t& consumed) {
  auto const* p = reinterpret_cast<unsigned char const*>(s);
  if (n == 0) { consumed = 0; return 0; }
  unsigned char c0 = p[0];
  if (c0 < 0x80) { consumed = 1; return c0; }
  int len = utf8SeqLen(c0);
  if ((size_t)len > n) len = (int)n;
  uint32_t cp = 0;
  switch (len) {
    case 2: cp = ((uint32_t)(c0 & 0x1F) << 6); break;
    case 3: cp = ((uint32_t)(c0 & 0x0F) << 12); break;
    case 4: cp = ((uint32_t)(c0 & 0x07) << 18); break;
    default: consumed = 1; return 0xFFFD;
  }
  for (int i = 1; i < len; i++) {
    unsigned char c = p[i];
    if ((c & 0xC0) != 0x80) { consumed = 1; return 0xFFFD; }
    cp |= ((uint32_t)(c & 0x3F) << (6 * (len - 1 - i)));
  }
  consumed = (size_t)len;
  return cp;
}

inline void utf8Append(std::string& out, uint32_t cp) {
  if (cp < 0x80) { out += (char)cp; return; }
  if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

inline std::string utf8Encode(uint32_t cp) {
  std::string s;
  utf8Append(s, cp);
  return s;
}

}  // namespace tui
