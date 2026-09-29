#include "agent/app.hpp"
#include "agent/version.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void printHelp() {
  std::printf("Ember %s - text-first CLI agent (OpenAI-compatible / Ollama)\n",
              agent::kVersion);
  std::printf(
      "\n"
      "Usage: ember [options]\n"
      "\n"
      "Options:\n"
      "  --model NAME        model name (env AGENT_MODEL)\n"
      "  --base-url URL      API base URL, e.g. http://localhost:11434/v1\n"
      "                      (env AGENT_BASE_URL)\n"
      "  --api-key KEY       API key (env AGENT_API_KEY or OPENAI_API_KEY)\n"
"  --system TEXT       system prompt\n"
  "  --budget N          context budget in tokens (0 disables; default 6000)\n"
  "  --tool-cap N        tool result byte cap (0 = unlimited; default 4096)\n"
  "  --no-prune          disable redundant-output pruning before each request\n"
  "  --rules PATH        AGENTS.md/SKILL.md file injected at turn start\n"
  "  --mode NAME        run-mode preset: standard|minimal|ptc|creator (env AGENT_MODE)\n"
  "  --thinking NAME    model thinking strength: auto|none|minimal|low|medium|high|max\n"
  "                      (auto sends no field; the wire spelling comes from the\n"
  "                      provider's thinking_style; env AGENT_THINKING)\n"
  "  --token-summary     print a token ledger line on exit\n"
      "  --session PATH      persist/resume the conversation in this JSON file\n"
      "                      (env AGENT_SESSION)\n"
      "  --sessions DIR      multi-session mode: store one conversation per file in\n"
      "                      DIR (sess-*.json); /new, /sessions and Ctrl+O switch\n"
      "                      between them (env AGENT_SESSIONS). Default when no\n"
      "                      session flags are given: %%LOCALAPPDATA%%\\Ember\\sessions\n"
      "  --trace PATH        append the trajectory event log (JSONL, viewed with\n"
      "                      /trajectory; env AGENT_TRACE)\n"
      "  --plugins DIR       load plugin executables (JSON-RPC over stdio) from a\n"
      "                      directory; each advertised tool becomes a tool\n"
      "                      (env AGENT_PLUGINS)\n"
      "  --config PATH       settings JSON file to load/save (default: settings.json\n"
      "                      in the working directory; precedence: defaults < env\n"
      "                      < settings file < CLI options, and /settings writes\n"
      "                      back here)\n"
      "  --json              structured JSON output mode\n"
      "  --serve             headless NDJSON stdio protocol (desktop frontend /\n"
      "                      scripts): commands in on stdin, events out on stdout,\n"
      "                      one JSON document per line. No TUI.\n"
      "  --no-shell          disable the shell_exec tool\n"
      "\n"
      "Command/flag alignment (same names inside the TUI):\n"
      "  /model NAME <-> --model    /json <-> --json\n"
      "  /think NAME <-> --thinking NAME    /mode NAME <-> --mode NAME\n"
      "  /sessions   <-> --sessions DIR    /save <-> --session FILE\n"
      "  /trajectory <-> --trace(/trace alias)    /settings <-> --config\n"
      "  -h, --help          show this help\n"
      "  -v, --version       print the build version (see VERSION) and exit\n");
}

std::string envOr(char const* name, std::string def) {
  char const* v = std::getenv(name);
  return (v && *v) ? std::string(v) : std::move(def);
}

// Default multi-session directory when neither --session nor --sessions is
// given, so conversations are saved and resumed automatically. Same order as
// DiskCache::resolveDir: XDG first, then the Windows known folders, then HOME.
std::string defaultSessionDir() {
  if (char const* x = std::getenv("XDG_DATA_HOME"); x && *x)
    return std::string(x) + "/ember/sessions";
  if (char const* l = std::getenv("LOCALAPPDATA"); l && *l)
    return std::string(l) + "\\Ember\\sessions";
  if (char const* u = std::getenv("USERPROFILE"); u && *u)
    return std::string(u) + "\\.ember\\sessions";
  if (char const* h = std::getenv("HOME"); h && *h)
    return std::string(h) + "/.local/share/ember/sessions";
  return "sessions";
}

}  // namespace

