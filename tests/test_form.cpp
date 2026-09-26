#include "tui/form.hpp"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    checks++;                                                             \
    if (!(cond)) {                                                        \
      failures++;                                                         \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                     \
  } while (0)

static tui::Event key(uint32_t ch) {
  tui::Event e;
  e.type = tui::EventType::Key;
  e.ch = ch;
  return e;
}

static void test_navigation() {
  tui::Form f;
  std::vector<tui::Field> fields;
  for (int i = 0; i < 25; i++) {
    tui::Field fd;
    fd.label = "field" + std::to_string(i);
    fd.tag = "f" + std::to_string(i);
    fd.value = "v" + std::to_string(i);
    fields.push_back(fd);
  }
  f.setFields(fields);
  CHECK(f.selected() == 0);

  f.handle(key(tui::KeyDown));
  CHECK(f.selected() == 1);
  f.handle(key(tui::KeyDown));
  f.handle(key(tui::KeyDown));
  CHECK(f.selected() == 3);

  f.handle(key(tui::KeyUp));
  CHECK(f.selected() == 2);

  f.handle(key(tui::KeyPageDown));
  CHECK(f.selected() == 12);
  f.handle(key(tui::KeyPageUp));
  CHECK(f.selected() == 2);

  f.handle(key(tui::KeyHome));
  CHECK(f.selected() == 0);
  f.handle(key(tui::KeyEnd));
  CHECK(f.selected() == 24);
}

static void test_toggle() {
  tui::Form f;
  std::vector<tui::Field> fields;
  tui::Field a;
  a.label = "json";
  a.kind = tui::FieldKind::Toggle;
  a.value = "0";
  fields.push_back(a);
  f.setFields(fields);

  f.handle(key(tui::KeyEnter));
  auto snap = f.fieldsSnapshot();
  CHECK(snap[0].value == "1");
  f.handle(key(' '));
  snap = f.fieldsSnapshot();
  CHECK(snap[0].value == "0");
  f.handle(key(tui::KeyLeft));
  snap = f.fieldsSnapshot();
  CHECK(snap[0].value == "1");
  f.handle(key(tui::KeyRight));
  snap = f.fieldsSnapshot();
  CHECK(snap[0].value == "0");
}

static void test_edit() {
  tui::Form f;
  std::vector<tui::Field> fields;
  tui::Field a;
  a.label = "model";
  a.kind = tui::FieldKind::Text;
  a.value = "llama";
  fields.push_back(a);
  f.setFields(fields);

  f.handle(key(tui::KeyEnter));
  CHECK(f.editing());
  for (char ch : std::string("3.1")) {
    f.handle(key((unsigned char)ch));
  }
  CHECK(f.editing());  // still editing while typing
  f.handle(key(tui::KeyEnter));
  CHECK(!f.editing());
  auto snap = f.fieldsSnapshot();
  CHECK(snap[0].value == "llama3.1");
}

static void test_edit_escape_restores() {
  tui::Form f;
  std::vector<tui::Field> fields;
  tui::Field a;
  a.label = "model";
  a.kind = tui::FieldKind::Text;
  a.value = "llama";
  fields.push_back(a);
  f.setFields(fields);

  f.handle(key(tui::KeyEnter));
  CHECK(f.editing());
  f.handle(key('x'));
  f.handle(key(tui::KeyEscape));
  CHECK(!f.editing());
  auto snap = f.fieldsSnapshot();
  CHECK(snap[0].value == "llama");
}

int main() {
  test_navigation();
  test_toggle();
  test_edit();
  test_edit_escape_restores();
  if (failures == 0) {
    std::printf("test_form: all %d checks passed\n", checks);
    return 0;
  }
  std::printf("test_form: %d/%d FAILED\n", failures, checks);
  return 1;
}
