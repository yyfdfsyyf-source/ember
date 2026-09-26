// One end-to-end "task" exercising every built-in tool in one story:
//
//   Prepare  -> shell_exec mkdir-ish echo, file_write, file_read, edit, patch, file_list
//   Fetch    -> web_fetch (served by the local mini HTTP server below)
//   Browse   -> browser_open/snapshot/click/type/upload/key/scroll/wait/screenshot/
//               reload/eval/back/forward/close against the same page
//
// If no Chrome/Edge binary can be found the browser section is skipped
// (other tools still run).
#ifndef _WIN32
#error test_alltools.cpp is Windows-only (needs winsock + headless browser)
#endif

#include "agent/tools.hpp"
#include "agent/browser.hpp"
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <atomic>

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

// JSON-escape a string for embedding into tool args.
static std::string esc(std::string const& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\\': o += "\\\\"; break;
      case '"':  o += "\\\""; break;
      default:   o += c;
    }
  }
  return o;
}

// ---------------------------------------------------------------------------
// Mini static HTTP server on 127.0.0.1:<ephemeral port>. Serves the test page
// and answers POST /submit with a small JSON body.
// ---------------------------------------------------------------------------

static char const* kPageHtml =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>Tool Test Page</title></head><body>"
    "<h1>All Tools Page</h1>"
    "<form id=\"f\">"
    "<input id=\"name\" type=\"text\" placeholder=\"Your name\">"
    "<input id=\"file\" type=\"file\">"
    "<button id=\"submit\" type=\"submit\">Submit</button>"
    "</form>"
    "<p id=\"out\"></p>"
    "<script>"
    "document.getElementById('f').addEventListener('submit',function(ev){"
    "ev.preventDefault();"
    "var name=document.getElementById('name').value;"
    "var f=document.getElementById('file').files[0];"
    "document.getElementById('out').textContent="
    "'received '+name+(f?' file='+f.name:'');});"
    "</script></body></html>";

struct MiniServer {
  SOCKET srv = INVALID_SOCKET;
  std::atomic<bool> stop{false};
  std::thread th;
  uint16_t port = 0;

  MiniServer() {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return;
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(srv, (sockaddr*)&a, sizeof a) == SOCKET_ERROR) return;
    listen(srv, 8);
    sockaddr_in got{};
    int gl = sizeof got;
    getsockname(srv, (sockaddr*)&got, &gl);
    port = ntohs(got.sin_port);
    th = std::thread([this] { loop(); });
  }

  ~MiniServer() {
    stop = true;
    // Windows does not reliably wake a blocked accept() by closing the
    // listening socket from another thread; poke it with a self-connect.
    SOCKET w = socket(AF_INET, SOCK_STREAM, 0);
    if (w != INVALID_SOCKET) {
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(port);
      a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      connect(w, (sockaddr*)&a, sizeof a);
      closesocket(w);
    }
    if (srv != INVALID_SOCKET) closesocket(srv);
    if (th.joinable()) th.join();
    WSACleanup();
  }

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port) + "/";
  }

  void loop() {
    while (!stop) {
      SOCKET c = accept(srv, nullptr, nullptr);
      if (c == INVALID_SOCKET) break;
      std::string req;
      char buf[4096];
      while (req.find("\r\n\r\n") == std::string::npos) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, (size_t)n);
      }
      bool post = req.rfind("POST ", 0) == 0;
      if (post) {
        size_t clPos = req.find("Content-Length:");
        if (clPos != std::string::npos) {
          size_t len = (size_t)std::strtoul(req.c_str() + clPos + 15, nullptr, 10);
          while (req.size() < req.find("\r\n\r\n") + 4 + len) {
            int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            req.append(buf, (size_t)n);
          }
        }
      }
      static char const* jsonBody = "{\"received\":true}";
      std::string resp;
      if (post) {
        resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
               "Content-Length: 17\r\nConnection: close\r\n\r\n" + std::string(jsonBody);
      } else {
        resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
               "Content-Length: " + std::to_string(std::strlen(kPageHtml)) +
               "\r\nConnection: close\r\n\r\n" + kPageHtml;
      }
      send(c, resp.data(), (int)resp.size(), 0);
      closesocket(c);
    }
  }
};

