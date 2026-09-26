// Drives the real App::handleEvent with synthetic key events and asserts the
// input line, suggest popup and command palette behave. No console required.
// std:: headers must be pre-included BEFORE `private public` so libstdc++
// internals (sstream & co) are not mangled by the macro.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <iostream>

#define private public
#include "agent/app.hpp"
#undef private

static int g_fail = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cerr << "FAIL line " << __LINE__ << ": " << #cond << "\n";   \
      g_fail++;                                                         \
    }                                                                   \
  } while (0)

static tui::Event key(uint32_t ch, bool shift = false, bool ctrl = false) {
  tui::Event e;
  e.type = tui::EventType::Key;
  e.ch = ch;
  e.shift = shift;
  e.ctrl = ctrl;
  return e;
}

// The whole screen as UTF-8 text, for asserting on what the UI actually drew.
static std::string screenText(agent::App& app) {
  std::string out;
  for (int y = 0; y < app.rows_; y++) {
    for (int x = 0; x < app.cols_; x++) {
      auto const& c = app.screen_.cell(x, y);
      if (c.cont) continue;
      if (!c.ch) { out += ' '; continue; }
      char t[8];
      size_t k = 0;
      if (c.ch < 0x80) t[k++] = (char)c.ch;
      else if (c.ch < 0x800) { t[k++] = (char)(0xC0 | (c.ch >> 6)); t[k++] = (char)(0x80 | (c.ch & 0x3F)); }
      else { t[k++] = (char)(0xE0 | (c.ch >> 12)); t[k++] = (char)(0x80 | ((c.ch >> 6) & 0x3F)); t[k++] = (char)(0x80 | (c.ch & 0x3F)); }
      out.append(t, k);
    }
    out += '\n';
  }
  return out;
}

