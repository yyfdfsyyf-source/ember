#pragma once
#include "agent/client.hpp"
#include "agent/context.hpp"
#include "agent/thinking.hpp"
#include "agent/tools.hpp"
#include "agent/plugins.hpp"
#include "agent/settings.hpp"
#include "agent/session.hpp"
#include "agent/trajectory.hpp"
#include "agent/usage.hpp"
#include "tui/tui.hpp"
#include "tui/widgets.hpp"
#include "tui/form.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent {

// Optional per-color override for the palette themes (nord / gruvbox /
// dracula / solarized), loaded from settings.json "theme_colors". Values are
// [r, g, b] with 0-255; any set field replaces the theme's color.
struct ThemeOverride {
  bool active = false;
  unsigned green[3] = {}, gold[3] = {}, blue[3] = {}, purple[3] = {}, red[3] = {};
  unsigned fg[3] = {}, fgDim[3] = {}, bg[3] = {}, headBg[3] = {}, rule[3] = {};
};

// A named API endpoint. Groups reference providers by name; several groups
// may share one provider, and each model belongs to exactly one group.
struct ProviderConfig {
  std::string name;
  std::string baseUrl;
  std::string apiKey;
  std::string apiKeyEnv;  // if non-empty, resolved from the env at load time
  std::string thinkingStyle;  // wire style for this endpoint; "" = "effort"
};

// A named bundle of models that share one provider connection.
// Empty `provider` = use the top-level (default) connection.
struct ModelGroup {
  std::string name;
  std::string provider;
  std::vector<std::string> models;
};

struct AppConfig {
  std::string baseUrl = "http://localhost:11434/v1";
  std::string apiKey;
  std::string apiKeyEnv;  // if non-empty, load the key from this env var at startup
  std::string model = "llama3.2";
  std::vector<std::string> models;  // extra configured models for /model switching & Tab completion
  std::string systemPrompt =
      "You are Ember, a software-engineering agent inside a terminal. You help the user "
      "build and fix software by inspecting the codebase and running real tools.\n"
      "Work habits:\n"
      "- Be small-step: inspect before you act, act before you claim.\n"
      "- After changing code, run the project's build and tests to verify; if they "
      "fail, read the failure, fix, and re-verify. Never repeat the same wrong move twice.\n"
      "- Prefer dedicated tools (file_read/file_write/edit/patch/grep/glob) over shell "
      "for file work; use shell_exec for builds, tests, git and anything that runs a "
      "program.\n"
      "- Use background_start for long-running work and check background_status when "
      "asked, you will also be notified when it finishes.\n"
      "- Read files before editing them so your edits match exact text. State what you "
      "changed and one line of evidence (e.g. a test that now passes).\n"
      "- When stuck, ask the user (ask_user) instead of guessing.";
  size_t budgetTokens = 6000;  // 0 disables context compression
  bool jsonMode = false;
  // --serve: headless NDJSON stdio protocol instead of the TUI (desktop
  // frontend / scripts). Commands on stdin, events on stdout, one JSON per line.
  bool serveMode = false;
  bool shellEnabled = true;
  std::string theme = "dark";  // "dark" | "light" | "terminal" color preset
  ThemeOverride themeOverride; // optional color overrides for palette themes
  std::string sessionPath;  // non-empty: persist/resume one explicit file (legacy --session)
  std::string sessionDir;   // non-empty: multi-session dir; files named <dir>/<id>.json
  std::string pluginsDir;   // non-empty: load plugin executables from this dir
  std::vector<McpConfig> mcpServers;  // MCP stdio servers (tools prefixed "<name>_")
  std::vector<ProviderConfig> providers;  // named API endpoints (see groups)
  std::vector<ModelGroup> groups;         // model groups bound to a provider
  // Workspaces: when non-empty, tools resolve relative paths from the active
  // workspace (the process CWD) and any operation outside ALL registered roots
  // asks for interactive approval. Empty list = no boundary (permissive).
  std::vector<std::string> workspaces;
  int activeWorkspace = 0;                // index into workspaces
  bool workspaceGuard = true;             // enable the outside-access approval
  std::string settingsPath; // non-empty: settings.json; saved by the settings UI
  // Pristine copy of the top-level (default) connection, captured at load
  // time. Provider switches overwrite baseUrl/apiKey; unbound groups and the
  // settings file restore from these. Not persisted on its own.
  std::string defaultBaseUrl;
  std::string defaultApiKey;
  std::string tracePath;    // non-empty: append the trajectory (event log) here
  std::string rulesPath;    // non-empty: AGENTS.md/SKILL.md to inject at turn start
  std::string mode = "standard";  // run-mode preset: standard | minimal | ptc | creator
  // Thinking strength sent with every request: auto (send nothing) | none |
  // minimal | low | medium | high. The wire spelling comes from the active
  // provider's thinkingStyle.
  std::string thinking = "auto";
  std::string thinkingStyle;  // the default connection's wire style; "" = "effort"
  size_t toolResultCap = 4096;  // bytes; tool results longer are truncated (0 = unlimited)
  bool pruneBeforeSend = true;  // drop redundant tool output before each request
  bool tokenSummary = false;    // print a token ledger line at exit
  // Cost estimation for the usage view: USD per 1M tokens.
  double costInPerM = 0.15;
  double costOutPerM = 0.60;
  double costBudgetUsd = 10.0;  // 0 = no budget (cost ring shows 0%)
};

