#include <agt/mcp.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <curl/curl.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

using json = nlohmann::json;

namespace agt {

// Forward declare — defined below

// ---------------------------------------------------------------------------
// mcp_tool: private tool subclass forwarding to MCP server via JSON-RPC
// ---------------------------------------------------------------------------

class mcp_tool : public Tool {
  McpServerImpl *impl_;
  std::string name_;
  std::string description_;
  Json parameters_;

public:
  mcp_tool(McpServerImpl &impl, std::string name, std::string description, Json parameters)
      : impl_(&impl), name_(std::move(name)),
        description_(std::move(description)), parameters_(std::move(parameters)) {}

  const char *name() const noexcept override { return name_.c_str(); }
  const char *description() const noexcept override { return description_.c_str(); }
  Json parameters() const override { return parameters_; }
  Json execute(const Json &input, void * = nullptr) override;
};

// ---------------------------------------------------------------------------
// curl write callback
// ---------------------------------------------------------------------------

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<std::string *>(userdata);
  buf->append(static_cast<char *>(ptr), size * nmemb);
  return size * nmemb;
}

// ---------------------------------------------------------------------------
// mcp_server_impl
// ---------------------------------------------------------------------------

struct McpServerImpl {
  mcp_config config;
  int next_id = 1;
  Json tools_cache;

  // owned tool instances
  std::vector<std::shared_ptr<mcp_tool>> owned_tools;

  // stdio transport
  pid_t child_pid = -1;
  int fd_in = -1;
  FILE *fp_out = nullptr;

  // http transport
  CURL *curl = nullptr;

  // sse transport — client does GET on the SSE URL, server streams events.
  // First event carries the `endpoint` URL where client POSTs JSON-RPC. The
  // POST response is an empty 202; the actual JSON-RPC reply arrives back on
  // the same SSE stream, matched by JSON-RPC id. Notifications never get a
  // reply.
  std::thread sse_thread;
  std::atomic<bool> sse_stop{false};
  std::string sse_base_origin;     ///< scheme://host[:port], for resolving relative endpoints
  std::string sse_post_endpoint;   ///< absolute URL client POSTs to (set once, by the first event)
  std::string sse_buffer;          ///< accumulates bytes across write_cb invocations
  bool sse_have_endpoint = false;  ///< set under endpoint_mu when the endpoint event arrives
  std::string sse_thread_error;    ///< non-empty if sse_loop exited unexpectedly
  std::mutex endpoint_mu;
  std::condition_variable endpoint_cv;
  std::mutex pending_mu;
  std::condition_variable pending_cv;
  std::unordered_map<long long, Json> pending_results;  ///< id → raw JSON-RPC envelope

  // Serializes JSON-RPC transactions: the runner fans tool calls out across
  // threads, but a single CURL easy handle (and the stdio fds + next_id) must
  // only be touched by one thread at a time.
  std::mutex mu;

  explicit McpServerImpl(const mcp_config &cfg) : config(cfg) {}
  ~McpServerImpl() noexcept { close(); }

  McpServerImpl(const McpServerImpl&) = delete;
  McpServerImpl& operator=(const McpServerImpl&) = delete;

  McpServerImpl(const McpServerImpl&&) = delete;
  McpServerImpl& operator=(const McpServerImpl&&) = delete;

  // --- JSON-RPC ---

  Json jsonrpc_request(const std::string &method, Json params = Json::object()) {
    return {
        {"jsonrpc", "2.0"}, {"id", next_id++}, {"method", method}, {"params", std::move(params)}};
  }

  Json jsonrpc_notification(const std::string &method) {
    return {{"jsonrpc", "2.0"}, {"method", method}};
  }

  // --- Stdio ---

  void send_stdio(const Json &msg) const {
    auto str = msg.dump() + "\n";
    const auto *p = str.c_str();
    auto rem = str.size();
    while (rem > 0) {
      auto n = ::write(fd_in, p, rem);
      if (n < 0)
        throw std::runtime_error("mcp: stdio write failed");
      p += n;
      rem -= static_cast<size_t>(n);
    }
  }