int main() {
  agent::AppConfig cfg;
  cfg.baseUrl = "http://127.0.0.1:1/v1";  // offline; commands only
  cfg.tracePath = "";
  agent::App app;
  if (!app.init(cfg)) {
    std::cerr << "FAIL: init\n";
    return 1;
  }

  // --- plain typing --------------------------------------------------------
  for (char c : std::string("hel"))
    app.handleEvent(key((uint32_t)c));
  CHECK(app.input_.text() == "hel");
  app.handleEvent(key(tui::KeyBackspace));
  CHECK(app.input_.text() == "he");
  app.handleEvent(key(tui::KeyLeft));
  app.handleEvent(key((uint32_t)'L'));
  CHECK(app.input_.text() == "hLe");
  app.handleEvent(key(tui::KeyRight));
  app.handleEvent(key(tui::KeyRight));
  app.handleEvent(key((uint32_t)'l'));
  app.handleEvent(key((uint32_t)'l'));
  app.handleEvent(key((uint32_t)'o'));
  CHECK(app.input_.text() == "hLello");
  app.handleEvent(key(tui::KeyEscape));
  CHECK(app.input_.text().empty());

  // forward delete (KeyDelete) removes the codepoint AFTER the cursor
  for (char c : std::string("abc"))
    app.handleEvent(key((uint32_t)c));
  app.handleEvent(key(tui::KeyLeft));  // cursor between 'b' and 'c'
  CHECK(app.input_.text() == "abc");
  app.handleEvent(key(tui::KeyDelete));
  CHECK(app.input_.text() == "ab");  // 'c' gone
  app.handleEvent(key(tui::KeyBackTab));
  CHECK(app.input_.text() == "ab");  // Shift+Tab never inserts
  app.handleEvent(key(tui::KeyBackspace));
  CHECK(app.input_.text() == "a");  // 'b' gone (backward delete)
  app.handleEvent(key(tui::KeyEscape));
  CHECK(app.input_.text().empty());

  // --- suggest popup --------------------------------------------------------
  app.handleEvent(key((uint32_t)'/'));
  CHECK(!app.cmdSuggestActive());
  app.handleEvent(key((uint32_t)'m'));
  CHECK(app.cmdSuggestActive());
  app.handleEvent(key((uint32_t)'o'));
  CHECK(!app.commandMatches(app.input_.text().substr(1), true).empty());
  int n0 = (int)app.commandMatches("mo", true).size();
  app.handleEvent(key(tui::KeyDown));
  CHECK(app.cmdSugSel_ == 1 % n0);  // "mo" matches: model, mode (list order)
  // model is the first match (kCommands lists model before mode); wrap back to it
  while (app.cmdSugSel_ != 0) app.handleEvent(key(tui::KeyUp));
  app.handleEvent(key(tui::KeyEnter));
  CHECK(app.input_.text() == "/model ");  // needsArg: prefill, no submit
  CHECK(!app.cmdOpen_);
  // popup gone once a space is typed (prefix no longer matches)
  CHECK(!app.cmdSuggestActive());
  app.handleEvent(key((uint32_t)'x'));
  CHECK(app.input_.text() == "/model x");
  app.handleEvent(key(tui::KeyEscape));
  CHECK(app.input_.text().empty());

  // Esc in the chat closes clears the input (pre-existing chat behavior)
  app.handleEvent(key((uint32_t)'/'));
  app.handleEvent(key((uint32_t)'c'));
  CHECK(!app.commandMatches(app.input_.text().substr(1), true).empty());
  app.handleEvent(key(tui::KeyEscape));
  CHECK(app.input_.text().empty());

  // unknown /xyzzy: no matches -> popup hidden; Enter submits the line
  for (char c : std::string("/xyzzy"))
    app.handleEvent(key((uint32_t)c));
  CHECK(app.commandMatches(app.input_.text().substr(1), true).empty());
  app.handleEvent(key(tui::KeyEnter));
  CHECK(app.input_.text().empty());
  CHECK(!app.busy_);

  // --- command palette -------------------------------------------------------
  for (char c : std::string("draft"))
    app.handleEvent(key((uint32_t)c));
  CHECK(app.input_.text() == "draft");
  app.handleEvent(key(tui::KeyCtrlP));
  CHECK(app.cmdOpen_);
  CHECK(app.input_.text().empty());  // "draft" has no '/' -> empty filter
  for (char c : std::string("cl"))
    app.handleEvent(key((uint32_t)c));
  CHECK(app.input_.text() == "cl");
  auto idxs0 = app.commandMatches("cl");
  CHECK(idxs0.size() == 1);  // only /clear
  app.handleEvent(key(tui::KeyEnter));
  CHECK(!app.cmdOpen_);       // /clear executed
  CHECK(app.input_.text().empty());  // palette leaves no residue

  // needsArg from the palette -> "/model " remains in the input
  app.handleEvent(key(tui::KeyCtrlP));
  CHECK(app.input_.text().empty());
  for (char c : std::string("mo"))
    app.handleEvent(key((uint32_t)c));
  app.handleEvent(key(tui::KeyEnter));
  CHECK(!app.cmdOpen_);
  CHECK(app.input_.text() == "/model ");
  app.handleEvent(key(tui::KeyEscape));
  CHECK(app.input_.text().empty());

  // Esc restores the previous draft
  for (char c : std::string("saved"))
    app.handleEvent(key((uint32_t)c));
  app.handleEvent(key(tui::KeyCtrlP));
  app.handleEvent(key(tui::KeyEscape));
  CHECK(!app.cmdOpen_);
  CHECK(app.input_.text() == "saved");

  // Tab cycles the palette selection
  app.handleEvent(key(tui::KeyCtrlP));
  int start = app.cmdSel_;
  app.handleEvent(key(tui::KeyTab));
  CHECK(app.cmdSel_ == start + 1);
  app.handleEvent(key(tui::KeyEscape));

  // Enter with an empty filter executes the first command (/help)
  app.handleEvent(key(tui::KeyCtrlP));
  app.handleEvent(key(tui::KeyEnter));
  CHECK(!app.cmdOpen_);
  CHECK(app.input_.text().empty());
  CHECK(app.convo_.lineCount() > 10);  // help listing appended

  // --- busy gating --------------------------------------------------------
  app.busy_.store(true);
  for (char c : std::string("zz"))
    app.handleEvent(key((uint32_t)c));
  CHECK(app.input_.text().empty());
  app.handleEvent(key(tui::KeyCtrlP));  // palette refused while busy
  CHECK(!app.cmdOpen_);
  app.busy_.store(false);

  // --- /clear still works end-to-end (input consumed, no busy) -------------
  for (char c : std::string("/clear"))
    app.handleEvent(key((uint32_t)c));
  app.handleEvent(key(tui::KeyEnter));
  CHECK(app.input_.text().empty());
  CHECK(!app.busy_);

  // --- full render + command key sequences (crash regression) --------------
  // Drives every command through the real rendering path (suggest popup is
  // painted on every keystroke), including the overlays they open. This used
  // to crash the app outright; here it must just survive.
  app.resize(120, 36);
  for (int round = 0; round < 30; round++) {
    for (char c : std::string("/sessions")) {
      app.handleEvent(key((uint32_t)c));
      app.render();
    }
    app.handleEvent(key(tui::KeyEnter));  // suggest Enter -> execSuggest
    app.render();
    CHECK(app.input_.text().empty());
    CHECK(!app.sessOpen_);  // no session dir: status-only path

    for (char c : std::string("/s")) {
      app.handleEvent(key((uint32_t)c));
      app.render();
    }
    for (char c : std::string("s")) {
      app.handleEvent(key((uint32_t)c));
      app.render();
    }
    app.handleEvent(key(tui::KeyDown));
    app.render();
    app.handleEvent(key(tui::KeyUp));
    app.render();
    app.handleEvent(key(tui::KeyTab));
    app.render();
    app.handleEvent(key(tui::KeyEnter));
    app.render();
    app.handleEvent(key(tui::KeyEscape));
    app.render();
    CHECK(app.input_.text().empty());

    for (char c : std::string("/cl")) {
      app.handleEvent(key((uint32_t)c));
      app.render();
    }
    app.handleEvent(key(tui::KeyEnter));
    app.render();

    for (char c : std::string("/us")) {
      app.handleEvent(key((uint32_t)c));
      app.render();
    }
    app.handleEvent(key(tui::KeyEnter));
    app.render();
    CHECK(app.usageOpen_);
    app.handleEvent(key(tui::KeyEscape));
    app.render();
    CHECK(!app.usageOpen_);

    for (char c : std::string("/st")) {
      app.handleEvent(key((uint32_t)c));
      app.render();
    }
    app.handleEvent(key(tui::KeyEnter));
    app.render();

    for (char c : std::string("/tr")) {
      app.handleEvent(key((uint32_t)c));
      app.render();
    }
    app.handleEvent(key(tui::KeyEnter));
    app.render();
    CHECK(app.trajOpen_);
    app.handleEvent(key('2'));  // filter row: none match (empty log) but must not crash
    app.render();
    app.handleEvent(key(tui::KeyEscape));
    app.render();
    CHECK(!app.trajOpen_);

    for (char c : std::string("/set")) {
      app.handleEvent(key((uint32_t)c));
      app.render();
    }
    app.handleEvent(key(tui::KeyTab));
    app.render();
    app.handleEvent(key(tui::KeyEnter));
    app.render();
    CHECK(app.settingsOpen_);
    app.handleEvent(key(tui::KeyEscape));
    app.render();
    CHECK(!app.settingsOpen_);
  }
  std::cout << "render loop survived\n";

  // --- session restore keeps markdown styling ---------------------------------
  // Live streaming renders assistant content as markdown; restoring a session
  // must not regress to plain text (regression: fillConvoFromMessages used
  // toPlainText and lost all md styling after open/switch).
  {
    app.messages_.clear();
    app.messages_.push_back(agent::Message("user", "render this"));
    app.messages_.push_back(agent::Message("assistant", "# Title\n\n```cpp\nint x = 1;\n```\n\n**bold** done"));
    app.fillConvoFromMessages();
    bool sawColored = false;
    int n = app.convo_.lineCount();
    for (int i = 0; i < n; i++) {
      auto const* runs = app.convo_.runsAt(i);
      if (runs && !runs->empty()) {
        for (auto const& r : *runs) {
          // A real md render must carry an fg color, not just attribute bits
          // (a zero-theme run would pass `v != 0` via attrs only).
          if ((r.st.v & 0xFFFFu) != 0) { sawColored = true; break; }
        }
      }
      if (sawColored) break;
    }
    CHECK(sawColored);
    bool sawFence = false;
    for (int i = 0; i < n; i++) {
      if (app.convo_.line(i).find("int x = 1;") != std::string::npos) sawFence = true;
    }
    CHECK(sawFence);
    std::cout << "restore keeps markdown styling\n";
  }

  // --- thinking strength: level -> cfg_ -> wire spelling -> the model tag ----
  {
    app.resize(90, 30);
    app.runCommand("/think high");
    app.render();
    CHECK(app.cfg_.thinking == "high");
    CHECK(app.status_.find("thinking: high") != std::string::npos);
    CHECK(screenText(app).find("[think:high]") != std::string::npos);

    app.runCommand("/think ultra");  // unknown level: rejected, previous kept
    CHECK(app.cfg_.thinking == "high");
    CHECK(app.status_.find("auto | none") != std::string::npos);

    // Tab completion over the levels, same as /theme and /mode.
    CHECK(app.argSuggest("/think l").cmd == "think");
    CHECK(app.argSuggest("/think l").cands.size() == 1 &&
          app.argSuggest("/think l").cands[0] == "low");

    // The spelling follows the provider that serves the current model.
    agent::ProviderConfig p;
    p.name = "svc"; p.baseUrl = "http://127.0.0.1:1/v1"; p.thinkingStyle = "thinking";
    agent::ModelGroup g;
    g.name = "gg"; g.provider = "svc"; g.models = {app.cfg_.model};
    app.cfg_.providers.push_back(p);
    app.cfg_.groups.push_back(g);
    agent::ChatOptions o;
    o.model = app.cfg_.model;
    app.applyThinking(o);
    CHECK(o.thinking == "high" && o.thinkingStyle == "thinking");
    app.runCommand("/think low");
    CHECK(app.status_.find("只分开关") != std::string::npos);  // 二值写法：档位会塌缩

    app.cfg_.groups.clear();
    app.cfg_.providers.clear();
    o = agent::ChatOptions{};
    o.model = app.cfg_.model;
    app.applyThinking(o);
    CHECK(o.thinkingStyle.empty());  // ungrouped model -> the default connection

    app.runCommand("/think auto");
    app.render();
    CHECK(app.cfg_.thinking == "auto");
    CHECK(screenText(app).find("[think:") == std::string::npos);  // tag leaves chrome
  }

  if (g_fail == 0) {
    std::cout << "ALL INPUT-FLOW CHECKS PASSED\n";
    return 0;
  }
  std::cerr << g_fail << " check(s) FAILED\n";
  return 1;
}