// Text-first TUI agent: conversation pane + input line + status bar.
// Network work runs on a worker thread; the UI thread only renders and
// consumes stream events through a mutex-protected queue.
class App {
 public:
  ~App();
  bool init(AppConfig const& cfg);
  int run();
  // Headless NDJSON stdio loop (--serve): reads one command per line from stdin
  // and writes one event per line to stdout. Same turn machinery as the TUI.
  int runServe();

 private:
  // --serve: the usage event (needs cfg_ / messages_, so it is not next to the
  // other serve emitters in app.cpp's anonymous namespace).
  void serveEmitUsage();
  // Structured replies for the desktop frontend: the slash commands write into
  // convo_/the status line, which do not exist in serve mode, so the GUI asks
  // for state/lists/sets by name instead of scraping text.
  void serveEmitState();
  void serveEmitSessions();
  void serveEmitModelList();
  void serveEmitWorkspaces();
  void serveEmitHistory();
  // /new 与 /clear 在桌面端还需要"清空对话流"这一额外回执，抽出来让类型命令
  // （new_session / clear）与斜杠命令共用同一条路径。
  void serveNewSession();
  void serveClearTranscript();
  // Routes {"type":"set","key":..,"value":..} onto the matching slash command so
  // validation, settings save and the wire-style notes stay in exactly one place.
  void serveSet(std::string const& key, std::string const& value);
  void resize(int cols, int rows);
  int convoHeight() const;  // conversation viewport height (rows)
  std::string sizeTag() const;   // "COLSxROWS" for the page footers
  std::string buildTag() const;  // "v<kVersion> COLSxROWS" for the frame foot
  // Shared overlay-page chrome: header row 0 (title + right tag) + faded rule.
  void overlayChrome(std::string const& title, std::string const& rightTag);
  void render();
  void handleEvent(tui::Event const& e);
  void submit(std::string const& text);
  void runCommand(std::string const& line);
  void openSettings();
  void saveSettings();
  // Provider setup popup: modal base url / api key / model entry (first run
  // and /provider). Replaces the old full-screen welcome form.
  void openProvider();
  void saveProvider();
  void openTrajectory();
  void closeTrajectory();
  void rebuildTrajList();
  void renderTrajectory();
  void openSessions();     // multi-session browser (sessionDir mode)
  void closeSessions();
  void refreshSessList();
  void renderSessions();
  void updateSessPreview();  // refill the selected session's preview pane
  void switchSession(int idx);
  void cycleSession(int delta);  // jump to the next/previous session by key
  void newSessionNow();    // persist current, then start a fresh session
  void renameSelectedSession(std::string const& newTitle);
  void deleteSelectedSession();
  // Modal popup (tui::Dialog): one at a time; onResult receives the button
  // index (or -1 on Esc with no cancel button).
  void openDialog(std::string const& title, std::string const& message,
                  std::vector<std::string> const& buttons,
                  std::function<void(int)> onResult, int defaultBtn = 0);
  void drawModal();  // topmost layer: confirm dialog, else provider popup
  void renderUsage();
  void printHelp(bool firstRun);  // first-run guide / /help listing
  void printWelcome();            // logo card on an empty screen
  // Command palette (Ctrl+P) + "/" suggest list over the input line.
  void openCommandPalette();
  void renderCommandPalette();
  void execCommandPalette();
  void execSuggest();
  void drawCommandSuggest();
  std::vector<int> commandMatches(std::string const& filter, bool prefix = false) const;
  bool cmdSuggestActive() const;
  // Argument completion for "/model <prefix>" "/theme <prefix>" "/mode <prefix>"
  // "/think <prefix>":
  // candidate pool shown in the same popup style as command names.
  struct ArgSug {
    std::string cmd;                    // command word, empty when not applicable
    std::vector<std::string> cands;     // candidates matching the typed prefix
  };
  ArgSug argSuggest(std::string const& text) const;
  bool cmdArgSuggestActive() const;
  void fetchModelList();                // async GET /models (used by bare /model & /model fetch)
  // Flattened /model picker pool: grouped models first, then ungrouped cfg_.models.
  std::vector<std::string> modelChoices() const;
  // Apply the group provider for `model` to cfg_/client_.
  // 0: model in no group (current connection kept); 1: applied;
  // 2: the group references an unknown provider.
  int applyProviderForModel(std::string const& model,
                            std::string* providerName = nullptr);
  // Fill opts.thinking / opts.thinkingStyle from cfg_ and the provider that
  // serves `opts.model` (its group's provider, else the default connection).
  void applyThinking(ChatOptions& opts) const;
  void settleTrajStream();  // flush accumulated reasoning/assistant blocks
  void startTurn(std::string const& userText);
  void pushEvent(StreamEvent const& ev);
  // Returns true when any event was processed. When `out` is non-null it also
  // receives the coalesced events, which --serve serializes instead of drawing.
  bool drainEvents(std::vector<StreamEvent>* out = nullptr);
  void beginStream(tui::Style st);
  void appendBlock(std::string const& text, tui::Style st, bool asLine);
  void appendError(std::string const& text);
  // Rebuild the visible conversation pane from `messages_` (restore / switch).
  void fillConvoFromMessages();
  std::string compressNewlines(std::string s) const;  // collapse \n runs for preview
  // In serve mode the status line has no screen to show on, so every write is
  // mirrored out as a "notice" event (app.cpp).
  void setStatus(std::string const& s);
  // Report the per-turn token ledger to the trajectory (TurnEnd event).
  void appendTurnEndTraj(int turn, int rc);
  // Blocks the worker thread until the user answers (or cancels); returns "" on
  // cancel. Called through the ask_user tool only.
  std::string askUser(std::string const& question);
  bool asking() const;
  // Runs one tool and records its wall time in the session usage stats.
  std::string execTool(std::string const& name, std::string const& argsJson);
  // Workspace policy hook (installed on ToolRegistry): paths outside all
  // registered roots trigger an interactive approval through askUser.
  ToolRegistry::GuardResult workspaceGuard(std::string const& tool,
                                            std::string const& argsJson);
  bool requestOutsideAccess(std::string const& tool, std::string const& absPath);
  void applyActiveWorkspace();          // chdir into the active root
  std::string workspaceNote() const;    // system-prompt suffix when guarded
  void runWsCommand(std::string const& rest);  // /ws list|use|add|rm
  // Tool gating (P4): always-advertised core tools + any tool actually used
  // this session, so the request body shrinks as the conversation grows.
  std::vector<std::string> makeToolSubset() const;
  void noteToolUsed(std::string const& name);
  // Delegated sub-agent turn: runs one full runTurn() loop on the calling
  // thread with a fresh message list and returns the final text answer.
  // Sub-agents get their own Client (parallel-safe), context compression,
  // and no subagent/ask_user tools (no unbounded nesting).
  std::string runSubagent(std::string const& description, std::string const& prompt,
                          int maxToolLoops = 0);
  // Parallel delegation: parses tasksJson, farms each task out to its own
  // thread running runSubagent, returns a JSON array of results.
  std::string runSubagentsParallel(std::string const& tasksJson);
  // Session --session support.
  void restoreSession();
  void persistSession(bool force = false);

