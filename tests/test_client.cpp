// End-to-end client test against a local mock HTTP server (WinSock).
// Verifies SSE streaming, delta accumulation, and tool-call parsing.
#include "agent/client.hpp"
#include "agent/context.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
// The mock server below uses the Winsock spellings; map them onto BSD sockets.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket(s) ::close(s)
#endif

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

static std::string g_response;
static std::vector<std::string> g_responses;  // if non-empty, served in order per connection
static int g_respIdx = 0;
static bool g_served = false;
static std::string g_requestBody;

static void serveOnce(int sock) {
  // read request headers + body
  std::string req;
  char buf[8192];
  int total = 0;
  while (total < (int)sizeof(buf)) {
    int n = (int)recv(sock, buf, (int)sizeof(buf), 0);
    if (n <= 0) break;
    req.append(buf, n);
    total += n;
    if (req.find("\r\n\r\n") != std::string::npos) break;
  }
  int clen = 0;
  {
    size_t h = req.find("Content-Length:");
    if (h != std::string::npos) {
      size_t e = req.find("\r\n", h);
      clen = atoi(req.substr(h + 15, e - (h + 15)).c_str());
    }
  }
  while ((int)req.size() < (int)req.find("\r\n\r\n") + 4 + clen) {
    int n = (int)recv(sock, buf, (int)sizeof(buf), 0);
    if (n <= 0) break;
    req.append(buf, n);
  }
  size_t hb = req.find("\r\n\r\n");
  g_requestBody = (hb == std::string::npos) ? "" : req.substr(hb + 4);
  std::string body;
  if (!g_responses.empty()) {
    int idx = g_respIdx;
    if (idx >= (int)g_responses.size()) idx = (int)g_responses.size() - 1;
    g_respIdx++;
    body = g_responses[idx];
  } else {
    body = g_response;
  }
  std::string resp = "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/event-stream\r\n"
                     "Connection: close\r\n\r\n" + body;
  send(sock, resp.data(), (int)resp.size(), 0);
  g_served = true;
  closesocket(sock);
}

