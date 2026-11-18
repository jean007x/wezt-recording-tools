#pragma once
// Optional connection to a language model that runs ON THIS COMPUTER (for example a llama.cpp or
// Ollama server). Wezt never contacts anything outside the machine: the address must be a loopback
// address, and that is checked before any connection is made. The model can only produce text; it
// cannot start actions. Actions come only from the assistant's own command parser.
#include <string>
#include <vector>

namespace wezt::ai {

struct ChatMessage {
  std::string role;     // "system", "user" or "assistant"
  std::string content;
};

class ILanguageModel {
 public:
  virtual ~ILanguageModel() = default;
  virtual bool complete(const std::vector<ChatMessage>& chat, std::string& reply, std::string& error) = 0;
};

// Talks to an OpenAI-compatible /v1/chat/completions endpoint over plain HTTP on loopback.
class LocalHttpModel : public ILanguageModel {
 public:
  // host must be "127.0.0.1", "localhost" or "::1"; anything else makes complete() fail without
  // opening a socket.
  LocalHttpModel(std::string host, int port, std::string modelName = "local", int timeoutSeconds = 120)
      : host_(std::move(host)), port_(port), model_(std::move(modelName)), timeout_(timeoutSeconds) {}
  static bool isLoopback(const std::string& host);
  bool complete(const std::vector<ChatMessage>& chat, std::string& reply, std::string& error) override;

  // exposed for tests
  static std::string buildRequestBody(const std::string& model, const std::vector<ChatMessage>& chat);
  static bool parseReply(const std::string& httpBody, std::string& text, std::string& error);

 private:
  std::string host_;
  int port_;
  std::string model_;
  int timeout_;
};

}  // namespace wezt::ai
