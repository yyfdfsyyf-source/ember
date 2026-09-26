#include "agent/app.hpp"
#include "agent/mode.hpp"
#include "agent/settings.hpp"
#include "minijson.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace agent {

namespace {

mini::Value readFile(std::string const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return mini::Value::makeNull();  // unreadable => not-a-file
  std::stringstream ss;
  ss << in.rdbuf();
  mini::Value v = mini::parse(ss.str());
  return v.type == mini::Value::Object ? std::move(v) : mini::Value::makeNull();
}

bool writeFile(std::string const& path, std::string const& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out << text;
  return out.good();
}

// A thinking_style value is stored verbatim only when it names a known wire
// style; anything else leaves the field unset (which means "effort").
void readThinkingStyle(mini::Value const* v, std::string& out) {
  if (!v || v->type != mini::Value::String || v->s.empty()) return;
  for (auto const& s : thinkingStyles())
    if (s == v->s) out = s;
}

// Re-indent a minified JSON dump: one key per line, 2-space nesting.
// Purely textual (string literals are copied verbatim, escapes honored).
std::string prettyJson(std::string const& src) {
  std::string out;
  out.reserve(src.size() + 128);
  int depth = 0;
  bool inStr = false;
  for (size_t i = 0; i < src.size(); i++) {
    char c = src[i];
    if (inStr) {
      out += c;
      if (c == '\\' && i + 1 < src.size()) out += src[++i];
      else if (c == '"') inStr = false;
      continue;
    }
    switch (c) {
      case '"': inStr = true; out += c; break;
      case '{':
      case '[':
        out += c;
        out += '\n';
        out.append((size_t)(++depth) * 2, ' ');
        break;
      case '}':
      case ']':
        out += '\n';
        out.append((size_t)(--depth < 0 ? 0 : depth) * 2, ' ');
        out += c;
        break;
      case ':': out += ": "; break;
      case ',':
        out += ",\n";
        out.append((size_t)depth * 2, ' ');
        break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

bool loadSettingsFile(std::string const& path, AppConfig& cfg) {
  mini::Value root = readFile(path);
  if (root.type != mini::Value::Object) return false;
  mini::Value const* v = nullptr;
  if ((v = root.get("base_url")) && v->type == mini::Value::String) cfg.baseUrl = v->s;
  if ((v = root.get("api_key")) && v->type == mini::Value::String) cfg.apiKey = v->s;
  if ((v = root.get("api_key_env")) && v->type == mini::Value::String && !v->s.empty()) {
    cfg.apiKeyEnv = v->s;
    if (char const* e = std::getenv(v->s.c_str())) cfg.apiKey = e;
  }
  if ((v = root.get("model")) && v->type == mini::Value::String) cfg.model = v->s;
  if ((v = root.get("models")) && v->type == mini::Value::Array) {
    cfg.models.clear();
    for (auto const& item : v->arr)
      if (item.type == mini::Value::String && !item.s.empty()) cfg.models.push_back(item.s);
  }
  if ((v = root.get("providers")) && v->type == mini::Value::Array) {
    cfg.providers.clear();
    for (auto const& item : v->arr) {
      if (item.type != mini::Value::Object) continue;
      mini::Value const* pn = item.get("name");
      mini::Value const* pb = item.get("base_url");
      if (!pn || pn->type != mini::Value::String || pn->s.empty()) continue;
      if (!pb || pb->type != mini::Value::String || pb->s.empty()) continue;
      ProviderConfig p;
      p.name = pn->s;
      p.baseUrl = pb->s;
      if (mini::Value const* k = item.get("api_key"))
        if (k->type == mini::Value::String) p.apiKey = k->s;
      if (mini::Value const* ke = item.get("api_key_env"))
        if (ke->type == mini::Value::String && !ke->s.empty()) {
          p.apiKeyEnv = ke->s;
          if (char const* e = std::getenv(ke->s.c_str())) p.apiKey = e;
        }
      readThinkingStyle(item.get("thinking_style"), p.thinkingStyle);
      cfg.providers.push_back(std::move(p));
    }
  }
  if ((v = root.get("groups")) && v->type == mini::Value::Array) {
    cfg.groups.clear();
    for (auto const& item : v->arr) {
      if (item.type != mini::Value::Object) continue;
      mini::Value const* gn = item.get("name");
      if (!gn || gn->type != mini::Value::String || gn->s.empty()) continue;
      ModelGroup g;
      g.name = gn->s;
      if (mini::Value const* gp = item.get("provider"))
        if (gp->type == mini::Value::String) g.provider = gp->s;
      if (mini::Value const* gm = item.get("models"))
        if (gm->type == mini::Value::Array)
          for (auto const& x : gm->arr)
            if (x.type == mini::Value::String && !x.s.empty()) g.models.push_back(x.s);
      cfg.groups.push_back(std::move(g));
    }
  }
  if ((v = root.get("system_prompt")) && v->type == mini::Value::String)
    cfg.systemPrompt = v->s;
  if ((v = root.get("theme")) && v->type == mini::Value::String) cfg.theme = v->s;
  if ((v = root.get("mode")) && v->type == mini::Value::String) cfg.mode = v->s;
  if ((v = root.get("thinking")) && v->type == mini::Value::String)
    cfg.thinking = thinkingLevelFromString(v->s);
  readThinkingStyle(root.get("thinking_style"), cfg.thinkingStyle);
  if ((v = root.get("theme_colors")) && v->type == mini::Value::Object) {
    auto read = [&](char const* k, unsigned out[3]) {
      mini::Value const* c = v->get(k);
      if (!c || c->type != mini::Value::Array || c->arr.size() != 3) return;
      for (int i = 0; i < 3; i++) {
        int val = 0;
        if (c->arr[(size_t)i].type == mini::Value::Int) val = (int)c->arr[(size_t)i].i;
        out[i] = (unsigned)(val < 0 ? 0 : (val > 255 ? 255 : val));
      }
    };
    cfg.themeOverride.active = true;
    read("green", cfg.themeOverride.green);
    read("gold", cfg.themeOverride.gold);
    read("blue", cfg.themeOverride.blue);
    read("purple", cfg.themeOverride.purple);
    read("red", cfg.themeOverride.red);
    read("fg", cfg.themeOverride.fg);
    read("fg_dim", cfg.themeOverride.fgDim);
    read("bg", cfg.themeOverride.bg);
    read("head_bg", cfg.themeOverride.headBg);
    read("rule", cfg.themeOverride.rule);
  }
  if ((v = root.get("budget")) && (v->type == mini::Value::Int || v->type == mini::Value::Double))
    cfg.budgetTokens = (size_t)v->asInt((int64_t)cfg.budgetTokens);
  if ((v = root.get("tool_result_cap")) && (v->type == mini::Value::Int || v->type == mini::Value::Double))
    cfg.toolResultCap = (size_t)v->asInt((int64_t)cfg.toolResultCap);
  if ((v = root.get("prune_before_send")) && (v->type == mini::Value::Bool || v->type == mini::Value::Int))
    cfg.pruneBeforeSend = v->asBool(cfg.pruneBeforeSend);
  if ((v = root.get("rules_path")) && v->type == mini::Value::String) cfg.rulesPath = v->s;
  if ((v = root.get("session_dir")) && v->type == mini::Value::String) cfg.sessionDir = v->s;
  if ((v = root.get("json_mode")) && (v->type == mini::Value::Bool || v->type == mini::Value::Int))
    cfg.jsonMode = v->asBool(cfg.jsonMode);
  if ((v = root.get("shell_enabled")) && (v->type == mini::Value::Bool || v->type == mini::Value::Int))
    cfg.shellEnabled = v->asBool(cfg.shellEnabled);
  if ((v = root.get("mcp_servers")) && v->type == mini::Value::Array) {
    cfg.mcpServers.clear();
    for (auto const& item : v->arr) {
      if (item.type != mini::Value::Object) continue;
      mini::Value const* mn = item.get("name");
      mini::Value const* mc = item.get("command");
      if (!mn || mn->type != mini::Value::String || mn->s.empty()) continue;
      if (!mc || mc->type != mini::Value::String || mc->s.empty()) continue;
      McpConfig ms;
      ms.name = mn->s;
      ms.command = mc->s;
      if (mini::Value const* a = item.get("args"))
        if (a->type == mini::Value::Array)
          for (auto const& x : a->arr)
            if (x.type == mini::Value::String) ms.args.push_back(x.s);
      if (mini::Value const* ev = item.get("env"))
        if (ev->type == mini::Value::Object)
          for (auto const& kv : ev->obj)
            if (kv.second.type == mini::Value::String)
              ms.env.emplace_back(kv.first, kv.second.s);
      if (mini::Value const* en = item.get("enabled")) ms.enabled = en->asBool(true);
      cfg.mcpServers.push_back(std::move(ms));
    }
  }
  if ((v = root.get("cost_in_per_m")) && (v->type == mini::Value::Double || v->type == mini::Value::Int))
    cfg.costInPerM = v->asDouble(cfg.costInPerM);
  if ((v = root.get("cost_out_per_m")) && (v->type == mini::Value::Double || v->type == mini::Value::Int))
    cfg.costOutPerM = v->asDouble(cfg.costOutPerM);
  if ((v = root.get("cost_budget_usd")) && (v->type == mini::Value::Double || v->type == mini::Value::Int))
    cfg.costBudgetUsd = v->asDouble(cfg.costBudgetUsd);
  if ((v = root.get("workspaces")) && v->type == mini::Value::Array) {
    cfg.workspaces.clear();
    for (auto const& item : v->arr)
      if (item.type == mini::Value::String && !item.s.empty()) cfg.workspaces.push_back(item.s);
  }
  if ((v = root.get("active_workspace")) && v->type == mini::Value::Int)
    cfg.activeWorkspace = (int)v->i;
  if ((v = root.get("workspace_guard")) && (v->type == mini::Value::Bool || v->type == mini::Value::Int))
    cfg.workspaceGuard = v->asBool(cfg.workspaceGuard);
  cfg.defaultBaseUrl = cfg.baseUrl;
  cfg.defaultApiKey = cfg.apiKey;
  return true;
}

bool saveSettingsFile(std::string const& path, AppConfig const& cfg) {
  mini::Value root = mini::Value::makeObject();
  // The top-level connection in the file is the *default* connection: when a
  // grouped model switched the active baseUrl/apiKey, persist the pristine
  // values instead of the transient ones.
  root.set("base_url", mini::Value::makeString(
             cfg.defaultBaseUrl.empty() ? cfg.baseUrl : cfg.defaultBaseUrl));
  if (!cfg.apiKeyEnv.empty()) {
    root.set("api_key_env", mini::Value::makeString(cfg.apiKeyEnv));
  } else if (!cfg.defaultApiKey.empty()) {
    root.set("api_key", mini::Value::makeString(cfg.defaultApiKey));
  } else if (cfg.defaultBaseUrl.empty() || cfg.baseUrl == cfg.defaultBaseUrl) {
    if (!cfg.apiKey.empty()) root.set("api_key", mini::Value::makeString(cfg.apiKey));
  }
  root.set("model", mini::Value::makeString(cfg.model));
  if (!cfg.models.empty()) {
    mini::Value ma = mini::Value::makeArray();
    for (auto const& m : cfg.models) ma.arr.push_back(mini::Value::makeString(m));
    root.set("models", std::move(ma));
  }
  if (!cfg.providers.empty()) {
    mini::Value arr = mini::Value::makeArray();
    for (auto const& p : cfg.providers) {
      mini::Value o = mini::Value::makeObject();
      o.set("name", mini::Value::makeString(p.name));
      o.set("base_url", mini::Value::makeString(p.baseUrl));
      if (!p.apiKeyEnv.empty())
        o.set("api_key_env", mini::Value::makeString(p.apiKeyEnv));
      else if (!p.apiKey.empty())
        o.set("api_key", mini::Value::makeString(p.apiKey));
      if (!p.thinkingStyle.empty())
        o.set("thinking_style", mini::Value::makeString(p.thinkingStyle));
      arr.arr.push_back(std::move(o));
    }
    root.set("providers", std::move(arr));
  }
  if (!cfg.groups.empty()) {
    mini::Value arr = mini::Value::makeArray();
    for (auto const& g : cfg.groups) {
      mini::Value o = mini::Value::makeObject();
      o.set("name", mini::Value::makeString(g.name));
      o.set("provider", mini::Value::makeString(g.provider));
      mini::Value ma = mini::Value::makeArray();
      for (auto const& m : g.models) ma.arr.push_back(mini::Value::makeString(m));
      o.set("models", std::move(ma));
      arr.arr.push_back(std::move(o));
    }
    root.set("groups", std::move(arr));
  }
  root.set("system_prompt", mini::Value::makeString(cfg.systemPrompt));
  root.set("theme", mini::Value::makeString(cfg.theme));
  root.set("mode", mini::Value::makeString(cfg.mode));
  root.set("thinking", mini::Value::makeString(thinkingLevelFromString(cfg.thinking)));
  if (!cfg.thinkingStyle.empty())
    root.set("thinking_style", mini::Value::makeString(cfg.thinkingStyle));
  if (cfg.themeOverride.active) {
    mini::Value tc = mini::Value::makeObject();
    auto wr = [&](char const* k, unsigned const arr[3]) {
      mini::Value a = mini::Value::makeArray();
      for (int i = 0; i < 3; i++)
        a.arr.push_back(mini::Value::makeInt((int64_t)arr[i]));
      tc.set(k, std::move(a));
    };
    wr("green", cfg.themeOverride.green);
    wr("gold", cfg.themeOverride.gold);
    wr("blue", cfg.themeOverride.blue);
    wr("purple", cfg.themeOverride.purple);
    wr("red", cfg.themeOverride.red);
    wr("fg", cfg.themeOverride.fg);
    wr("fg_dim", cfg.themeOverride.fgDim);
    wr("bg", cfg.themeOverride.bg);
    wr("head_bg", cfg.themeOverride.headBg);
    wr("rule", cfg.themeOverride.rule);
    root.set("theme_colors", std::move(tc));
  }
  root.set("budget", mini::Value::makeInt((int64_t)cfg.budgetTokens));
  root.set("tool_result_cap", mini::Value::makeInt((int64_t)cfg.toolResultCap));
  root.set("prune_before_send", mini::Value::makeBool(cfg.pruneBeforeSend));
  if (!cfg.rulesPath.empty()) root.set("rules_path", mini::Value::makeString(cfg.rulesPath));
  if (!cfg.sessionDir.empty()) root.set("session_dir", mini::Value::makeString(cfg.sessionDir));
  root.set("json_mode", mini::Value::makeBool(cfg.jsonMode));
  root.set("shell_enabled", mini::Value::makeBool(cfg.shellEnabled));
  if (!cfg.mcpServers.empty()) {
    mini::Value arr = mini::Value::makeArray();
    for (auto const& ms : cfg.mcpServers) {
      mini::Value o = mini::Value::makeObject();
      o.set("name", mini::Value::makeString(ms.name));
      o.set("command", mini::Value::makeString(ms.command));
      mini::Value a = mini::Value::makeArray();
      for (auto const& x : ms.args) a.arr.push_back(mini::Value::makeString(x));
      o.set("args", std::move(a));
      if (!ms.env.empty()) {
        mini::Value ev = mini::Value::makeObject();
        for (auto const& kv : ms.env) ev.set(kv.first, mini::Value::makeString(kv.second));
        o.set("env", std::move(ev));
      }
      o.set("enabled", mini::Value::makeBool(ms.enabled));
      arr.arr.push_back(std::move(o));
    }
    root.set("mcp_servers", std::move(arr));
  }
  root.set("cost_in_per_m", mini::Value::makeDouble(cfg.costInPerM));
  root.set("cost_out_per_m", mini::Value::makeDouble(cfg.costOutPerM));
  root.set("cost_budget_usd", mini::Value::makeDouble(cfg.costBudgetUsd));
  if (!cfg.workspaces.empty()) {
    mini::Value wa = mini::Value::makeArray();
    for (auto const& w : cfg.workspaces) wa.arr.push_back(mini::Value::makeString(w));
    root.set("workspaces", std::move(wa));
    root.set("active_workspace", mini::Value::makeInt((int64_t)cfg.activeWorkspace));
    root.set("workspace_guard", mini::Value::makeBool(cfg.workspaceGuard));
  }
  return writeFile(path, prettyJson(mini::dump(root)) + "\n");
}

std::vector<tui::Field> buildSettingsFields(AppConfig const& cfg,
                                            ToolRegistry const& tools) {
  std::vector<tui::Field> fields;

  tui::Field f;
  f.label = "base url";
  f.tag = "base_url";
  f.value = cfg.baseUrl;
  f.hint = "OpenAI-compatible API endpoint";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "api key";
  f.tag = "api_key";
  f.kind = tui::FieldKind::Secret;
  f.value = cfg.apiKey;
  f.hint = "stored in settings.json";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "model";
  f.tag = "model";
  f.value = cfg.model;
  fields.push_back(f);

  f = tui::Field{};
  f.label = "model list";
  f.tag = "models";
  {
    std::string joined;
    for (auto const& m : cfg.models) {
      if (!joined.empty()) joined += ", ";
      joined += m;
    }
    f.value = joined;
  }
  f.hint = "comma separated; /model name|N, Tab completes";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "workspaces";
  f.tag = "workspaces";
  {
    std::string joined;
    for (auto const& w : cfg.workspaces) {
      if (!joined.empty()) joined += "; ";
      joined += w;
    }
    f.value = joined;
  }
  f.hint = "dir roots, ';'-separated; empty = no boundary";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "workspace guard";
  f.tag = "workspace_guard";
  f.kind = tui::FieldKind::Toggle;
  f.value = cfg.workspaceGuard ? "1" : "0";
  f.hint = "ask before touching outside roots";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "system prompt";
  f.tag = "system_prompt";
  f.value = cfg.systemPrompt;
  f.hint = "one line";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "theme";
  f.tag = "theme";
  f.value = cfg.theme;
  f.hint = "dark | light | terminal | nord | gruvbox | dracula | solarized";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "mode";
  f.tag = "mode";
  f.value = cfg.mode;
  f.hint = "standard | minimal | ptc | creator";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "thinking";
  f.tag = "thinking";
  f.value = thinkingLevelFromString(cfg.thinking);
  f.hint = "auto | none | minimal | low | medium | high";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "session folder";
  f.tag = "session_dir";
  f.value = cfg.sessionDir;
  f.hint = "auto-save dir; empty = %LOCALAPPDATA%\\Ember\\sessions";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "budget tokens";
  f.tag = "budget";
  f.kind = tui::FieldKind::Int;
  f.value = std::to_string(cfg.budgetTokens);
  f.hint = "0 = disabled";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "tool result cap";
  f.tag = "tool_result_cap";
  f.kind = tui::FieldKind::Int;
  f.value = std::to_string(cfg.toolResultCap);
  f.hint = "bytes; 0 = unlimited";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "prune before send";
  f.tag = "prune_before_send";
  f.kind = tui::FieldKind::Toggle;
  f.value = cfg.pruneBeforeSend ? "1" : "0";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "json mode";
  f.tag = "json_mode";
  f.kind = tui::FieldKind::Toggle;
  f.value = cfg.jsonMode ? "1" : "0";
  fields.push_back(f);

  f = tui::Field{};
  f.label = "shell tool";
  f.tag = "shell_enabled";
  f.kind = tui::FieldKind::Toggle;
  f.value = cfg.shellEnabled ? "1" : "0";
  fields.push_back(f);

  auto costField = [&](std::string label, std::string tag, double v, char const* hint) {
    tui::Field cf;
    cf.label = std::move(label);
    cf.tag = std::move(tag);
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", v);
    cf.value = buf;
    cf.hint = hint;
    fields.push_back(cf);
  };
  costField("cost in $/M", "cost_in_per_m", cfg.costInPerM,
            "USD per 1M input tokens");
  costField("cost out $/M", "cost_out_per_m", cfg.costOutPerM,
            "USD per 1M output tokens");
  costField("cost budget $", "cost_budget_usd", cfg.costBudgetUsd,
            "0 = no budget");

  for (auto const& name : tools.names()) {
    Tool const* t = tools.find(name);
    tui::Field tf;
    tf.label = name;
    tf.tag = "tool:" + name;
    tf.kind = tui::FieldKind::Toggle;
    tf.value = (t && t->enabled) ? "1" : "0";
    fields.push_back(tf);
  }
  return fields;
}

void applySettingsFields(AppConfig& cfg, std::vector<tui::Field> const& fields,
                         ToolRegistry& tools) {
  for (auto const& f : fields) {
    if (f.tag == "base_url") {
      cfg.baseUrl = f.value;
      cfg.defaultBaseUrl = f.value;  // editing the form edits the default connection
    } else if (f.tag == "api_key") {
      // If the user edits the key directly, stop resolving it from the
      // api_key_env variable so the new value is what actually takes effect
      // (and gets persisted as a literal key).
      bool changed = (f.value != cfg.apiKey);
      cfg.apiKey = f.value;
      cfg.defaultApiKey = f.value;
      if (changed && !cfg.apiKeyEnv.empty()) cfg.apiKeyEnv.clear();
    } else if (f.tag == "model") {
      cfg.model = f.value;
    } else if (f.tag == "models") {
      std::vector<std::string> list;
      size_t start = 0;
      while (start <= f.value.size()) {
        size_t comma = f.value.find(',', start);
        std::string item = comma == std::string::npos
                               ? f.value.substr(start)
                               : f.value.substr(start, comma - start);
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b != std::string::npos) list.push_back(item.substr(b, e - b + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
      cfg.models = std::move(list);
    } else if (f.tag == "workspaces") {
      std::vector<std::string> list;
      size_t start = 0;
      while (start <= f.value.size()) {
        size_t sep = f.value.find(';', start);
        std::string item = sep == std::string::npos
                               ? f.value.substr(start)
                               : f.value.substr(start, sep - start);
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b != std::string::npos) list.push_back(item.substr(b, e - b + 1));
        if (sep == std::string::npos) break;
        start = sep + 1;
      }
      cfg.workspaces = std::move(list);
      if (cfg.activeWorkspace >= (int)cfg.workspaces.size()) cfg.activeWorkspace = 0;
    } else if (f.tag == "workspace_guard") {
      cfg.workspaceGuard = (f.value == "1");
    } else if (f.tag == "system_prompt") {
      cfg.systemPrompt = f.value;
    } else if (f.tag == "theme") {
      cfg.theme = f.value;
    } else if (f.tag == "mode") {
      cfg.mode = modeToString(modeFromString(f.value));  // normalize unknown -> standard
    } else if (f.tag == "thinking") {
      cfg.thinking = thinkingLevelFromString(f.value);  // normalize unknown -> auto
    } else if (f.tag == "budget") {
      char* end = nullptr;
      long long n = std::strtoll(f.value.c_str(), &end, 10);
      if (end && *end == '\0' && n >= 0) cfg.budgetTokens = (size_t)n;
    } else if (f.tag == "tool_result_cap") {
      char* end = nullptr;
      long long n = std::strtoll(f.value.c_str(), &end, 10);
      if (end && *end == '\0' && n >= 0) cfg.toolResultCap = (size_t)n;
    } else if (f.tag == "prune_before_send") {
      cfg.pruneBeforeSend = (f.value == "1");
    } else if (f.tag == "json_mode") {
      cfg.jsonMode = (f.value == "1");
    } else if (f.tag == "shell_enabled") {
      cfg.shellEnabled = (f.value == "1");
    } else if (f.tag == "cost_in_per_m" || f.tag == "cost_out_per_m" ||
               f.tag == "cost_budget_usd") {
      char* end = nullptr;
      double d = std::strtod(f.value.c_str(), &end);
      if (end && *end == '\0' && d >= 0) {
        if (f.tag == "cost_in_per_m") cfg.costInPerM = d;
        else if (f.tag == "cost_out_per_m") cfg.costOutPerM = d;
        else cfg.costBudgetUsd = d;
      }
    } else if (f.tag.rfind("tool:", 0) == 0) {
      std::string name = f.tag.substr(5);
      tools.setEnabled(name, f.value == "1");
    }
  }
}

}  // namespace agent