int main() {
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

  // --- streaming SSE with content + a tool call -----------------------------
  g_response =
      "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"}}]}\n\n"
      "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"}}]}\n\n"
      "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\" world\"}}]}\n\n"
      "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"get_time\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
      "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"tz\\\":\"}}]},\"finish_reason\":null}]}\n\n"
      "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"utc\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
      "data: [DONE]\n\n";
  g_served = false;

  int ls = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  CHECK(bind(ls, (sockaddr*)&addr, sizeof(addr)) == 0);
  socklen_t alen = sizeof(addr);
  getsockname(ls, (sockaddr*)&addr, &alen);
  int port = ntohs(addr.sin_port);
  listen(ls, 1);
  std::thread server([&] {
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int cs = accept(ls, (sockaddr*)&caddr, &clen);
    if (cs >= 0) serveOnce(cs);
  });

  agent::Client client;
  client.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
  agent::ChatOptions opts;
  opts.model = "test-model";
  opts.stream = true;

  std::vector<agent::Message> msgs;
  msgs.push_back({"user", "what time is it?"});

  std::string deltaAcc;
  std::string toolName, toolArgs;
  int rc = client.chat(msgs, opts, [&](agent::StreamEvent const& e) {
    if (e.kind == agent::StreamKind::Delta) deltaAcc += e.text;
    if (e.kind == agent::StreamKind::ToolCall) { toolName = e.toolName; toolArgs = e.toolArgs; }
  });
  server.join();
  closesocket(ls);

  CHECK(rc == 0);
  CHECK(g_served);
  CHECK(deltaAcc == "Hello world");
  CHECK(g_requestBody.find("\"stream\":true") != std::string::npos);
  CHECK(g_requestBody.find("\"model\":\"test-model\"") != std::string::npos);
  CHECK(toolName == "get_time");
  CHECK(toolArgs == "{\"tz\":\"utc\"}");
  CHECK(!msgs.empty() && msgs.back().role == "assistant");
  CHECK(msgs.back().toolCalls.size() == 1);
  CHECK(msgs.back().toolCalls[0].name == "get_time");
  CHECK(msgs.back().toolCalls[0].arguments == "{\"tz\":\"utc\"}");
  CHECK(msgs.back().content == "Hello world");

  // --- streaming reasoning_content + content --------------------------------
  g_response =
      "data: {\"id\":\"c3\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"Let me think\"}}]}\n\n"
      "data: {\"id\":\"c3\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\" about it\"}}]}\n\n"
      "data: {\"id\":\"c3\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Answer\"}}]}\n\n"
      "data: [DONE]\n\n";
  g_served = false;

  ls = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addrA{};
  addrA.sin_family = AF_INET;
  addrA.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addrA.sin_port = 0;
  CHECK(bind(ls, (sockaddr*)&addrA, sizeof(addrA)) == 0);
  socklen_t alenA = sizeof(addrA);
  getsockname(ls, (sockaddr*)&addrA, &alenA);
  int portA = ntohs(addrA.sin_port);
  listen(ls, 1);
  std::thread serverA([&] {
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int cs = accept(ls, (sockaddr*)&caddr, &clen);
    if (cs >= 0) serveOnce(cs);
  });

  agent::Client clientA;
  clientA.configure("http://127.0.0.1:" + std::to_string(portA) + "/v1", "");
  agent::ChatOptions optsA;
  optsA.model = "test-model";
  optsA.stream = true;

  std::vector<agent::Message> msgsA;
  msgsA.push_back({"user", "think then answer"});

  std::string reasoningAccA, deltaAccA;
  int rcountA = 0;
  rc = clientA.chat(msgsA, optsA, [&](agent::StreamEvent const& e) {
    if (e.kind == agent::StreamKind::Reasoning) { reasoningAccA += e.text; rcountA++; }
    if (e.kind == agent::StreamKind::Delta) deltaAccA += e.text;
  });
  serverA.join();
  closesocket(ls);

  CHECK(rc == 0);
  CHECK(g_served);
  CHECK(rcountA == 2);
  CHECK(reasoningAccA == "Let me think about it");
  CHECK(deltaAccA == "Answer");
  CHECK(msgsA.back().content == "Answer");  // reasoning must NOT leak into content

  // --- non-streaming JSON with content --------------------------------------
  g_response =
      "{\"id\":\"c2\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"plain answer\"},\"finish_reason\":\"stop\"}]}";
  g_served = false;
  ls = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = 0;
  bind(ls, (sockaddr*)&addr, sizeof(addr));
  alen = sizeof(addr);
  getsockname(ls, (sockaddr*)&addr, &alen);
  port = ntohs(addr.sin_port);
  listen(ls, 1);
  std::thread server2([&] {
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int cs = accept(ls, (sockaddr*)&caddr, &clen);
    if (cs >= 0) serveOnce(cs);
  });

  agent::Client client2;
  client2.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
  agent::ChatOptions opts2;
  opts2.model = "test-model";
  opts2.stream = false;
  std::vector<agent::Message> msgs2;
  msgs2.push_back({"user", "hi"});
  rc = client2.chat(msgs2, opts2, [](agent::StreamEvent const&) {});
  server2.join();
  closesocket(ls);

  CHECK(rc == 0);
  CHECK(msgs2.back().content == "plain answer");
  CHECK(msgs2.back().toolCalls.empty());

  // --- non-streaming message with reasoning_content --------------------------
  g_response =
      "{\"id\":\"c5\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"final\",\"reasoning_content\":\"hidden thinking\"},\"finish_reason\":\"stop\"}]}";
  g_served = false;
  ls = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = 0;
  bind(ls, (sockaddr*)&addr, sizeof(addr));
  alen = sizeof(addr);
  getsockname(ls, (sockaddr*)&addr, &alen);
  port = ntohs(addr.sin_port);
  listen(ls, 1);
  std::thread serverB([&] {
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int cs = accept(ls, (sockaddr*)&caddr, &clen);
    if (cs >= 0) serveOnce(cs);
  });

  agent::Client clientB;
  clientB.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
  agent::ChatOptions optsB;
  optsB.model = "test-model";
  optsB.stream = false;
  std::vector<agent::Message> msgsB;
  msgsB.push_back({"user", "hi"});
  std::string reasoningB;
  int rcountB = 0;
  rc = clientB.chat(msgsB, optsB, [&](agent::StreamEvent const& e) {
    if (e.kind == agent::StreamKind::Reasoning) { reasoningB += e.text; rcountB++; }
  });
  serverB.join();
  closesocket(ls);

  CHECK(rc == 0);
  CHECK(rcountB == 1);
  CHECK(reasoningB == "hidden thinking");
  CHECK(msgsB.back().content == "final");  // reasoning must NOT leak into content

  // --- tools JSON embedded in request body -----------------------------------
  g_response =
      "{\"id\":\"c4\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}";
  g_served = false;
  ls = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = 0;
  bind(ls, (sockaddr*)&addr, sizeof(addr));
  alen = sizeof(addr);
  getsockname(ls, (sockaddr*)&addr, &alen);
  port = ntohs(addr.sin_port);
  listen(ls, 1);
  std::thread server4([&] {
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int cs = accept(ls, (sockaddr*)&caddr, &clen);
    if (cs >= 0) serveOnce(cs);
  });

  agent::Client client4;
  client4.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
  agent::ChatOptions opts4;
  opts4.model = "test-model";
  opts4.stream = false;
  opts4.toolsJson = "[{\"type\":\"function\",\"function\":{\"name\":\"shell_exec\",\"description\":\"run\",\"parameters\":{\"type\":\"object\"}}}]";
  std::vector<agent::Message> msgs4;
  msgs4.push_back({"user", "hi"});
  rc = client4.chat(msgs4, opts4, [](agent::StreamEvent const&) {});
  server4.join();
  closesocket(ls);

  CHECK(rc == 0);
  CHECK(g_requestBody.find("\"tools\":[{\"type\":\"function\"") != std::string::npos);
  CHECK(g_requestBody.find("shell_exec") != std::string::npos);

  // --- runTurn with tool executor --------------------------------------------
  g_response =
      "{\"id\":\"c3\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"t1\",\"type\":\"function\",\"function\":{\"name\":\"add\",\"arguments\":\"{\\\"a\\\":1,\\\"b\\\":2}\"}}]},\"finish_reason\":\"tool_calls\"}]}";
  g_served = false;
  ls = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = 0;
  bind(ls, (sockaddr*)&addr, sizeof(addr));
  alen = sizeof(addr);
  getsockname(ls, (sockaddr*)&addr, &alen);
  port = ntohs(addr.sin_port);
  listen(ls, 1);
  std::thread server3([&] {
    // The mock serves the same tool-call body every time, so runTurn burns its
    // budget (1 loop) plus the auto-conclude retries (3) = 4 chat calls total.
    for (int i = 0; i < 4; i++) {
      sockaddr_in caddr{};
      socklen_t clen = sizeof(caddr);
      int cs = accept(ls, (sockaddr*)&caddr, &clen);
      if (cs < 0) break;
      serveOnce(cs);
    }
  });

  agent::Client client3;
  client3.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
  agent::ChatOptions opts3;
  opts3.model = "test-model";
  opts3.stream = false;
  std::vector<agent::Message> msgs3;
  msgs3.push_back({"user", "add 1 and 2"});
  int toolRuns = 0;
  // The mock serves the same body every time, so the model "keeps calling
  // tools"; the loop cap plus the auto-conclude retries must end with -2.
  rc = runTurn(client3, msgs3, opts3,
               [&](std::string const&, std::string const&) {
                 toolRuns++;
                 return "3";
               },
               [](agent::StreamEvent const&) {}, 1);
  closesocket(ls);  // unblock a possibly-still-waiting accept()
  server3.join();
  CHECK(rc == -2);  // tool loop limited even after the conclude retries
  CHECK(toolRuns >= 1);

  // --- runTurnStructured: invalid JSON then valid JSON ------------------------
  g_responses = {
      "{\"id\":\"c5\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"oops this is not json\"},\"finish_reason\":\"stop\"}]}",
      "{\"id\":\"c6\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"ok\\\":true}\"},\"finish_reason\":\"stop\"}]}"};
  g_respIdx = 0;
  g_served = false;
  ls = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = 0;
  bind(ls, (sockaddr*)&addr, sizeof(addr));
  alen = sizeof(addr);
  getsockname(ls, (sockaddr*)&addr, &alen);
  port = ntohs(addr.sin_port);
  listen(ls, 2);
  std::thread server5([&] {
    for (int i = 0; i < 2; i++) {
      sockaddr_in caddr{};
      socklen_t clen = sizeof(caddr);
      int cs = accept(ls, (sockaddr*)&caddr, &clen);
      if (cs >= 0) serveOnce(cs);
    }
  });

  agent::Client client5;
  client5.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
  agent::ChatOptions opts5;
  opts5.model = "test-model";
  opts5.stream = false;

  std::vector<agent::Message> msgs5;
  msgs5.push_back({"user", "give me json"});

  // Attempt 1 gets invalid JSON -> retry; attempt 2 gets valid JSON.
  rc = runTurnStructured(
      client5, msgs5, opts5,
      [](std::string const&, std::string const&) { return ""; },
      [](agent::StreamEvent const&) {}, {}, 2);
  server5.join();
  closesocket(ls);

  CHECK(rc == 0);
  CHECK(!msgs5.empty());
  CHECK(msgs5.back().role == "assistant");
  CHECK(msgs5.back().content.find("\"ok\":true") != std::string::npos);
  CHECK(msgs5.size() >= 4);  // user + (invalid asst + feedback) + (valid asst)

  // retry-limit: always-invalid response => -3
  g_responses = {
      "{\"id\":\"c7\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"not json\"},\"finish_reason\":\"stop\"}]}",
      "{\"id\":\"c8\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"not json\"},\"finish_reason\":\"stop\"}]}",
      "{\"id\":\"c9\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"not json\"},\"finish_reason\":\"stop\"}]}"};
  g_respIdx = 0;
  g_served = false;
  ls = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = 0;
  bind(ls, (sockaddr*)&addr, sizeof(addr));
  alen = sizeof(addr);
  getsockname(ls, (sockaddr*)&addr, &alen);
  port = ntohs(addr.sin_port);
  listen(ls, 5);
  std::thread server7([&] {
    for (int i = 0; i < 3; i++) {
      sockaddr_in caddr{};
      socklen_t clen = sizeof(caddr);
      int cs = accept(ls, (sockaddr*)&caddr, &clen);
      if (cs >= 0) serveOnce(cs);
    }
  });
  agent::Client client7;
  client7.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
  std::vector<agent::Message> msgs7;
  msgs7.push_back({"user", "json please"});
  rc = runTurnStructured(
      client7, msgs7, opts5,
      [](std::string const&, std::string const&) { return ""; },
      [](agent::StreamEvent const&) {}, {}, 2);
  server7.join();
  closesocket(ls);
  CHECK(rc == -3);

  // --- compactConversation: over budget triggers model summary ---------------
  g_responses = {
      "{\"id\":\"c10\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"COMPRESSED SUMMARY CONTENT\"},\"finish_reason\":\"stop\"}]}"};
  g_respIdx = 0;
  g_served = false;
  ls = socket(AF_INET, SOCK_STREAM, 0);
  addr.sin_port = 0;
  bind(ls, (sockaddr*)&addr, sizeof(addr));
  alen = sizeof(addr);
  getsockname(ls, (sockaddr*)&addr, &alen);
  port = ntohs(addr.sin_port);
  listen(ls, 1);
  std::thread server10([&] {
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int cs = accept(ls, (sockaddr*)&caddr, &clen);
    if (cs >= 0) serveOnce(cs);
  });

  agent::Client client10;
  client10.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
  std::vector<agent::Message> msgs10;
  for (int i = 0; i < 8; i++) {
    msgs10.push_back({"user", "a short user line number " + std::to_string(i)});
    msgs10.push_back({"assistant", "a short assistant line number " + std::to_string(i)});
  }
  size_t beforeCount = msgs10.size();
  agent::ChatOptions copts;
  copts.model = "test-model";
  copts.stream = false;
  std::string note;
  bool ok = compactConversation(client10, msgs10, copts, 10, [&](std::string const& n) { note = n; });
  server10.join();
  closesocket(ls);

  CHECK(ok);
  CHECK(msgs10.size() < beforeCount);
  CHECK(msgs10[0].role == "system");
  CHECK(msgs10[0].content.find("COMPRESSED SUMMARY CONTENT") != std::string::npos);
  CHECK(!note.empty());

  // under budget -> no-op
  g_respIdx = 0;
  std::vector<agent::Message> msgs11;
  msgs11.push_back({"user", "hi"});
  bool ok2 = compactConversation(client10, msgs11, copts, 100000);
  CHECK(!ok2);
  CHECK(msgs11.size() == 1);

  // --- thinking strength: how the level reaches the request body --------------
  {
    struct Case { char const* level; char const* style; char const* expect; };
    // expect == "" means the body must carry no thinking field at all.
    Case cases[] = {
        {"auto", "", ""},
        {"high", "", "\"reasoning_effort\":\"high\""},
        {"low", "effort", "\"reasoning_effort\":\"low\""},
        {"max", "effort", "\"reasoning_effort\":\"max\""},
        {"medium", "thinking", "\"thinking\":{\"type\":\"enabled\"}"},
        {"none", "thinking", "\"thinking\":{\"type\":\"disabled\"}"},
        {"minimal", "enable_thinking", "\"enable_thinking\":true"},
        {"none", "enable_thinking", "\"enable_thinking\":false"},
        {"high", "chat_template_kwargs",
         "\"chat_template_kwargs\":{\"enable_thinking\":true}"},
        {"none", "chat_template_kwargs",
         "\"chat_template_kwargs\":{\"enable_thinking\":false}"},
        {"high", "none", ""},
        {"bogus", "", ""},
    };
    int const n = (int)(sizeof(cases) / sizeof(cases[0]));
    g_responses.clear();
    g_response =
        "{\"id\":\"c11\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}";
    g_served = false;
    ls = socket(AF_INET, SOCK_STREAM, 0);
    addr.sin_port = 0;
    bind(ls, (sockaddr*)&addr, sizeof(addr));
    alen = sizeof(addr);
    getsockname(ls, (sockaddr*)&addr, &alen);
    port = ntohs(addr.sin_port);
    listen(ls, n);
    std::thread serverT([&] {
      for (int i = 0; i < n; i++) {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int cs = accept(ls, (sockaddr*)&caddr, &clen);
        if (cs < 0) return;
        serveOnce(cs);
      }
    });
    agent::Client clientT;
    clientT.configure("http://127.0.0.1:" + std::to_string(port) + "/v1", "");
    for (int i = 0; i < n; i++) {
      agent::ChatOptions o;
      o.model = "test-model";
      o.stream = false;
      o.thinking = cases[i].level;
      o.thinkingStyle = cases[i].style;
      std::vector<agent::Message> m;
      m.push_back({"user", "hi"});
      CHECK(clientT.chat(m, o, [](agent::StreamEvent const&) {}) == 0);
      if (cases[i].expect[0])
        CHECK(g_requestBody.find(cases[i].expect) != std::string::npos);
      else {
        CHECK(g_requestBody.find("reasoning_effort") == std::string::npos);
        CHECK(g_requestBody.find("enable_thinking") == std::string::npos);
        CHECK(g_requestBody.find("\"thinking\"") == std::string::npos);
      }
      // The model/temperature header must stay intact in every spelling.
      CHECK(g_requestBody.find("\"model\":\"test-model\"") != std::string::npos);
    }
    closesocket(ls);
    serverT.join();
  }

#ifdef _WIN32
  WSACleanup();
#endif
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