  AppConfig cfg_;
  ToolRegistry tools_;
  Client client_;
  std::vector<std::unique_ptr<PluginProcess>> plugins_;
  std::vector<std::unique_ptr<PluginProcess>> mcp_;
  std::vector<std::string> builtinNames_;  // tools registered before plugins/MCP
  std::vector<std::string> modelList_;  // last `/model` fetch, for /model <N>
  bool modelFetching_ = false;         // async /model fetch in flight

  tui::Capabilities caps_;
  tui::Screen screen_;
  tui::Renderer renderer_;
  tui::Out out_;
  tui::InputParser parser_;
  tui::TextView convo_;
  tui::Input input_;
  tui::Form settingsForm_;
  bool settingsOpen_ = false;
  tui::Dialog dialog_;             // modal popup over any view
  std::function<void(int)> dialogCb_;
  std::vector<std::string> wsApproved_;  // normalized paths allowed this session
  std::mutex wsMu_;                      // serialize approvals across workers
  bool providerOpen_ = false;      // provider setup popup visible
  bool providerEditing_ = false;   // editing the selected field inline
  int providerSel_ = 0;            // 0 base url | 1 api key | 2 model
  tui::Input providerInput_;       // editor for the selected field
  std::string provBase_, provKey_, provModel_;  // working copies

