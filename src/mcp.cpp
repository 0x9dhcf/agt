#include <agt/mcp.hpp>
#include <cstdio>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using json = nlohmann::json;

namespace agt {

// Forward declare — defined below

// ---------------------------------------------------------------------------
// mcp_tool: private tool subclass forwarding to MCP server via JSON-RPC
// ---------------------------------------------------------------------------

class mcp_tool : public Tool {
  McpServerImpl &impl_;
  std::string name_;
  std::string description_;
  Json parameters_;

public:
  mcp_tool(McpServerImpl &impl, std::string name, std::string description, Json parameters)
      : impl_(impl), name_(std::move(name)),
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

  // --- Transport-agnostic ---

  void notify(const std::string &method) {
    auto msg = jsonrpc_notification(method);
    if (config.transport == McpTransport::stdio)
      send_stdio(msg);
    else
      http_post(msg.dump());
  }

  Json call(const std::string &method, Json params = Json::object()) {
    auto req = jsonrpc_request(method, std::move(params));

    Json raw;
    if (config.transport == McpTransport::stdio) {
      send_stdio(req);
      raw = recv_stdio();
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

  void connect() {
    if (config.transport == McpTransport::stdio)
      connect_stdio();
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
    return impl_.call_tool(name_, input);
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