int main() {
  MiniServer srv;

  // ==================================================================
  // PART 1 - prepare files with the file/shell toolset
  // ==================================================================

  // shell_exec: verify the shell runs at all.
  std::string r = agent::toolShellExec("{\"command\":\"echo task-start\"}");
  CHECK(has(r, "\"output\""));
  CHECK(has(r, "task-start"));

  // file_write: create a small text file.
  std::string file = "test_alltools.txt";
  std::string content = "one\ntwo\nthree\n";
  r = agent::toolFileWrite("{\"path\":\"" + file + "\",\"content\":\"" + esc(content) + "\"}");
  CHECK(has(r, "\"ok\":true"));

  // file_read: read it back.
  r = agent::toolFileRead("{\"path\":\"" + file + "\"}");
  CHECK(has(r, "\"total_lines\":3"));
  CHECK(has(r, "two"));

  // edit: small localized replacement.
  r = agent::toolEdit("{\"path\":\"" + file + "\",\"old_string\":\"two\",\"new_string\":\"TWO\"}");
  CHECK(has(r, "\"ok\":true"));

  // patch: multi-line unified diff replacement.
  std::string diff = "--- a/" + file + "\n+++ b/" + file + "\n@@ -1,3 +1,3 @@\n one\n TWO\n-three\n+THREE\n";
  r = agent::toolPatch("{\"patch\":\"" + esc(diff) + "\"}");
  CHECK(has(r, "\"ok\":true"));
  r = agent::toolFileRead("{\"path\":\"" + file + "\"}");
  CHECK(has(r, "THREE"));

  // file_list: find the file we created.
  r = agent::toolFileList("{\"path\":\".\",\"pattern\":\"test_alltools\"}");
  CHECK(has(r, "test_alltools.txt"));

  // ==================================================================
  // PART 1b - interactive helpers (ask_user with a stubbed answer, todowrite)
  // ==================================================================

  // ask_user: without a callback it must fail gracefully.
  r = agent::toolAskUser("{\"question\":\"proceed?\"}");
  CHECK(has(r, "non-interactive"));

  // With a callback installed the tool returns the user's answer.
  agent::setAskUserCallback([](std::string const& q) { return "yes-" + q; });
  r = agent::toolAskUser("{\"question\":\"proceed?\"}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "yes-proceed?"));
  r = agent::toolAskUser("{}");
  CHECK(has(r, "question is required"));

  // subagent: without a callback it must fail gracefully.
  r = agent::toolSubagent("{\"prompt\":\"do the thing\"}");
  CHECK(has(r, "non-interactive"));
  r = agent::toolSubagent("{}");
  CHECK(has(r, "prompt is required"));
  // With a callback installed the tool returns the subagent's answer.
  agent::setSubagentCallback(
      [](std::string const& d, std::string const& p) { return "done:[" + d + "|" + p + "]"; });
  r = agent::toolSubagent("{\"description\":\"quick job\",\"prompt\":\"sum 2+2\"}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "done:[quick job|sum 2+2]"));
  // The tool is registered and advertised by default, but a subagent turn
  // must not see it (subset list excluding "subagent").
  agent::ToolRegistry reg2;
  agent::registerBuiltinTools(reg2);
  CHECK(has(reg2.toolsJson(), "subagent"));
  std::string subOnly = reg2.toolsJson({"subagent"});
  CHECK(has(subOnly, "subagent"));
  CHECK(!has(subOnly, "grep"));

  // todowrite: replaces the whole plan, returns the new list.
  r = agent::toolTodoWrite(
      "{\"todos\":[{\"content\":\"step one\",\"status\":\"in_progress\",\"priority\":\"high\"},"
      "{\"content\":\"step two\"},{\"content\":\"step three\",\"status\":\"completed\"}]}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "step one"));
  CHECK(has(r, "step two"));
  CHECK(has(r, "\"status\":\"completed\""));
  CHECK(has(r, "\"priority\":\"high\""));
  // Progress: mark step one done, drop step three, add a new step.
  r = agent::toolTodoWrite(
      "{\"todos\":[{\"content\":\"step one\",\"status\":\"completed\"},"
      "{\"content\":\"step two\",\"status\":\"in_progress\"},{\"content\":\"step four\"}]}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(!has(r, "step three"));
  CHECK(has(r, "step four"));
  // Invalid status is rejected and the list is left untouched.
  r = agent::toolTodoWrite("{\"todos\":[{\"content\":\"x\",\"status\":\"done\"}]}");
  CHECK(has(r, "invalid status"));
  r = agent::todoListJson();
  CHECK(has(r, "step four"));
  CHECK(!has(r, "step three"));
  // Clearing the plan.
  r = agent::toolTodoWrite("{\"todos\":[]}");
  CHECK(has(r, "\"todos\":[]"));

  // ==================================================================
  // PART 2 - fetch the same content over HTTP (mini local server)
  // ==================================================================

  // web_fetch: raw=true returns the page HTML unchanged.
  r = agent::toolWebFetch("{\"url\":\"" + srv.url() + "\",\"raw\":true}");
  CHECK(has(r, "<title>Tool Test Page"));
  CHECK(has(r, "\"status\":200"));

  // ==================================================================
  // PART 3 - do it all again like a human in the headless browser
  // ==================================================================

  std::string startErr = agent::browserSession().ensureStarted();
  if (!startErr.empty()) {
    std::printf("SKIP browser section (no headless browser: %s)\n", startErr.c_str());
    std::printf("checks=%d failures=%d\n", checks, failures);
    return 0;
  }

  // browser_open: navigate to our page.
  r = agent::toolBrowserOpen("{\"url\":\"" + srv.url() + "\",\"wait_ms\":15000}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "127.0.0.1"));

  // browser_snapshot: outline shows the form elements.
  r = agent::toolBrowserSnapshot("{}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "Tool Test"));
  CHECK(has(r, "input#name"));
  CHECK(has(r, "#submit"));

  // browser_click: focus the name input.
  r = agent::toolBrowserClick("{\"selector\":\"#name\"}");
  CHECK(has(r, "\"ok\":true"));

  // browser_type: type a name.
  r = agent::toolBrowserType("{\"selector\":\"#name\",\"text\":\"Agent\"}");
  CHECK(has(r, "\"ok\":true"));

  // browser_upload: attach the file we edited earlier (one/TWO/THREE).
  r = agent::toolBrowserUpload("{\"selector\":\"#file\",\"file\":\"" + file + "\"}");
  CHECK(has(r, "\"ok\":true"));

  // browser_click: press the submit button; the page JS writes to #out.
  r = agent::toolBrowserClick("{\"selector\":\"#submit\"}");
  CHECK(has(r, "\"ok\":true"));

  // browser_eval: read back what the form produced.
  r = agent::toolBrowserEval(
      "{\"expression\":\"document.getElementById('out').textContent\"}");
  CHECK(has(r, "received Agent"));
  CHECK(has(r, "test_alltools.txt"));

  // browser_key: select-all + retype over the field like a human.
  r = agent::toolBrowserClick("{\"selector\":\"#name\"}");
  CHECK(has(r, "\"ok\":true"));
  r = agent::toolBrowserKey("{\"key\":\"ctrl+a\"}");
  CHECK(has(r, "\"ok\":true"));
  r = agent::toolBrowserType("{\"text\":\"Merge\"}");
  CHECK(has(r, "\"ok\":true"));
  r = agent::toolBrowserClick("{\"selector\":\"#submit\"}");
  CHECK(has(r, "\"ok\":true"));
  r = agent::toolBrowserEval(
      "{\"expression\":\"document.getElementById('out').textContent\"}");
  CHECK(has(r, "received Merge"));

  // browser_scroll: wheel scroll both ways.
  r = agent::toolBrowserScroll("{\"direction\":\"down\",\"amount\":200}");
  CHECK(has(r, "\"ok\":true"));
  r = agent::toolBrowserScroll("{\"direction\":\"up\",\"amount\":200}");
  CHECK(has(r, "\"ok\":true"));

  // browser_wait: pause mid-task.
  r = agent::toolBrowserWait("{\"ms\":150}");
  CHECK(has(r, "\"ok\":true"));

  // browser_screenshot: save a PNG of the page.
  std::string shot = "out/browser_alltools_shot.png";
  r = agent::toolBrowserScreenshot("{\"path\":\"" + shot + "\"}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(std::filesystem::is_regular_file(shot));

  // browser_reload: page state resets.
  r = agent::toolBrowserReload("{}");
  CHECK(has(r, "\"ok\":true"));
  r = agent::toolBrowserEval("{\"expression\":\"document.title\"}");
  CHECK(has(r, "Tool Test Page"));
  r = agent::toolBrowserEval(
      "{\"expression\":\"document.getElementById('out').textContent.length\"}");
  CHECK(has(r, "\"value\":\"0\""));

  // browser_back / browser_forward: history navigation.
  r = agent::toolBrowserBack("{}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "about:blank"));
  r = agent::toolBrowserForward("{}");
  CHECK(has(r, "\"ok\":true"));
  CHECK(has(r, "127.0.0.1"));

  // browser_close: tear the browser down.
  r = agent::toolBrowserClose("{}");
  CHECK(has(r, "\"closed\":true"));
  CHECK(!agent::browserSession().running());

  // Clean up the artifacts the task created.
  std::filesystem::remove(file);
  std::filesystem::remove(shot);

  // ==================================================================
  // PART 5 - git wrappers (git_status / git_diff / git_log / git_commit)
  // ==================================================================
  {
    // Detect git on PATH; skip if absent.
    bool gitOk = false;
    {
      FILE* p = _popen("git --version 2>&1", "r");
      if (p) {
        char buf[128];
        if (fgets(buf, sizeof buf, p) &&
            std::string(buf).find("git version") != std::string::npos)
          gitOk = true;
        _pclose(p);
      }
    }

    // sweep leftovers from older runs (best effort; AV locks may block some)
    {
      std::error_code edec;
      for (auto const& de : std::filesystem::directory_iterator("out", edec)) {
        std::string nm = de.path().filename().string();
        if (nm.rfind("tmp_git_repo", 0) == 0) {
          std::error_code ec0;
          std::filesystem::remove_all(de.path(), ec0);
        }
      }
    }
    // Unique per run AND relative (the project path contains spaces, and the
    // git tool's safeGitArg whitelist would reject absolute paths; git -C
    // resolves from the test CWD = project root). Uniqueness matters: a zombie
    // tmp dir whose .git was half-removed by AV locking makes `git -C` walk up
    // to the PARENT repo (this project's own dir) and silently run checks —
    // and commits — against it.
    std::string tmp = "out/tmp_git_repo_" +
                      std::to_string((long long)std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count());
    // Best-effort cleanup: Windows AV/Defender may briefly lock files left by
    // a previous aborted run, which makes the throwing remove_all terminate
    // the test before any check runs. Swallow and retry with backoff.
    for (int attempt = 0; attempt < 12; attempt++) {
      std::error_code ec0;
      std::filesystem::remove_all(tmp, ec0);
      if (!ec0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
    }
    std::filesystem::create_directories(tmp);

    auto git = [&](std::string const& args) {
      std::string cmd = "git -C \"" + tmp + "\" " + args + " 2>&1";
      FILE* p = _popen(cmd.c_str(), "r");
      std::string o;
      char b[4096];
      if (p) {
        while (fgets(b, sizeof b, p)) o += b;
        _pclose(p);
      }
      return o;
    };

    if (gitOk) {
      // Invalid argument whitelist.
      r = agent::toolGitStatus("{\"dir\":\"" + tmp + ";rm -rf /\",\"d\":1}");
      CHECK(has(r, "invalid 'dir'"));
      r = agent::toolGitDiff("{\"file\":\"a;b\"}");
      CHECK(has(r, "invalid 'file'"));

      git("init");
      if (!std::filesystem::exists(tmp + "/.git")) {
        std::printf("FAIL git init produced no .git under %s "
                    "(skipping to protect the parent repo)\n", tmp.c_str());
        failures++;
      } else {
      git("config user.email test@example.com");
      git("config user.name test");
      git("config commit.gpgsign false");

      std::filesystem::create_directories(tmp + "/src");
      auto write = [&](std::string const& rel, std::string const& c) {
        FILE* f = fopen((tmp + "/" + rel).c_str(), "wb");
        if (f) {
          fputs(c.c_str(), f);
          fclose(f);
        }
      };
      write("src/util.cpp",
            "int helper() { return 1; }\n");  // untracked, not staged

      // status shows the untracked dir (git status --short collapses an
      // untracked directory to "?? src/", so the file name itself does not
      // appear until it is tracked).
      r = agent::toolGitStatus("{\"dir\":\"" + tmp + "\"}");
      CHECK(has(r, "\"ok\":true"));
      CHECK(has(r, "??"));
      CHECK(has(r, "src/"));

      // commit stages all and commits (needs identity).
      r = agent::toolGitCommit(
          "{\"dir\":\"" + tmp + "\",\"message\":\"add helper\\n\\nsecond line\"}");
      CHECK(has(r, "\"ok\":true"));
      CHECK(has(r, "util.cpp"));  // committed file listed

      // fresh status is clean: --short --branch prints the branch line
      // ("## main") with no "??" entries; the literal word "clean" only
      // appears in the long-form status, so assert on the absence of "??".
      r = agent::toolGitStatus("{\"dir\":\"" + tmp + "\"}");
      CHECK(has(r, "\"ok\":true"));
      CHECK(!has(r, "??"));
      CHECK(has(r, "\"rc\":0"));

      // log shows the commit.
      r = agent::toolGitLog("{\"dir\":\"" + tmp + "\",\"n\":5}");
      CHECK(has(r, "helper"));
      CHECK(has(r, "\"rc\":0"));

      // modify a tracked file -> diff shows it.
      write("src/util.cpp", "int helper() { return 2; }\n");
      r = agent::toolGitDiff("{\"dir\":\"" + tmp + "\"}");
      CHECK(has(r, "\"rc\":0"));
      CHECK(has(r, "-int helper() { return 1; }"));
      CHECK(has(r, "+int helper() { return 2; }"));

      // staged variant.
      git("add src/util.cpp");
      r = agent::toolGitDiff("{\"dir\":\"" + tmp + "\",\"staged\":true}");
      CHECK(has(r, "helper"));
      }  // end: validated nested .git present
    }

    // Best-effort cleanup: Windows AV/Defender briefly locks freshly
    // created .git object files, which makes remove_all throw access-denied.
    // Retry a few times and swallow the error so the log line below still
    // reports the real check results.
    for (int attempt = 0; attempt < 5; attempt++) {
      std::error_code ec;
      std::filesystem::remove_all(tmp, ec);
      if (!ec) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  std::printf("checks=%d failures=%d\n", checks, failures);
  std::fflush(stdout);
  // Failures must be reflected in the exit code, or CI/scripts that only look
  // at the exit status would report green while checks actually failed.
  return failures > 0 ? 1 : 0;
}