  // Trajectory (event log) view.
  TrajectoryLog traj_;
  tui::List trajList_;
  tui::TextView trajDetailText_;
  bool trajOpen_ = false;
  bool trajDetail_ = false;
  int trajDetailScroll_ = 0;
  TrajFilter trajFilter_ = TrajFilter::All;
  std::vector<int> trajVisible_;  // filtered event indexes, in log order
  int turnNum_ = 0;

  // Multi-session browser (sessionDir mode).
  tui::List sessList_;
  std::vector<agent::SessionInfo> sessVisible_;
  tui::TextView sessPreview_;   // split-pane preview of the selected session
  int sessPreviewIdx_ = -1;     // which session the preview reflects
  int sessPreviewScroll_ = 0;   // preview scroll offset
  bool sessOpen_ = false;
  bool sessRenaming_ = false;    // editing the selected session's title
  std::string sessRenameSel_;    // session id being renamed
  std::string curSessId_;        // active session id (empty = none yet)
  std::string curSessTitle_;     // cached title of the active session
  bool usageOpen_ = false;  // usage overlay (rings + session summary)
  std::string pendingReasoning_;  // assembled stream blocks, flushed per event
  std::string pendingDelta_;

  // Command palette (Ctrl+P) + "/" suggest popup.
  bool cmdOpen_ = false;
  std::string cmdFilter_;
  int cmdSel_ = 0;
  bool cmdSug_ = false;    // suggest popup visible this frame
  int cmdSugSel_ = 0;      // highlighted candidate index
  int cmdArgSel_ = 0;      // highlighted index in the /model /theme /mode arg popup
  int cmdSugRows_ = 0;     // popup rows drawn last frame (forceFull tracking)
  std::string savedInput_; // chat draft while the palette borrows the Input

  // Rendering: only repaint when something changed (idle = zero redraws), and
  // only clear the screen on structural changes (resize / overlay switches).
  bool forceFull_ = true;
  uint32_t lastLayoutSig_ = 0;
  std::chrono::steady_clock::time_point lastCtxAt_;
  size_t lastCtxEst_ = 0;
  bool trajDirty_ = true;
  std::chrono::steady_clock::time_point lastPersist_;

  std::vector<Message> messages_;
  std::mutex msgsMutex_;
  std::atomic<bool> busy_{false};
  std::thread worker_;
  std::mutex qmutex_;
  std::vector<StreamEvent> events_;
  agent::SessionUsage turnStartUsage_;  // usage snapshot at the turn start
  // Names of tools invoked at least once in this session (tool gating).
  mutable std::mutex toolsUsedMutex_;
  std::vector<std::string> toolsUsed_;

  bool running_ = true;
  // First visible conversation line when the user has scrolled up;
  // -1 means pinned to the bottom (follows the stream tail).
  int scrollAnchor_ = -1;
  int cols_ = 0, rows_ = 0;
  std::string status_;
  bool streamReasoning_ = false;  // active stream block is dim reasoning (not answer)
  uint64_t frame_ = 0;

  // ask_user bridge: the worker thread parks in askUser() until the UI thread
  // collects a reply from the input line.
  mutable std::mutex askMutex_;
  std::condition_variable askCv_;
  std::string askQuestion_;
  std::string askAnswer_;
  bool askAnswered_ = false;

  // Background-task completion notices, injected into the next worker turn so
  // the model learns a task finished without polling.
  std::mutex bgMutex_;
  std::vector<std::string> bgNotes_;
};

}  // namespace agent
