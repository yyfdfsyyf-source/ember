#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace agent {

struct HttpRequest {
  std::string url;
  std::string method = "POST";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
  int timeoutSec = 120;
};

// Perform a request, delivering body chunks to `onChunk` as they arrive.
// Returns HTTP status code, or -1 on transport/network error.
int httpRequest(HttpRequest const& req, std::function<void(char const*, size_t)> onChunk);

// Human-readable detail about the last transport failure (empty if none).
std::string httpLastErrorDetail();

}  // namespace agent