int main(int argc, char** argv) {
  agent::AppConfig cfg;

  // Precedence: defaults < AGENT_* env vars (fallback) < settings.json (the
  // user's explicit, UI-saved configuration) < CLI options. Env vars are
  // applied BEFORE loading settings.json so a saved settings file always wins
  // over stale environment variables (which otherwise lock the app onto one
  // provider and make switching providers appear broken).
  cfg.settingsPath = "settings.json";
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--config") {
      if (i + 1 < argc) {
        cfg.settingsPath = argv[i + 1];
      }
      break;
    }
  }

  cfg.baseUrl = envOr("AGENT_BASE_URL", cfg.baseUrl);
  cfg.model = envOr("AGENT_MODEL", cfg.model);
  cfg.apiKey = envOr("AGENT_API_KEY", envOr("OPENAI_API_KEY", ""));
  cfg.systemPrompt = envOr("AGENT_SYSTEM_PROMPT", cfg.systemPrompt);
  cfg.sessionPath = envOr("AGENT_SESSION", cfg.sessionPath);
  cfg.sessionDir = envOr("AGENT_SESSIONS", cfg.sessionDir);
  cfg.pluginsDir = envOr("AGENT_PLUGINS", cfg.pluginsDir);
  cfg.tracePath = envOr("AGENT_TRACE", cfg.tracePath);
  cfg.mode = envOr("AGENT_MODE", cfg.mode);
  cfg.thinking = envOr("AGENT_THINKING", cfg.thinking);

  if (!cfg.settingsPath.empty()) {
    if (!agent::loadSettingsFile(cfg.settingsPath, cfg)) {
      // missing or unparsable file: keep defaults (the settings UI will
      // recreate it on save)
    }
  }

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](char const* flag) -> char const* {
      if (i + 1 < argc) return argv[++i];
      std::printf("missing value for %s\n", flag);
      std::exit(2);
    };
    if (a == "-h" || a == "--help") {
      printHelp();
      return 0;
    } else if (a == "-v" || a == "--version") {
      std::printf("Ember %s\n", agent::kVersion);
      return 0;
    } else if (a == "--model") {
      cfg.model = next("--model");
    } else if (a == "--base-url") {
      cfg.baseUrl = next("--base-url");
    } else if (a == "--api-key") {
      cfg.apiKey = next("--api-key");
    } else if (a == "--system") {
      cfg.systemPrompt = next("--system");
    } else if (a == "--budget") {
      cfg.budgetTokens = (size_t)std::strtoull(next("--budget"), nullptr, 10);
    } else if (a == "--tool-cap") {
      cfg.toolResultCap = (size_t)std::strtoull(next("--tool-cap"), nullptr, 10);
    } else if (a == "--no-prune") {
      cfg.pruneBeforeSend = false;
    } else if (a == "--token-summary") {
      cfg.tokenSummary = true;
    } else if (a == "--rules") {
      cfg.rulesPath = next("--rules");
    } else if (a == "--mode") {
      cfg.mode = next("--mode");
    } else if (a == "--thinking") {
      cfg.thinking = next("--thinking");
    } else if (a == "--session") {
      cfg.sessionPath = next("--session");
    } else if (a == "--sessions") {
      cfg.sessionDir = next("--sessions");
    } else if (a == "--trace") {
      cfg.tracePath = next("--trace");
    } else if (a == "--plugins") {
      cfg.pluginsDir = next("--plugins");
    } else if (a == "--config") {
      cfg.settingsPath = next("--config");
    } else if (a == "--serve") {
      cfg.serveMode = true;
    } else if (a == "--json") {
      cfg.jsonMode = true;
    } else if (a == "--no-shell") {
      cfg.shellEnabled = false;
    } else {
      std::printf("unknown option: %s\n", a.c_str());
      printHelp();
      return 2;
    }
  }

  cfg.thinking = agent::thinkingLevelFromString(cfg.thinking);  // env/CLI typo -> auto

  // Automatic persistence: without --session/--sessions, use a default
  // session directory so every conversation is saved and resumed on next run.
  if (cfg.sessionDir.empty() && cfg.sessionPath.empty()) {
    cfg.sessionDir = defaultSessionDir();
  }

  agent::App app;
  if (!app.init(cfg)) {
    std::fprintf(stderr, "failed to initialize client (empty base URL?)\n");
    return 1;
  }
  return cfg.serveMode ? app.runServe() : app.run();
}
