#include "agent/http.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>
#else
#include <curl/curl.h>
#include <cstring>
#endif

namespace agent {

// Shared by both backends: httpLastErrorDetail() reports whatever the platform
// transport failed with.
thread_local std::string g_lastError;

#ifdef _WIN32

namespace {

std::wstring toWide(std::string const& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring out((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
  return out;
}

std::string fromWide(std::wstring const& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  std::string out((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
  return out;
}

struct UrlParts {
  std::wstring scheme, host, path;
  int port = 80;
  bool secure = false;
};

bool crackUrl(std::string const& url, UrlParts& out) {
  std::wstring w = toWide(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwSchemeLength = (DWORD)-1;
  uc.dwHostNameLength = (DWORD)-1;
  uc.dwUrlPathLength = (DWORD)-1;
  if (!WinHttpCrackUrl(w.c_str(), (DWORD)w.size(), 0, &uc)) return false;
  out.scheme.assign(uc.lpszScheme, uc.dwSchemeLength);
  out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
  out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (out.path.empty()) out.path = L"/";
  out.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  out.port = uc.nPort ? uc.nPort : (out.secure ? 443 : 80);
  return true;
}

std::string envGet(char const* name) {
  char buf[1024];
  DWORD n = GetEnvironmentVariableA(name, buf, (DWORD)sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) return {};
  return buf;
}

// Best-effort proxy from environment variables (http_proxy/https_proxy,
// lowercase and uppercase; no_proxy to exempt hosts). Many VPN/proxy tools
// publish their local proxy there; WinHTTP's own registry config often does
// not match the machine's real network.
std::string envProxyFor(bool secure, std::string const& host, bool& bypass) {
  bypass = false;
  std::string proxy = envGet(secure ? "https_proxy" : "http_proxy");
  if (proxy.empty()) proxy = envGet(secure ? "HTTPS_PROXY" : "HTTP_PROXY");
  if (proxy.empty()) return {};
  std::string noProxy = envGet("no_proxy");
  if (noProxy.empty()) noProxy = envGet("NO_PROXY");
  if (!noProxy.empty()) {
    size_t pos = 0;
    while (pos <= noProxy.size()) {
      size_t comma = noProxy.find(',', pos);
      std::string entry = noProxy.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) entry.erase(entry.begin());
      while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) entry.pop_back();
      if (entry == "*") { bypass = true; return {}; }
      if (host == entry || (entry.size() && entry[0] == '.' &&
                            host.size() > entry.size() &&
                            host.compare(host.size() - entry.size(), entry.size(), entry) == 0)) {
        bypass = true;
        return {};
      }
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }
  if (host == "localhost" || host == "127.0.0.1" || host == "::1") { bypass = true; return {}; }
  return proxy;
}

void noteFail(char const* ctx, DWORD err) {
  wchar_t wbuf[256] = {0};
  DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                           wbuf, (DWORD)256, nullptr);
  while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n')) wbuf[--n] = 0;
  char mb[512] = {0};
  WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, mb, (int)sizeof(mb) - 1, nullptr, nullptr);
  g_lastError = std::string(ctx) + " (WinHTTP error " + std::to_string(err) + "): " + mb;
}

}  // namespace

int httpRequest(HttpRequest const& req, std::function<void(char const*, size_t)> onChunk) {
  g_lastError.clear();
  UrlParts u;
  if (!crackUrl(req.url, u)) {
    g_lastError = "invalid URL: " + req.url;
    return -1;
  }

  HINTERNET hSession = nullptr;
  bool proxyBypass = false;
  std::string proxy = envProxyFor(u.secure, fromWide(u.host), proxyBypass);
  if (proxyBypass) {
    hSession = WinHttpOpen(L"tui-agent/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  } else if (!proxy.empty()) {
    // Normalize to a "host:port" server string for WinHTTP.
    std::string hostPart = proxy;
    auto schemeAt = proxy.find("://");
    if (schemeAt != std::string::npos) hostPart = proxy.substr(schemeAt + 3);
    auto slash = hostPart.find('/');
    if (slash != std::string::npos) hostPart = hostPart.substr(0, slash);
    std::wstring wProxy = toWide(hostPart);
    hSession = WinHttpOpen(L"tui-agent/1.0", WINHTTP_ACCESS_TYPE_NAMED_PROXY, wProxy.c_str(),
                           WINHTTP_NO_PROXY_BYPASS, 0);
  } else {
    hSession = WinHttpOpen(L"tui-agent/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!hSession) { noteFail("WinHttpOpen", GetLastError()); return -1; }
  HINTERNET hConnect = WinHttpConnect(hSession, u.host.c_str(), (INTERNET_PORT)u.port, 0);
  if (!hConnect) { noteFail("WinHttpConnect", GetLastError()); WinHttpCloseHandle(hSession); return -1; }

  std::wstring wMethod = toWide(req.method);
  HINTERNET hRequest = WinHttpOpenRequest(hConnect, wMethod.c_str(), u.path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          u.secure ? WINHTTP_FLAG_SECURE : 0);
  if (!hRequest) {
    noteFail("WinHttpOpenRequest", GetLastError());
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return -1;
  }

  // headers
  std::wstring hdr;
  for (auto const& h : req.headers) {
    hdr += toWide(h.first);
    hdr += L": ";
    hdr += toWide(h.second);
    hdr += L"\r\n";
  }
  LPCWSTR wHdr = hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str();
  DWORD hdrLen = hdr.empty() ? 0 : (DWORD)hdr.size();

  DWORD timeout = (DWORD)(req.timeoutSec * 1000);
  WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);

  BOOL sent = WinHttpSendRequest(hRequest, wHdr, hdrLen,
                                 req.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)req.body.data(),
                                 (DWORD)req.body.size(), (DWORD)req.body.size(), 0);
  if (!sent) {
    noteFail("WinHttpSendRequest", GetLastError());
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return -1;
  }
  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    noteFail("WinHttpReceiveResponse", GetLastError());
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return -1;
  }

  DWORD status = 0;
  DWORD sz = sizeof(status);
  WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

  BYTE buf[8192];
  DWORD read = 0;
  while (WinHttpReadData(hRequest, buf, sizeof(buf), &read) && read > 0) {
    if (onChunk) onChunk((char const*)buf, read);
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return (int)status;
}

std::string httpLastErrorDetail() { return g_lastError; }

#else  // POSIX

namespace {
struct CurlSink {
  std::function<void(char const*, size_t)> cb;
  bool failed = false;
};

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* sink = (CurlSink*)userdata;
  size_t n = size * nmemb;
  if (sink->cb) sink->cb(ptr, n);
  return n;
}
}  // namespace

int httpRequest(HttpRequest const& req, std::function<void(char const*, size_t)> onChunk) {
  CURL* curl = curl_easy_init();
  if (!curl) return -1;
  curl_slist* headers = nullptr;
  for (auto const& h : req.headers) {
    std::string line = h.first + ": " + h.second;
    headers = curl_slist_append(headers, line.c_str());
  }
  CurlSink sink;
  sink.cb = onChunk;

  curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  if (!req.body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
  }
  if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)req.timeoutSec);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)req.timeoutSec);

  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    g_lastError = std::string("curl: ") + curl_easy_strerror(rc);
    return -1;
  }
  return (int)status;
}

std::string httpLastErrorDetail() { return g_lastError; }

#endif

}  // namespace agent
