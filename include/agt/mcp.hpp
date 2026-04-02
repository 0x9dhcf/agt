#pragma once

#include <agt/tool.hpp>
#include <memory>
#include <string>
#include <vector>

namespace agt {

enum class McpTransport { stdio, http };

struct mcp_config {
  McpTransport transport;
  std::string name;
  std::string command;           ///< stdio: executable path, http: URL.
  std::vector<std::string> args; ///< Command-line args (stdio only).
};

struct McpServerImpl;

/// Client for a single MCP (Model Context Protocol) server.
/// Manages the connection lifetime and exposes discovered tools.
class McpServer {
  std::unique_ptr<McpServerImpl> impl_;

public:
  explicit McpServer(const mcp_config &config);
  ~McpServer() noexcept;

  McpServer(const McpServer &) = delete;
  McpServer &operator=(const McpServer &) = delete;

  McpServer(const McpServer &&) = delete;
  McpServer &operator=(const McpServer &&) = delete;

  /// Starts the server process (stdio) or connects (http) and runs
  /// MCP initialize + tools/list handshake.
  void connect();
  /// Returns the tools discovered during connect(). The returned shared_ptrs
  /// keep the tools alive even if this mcp_server is destroyed.
  std::vector<std::shared_ptr<Tool>> tools();
};

} // namespace agt
