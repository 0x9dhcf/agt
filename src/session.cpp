#include <agt/session.hpp>

namespace agt {

Json MemorySession::messages(int count) const {
  if (count <= 0 || static_cast<size_t>(count) >= messages_.size())
    return messages_;

  auto start = messages_.size() - static_cast<size_t>(count);
  Json arr = Json::array();
  for (size_t i = start; i < messages_.size(); ++i)
    arr.push_back(messages_[i]);
  return arr;
}

void MemorySession::append(const Json &messages) {
  for (const auto &m : messages)
    messages_.push_back(m);
}

void MemorySession::replace(const Json &messages) {
  Json tmp = Json::array();
  for (const auto &m : messages)
    tmp.push_back(m);
  messages_ = std::move(tmp);
}

void MemorySession::clear() noexcept { messages_ = Json::array(); }

void MemorySession::compact(int keep) {
  auto sz = static_cast<int>(messages_.size());
  if (keep <= 0 || keep >= sz)
    return;

  // Start cutting at (size - keep), then walk forward past any orphaned
  // tool results so we don't split a tool_use / tool_result pair.
  auto cut = static_cast<size_t>(sz - keep);
  while (cut < messages_.size() && messages_[cut].value("role", "") == "tool")
    ++cut;

  Json trimmed = Json::array();
  for (size_t i = cut; i < messages_.size(); ++i)
    trimmed.push_back(messages_[i]);
  messages_ = std::move(trimmed);
}

} // namespace agt
