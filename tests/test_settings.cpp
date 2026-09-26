#include "agent/app.hpp"
#include "agent/settings.hpp"
#include "agent/tools.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

static std::string tempFile(char const* name) {
  char const* tmp = std::getenv("TEMP");
  if (!tmp) tmp = ".";
  return std::string(tmp) + "\\agent_test_" + name;
}

static void test_roundtrip() {
  std::string path = tempFile("settings_rt.json");
  agent::AppConfig cfg;
  cfg.baseUrl = "https://example.com/v1";
  cfg.apiKey = "sk-test";
  cfg.model = "gpt-test";
  cfg.systemPrompt = "hello";
  cfg.budgetTokens = 12345;
  cfg.jsonMode = true;
  cfg.shellEnabled = false;
  cfg.theme = "light";
  cfg.toolResultCap = 2048;
  cfg.pruneBeforeSend = false;
  cfg.rulesPath = "my.rules.md";
  cfg.models = {"alpha", "beta"};
  cfg.workspaces = {"C:/work/one", "D:/work/two"};
  cfg.activeWorkspace = 1;
  cfg.workspaceGuard = false;
  cfg.thinking = "high";
  cfg.thinkingStyle = "enable_thinking";
  agent::ProviderConfig p1, p2;
  p1.name = "svc"; p1.baseUrl = "https://svc.example/v1"; p1.apiKey = "k1";
  p1.thinkingStyle = "thinking";
  p2.name = "local"; p2.baseUrl = "http://localhost:11434/v1";
  p2.thinkingStyle = "chat_template_kwargs";
  cfg.providers.push_back(p1);
  cfg.providers.push_back(p2);
  agent::ModelGroup g1, g2;
  g1.name = "g1"; g1.provider = "svc"; g1.models = {"m-a", "m-b"};
  g2.name = "g2"; g2.provider = ""; g2.models = {"m-c"};
  cfg.groups.push_back(g1);
  cfg.groups.push_back(g2);

  CHECK(agent::saveSettingsFile(path, cfg));
  agent::AppConfig out;
  CHECK(agent::loadSettingsFile(path, out));
  CHECK(out.baseUrl == "https://example.com/v1");
  CHECK(out.apiKey == "sk-test");
  CHECK(out.model == "gpt-test");
  CHECK(out.models.size() == 2 && out.models[0] == "alpha" && out.models[1] == "beta");
  CHECK(out.workspaces.size() == 2 && out.workspaces[1] == "D:/work/two");
  CHECK(out.activeWorkspace == 1);
  CHECK(out.workspaceGuard == false);
  CHECK(out.providers.size() == 2);
  CHECK(out.providers.size() == 2 && out.providers[0].name == "svc" &&
        out.providers[0].baseUrl == "https://svc.example/v1" &&
        out.providers[0].apiKey == "k1" && out.providers[1].apiKey.empty());
  CHECK(out.groups.size() == 2);
  CHECK(out.groups.size() == 2 && out.groups[0].name == "g1" &&
        out.groups[0].provider == "svc" && out.groups[0].models.size() == 2 &&
        out.groups[1].provider.empty() && out.groups[1].models.size() == 1);
  CHECK(out.defaultBaseUrl == "https://example.com/v1");
  CHECK(out.defaultApiKey == "sk-test");
  CHECK(out.systemPrompt == "hello");
  CHECK(out.budgetTokens == 12345);
  CHECK(out.jsonMode == true);
  CHECK(out.shellEnabled == false);
  CHECK(out.theme == "light");
  CHECK(out.toolResultCap == 2048);
  CHECK(out.pruneBeforeSend == false);
  CHECK(out.rulesPath == "my.rules.md");
  CHECK(out.thinking == "high");
  CHECK(out.thinkingStyle == "enable_thinking");
  CHECK(out.providers[0].thinkingStyle == "thinking");
  CHECK(out.providers[1].thinkingStyle == "chat_template_kwargs");

  // missing file: defaults preserved, no crash
  agent::AppConfig def;
  CHECK(agent::loadSettingsFile(path + ".missing", def) == false);
  CHECK(def.model == "llama3.2");

  std::filesystem::remove(path);

  // Junk thinking values collapse to the safe defaults (auto / "effort").
  std::string junk = tempFile("settings_thinking.json");
  {
    std::ofstream w(junk, std::ios::binary);
    w << "{\"thinking\":\"ultra\",\"thinking_style\":\"magic\",\"providers\":[{\"name\":"
         "\"p\",\"base_url\":\"http://x/v1\",\"thinking_style\":\"nope\"}]}";
  }
  agent::AppConfig j;
  CHECK(agent::loadSettingsFile(junk, j));
  CHECK(j.thinking == "auto");
  CHECK(j.thinkingStyle.empty());
  CHECK(j.providers.size() == 1 && j.providers[0].thinkingStyle.empty());
  std::filesystem::remove(junk);
}

static void test_fields() {
  agent::AppConfig cfg;
  cfg.model = "m1";
  cfg.jsonMode = true;
  agent::ToolRegistry tools;
  agent::registerBuiltinTools(tools);

  auto fields = agent::buildSettingsFields(cfg, tools);
  CHECK(!fields.empty());

  // find the tool toggle for shell_exec and flip it off
  std::string target = "tool:shell_exec";
  bool found = false;
  for (auto& f : fields) {
    if (f.tag == target) {
      found = true;
      CHECK(f.value == "1");
      f.value = "0";
    }
  }
  CHECK(found);

  // toggle json mode field
  for (auto& f : fields) {
    if (f.tag == "json_mode") f.value = "0";
    if (f.tag == "model") f.value = "m2";
    if (f.tag == "models") f.value = "p, q , , r";
    if (f.tag == "workspaces") f.value = "x; y ; ; z";
    if (f.tag == "workspace_guard") f.value = "1";
    if (f.tag == "thinking") f.value = "medium";
  }

  agent::applySettingsFields(cfg, fields, tools);
  CHECK(cfg.model == "m2");
  CHECK(cfg.jsonMode == false);
  CHECK(cfg.thinking == "medium");
  CHECK(cfg.workspaces.size() == 3);
  CHECK(cfg.workspaces.size() == 3 && cfg.workspaces[0] == "x" &&
        cfg.workspaces[1] == "y" && cfg.workspaces[2] == "z");
  CHECK(cfg.workspaceGuard == true);
  CHECK(cfg.models.size() == 3);
  CHECK(cfg.models.size() == 3 && cfg.models[0] == "p" && cfg.models[1] == "q" &&
        cfg.models[2] == "r");
  auto* shell = tools.find("shell_exec");
  CHECK(shell != nullptr);
  CHECK(shell->enabled == false);
  auto* readTool = tools.find("file_read");
  CHECK(readTool != nullptr && readTool->enabled == true);
}

int main() {
  test_roundtrip();
  test_fields();
  if (failures == 0) {
    std::printf("test_settings: all %d checks passed\n", checks);
    return 0;
  }
  std::printf("test_settings: %d/%d FAILED\n", failures, checks);
  return 1;
}
