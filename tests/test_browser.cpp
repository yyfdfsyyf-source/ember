// End-to-end tests for the headless browser tools. These launch a real
// Chrome/Edge headlessly; if no browser binary is found the suite prints
// "SKIP" and exits 0 (e.g. in CI machines without a browser).
#include "agent/tools.hpp"
#include "agent/browser.hpp"
#include <cstdio>
#include <filesystem>
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

static bool has(std::string const& hay, std::string const& needle) {
  return hay.find(needle) != std::string::npos;
}

// Percent-encode the chars that are awkward in a data: URL.
static std::string pct(std::string const& s) {
  std::string out;
  for (char c : s) {
    if (c == ' ' || c == '#' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '&' ||
        c == '%') {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", (unsigned char)c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

int main() {
  agent::BrowserSession& bs = agent::browserSession();
  std::string startErr = bs.ensureStarted();
  if (!startErr.empty()) {
    // No usable browser: nothing to verify.
    std::printf("SKIP (no headless browser: %s)\n", startErr.c_str());
    std::printf("checks=%d failures=%d\n", checks, failures);
    return 0;
  }

  std::string html = "<button id=\"b\">Press me</button>"
                     "<input id=\"i\" type=\"text\">"
                     "<p id=\"o\"></p>";
  std::string url = "data:text/html," + pct(html);

  std::string r = agent::toolBrowserOpen("{\"url\":\"" + url + "\",\"wait_ms\":10000}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "data:text/html"));

  r = agent::toolBrowserEval("{\"expression\":\"21*2\"}");
  CHECK(has(r, "\"value\":\"42\""));

  r = agent::toolBrowserEval("{\"expression\":"
                             "\"document.getElementById('b').onclick=()=>{"
                             "document.getElementById('o').textContent='clicked'};'ok'\"}");
  CHECK(has(r, "\"ok\":true"));

  r = agent::toolBrowserClick("{\"selector\":\"#b\"}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "\"target\":\"button#b"));

  r = agent::toolBrowserEval("{\"expression\":\"document.getElementById('o').textContent\"}");
  CHECK(has(r, "clicked"));

  r = agent::toolBrowserType("{\"selector\":\"#i\",\"text\":\"h\xC3\xA9llo\"}");
  CHECK(has(r, "\"ok\":true"));
  r = agent::toolBrowserEval("{\"expression\":\"document.getElementById('i').value\"}");
  CHECK(has(r, "h\xC3\xA9llo"));

  r = agent::toolBrowserKey("{\"key\":\"ctrl+a\"}");
  CHECK(has(r, "\"ok\":true"));

  r = agent::toolBrowserSnapshot("{}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "Press me"));
  CHECK(has(r, "input#i"));

  // History: Chrome seeds the initial about:blank entry, so back lands there
  // and forward returns to the test page.
  r = agent::toolBrowserBack("{}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "about:blank"));
  r = agent::toolBrowserForward("{}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "data:text/html"));

  r = agent::toolBrowserScreenshot("{}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "\"path\":\"browser_screenshots/"));
  std::size_t q = r.find("\"path\":\"");
  if (q != std::string::npos) {
    std::string path = r.substr(q + 8, r.find('"', q + 9) - (q + 8));
    CHECK(std::filesystem::is_regular_file(path));
    std::filesystem::remove(path);
  }

  r = agent::toolBrowserClose("{}");
  CHECK(has(r, "\"closed\":true"));
  CHECK(!bs.running());

  std::printf("checks=%d failures=%d\n", checks, failures);
  return 0;
}
