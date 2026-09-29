#include "tui/input.hpp"
#include "tui/utf8.hpp"

namespace tui {

void InputParser::reset() {
  queue_.clear();
  queuePos_ = 0;
  st_ = St::Text;
  buf_.clear();
  nParams_ = 0;
  altBuf_.clear();
  pasteBuf_.clear();
  utfBuf_.clear();
  pendingEsc_ = false;
}

void InputParser::pushKey(uint32_t ch, bool ctrl, bool alt, bool shift) {
  Event ev;
  ev.type = EventType::Key;
  ev.ch = ch;
  ev.ctrl = ctrl;
  ev.alt = alt;
  ev.shift = shift;
  push(ev);
}

void InputParser::expireTimeout() {
  if (st_ == St::Esc) {
    pushKey(KeyEscape, false, false, false);
    st_ = St::Text;
    pendingEsc_ = false;
  } else if (st_ == St::Alt) {
    // Alt + partial utf8: flush what we have as an escaped char or Escape.
    if (!altBuf_.empty()) {
      size_t con = 0;
      uint32_t cp = utf8Decode(altBuf_.data(), altBuf_.size(), con);
      pushKey(cp, false, true, false);
    } else {
      pushKey(KeyEscape, false, false, false);
    }
    st_ = St::Text;
    altBuf_.clear();
    pendingEsc_ = false;
  }
}

void InputParser::feed(uint8_t const* data, size_t n) {
  for (size_t k = 0; k < n; k++) {
    unsigned char b = data[k];

    switch (st_) {
      case St::Text: {
        if (b == 0x1B) {
          st_ = St::Esc;
          pendingEsc_ = true;
          break;
        }
        if (b == 0x0D || b == 0x0A) { pushKey(KeyEnter, false, false, false); break; }
        if (b == 0x09) { pushKey(KeyTab, false, false, false); break; }
        if (b == 0x7F || b == 0x08) { pushKey(KeyBackspace, false, false, false); break; }
        if (b < 0x20) {
          // Raw control code (Ctrl+C=0x03, etc.). KeyCtrlX constants match.
          pushKey(b, true, false, false);
          break;
        }
        // UTF-8 lead or ASCII
        utfBuf_.clear();
        utfBuf_.push_back((char)b);
        if (utf8SeqLen(b) == 1) {
          pushKey(b, false, false, false);
        } else {
          // wait for continuation bytes; collect in a simple inline way
          size_t len = utf8SeqLen(b);
          size_t have = 1;
          while (have < len && k + 1 < n) {
            utfBuf_.push_back((char)data[++k]);
            have++;
          }
          size_t con = 0;
          uint32_t cp = utf8Decode(utfBuf_.data(), utfBuf_.size(), con);
          pushKey(cp, false, false, false);
        }
        break;
      }

      case St::Esc: {
        if (b == '[') {
          st_ = St::CSI;
          buf_.clear();
          nParams_ = 0;
          haveSemi_ = false;
          sgr_ = false;
          for (auto& p : params_) p = 0;
          break;
        }
        if (b == 'O') { st_ = St::SS3; buf_.clear(); nParams_ = 0; break; }
        if (b == ']') { st_ = St::OSC; buf_.clear(); break; }
        if (b == 0x1B) { break; }  // double ESC: stay pending
        // Legacy alt: treat following bytes as Alt+char
        altBuf_.clear();
        altBuf_.push_back((char)b);
        st_ = St::Alt;
        if (altBuf_.size() >= (size_t)utf8SeqLen((unsigned char)altBuf_[0])) {
          size_t con = 0;
          uint32_t cp = utf8Decode(altBuf_.data(), altBuf_.size(), con);
          pushKey(cp, false, true, false);
          altBuf_.clear();
          st_ = St::Text;
        }
        break;
      }

      case St::Alt: {
        altBuf_.push_back((char)b);
        size_t leadLen = utf8SeqLen((unsigned char)altBuf_[0]);
        if (altBuf_.size() >= leadLen) {
          size_t con = 0;
          uint32_t cp = utf8Decode(altBuf_.data(), altBuf_.size(), con);
          pushKey(cp, false, true, false);
          altBuf_.clear();
          st_ = St::Text;
        }
        break;
      }

      case St::CSI: {
        if (b == ';') { nParams_++; if (nParams_ >= 16) nParams_ = 15; break; }
        if (b >= '0' && b <= '9') {
          int& p = params_[nParams_];
          p = p * 10 + (b - '0');
          break;
        }
        if (b >= 0x3C && b <= 0x3F) {  // private/parameter prefix (<, =, >, ?)
          if (b == '<') sgr_ = true;
          break;
        }
        if (b >= 0x20 && b <= 0x2F) {
          buf_.push_back((char)b);  // intermediate byte (rarely needed)
          break;
        }
        if (b >= 0x40 && b <= 0x7E) {
          // final byte
          buf_.push_back((char)b);
          // SGR (ignore), Cursor position report, DSR, etc.
          if (b == 'm' && sgr_) { parseSgrMouse(); st_ = St::Text; break; }
          if (b == 'M' && sgr_) { parseSgrMouse(); st_ = St::Text; break; }
          if (b == 'm' || b == 'R' || b == 'n') { st_ = St::Text; break; }
          if (b == 'M') {
            if (mouseEnabled_) parseSgrMouse();
            st_ = St::Text;
            break;
          }
          dispatchCsi();
          if (st_ == St::CSI) st_ = St::Text;
          break;
        }
        // unexpected byte: bail to text
        st_ = St::Text;
        break;
      }

      case St::SS3: {
        buf_.clear();
        buf_.push_back((char)b);
        dispatchSs3();
        st_ = St::Text;
        break;
      }

      case St::OSC: {
        if (b == 0x07) { st_ = St::Text; break; }
        if (b == 0x1B) { oscBell_ = true; break; }
        if (oscBell_ && b == '\\') { oscBell_ = false; st_ = St::Text; break; }
        if (oscBell_) { oscBell_ = false; }
        break;
      }

      case St::Paste: {
        if (b == 0x1B) { pasteMatch_ = 0; st_ = St::PasteEsc; break; }
        pasteBuf_.push_back((char)b);
        break;
      }

      case St::PasteEsc: {
        static const char exp[] = {0x1B, '[', '2', '0', '1', '~'};
        if (b == (unsigned char)exp[pasteMatch_ + 1]) {
          pasteMatch_++;
          if (pasteMatch_ == 5) {
            Event ev;
            ev.type = EventType::Paste;
            ev.text = std::move(pasteBuf_);
            push(std::move(ev));
            pasteBuf_.clear();
            st_ = St::Text;
          }
          break;
        }
        // not a paste terminator: replay matched bytes (incl. leading ESC) + current
        for (int i = 0; i <= pasteMatch_; i++) pasteBuf_.push_back(exp[i]);
        pasteBuf_.push_back((char)b);
        pasteMatch_ = 0;
        st_ = St::Paste;
        break;
      }
    }
  }
}

// --- paste: track ESC [ 2 0 1 ~ -------------------------------------------------
// Handled in St::Paste when ESC arrives then '[' ... we need special handling.
// We implement it by intercepting CSI dispatch for param 200/201.

void InputParser::dispatchCsi() {
  char final = buf_.empty() ? 0 : buf_.back();
  int n = params_[0];
  int mod = params_[1] == 0 ? 1 : params_[1];
  bool shift = (mod == 2 || mod == 4 || mod == 6 || mod == 8);
  bool alt = (mod == 3 || mod == 4 || mod == 7 || mod == 8);
  bool ctrl = (mod >= 5);
  switch (final) {
    case 'A': pushKey(KeyUp, ctrl, alt, shift); break;
    case 'B': pushKey(KeyDown, ctrl, alt, shift); break;
    case 'C': pushKey(KeyRight, ctrl, alt, shift); break;
    case 'D': pushKey(KeyLeft, ctrl, alt, shift); break;
    case 'H': pushKey(KeyHome, ctrl, alt, shift); break;
    case 'F': pushKey(KeyEnd, ctrl, alt, shift); break;
    case 'Z': pushKey(KeyBackTab, false, false, true); break;
    case 't':
      if (n == 8) {  // CSI 8;<rows>;<cols>t terminal resize report
        Event ev;
        ev.type = EventType::Resize;
        ev.rows = params_[1];
        ev.cols = params_[2];
        push(std::move(ev));
      }
      break;
    case 'u': decodeKitty(n, params_[1]); break;
    case '~': {
      switch (n) {
        case 2: pushKey(KeyInsert, ctrl, alt, shift); break;
        case 3: pushKey(KeyDelete, ctrl, alt, shift); break;
        case 5: pushKey(KeyPageUp, ctrl, alt, shift); break;
        case 6: pushKey(KeyPageDown, ctrl, alt, shift); break;
        case 7: pushKey(KeyHome, ctrl, alt, shift); break;
        case 8: pushKey(KeyEnd, ctrl, alt, shift); break;
        case 11: case 12: case 13: case 14: case 15:
          pushKey(KeyF1 + (n - 11), ctrl, alt, shift); break;
        case 17: case 18: case 19: case 20: case 21:
          pushKey(KeyF5 + (n - 17), ctrl, alt, shift); break;
        case 23: pushKey(KeyF11, ctrl, alt, shift); break;
        case 24: pushKey(KeyF12, ctrl, alt, shift); break;
        case 200:  // begin bracketed paste
          if (pasteEnabled_) { st_ = St::Paste; pasteBuf_.clear(); }
          break;
        case 201:  // end bracketed paste (rarely reached; PasteEsc handles it)
          if (pasteEnabled_) {
            Event ev;
            ev.type = EventType::Paste;
            ev.text = std::move(pasteBuf_);
            push(std::move(ev));
          }
          break;
        default: break;
      }
      break;
    }
    default: break;
  }
}

void InputParser::dispatchSs3() {
  char final = buf_.empty() ? 0 : buf_.back();
  switch (final) {
    case 'P': pushKey(KeyF1, false, false, false); break;
    case 'Q': pushKey(KeyF2, false, false, false); break;
    case 'R': pushKey(KeyF3, false, false, false); break;
    case 'S': pushKey(KeyF4, false, false, false); break;
    case 'H': pushKey(KeyHome, false, false, false); break;
    case 'F': pushKey(KeyEnd, false, false, false); break;
    case 'A': pushKey(KeyUp, false, false, false); break;
    case 'B': pushKey(KeyDown, false, false, false); break;
    case 'C': pushKey(KeyRight, false, false, false); break;
    case 'D': pushKey(KeyLeft, false, false, false); break;
    default: break;
  }
}

void InputParser::decodeKitty(int code, int mod) {
  bool shift = (mod & 1) != 0, alt = (mod & 2) != 0, ctrl = (mod & 4) != 0;
  if (code >= 57346 && code <= 57357) { pushKey(KeyF1 + (code - 57346), ctrl, alt, shift); return; }
  switch (code) {
    case 9: pushKey(KeyTab, ctrl, alt, shift); break;
    case 13: pushKey(KeyEnter, ctrl, alt, shift); break;
    case 27: pushKey(KeyEscape, ctrl, alt, shift); break;
    case 127: pushKey(KeyBackspace, ctrl, alt, shift); break;
    case 57358: pushKey(KeyInsert, ctrl, alt, shift); break;
    case 57359: pushKey(KeyDelete, ctrl, alt, shift); break;
    case 57360: pushKey(KeyHome, ctrl, alt, shift); break;
    case 57361: pushKey(KeyEnd, ctrl, alt, shift); break;
    case 57362: pushKey(KeyPageUp, ctrl, alt, shift); break;
    case 57363: pushKey(KeyPageDown, ctrl, alt, shift); break;
    case 57364: pushKey(KeyUp, ctrl, alt, shift); break;
    case 57365: pushKey(KeyDown, ctrl, alt, shift); break;
    case 57366: pushKey(KeyLeft, ctrl, alt, shift); break;
    case 57367: pushKey(KeyRight, ctrl, alt, shift); break;
    default:
      if (code > 0 && code <= 0x10FFFF) pushKey((uint32_t)code, ctrl, alt, shift);
      break;
  }
}

void InputParser::parseSgrMouse() {
  // xterm SGR: ESC[<btn;x;yM (press/drag) / ESC[<btn;x;ym (release).
  //   bit0-1 button number; 3 = release; 4 shift, 8 alt, 16 ctrl; 32 = motion;
  //   64/65/66/67 = wheel up/down/left/right (bit 6 set, low bits pick dir).
  // The 32 bit is a *flag*, not a prefix: the old code subtracted it
  // unconditionally, which turned every plain left-press (btn 0 -> -32, whose
  // 0x40 bit is set) into a fake wheel-up. It only looked right on Windows
  // because translateMouse() there added 32 to everything.
  int raw = params_[0];
  Event ev;
  ev.type = EventType::Mouse;
  ev.mouse.x = params_[1] - 1;
  ev.mouse.y = params_[2] - 1;
  ev.mouse.motion = (raw & 32) != 0;
  ev.mouse.shift = (raw & 4) != 0;
  ev.mouse.alt = (raw & 8) != 0;
  ev.mouse.ctrl = (raw & 16) != 0;
  if (raw & 0x40) {                      // wheel: 64 up, 65 down, 66 right, 67 left
    int dir = raw & 0x03;
    if (dir == 0) ev.mouse.wheel = 1;
    else if (dir == 1) ev.mouse.wheel = -1;
    else if (dir == 2) ev.mouse.wheel = 2;
    else ev.mouse.wheel = -2;
    ev.mouse.buttons = 0;
  } else {
    int b = raw & 3;
    if (b == 0) ev.mouse.buttons = 1;
    else if (b == 1) ev.mouse.buttons = 2;
    else if (b == 2) ev.mouse.buttons = 4;
    else {                               // b == 3: button release
      ev.mouse.release = true;
      ev.mouse.buttons = 0;
    }
  }
  if (!ev.mouse.release && ev.mouse.buttons != 0) ev.mouse.press = true;
  push(std::move(ev));
}

bool InputParser::next(Event& ev) {
  if (queuePos_ >= queue_.size()) {
    queue_.clear();
    queuePos_ = 0;
    return false;
  }
  ev = std::move(queue_[queuePos_++]);
  return true;
}

}  // namespace tui
