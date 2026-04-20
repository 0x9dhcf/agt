#pragma once

#include <agt/llm.hpp>
#include <curl/curl.h>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace agt {

/// Callback for SSE events: (event_type, data).
using SseCallback = std::function<void(const std::string& event, const std::string& data)>;

/// Thin wrapper around libcurl for JSON GET/POST requests.
/// Owns a single CURL handle, reused across calls via curl_easy_reset.
/// Thread-safe: every exposed call serialises through `mu_` so callers may
/// share one Http (and therefore one Llm) across threads without tearing
/// libcurl's handle state. `CURLOPT_NOSIGNAL=1` on every perform neutralises
/// libcurl's SIGALRM-based DNS timeout (which is process-global and unsafe
/// under threading).
class Http {
  CURL* curl_;
  std::mutex mu_;

  // CURLOPT_WRITEFUNCTION callback: appends data to a std::string.
  static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata);

  // Converts a vector of "Header: value" strings to a curl_slist.
  // Caller must curl_slist_free_all the result.
  static struct curl_slist* to_slist(const std::vector<std::string>& headers);

  // SSE streaming state.
  struct stream_context {
    std::string buffer;
    std::string current_event; // current "event:" value
    std::string raw;           // full response body for error reporting
    SseCallback cb;
  };

  // CURLOPT_WRITEFUNCTION callback for SSE streaming.
  static size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata);

  // Try to parse a response body as JSON and extract error.message;
  // fall back to the first `limit` bytes of the raw body.
  static std::string extract_error(const std::string& body, long http_code);

public:
  Http();
  ~Http() noexcept;

  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  Http(const Http&&) = delete;
  Http& operator=(const Http&&) = delete;

  Json get(const std::string& url, const std::vector<std::string>& headers);

  Json post(const std::string& url, const std::string& body,
            const std::vector<std::string>& headers);

  /// Streaming POST: sends body, parses SSE events, calls on_event for each.
  void post_stream(const std::string& url, const std::string& body,
                   const std::vector<std::string>& headers, SseCallback on_event);
};

} // namespace agt
