#include "tui/form.hpp"
#include "tui/utf8.hpp"
#include <algorithm>
#include <cstring>

namespace tui {

void Form::setFields(std::vector<Field> fields) {
  fields_ = std::move(fields);
  groups_.clear();
  selected_ = 0;
  topVisible_ = 0;
  editing_ = false;
  clampSelected();
}

void Form::setGroup(std::string const& name, size_t firstIndex, size_t count) {
  groups_.push_back({name, firstIndex});
  (void)count;
}

Field const* Form::selectedField() const {
  if (selected_ < 0 || selected_ >= (int)fields_.size()) return nullptr;
  return &fields_[selected_];
}

void Form::clampSelected() {
  if (selected_ < 0) selected_ = 0;
  if (selected_ >= (int)fields_.size() && !fields_.empty()) {
    selected_ = (int)fields_.size() - 1;
  }
  if (fields_.empty()) selected_ = 0;
}

void Form::commitEditing() {
  if (!editing_) return;
  if (selected_ >= 0 && selected_ < (int)fields_.size())
    fields_[selected_].value = editor_.text();
  editing_ = false;
}

void Form::cancelEditing() {
  if (!editing_) return;
  editor_.setText(fields_[selected_].value);
  editing_ = false;
}

bool Form::handle(Event const& e) {
  if (e.type != EventType::Key) return false;

  // ---- inline editing mode: forward to the editor -------------------------
  if (editing_) {
    if (e.ch == KeyEscape) { cancelEditing(); return true; }
    if (e.ch == KeyEnter) {
      fields_[selected_].value = editor_.text();
      editor_.consumeEnter();
      editing_ = false;
      return true;
    }
    editor_.handle(e);
    return true;
  }

  // ---- navigation ---------------------------------------------------------
  bool consumed = true;
  switch (e.ch) {
    case KeyUp:
      if (selected_ > 0) { selected_--; clampSelected(); }
      break;
    case KeyDown:
      if (selected_ + 1 < (int)fields_.size()) { selected_++; clampSelected(); }
      break;
    case KeyPageUp:
      selected_ -= 10; clampSelected();
      break;
    case KeyPageDown:
      selected_ += 10; clampSelected();
      break;
    case KeyHome:
      selected_ = 0;
      break;
    case KeyEnd:
      selected_ = (int)fields_.size() - 1; clampSelected();
      break;
    case KeyTab:
      if (selected_ + 1 < (int)fields_.size()) { selected_++; clampSelected(); }
      break;
    case KeyBackTab:
      if (selected_ > 0) { selected_--; clampSelected(); }
      break;
    case KeyEnter:
    case KeyCtrlM:
      if (selected_ < (int)fields_.size()) {
        Field& f = fields_[selected_];
        if (f.kind == FieldKind::Toggle) {
          f.value = (f.value == "1") ? "0" : "1";
        } else {
          editor_.setText(f.value);
          editor_.setCursor((int)f.value.size());
          editing_ = true;
        }
      }
      break;
    case KeyCtrlJ:
      // Ctrl+J (LF) is aliased to Enter by some terminals; treat as Enter.
      if (selected_ < (int)fields_.size()) {
        Field& f = fields_[selected_];
        if (f.kind == FieldKind::Toggle) {
          f.value = (f.value == "1") ? "0" : "1";
        } else {
          editor_.setText(f.value);
          editor_.setCursor((int)f.value.size());
          editing_ = true;
        }
      }
      break;
    default:
      if (e.ch == ' ' || e.ch == KeyLeft || e.ch == KeyRight) {
        if (selected_ < (int)fields_.size()) {
          Field& f = fields_[selected_];
          if (f.kind == FieldKind::Toggle) {
            f.value = (f.value == "1") ? "0" : "1";
          } else if (e.ch == ' ') {
            editor_.setText(f.value);
            editor_.setCursor((int)f.value.size());
            editing_ = true;
          }
        }
      } else {
        consumed = false;
      }
      break;
  }
  return consumed;
}

void Form::render(Screen& s, Rect box, Style labelSt, Style valueSt, Style focusSt,
                  Style groupSt, Style hintSt, Style editSt, Style cursorSt) {
  // compute label column width
  int labelW = 0;
  for (auto const& f : fields_) labelW = std::max(labelW, (int)f.label.size());
  labelW += 2;

  int rows = box.h;
  // scroll so the selected field is visible
  if (selected_ < topVisible_) topVisible_ = selected_;
  if (selected_ >= topVisible_ + rows) topVisible_ = selected_ - rows + 1;
  if (topVisible_ < 0) topVisible_ = 0;

  // group header rows: collect a flat list of (type, idx) rows
  struct Row {
    bool isGroup;
    size_t idx;
    std::string label;
  };
  std::vector<Row> rowsList;
  size_t gi = 0;
  for (size_t i = 0; i < fields_.size(); i++) {
    while (gi < groups_.size() && groups_[gi].second <= i) {
      rowsList.push_back({true, 0, groups_[gi].first});
      gi++;
    }
    rowsList.push_back({false, i, {}});
  }
  while (gi < groups_.size()) { rowsList.push_back({true, 0, groups_[gi].first}); gi++; }

  // find the row index of the selected field
  int selRow = -1;
  int rowIdx = 0;
  for (auto const& r : rowsList) {
    if (!r.isGroup && r.idx == (size_t)selected_) { selRow = rowIdx; break; }
    rowIdx++;
  }
  if (selRow >= topVisible_ + rows) topVisible_ = selRow - rows + 1;
  if (selRow < topVisible_) topVisible_ = selRow;
  if (topVisible_ < 0) topVisible_ = 0;

  // draw visible rows
  int y = 0;
  for (size_t k = (size_t)topVisible_; k < rowsList.size() && y < rows; k++) {
    Row const& r = rowsList[k];
    int ry = box.y + y;
    // Wipe the row first so a shorter value/hint cannot leave this row's
    // previous-frame tail (Screen is an accumulation buffer; see
    // drawTextView for the same issue).
    s.fill(Rect(box.x, ry, box.w, 1), Cell{});
    if (r.isGroup) {
      s.putText(box.x, ry, r.label.c_str(), groupSt);
      y++;
      continue;
    }
    Field const& f = fields_[r.idx];
    bool isSel = (int)r.idx == selected_;
    Style ls = isSel ? focusSt : labelSt;
    s.putText(box.x, ry, f.label.c_str(), ls);
    int vx = box.x + labelW;
    int vw = box.x + box.w - vx;
    if (vw < 1) { y++; continue; }
    if (isSel && editing_) {
      // inline editor for this field
      editor_.setBox(Rect(vx, ry, vw, 1));
      char const* prompt = "";
      editor_.render(s, prompt, editSt, editSt, cursorSt);
    } else {
      std::string val = f.value;
      if (f.kind == FieldKind::Secret && !val.empty()) val = std::string(val.size(), '*');
      if (f.kind == FieldKind::Toggle) val = (f.value == "1") ? "[x] on" : "[ ] off";
      if ((int)val.size() > vw) val = val.substr(0, (size_t)std::max(0, vw));
      s.putText(vx, ry, val.c_str(), isSel ? focusSt : valueSt);
      if (!f.hint.empty() && !isSel) {
        int hx = vx + (int)val.size() + 2;
        if (hx + (int)f.hint.size() < box.x + box.w) {
          s.putText(hx, ry, f.hint.c_str(), hintSt);
        }
      }
    }
    y++;
  }
  if (y < rows)
    s.fill(Rect(box.x, box.y + y, box.w, rows - y), Cell{});
}

}  // namespace tui
