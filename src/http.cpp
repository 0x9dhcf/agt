#include "http.hpp"

namespace agt {

size_t Http::write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* buf = static_cast<std::string*>(userdata);
  buf->append(static_cast<char*>(ptr), size * nmemb);
  return size * nmemb;
}

struct curl_slist* Http::to_slist(const std::vector<std::string>& headers) {
  struct curl_slist* h = nullptr;
  for (const auto& s : headers)
    h = curl_slist_append(h, s.c_str());
  return h;
}

size_t Http::stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<stream_context*>(userdata);
  size_t total = size * nmemb;
  ctx->buffer.append(static_cast<char*>(ptr), total);
  ctx->raw.append(static_cast<char*>(ptr), total);

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

std::string Http::extract_error(const std::string& body, long http_code) {
  std::string prefix = "api_error: HTTP " + std::to_string(http_code);
  if (body.empty())
    return prefix;

  // Try to pull error.message from a JSON error envelope.
  try {
    auto j = nlohmann::json::parse(body);
    if (j.contains("error") && j["error"].is_object()) {
      auto msg = j["error"].value("message", "");
      if (!msg.empty())
        return prefix + " - " + msg;
    }
  } catch (...) {
  }

  // Fall back to raw body, truncated.
  constexpr std::string::size_type limit = 200;
  if (body.size() <= limit)
    return prefix + " - " + body;
  return prefix + " - " + body.substr(0, limit) + "...";
}

Http::Http() : curl_(curl_easy_init()) {
  if (!curl_)
    throw LlmError("network_error: failed to init curl");
}

Http::~Http() noexcept { curl_easy_cleanup(curl_); }

Json Http::get(const std::string& url, const std::vector<std::string>& headers) {
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

  long http_code = 0;
  curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

  if (response.empty())
    throw LlmError("network_error: empty response");

  // On HTTP errors, try JSON first for a clean message; otherwise report raw.
  if (http_code >= 400) {
    try {
      return nlohmann::json::parse(response); // let provider handle the error object
    } catch (...) {
      throw LlmError(extract_error(response, http_code));
    }
  }

  return nlohmann::json::parse(response);
}

Json Http::post(const std::string& url, const std::string& body,
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

  long http_code = 0;
  curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

  if (response.empty())
    throw LlmError("network_error: empty response");

  // On HTTP errors, try JSON first for a clean message; otherwise report raw.
  if (http_code >= 400) {
    try {
      return nlohmann::json::parse(response); // let provider handle the error object
    } catch (...) {
      throw LlmError(extract_error(response, http_code));
    }
  }

  return nlohmann::json::parse(response);
}

void Http::post_stream(const std::string& url, const std::string& body,
                       const std::vector<std::string>& headers, SseCallback on_event) {
  struct curl_slist* hlist = to_slist(headers);
  stream_context ctx{{}, {}, {}, std::move(on_event)};

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
  if (http_code >= 400)
    throw LlmError(extract_error(ctx.raw, http_code));
}

} // namespace agt
