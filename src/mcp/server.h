#pragma once
// A local tool server that lets an AI assistant which supports the Model Context Protocol (for
// example Claude Desktop, which starts local servers over stdio) drive Wezt: compose a song, add
// parts, split stems, find mix problems, master and export. It speaks JSON-RPC 2.0 on stdin/stdout,
// opens no network port, and writes files only inside its work folder.
#include <memory>
#include <string>

#include "agent/plugin_agent.h"
#include "ai/producer.h"
#include "util/json.h"

namespace wezt::mcp {

class Server {
 public:
  explicit Server(std::string workDir, double sampleRate = 48000.0);
  // One incoming message (a single line of JSON). Returns the reply, or "" when none is due
  // (notifications). Never throws.
  std::string handle(const std::string& line);

  session::Session& session() { return *session_; }

 private:
  json::Value callTool(const std::string& name, const json::Value& args, bool& isError);
  json::Value toolList() const;
  std::string resolveOut(const std::string& p, std::string& err) const;
  std::string resolveIn2(const std::string& p, const std::string& wantExt, std::string& err) const;
  std::string resolveIn(const std::string& p, std::string& err) const;
  void reset();

  std::string work_;
  double sr_;
  std::unique_ptr<session::Session> session_;
  std::unique_ptr<ai::Producer> producer_;
  std::unique_ptr<agent::PluginAgent> agent_;
  double bpm_ = 0;
};

}  // namespace wezt::mcp
