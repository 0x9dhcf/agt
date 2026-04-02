// vim: set ts=2 sw=2 sts=2 et:
#include <agt/agent.hpp>
#include <agt/llm.hpp>
#include <agt/runner.hpp>
#include <agt/session.hpp>
#include <agt/tool.hpp>
#include <future>
#include <memory>
#include <nlohmann/json-schema.hpp>
#include <string>
#include <unordered_map>

namespace agt {

static std::string get_content(const Json& resp) {
  auto it = resp.find("content");
  if (it == resp.end() || it->is_null())
    return "";
  return it->get<std::string>();
}

static Json build_tools_array(const std::vector<std::shared_ptr<Tool>>& tools) {
  auto arr = Json::array();
  for (const auto& t : tools) {
    arr.push_back(
        {{"name", t->name()}, {"description", t->description()}, {"parameters", t->parameters()}});
  }
  return arr;
}

static Json build_request(const Agent& a, const RunnerOptions& opts, const std::string& query) {
  Json req = {{"messages", Json::array()}};

  if (!a.instructions.empty())
    req["system"] = a.instructions;
  if (!a.tools.empty())
    req["tools"] = build_tools_array(a.tools);
  if (opts.max_tokens > 0)
    req["max_tokens"] = opts.max_tokens;
  if (opts.thinking_effort)
    req["thinking_effort"] = *opts.thinking_effort;

  if (a.session) {
    auto history = a.session->messages();
    for (auto& m : history)
      req["messages"].push_back(std::move(m));
  }

  req["messages"].push_back({{"role", "user"}, {"content", query}});
  return req;
}

static void append_assistant_message(Json& messages, const Json& resp) {
  Json msg = {{"role", "assistant"}, {"content", resp["content"]}};
  if (resp.contains("calls") && resp["calls"].is_array() && !resp["calls"].empty())
    msg["calls"] = resp["calls"];
  messages.push_back(std::move(msg));
}

static void
execute_tool_calls(const RunnerOptions& options, const RunnerHooks& hooks, Json& messages,
                   const Json& resp,
                   const std::unordered_map<std::string, std::shared_ptr<Tool>>& tool_map) {

  struct Pending {
    std::shared_ptr<Tool> tool;
    std::string call_id;
    Json args;
    Json result;
  };

  std::vector<std::future<Pending>> futures;
  if (!resp.contains("calls") || !resp["calls"].is_array())
    return;
  futures.reserve(resp["calls"].size());

  for (const auto& call : resp["calls"]) {
    const auto& name = call["name"].get<std::string>();
    const auto& id = call["id"].get<std::string>();
    const auto& input = call["input"].get<std::string>();

    Json args = Json::parse(input);
    Json result;
    auto it = tool_map.find(name);
    if (it != tool_map.end()) {
      try {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(it->second->parameters());
        validator.validate(args);
      } catch (const std::exception& e) {
        futures.push_back(std::async(std::launch::async, [it, id, e] -> Pending {
          return {.tool = it->second,
                  .call_id = id,
                  .args = {},
                  .result = {"error", "invalid tool input: " + std::string(e.what())}};
        }));
        continue;
      }
      if (hooks.on_tool_start && !hooks.on_tool_start(*it->second, args)) {
        futures.push_back(std::async(std::launch::async, [it, id, args] -> Pending {
          return {.tool = it->second, .call_id = id, .args = args, .result = {"error", "tool call denied"}};
        }));
      } else {
        futures.push_back(std::async(std::launch::async, [it, id, args, options] -> Pending {
          return {
              .tool = it->second, .call_id = id, .args = args, .result = it->second->execute(args, options.context)};
        }));
      }
    } else {
      futures.push_back(std::async(std::launch::async, [id, name] -> Pending {
        return {.tool = nullptr, .call_id = id, .args = {}, .result = {"error", "tool not found: " + name}};
      }));
    }
  }

  for (auto& f : futures) {
    auto pending = f.get();
    if (hooks.on_tool_stop && pending.tool)
      hooks.on_tool_stop(*pending.tool, pending.args, pending.result);
    messages.push_back(
        {{"role", "tool"}, {"call_id", pending.call_id}, {"content", pending.result.dump()}});
  }
}

static void save_session(std::shared_ptr<Session> s, const Json& messages) {
  if (!s)
    return;
  s->replace(messages);
}

// Agentic loop: send query -> get response -> execute tool calls -> repeat.
// Terminates when the LLM signals "end", hits a limit, or errors.
Response Runner::run(Llm& llm, const Agent& a, const std::string& query, const RunnerOptions& opts,
                     const RunnerHooks& hooks) {

  if (hooks.on_start)
    hooks.on_start();

  std::unordered_map<std::string, std::shared_ptr<Tool>> tool_map;
  for (const auto& t : a.tools)
    tool_map[t->name()] = t;

  Json req = build_request(a, opts, query);
  int input_tokens = 0;
  int output_tokens = 0;

  for (unsigned int turn = 0; turn < opts.max_turns; ++turn) {
    if (hooks.on_llm_start)
      hooks.on_llm_start(llm, req);

    auto res = llm.complete(req, hooks.on_token);

    if (hooks.on_llm_stop)
      hooks.on_llm_stop(llm, res);

    if (res.contains("usage")) {
      input_tokens += res["usage"].value("input_tokens", 0);
      output_tokens += res["usage"].value("output_tokens", 0);
    }

    auto stop = res.value("stop_reason", "end");

    if (stop == "end") {
      append_assistant_message(req["messages"], res);
      save_session(a.session, req["messages"]);

      if (hooks.on_stop)
        hooks.on_stop();

      return {Response::ok, get_content(res), req["messages"], input_tokens, output_tokens};
    }

    if (stop == "max_tokens") {
      append_assistant_message(req["messages"], res);
      save_session(a.session, req["messages"]);

      if (hooks.on_stop)
        hooks.on_stop();

      return {Response::cancelled, "token limit reached", req["messages"], input_tokens,
              output_tokens};
    }

    // LLM wants to call tools: execute them and loop back for another turn.
    if (stop == "tool_use") {
      append_assistant_message(req["messages"], res);
      execute_tool_calls(opts, hooks, req["messages"], res, tool_map);

      // Compact if over budget.
      if (opts.max_input_tokens > 0 && a.session) {
        int used = res["usage"].value("input_tokens", 0);
        if (used > opts.max_input_tokens) {
          save_session(a.session, req["messages"]);
          a.session->compact(opts.compact_keep);
          req["messages"] = a.session->messages();
        }
      }
      continue;
    }

    save_session(a.session, req["messages"]);

    if (hooks.on_stop)
      hooks.on_stop();

    return {Response::error, "unknown stop reason: " + stop, req["messages"], input_tokens,
            output_tokens};
  }

  save_session(a.session, req["messages"]);

  if (hooks.on_stop)
    hooks.on_stop();

  return {Response::cancelled, "max turns reached", req["messages"], input_tokens, output_tokens};
}

RunnerHooks debug_hooks(std::ostream& os) {
  RunnerHooks h;
  h.on_start = [&os]() { os << "[agt] run started\n"; };
  h.on_llm_start = [&os](const Llm&, const Json& req) {
    size_t n = req.contains("messages") ? req["messages"].size() : 0;
    os << "[agt] llm request (" << n << " messages)\n";
  };
  h.on_llm_stop = [&os](const Llm&, const Json& res) {
    auto reason = res.value("stop_reason", "unknown");
    int in = 0;
    int out = 0;
    if (res.contains("usage")) {
      in = res["usage"].value("input_tokens", 0);
      out = res["usage"].value("output_tokens", 0);
    }
    os << "[agt] llm response: " << reason << " (" << in << "in / " << out << "out)\n";
  };
  h.on_tool_start = [&os](const Tool& t, const Json& args) -> bool {
    auto s = args.dump();
    if (s.size() > 80)
      s = s.substr(0, 77) + "...";
    os << "[agt] tool call: " << t.name() << "(" << s << ")\n";
    return true;
  };
  h.on_tool_stop = [&os](const Tool& t, const Json&, const Json& result) {
    os << "[agt] tool result: " << t.name() << " (" << result.dump().size() << " bytes)\n";
  };
  h.on_stop = [&os]() { os << "[agt] run stopped\n"; };
  return h;
}

} // namespace agt
