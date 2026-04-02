#pragma once

#include <agt/llm.hpp>
#include <curl/curl.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace agt {

/// Callback for SSE events: (event_type, data).
using SseCallback = std::function<void(const std::string& event, const std::string& data)>;

/// Thin wrapper around libcurl for JSON GET/POST requests.
/// Owns a single CURL handle, reused across calls via curl_easy_reset.
class Http {
  CURL* curl_;

  // CURLOPT_WRITEFUNCTION callback: appends data to a std::string.
  static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
  }

  // Converts a vector of "Header: value" strings to a curl_slist.
  // Caller must curl_slist_free_all the result.
  static struct curl_slist* to_slist(const std::vector<std::string>& headers) {
    struct curl_slist* h = nullptr;
    for (const auto& s : headers)
      h = curl_slist_append(h, s.c_str());
    return h;
  }

  // SSE streaming state.
  struct stream_context {
    std::string buffer;
    std::string current_event; // current "event:" value
    SseCallback cb;
  };

  // CURLOPT_WRITEFUNCTION callback for SSE streaming.
  static size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<stream_context*>(userdata);
    size_t total = size * nmemb;
    ctx->buffer.append(static_cast<char*>(ptr), total);

    // Process complete lines.
    std::string::size_type pos{};
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
      auto line = ctx->buffer.substr(0, pos);
      ctx->buffer.erase(0, pos + 1);

      // Strip trailing \r.
      if (!line.empty() && line.back() == '\r')
        line.pop_back();

      // Empty line = end of SSE event (but we dispatch on each data: line).
      if (line.empty()) {
        ctx->current_event.clear();
        continue;
      }

      // Comment line.
      if (line[0] == ':')
        continue;

      // "event: <type>" line.
      if (line.starts_with("event:")) {
        auto val = line.substr(6);
        if (!val.empty() && val[0] == ' ')
          val.erase(0, 1);
        ctx->current_event = val;
        continue;
      }

      // "data: <payload>" line.
      if (line.starts_with("data:")) {
        auto data = line.substr(5);
        if (!data.empty() && data[0] == ' ')
          data.erase(0, 1);

        // Skip [DONE] sentinel.
        if (data == "[DONE]")
          continue;

        ctx->cb(ctx->current_event, data);
      }
    }

    return total;
  }

public:
  Http() : curl_(curl_easy_init()) {
    if (!curl_)
      throw LlmError("network_error: failed to init curl");
  }
  ~Http() noexcept { curl_easy_cleanup(curl_); }

  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  Http(const Http&&) = delete;
  Http& operator=(const Http&&) = delete;

  Json get(const std::string& url, const std::vector<std::string>& headers) {
    struct curl_slist* hlist = to_slist(headers);
    std::string response;

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(hlist);

    if (res != CURLE_OK)
      throw LlmError(std::string("network_error: ") + curl_easy_strerror(res));

    if (response.empty())
      throw LlmError("network_error: empty response");

    return nlohmann::json::parse(response);
  }

  Json post(const std::string& url, const std::string& body,
            const std::vector<std::string>& headers) {
    struct curl_slist* hlist = to_slist(headers);
    std::string response;

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(hlist);

    if (res != CURLE_OK)
      throw LlmError(std::string("network_error: ") + curl_easy_strerror(res));

    if (response.empty())
      throw LlmError("network_error: empty response");

    return nlohmann::json::parse(response);
  }

  /// Streaming POST: sends body, parses SSE events, calls on_event for each.
  void post_stream(const std::string& url, const std::string& body,
                   const std::vector<std::string>& headers, SseCallback on_event) {
    struct curl_slist* hlist = to_slist(headers);
    stream_context ctx{{}, {}, std::move(on_event)};

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &ctx);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(hlist);

    if (res != CURLE_OK)
      throw LlmError(std::string("network_error: ") + curl_easy_strerror(res));

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400) {
      // Try to parse remaining buffer for error details.
      if (!ctx.buffer.empty())
        throw LlmError("api_error: HTTP " + std::to_string(http_code) + " - " + ctx.buffer);
      throw LlmError("api_error: HTTP " + std::to_string(http_code));
    }
  }
};

} // namespace agt
