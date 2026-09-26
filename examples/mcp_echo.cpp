// Minimal MCP stdio server used by the MCP client tests. Speaks the real
// Model Context Protocol over newline-delimited JSON-RPC 2.0:
//
//   initialize (protocol params) -> {protocolVersion, capabilities, serverInfo}
//   notifications/initialized    -> no reply
//   tools/list                   -> {tools:[{name, description, inputSchema}]}
//   tools/call                   -> {content:[{type:"text",text}], isError}
//
// Build standalone:
//   g++ -std=c++20 -O2 -Ithird_party examples/mcp_echo.cpp -o mcp_echo(.exe)
#include "minijson.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

static std::string jstr(mini::Value const& o, char const* key, std::string def = "") {
  return o.has(key) ? o.get(key)->asString() : def;
}

static int jint(mini::Value const& o, char const* key, int def = 0) {
  return o.has(key) ? (int)o.get(key)->asInt(def) : def;
}

static mini::Value response(mini::Value const& req, mini::Value res) {
  mini::Value o = mini::Value::makeObject();
  o.set("jsonrpc", mini::Value::makeString("2.0"));
  o.set("id", req.has("id") ? *req.get("id") : mini::Value::makeNull());
  o.set("result", std::move(res));
  return o;
}

static mini::Value textResult(mini::Value const& req, std::string const& text, bool isErr) {
  mini::Value res = mini::Value::makeObject();
  mini::Value content = mini::Value::makeArray();
  mini::Value item = mini::Value::makeObject();
  item.set("type", mini::Value::makeString("text"));
  item.set("text", mini::Value::makeString(text));
  content.arr.push_back(std::move(item));
  res.set("content", std::move(content));
  res.set("isError", mini::Value::makeBool(isErr));
  return response(req, std::move(res));
}

static mini::Value toolDef(std::string const& name, std::string const& desc,
                           std::vector<std::pair<std::string, std::string>> const& props) {
  mini::Value t = mini::Value::makeObject();
  t.set("name", mini::Value::makeString(name));
  t.set("description", mini::Value::makeString(desc));
  mini::Value schema = mini::Value::makeObject();
  schema.set("type", mini::Value::makeString("object"));
  mini::Value p = mini::Value::makeObject();
  for (auto const& [k, ty] : props) {
    mini::Value one = mini::Value::makeObject();
    one.set("type", mini::Value::makeString(ty));
    p.set(k, std::move(one));
  }
  schema.set("properties", std::move(p));
  t.set("inputSchema", std::move(schema));
  return t;
}

int main() {
  std::string line;
  char buf[8192];
  while (fgets(buf, sizeof buf, stdin)) {
    line = buf;
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    mini::Value req;
    if (!mini::tryParse(line, req) || req.type != mini::Value::Object) continue;
    std::string method = jstr(req, "method");
    if (method == "notifications/initialized") continue;

    if (method == "initialize") {
      mini::Value r = mini::Value::makeObject();
      r.set("protocolVersion", mini::Value::makeString("2025-06-18"));
      mini::Value cap = mini::Value::makeObject();
      cap.set("tools", mini::Value::makeObject());
      r.set("capabilities", std::move(cap));
      mini::Value si = mini::Value::makeObject();
      si.set("name", mini::Value::makeString("mcp_echo"));
      si.set("version", mini::Value::makeString("1.0"));
      r.set("serverInfo", std::move(si));
      std::puts(mini::dump(response(req, std::move(r))).c_str());
    } else if (method == "tools/list") {
      mini::Value tools = mini::Value::makeArray();
      tools.arr.push_back(toolDef("echo_text", "Echo the given text back verbatim.",
                                  {{"text", "string"}}));
      tools.arr.push_back(toolDef("add_numbers", "Add two integers and return the sum.",
                                  {{"a", "integer"}, {"b", "integer"}}));
      tools.arr.push_back(toolDef("env_echo", "Return the MCP_TEST_ENV environment value.",
                                  {}));
      tools.arr.push_back(toolDef("always_error", "Always fail (isError=true).", {}));
      mini::Value r = mini::Value::makeObject();
      r.set("tools", std::move(tools));
      std::puts(mini::dump(response(req, std::move(r))).c_str());
    } else if (method == "tools/call") {
      mini::Value const* params = req.get("params");
      mini::Value no = mini::Value::makeObject();
      if (!params) params = &no;
      std::string tname = jstr(*params, "name");
      mini::Value const* args = params->get("arguments");
      if (!args) args = &no;
      if (tname == "echo_text") {
        std::puts(mini::dump(textResult(req, "echo: " + jstr(*args, "text"), false)).c_str());
      } else if (tname == "add_numbers") {
        int sum = jint(*args, "a") + jint(*args, "b");
        std::puts(mini::dump(textResult(req, std::to_string(sum), false)).c_str());
      } else if (tname == "env_echo") {
        char const* v = std::getenv("MCP_TEST_ENV");
        std::puts(mini::dump(textResult(req, v ? v : "(unset)", false)).c_str());
      } else if (tname == "always_error") {
        std::puts(mini::dump(textResult(req, "boom", true)).c_str());
      } else {
        std::puts(mini::dump(textResult(req, "unknown tool: " + tname, true)).c_str());
      }
    }
    std::fflush(stdout);
  }
  return 0;
}