  Json recv_stdio() const {
    std::string line;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), fp_out)) {
      line += buf;
      if (!line.empty() && line.back() == '\n') {
        line.pop_back();
        break;
      }
    }
    if (line.empty())
      throw std::runtime_error("mcp: stdio read failed");
    return Json::parse(line);
  }

  // --- HTTP ---

  std::string http_post(const std::string &body) const {
    std::string response;

    curl_easy_reset(curl);
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
    for (const auto &[k, v] : config.headers) {
      auto line = k + ": " + v;
      headers = curl_slist_append(headers, line.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, config.command.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK)
      throw std::runtime_error(std::string("mcp: curl error: ") + curl_easy_strerror(rc));

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300)
      throw std::runtime_error("mcp: http " + std::to_string(status));

    return response;
  }

  static Json parse_sse(const std::string &body) {
    std::string_view sv(body);
    std::string_view last;
    const std::string_view prefix = "data: ";

    size_t pos = 0;
    while ((pos = sv.find(prefix, pos)) != std::string_view::npos) {
      auto start = pos + prefix.size();
      auto end = sv.find('\n', start);
      last = sv.substr(start, end == std::string_view::npos ? end : end - start);
      pos = start;
    }
    if (last.empty())
      throw std::runtime_error("mcp: no SSE data in response");
    return Json::parse(last);
  }

  // --- SSE transport -------------------------------------------------------
  //
  // Two libcurl transfers at work:
  //   * GET stream (sse_loop, its own thread + curl handle) — receives events.
  //   * POST per call (sse_send, uses the shared `curl` handle under `mu`) —
  //     sends JSON-RPC. Server replies 202 on the POST; the actual JSON-RPC
  //     response arrives back on the GET stream, id-matched into
  //     `pending_results` and woken via `pending_cv`.

  /// Auto-detect SSE endpoints even when the caller configured transport=http.
  /// Conservative: only flips when the URL's path ends in `/sse`.
  static bool url_looks_like_sse_transport(const std::string &url) {
    auto q = url.find('?');
    auto path_end = (q == std::string::npos) ? url.size() : q;
    constexpr std::string_view suffix = "/sse";
    if (path_end < suffix.size()) return false;
    return url.compare(path_end - suffix.size(), suffix.size(), suffix) == 0;
  }

  /// Derive "scheme://host[:port]" from the GET URL so we can resolve
  /// relative `endpoint` URLs (Houston MCP returns them relative).
  static std::string derive_origin(const std::string &url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {};
    auto host_start = scheme_end + 3;
    auto path_start = url.find('/', host_start);
    if (path_start == std::string::npos) return url;
    return url.substr(0, path_start);
  }

  /// Resolve the `endpoint` value (as received in the first SSE event)
  /// against the GET URL's origin. Accepts absolute and origin-relative forms.
  std::string resolve_endpoint(const std::string &endpoint) const {
    if (endpoint.rfind("http://", 0) == 0 || endpoint.rfind("https://", 0) == 0) {
      return endpoint;
    }
    if (endpoint.empty()) return sse_base_origin;
    if (endpoint.front() == '/') return sse_base_origin + endpoint;
    return sse_base_origin + "/" + endpoint;
  }

  /// libcurl CURLOPT_XFERINFOFUNCTION: aborts the transfer on shutdown so
  /// close() doesn't block on a long-idle SSE stream.
  static int sse_xferinfo_cb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto *self = static_cast<McpServerImpl *>(clientp);
    return self->sse_stop.load() ? 1 : 0;
  }

  /// libcurl CURLOPT_WRITEFUNCTION: buffers chunks, parses complete SSE frames
  /// (terminated by a blank line). Dispatches `endpoint` and `message` events
  /// as they arrive.
  static size_t sse_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *self = static_cast<McpServerImpl *>(userdata);
    self->sse_buffer.append(static_cast<char *>(ptr), size * nmemb);
    // SSE uses CRLF line endings. Strip the CRs so the frame/boundary search
    // below can key on plain `\n\n`; otherwise a raw `\r\n\r\n` would splinter
    // into four newlines and we'd emit half-frames with trailing CRs inside
    // the data field — a classic "endpoint has a stray \r on the end that
    // breaks libcurl's URL parser" symptom.
    self->sse_buffer.erase(std::remove(self->sse_buffer.begin(), self->sse_buffer.end(), '\r'),
                           self->sse_buffer.end());
    size_t boundary;
    while ((boundary = self->sse_buffer.find("\n\n")) != std::string::npos) {
      std::string frame = self->sse_buffer.substr(0, boundary);
      self->sse_buffer.erase(0, boundary + 2);
      self->dispatch_sse_frame(frame);
    }
    return size * nmemb;
  }

  /// Parse one SSE frame and dispatch. Frames look like:
  ///   event: endpoint
  ///   data: /messages?sessionId=abc
  void dispatch_sse_frame(const std::string &frame) {
    std::string event_name;
    std::string data;
    size_t pos = 0;
    while (pos < frame.size()) {
      size_t eol = frame.find('\n', pos);
      if (eol == std::string::npos) eol = frame.size();
      std::string_view line(frame.data() + pos, eol - pos);
      pos = eol + 1;
      if (line.empty() || line.front() == ':') continue;  // comment / keepalive
      auto colon = line.find(':');
      std::string_view field = colon == std::string_view::npos ? line : line.substr(0, colon);
      std::string_view value;
      if (colon != std::string_view::npos) {
        value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
      }
      if (field == "event") event_name = value;
      else if (field == "data") {
        if (!data.empty()) data.push_back('\n');
        data.append(value);
      }
    }
    if (event_name.empty()) event_name = "message";

    if (event_name == "endpoint") {
      std::lock_guard<std::mutex> lock(endpoint_mu);
      sse_post_endpoint = resolve_endpoint(data);
      sse_have_endpoint = true;
      endpoint_cv.notify_all();
      return;
    }
    if (event_name == "message") {
      Json raw;
      try {
        raw = Json::parse(data);
      } catch (...) {
        return;  // malformed; skip silently, matches original behaviour
      }
      if (!raw.contains("id") || !raw["id"].is_number_integer()) return;
      long long id = raw["id"].get<long long>();
      {
        std::lock_guard<std::mutex> lock(pending_mu);
        pending_results[id] = std::move(raw);
      }
      pending_cv.notify_all();
    }
  }

  /// Background thread: runs curl_easy_perform on the SSE GET until stop or
  /// error. On exit, records any error so waiting callers can surface it.
  void sse_loop() {
    CURL *sse_curl = curl_easy_init();
    if (!sse_curl) {
      std::lock_guard<std::mutex> lock(endpoint_mu);
      sse_thread_error = "mcp: sse curl_easy_init failed";
      endpoint_cv.notify_all();
      return;
    }
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    for (const auto &[k, v] : config.headers) {
      auto line = k + ": " + v;
      headers = curl_slist_append(headers, line.c_str());
    }
    curl_easy_setopt(sse_curl, CURLOPT_URL, config.command.c_str());
    curl_easy_setopt(sse_curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(sse_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(sse_curl, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(sse_curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(sse_curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(sse_curl, CURLOPT_XFERINFOFUNCTION, sse_xferinfo_cb);
    curl_easy_setopt(sse_curl, CURLOPT_XFERINFODATA, this);

    CURLcode rc = curl_easy_perform(sse_curl);
    if (rc != CURLE_OK && !sse_stop.load()) {
      std::lock_guard<std::mutex> lock(endpoint_mu);
      sse_thread_error = std::string("mcp: sse transport error: ") + curl_easy_strerror(rc);
      endpoint_cv.notify_all();
      pending_cv.notify_all();
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(sse_curl);
  }

  /// Launch sse_loop on a background thread, wait up to \p timeout for the
  /// server to emit its initial `endpoint` event (proves the connection is
  /// up). Throws on timeout or transport error.
  void try_start_sse(std::chrono::milliseconds timeout) {
    sse_base_origin = derive_origin(config.command);
    sse_thread = std::thread(&McpServerImpl::sse_loop, this);
    std::unique_lock<std::mutex> lock(endpoint_mu);
    if (!endpoint_cv.wait_for(lock, timeout,
                              [this] { return sse_have_endpoint || !sse_thread_error.empty(); })) {
      sse_stop.store(true);
      if (sse_thread.joinable()) sse_thread.join();
      throw std::runtime_error("mcp: sse endpoint handshake timed out");
    }
    if (!sse_thread_error.empty()) {
      sse_stop.store(true);
      if (sse_thread.joinable()) sse_thread.join();
      throw std::runtime_error(sse_thread_error);
    }
  }

  /// Send one JSON-RPC envelope over SSE. For requests (id set) blocks up to
  /// \p timeout waiting for the reply through the stream; for notifications
  /// returns immediately after the POST acks.
  Json sse_send(const Json &msg, bool has_id,
                std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
    if (!sse_have_endpoint) throw std::runtime_error("mcp: sse not connected");
    std::string endpoint;
    {
      std::lock_guard<std::mutex> lock(endpoint_mu);
      endpoint = sse_post_endpoint;
    }

    std::string body = msg.dump();
    std::string response;
    curl_easy_reset(curl);
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    for (const auto &[k, v] : config.headers) {
      auto line = k + ": " + v;
      headers = curl_slist_append(headers, line.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK)
      throw std::runtime_error(std::string("mcp: sse post error: ") + curl_easy_strerror(rc));

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    // Spec says 202 No Content; some servers return 200 with an empty body.
    if (status < 200 || status >= 300)
      throw std::runtime_error("mcp: sse post http " + std::to_string(status));

    if (!has_id) return Json::object();

    long long id = msg.value("id", 0LL);
    std::unique_lock<std::mutex> lock(pending_mu);
    if (!pending_cv.wait_for(lock, timeout, [this, id] {
          return pending_results.count(id) > 0 || !sse_thread_error.empty();
        })) {
      throw std::runtime_error("mcp: sse response timeout for id " + std::to_string(id));
    }
    if (!sse_thread_error.empty() && pending_results.count(id) == 0) {
      throw std::runtime_error(sse_thread_error);
    }
    Json out = std::move(pending_results[id]);
    pending_results.erase(id);
    return out;
  }

  // --- Transport-agnostic ---

  void notify(const std::string &method) {
    std::lock_guard<std::mutex> lock(mu);
    auto msg = jsonrpc_notification(method);
    if (config.transport == McpTransport::stdio)
      send_stdio(msg);
    else if (config.transport == McpTransport::sse)
      sse_send(msg, /*has_id=*/false);
    else
      http_post(msg.dump());
  }

  Json call(const std::string &method, Json params = Json::object()) {
    std::lock_guard<std::mutex> lock(mu);
    auto req = jsonrpc_request(method, std::move(params));

    Json raw;
    if (config.transport == McpTransport::stdio) {
      send_stdio(req);
      raw = recv_stdio();
    } else if (config.transport == McpTransport::sse) {
      raw = sse_send(req, /*has_id=*/true);
    } else {
      auto body = http_post(req.dump());
      try {
        raw = Json::parse(body);
      } catch (const Json::parse_error &) {
        raw = parse_sse(body);
      }
    }

    if (raw.contains("error"))
      throw std::runtime_error("mcp: server error: " + raw["error"].dump());

    return raw.value("result", Json::object());
  }

  // --- Connect ---

  void connect_stdio() {
    int to_child[2];
    int from_child[2];

    if (pipe(to_child) < 0 || pipe(from_child) < 0)
      throw std::runtime_error("mcp: pipe failed");

    pid_t pid = fork();
    if (pid < 0) {
      ::close(to_child[0]);
      ::close(to_child[1]);
      ::close(from_child[0]);
      ::close(from_child[1]);
      throw std::runtime_error("mcp: fork failed");
    }

    if (pid == 0) {
      ::close(to_child[1]);
      ::close(from_child[0]);
      dup2(to_child[0], STDIN_FILENO);
      dup2(from_child[1], STDOUT_FILENO);
      ::close(to_child[0]);
      ::close(from_child[1]);

      std::vector<char *> argv;
      argv.push_back(const_cast<char *>(config.command.c_str()));
      for (auto &arg : config.args)
        argv.push_back(const_cast<char *>(arg.c_str()));
      argv.push_back(nullptr);

      execvp(argv[0], argv.data());
      _exit(1);
    }

    ::close(to_child[0]);
    ::close(from_child[1]);

    child_pid = pid;
    fd_in = to_child[1];
    fp_out = fdopen(from_child[0], "r");
    if (!fp_out) {
      ::close(from_child[0]);
      throw std::runtime_error("mcp: fdopen failed");
    }
  }

  void connect_http() {
    curl = curl_easy_init();
    if (!curl)
      throw std::runtime_error("mcp: curl init failed");
  }

  void initialize() {
    Json params = {{"protocolVersion", "2024-11-05"},
                   {"capabilities", Json::object()},
                   {"clientInfo", {{"name", "agt"}, {"version", "0.1.0"}}}};

    call("initialize", std::move(params));
    notify("notifications/initialized");
    tools_cache = call("tools/list");
  }

  void connect_sse() {
    // sse reuses the `curl` handle for POSTs; the GET stream has its own
    // handle owned by sse_loop.
    curl = curl_easy_init();
    if (!curl) throw std::runtime_error("mcp: curl init failed");
    try_start_sse(std::chrono::seconds(30));
  }

  void connect() {
    // Auto-upgrade: a caller who specified plain http but aimed at a /sse
    // endpoint gets SSE transparently. Matches the draft's behaviour and
    // keeps the UI proxy (which hard-codes http) compatible with SSE servers.
    if (config.transport == McpTransport::http &&
        url_looks_like_sse_transport(config.command)) {
      config.transport = McpTransport::sse;
    }
    if (config.transport == McpTransport::stdio)
      connect_stdio();
    else if (config.transport == McpTransport::sse)
      connect_sse();
    else
      connect_http();
    initialize();
  }

  Json call_tool(const std::string &name, const Json &arguments) {
    Json params = {{"name", name}, {"arguments", arguments}};
    return call("tools/call", std::move(params));
  }

  // --- Cleanup ---

  void close() noexcept {
    if (config.transport == McpTransport::stdio) {
      if (fd_in >= 0) {
        ::close(fd_in);
        fd_in = -1;
      }
      if (fp_out) {
        std::fclose(fp_out);
        fp_out = nullptr;
      }
      if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        waitpid(child_pid, nullptr, 0);
        child_pid = -1;
      }
    } else {
      if (config.transport == McpTransport::sse) {
        // Signal sse_loop to exit (xferinfo_cb returns 1), wake any pending
        // waiters, then join. Safe to call even if the thread never started.
        sse_stop.store(true);
        pending_cv.notify_all();
        endpoint_cv.notify_all();
        if (sse_thread.joinable()) sse_thread.join();
      }
      if (curl) {
        curl_easy_cleanup(curl);
        curl = nullptr;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// mcp_tool::execute (needs impl to be complete)
// ---------------------------------------------------------------------------

Json mcp_tool::execute(const Json &input, void *) {
  // Surface failures as a tool result so the agent loop keeps running and
  // the model can decide how to recover, rather than letting the exception
  // tear down the runner.
  try {
    return impl_->call_tool(name_, input);
  } catch (const std::exception &e) {
    return Json{{"error", e.what()}};
  }
}

// ---------------------------------------------------------------------------
// mcp_server public interface
// ---------------------------------------------------------------------------

McpServer::McpServer(const mcp_config &config)
    : impl_(std::make_unique<McpServerImpl>(config)) {}

McpServer::~McpServer() noexcept = default;

void McpServer::connect() { impl_->connect(); }

std::vector<std::shared_ptr<Tool>> McpServer::tools() {
  if (!impl_->owned_tools.empty()) {
    // already built, return cached shared_ptrs
    std::vector<std::shared_ptr<Tool>> result;
    for (auto &t : impl_->owned_tools)
      result.push_back(t);
    return result;
  }

  if (!impl_->tools_cache.contains("tools"))
    return {};

  static const Json default_schema = {{"type", "object"}};

  std::vector<std::shared_ptr<Tool>> result;
  for (const auto &t : impl_->tools_cache["tools"]) {
    auto name = t.value("name", "");
    auto desc = t.value("description", "");
    auto schema = t.contains("inputSchema") ? t["inputSchema"] : default_schema;

    impl_->owned_tools.push_back(std::make_shared<mcp_tool>(*impl_, name, desc, std::move(schema)));
    result.push_back(impl_->owned_tools.back());
  }
  return result;
}

} // namespace agt
