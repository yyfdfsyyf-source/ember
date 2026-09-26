#pragma once
// tui/form.hpp - a keyboard-driven settings form.
//
// A Form is a vertical list of labeled fields. Fields have a type:
//   Text    - free-form string (edited inline via an embedded Input widget)
//   Secret  - like Text, but the value is masked with '*' when not editing
//   Int     - non-negative integer (digits only)
//   Toggle  - on/off, switched with Enter/Space/left/right
// A field may carry a tag (used by the app to map it back to config keys)
// and a group label shown as a dim header row before the first field of
// each group.
//
// Navigation: Up/Down move, PageUp/PageDown jump 10, Home/End jump to
// first/last field, Tab moves down, Shift+Tab moves up. Enter on a
// Text/Secret/Int field starts inline editing (Escape cancels, Enter
// commits); Enter on a Toggle flips it.
#include "tui/input.hpp"
#include "tui/screen.hpp"
#include "tui/widgets.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace tui {

enum class FieldKind : uint8_t { Text, Secret, Int, Toggle };

struct Field {
  std::string label;    // shown on the left, e.g. "model"
  FieldKind kind = FieldKind::Text;
  std::string value;    // Text/Secret/Int: current string; Toggle: "1"/"0"
  std::string tag;      // opaque id for the app
  std::string hint;     // dim suffix on the value line
};

// Returns: keys the form consumed (nav/editing). The caller still receives
// KeyEscape/KeyEnter for its own handling by the return flags below.
class Form {
 public:
  void setFields(std::vector<Field> fields);
  void setGroup(std::string const& name, size_t firstIndex, size_t count);

  int selected() const { return selected_; }
  Field const* selectedField() const;
  int count() const { return (int)fields_.size(); }
  // Copy of the current field values (committed; any in-progress edit is
  // committed by commitEditing() first).
  std::vector<Field> fieldsSnapshot() const { return fields_; }

  // True while inline editing is active (value is being typed).
  bool editing() const { return editing_; }
  // Call this after the app handled a commit (KeyEnter consumed) to leave
  // editing mode; the committed value is already stored in the field.
  void commitEditing();
  void cancelEditing();

  bool handle(Event const& e);
  void render(Screen& s, Rect box, Style labelSt, Style valueSt, Style focusSt,
              Style groupSt, Style hintSt, Style editSt, Style cursorSt);

 private:
  void clampSelected();
  int topVisible_ = 0;
  int selected_ = 0;
  bool editing_ = false;
  Input editor_;
  std::vector<Field> fields_;
  std::vector<std::pair<std::string, size_t>> groups_;  // (label, firstIndex)
  size_t groupEnd(size_t g) const {
    return (g + 1 < groups_.size()) ? groups_[g + 1].second : fields_.size();
  }
};

}  // namespace tui
