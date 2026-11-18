// The assistant: commands, memory, learning that survives a restart, mastering to a file, the
// local-only language model client (against a real loopback server), and its refusal to leave the machine.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
using socklen_t = int;
static void closeSock(sock_t s) { closesocket(s); }
static const sock_t kBadSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static void closeSock(sock_t s) { ::close(s); }
static const sock_t kBadSock = -1;
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <thread>

#include "ai/assistant.h"
#include "ai/audio_file.h"
#include "ai/features.h"
#include "ai/llm.h"
#include "io/wav.h"

using namespace wezt;
using namespace wezt::ai;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, tol) do { const double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("FAIL %s:%d %s=%g vs %g\n", __FILE__, __LINE__, #a, _a, _b); ++failures; } } while (0)
static bool contains(const std::string& s, const std::string& t) { return s.find(t) != std::string::npos; }

namespace {
constexpr double kRate = 44100.0;

void writeNoise(const std::string& path, double secs, double amp, int lowpasses, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> d(-1.f, 1.f);
  AudioBuffer b(2, static_cast<std::size_t>(secs * kRate));
  for (std::size_t c = 0; c < 2; ++c) {
    std::vector<float> x(b.frames);
    for (auto& v : x) v = d(rng);
    for (int p = 0; p < lowpasses; ++p) { float y = 0; for (auto& v : x) { y += 0.2f * (v - y); v = y; } }
    float pk = 0; for (float v : x) pk = std::max(pk, std::fabs(v));
    for (std::size_t i = 0; i < b.frames; ++i) b.channel(c)[i] = static_cast<float>(amp) * x[i] / pk;
  }
  CHECK(io::writeWavFloat(path, b, static_cast<unsigned>(kRate)));
}

// A tiny HTTP server on 127.0.0.1 that answers one request with `response` and records the request.
struct FakeServer {
  sock_t listenFd = kBadSock; int port = 0;
  std::string request;
  std::thread th;
  explicit FakeServer(std::string response) {
    listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = 0; inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    CHECK(::bind(listenFd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
    ::listen(listenFd, 1);
    socklen_t len = sizeof a;
    ::getsockname(listenFd, reinterpret_cast<sockaddr*>(&a), &len);
    port = ntohs(a.sin_port);
    th = std::thread([this, response] {
      const sock_t c = ::accept(listenFd, nullptr, nullptr);
      if (c == kBadSock) return;
      char buf[4096];
      std::string req;
      std::size_t need = std::string::npos;
      for (;;) {
        const long n = static_cast<long>(::recv(c, buf, static_cast<int>(sizeof buf), 0));
        if (n <= 0) break;
        req.append(buf, static_cast<std::size_t>(n));
        const auto he = req.find("\r\n\r\n");
        if (he != std::string::npos) {
          if (need == std::string::npos) { const auto cl = req.find("Content-Length: "); need = he + 4 + (cl != std::string::npos ? static_cast<std::size_t>(std::atoi(req.c_str() + cl + 16)) : 0); }
          if (req.size() >= need) break;
        }
      }
      request = req;
      ::send(c, response.data(), static_cast<int>(response.size()), 0);
      closeSock(c);
    });
  }
  ~FakeServer() { if (th.joinable()) th.join(); closeSock(listenFd); }
};
std::string http200(const std::string& body) { return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body; }

struct FakeModel : ILanguageModel {
  std::vector<ChatMessage> seen;
  bool fail = false;
  bool complete(const std::vector<ChatMessage>& chat, std::string& reply, std::string& error) override {
    seen = chat;
    if (fail) { error = "offline"; return false; }
    reply = "A kick around 60 Hz, then.";
    return true;
  }
};

void testLlmClient() {
  CHECK(LocalHttpModel::isLoopback("127.0.0.1") && LocalHttpModel::isLoopback("localhost") && LocalHttpModel::isLoopback("::1"));
  CHECK(!LocalHttpModel::isLoopback("example.com") && !LocalHttpModel::isLoopback("10.0.0.5") && !LocalHttpModel::isLoopback("127.0.0.1.evil.com") && !LocalHttpModel::isLoopback(""));
  // It must refuse an outside address without trying to connect: the error says so, and returns at once.
  {
    LocalHttpModel m("api.example.com", 443);
    std::string out, err;
    CHECK(!m.complete({{"user", "hi"}}, out, err) && contains(err, "on this computer"));
  }
  // request body escaping
  const std::string body = LocalHttpModel::buildRequestBody("m", {{"user", "say \"hi\"\nnew\\line\t\x01"}});
  CHECK(contains(body, "say \\\"hi\\\"\\nnew\\\\line\\t\\u0001"));
  // reply parsing: escapes, unicode, surrogate pair
  std::string t, e;
  CHECK(LocalHttpModel::parseReply(R"({"choices":[{"message":{"role":"assistant","content":"a\"b\né 🎵"}}]})", t, e));
  CHECK(t == "a\"b\n\xc3\xa9 \xf0\x9f\x8e\xb5");
  CHECK(!LocalHttpModel::parseReply(R"({"error":{"message":"x"}})", t, e));
  CHECK(!LocalHttpModel::parseReply(R"({"choices":[{"message":{"content":"unterminated)", t, e));
  CHECK(!LocalHttpModel::parseReply("", t, e));
  // a real round trip over loopback
  {
    FakeServer srv(http200(R"({"choices":[{"message":{"role":"assistant","content":"hello from the local model"}}]})"));
    LocalHttpModel m("127.0.0.1", srv.port, "tiny");
    std::string out, err;
    CHECK(m.complete({{"system", "be brief"}, {"user", "hi"}}, out, err));
    CHECK(out == "hello from the local model");
    srv.th.join();
    CHECK(contains(srv.request, "POST /v1/chat/completions HTTP/1.1") && contains(srv.request, "\"model\":\"tiny\"") && contains(srv.request, "be brief"));
  }
  {  // chunked transfer encoding
    const std::string j = R"({"choices":[{"message":{"content":"chunky"}}]})";
    char hex[16]; std::snprintf(hex, sizeof hex, "%zx", j.size());
    FakeServer srv("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + std::string(hex) + "\r\n" + j + "\r\n0\r\n\r\n");
    LocalHttpModel m("localhost", srv.port);
    std::string out, err;
    CHECK(m.complete({{"user", "x"}}, out, err) && out == "chunky");
  }
  {  // server errors and junk
    FakeServer srv("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n");
    LocalHttpModel m("127.0.0.1", srv.port);
    std::string out, err;
    CHECK(!m.complete({{"user", "x"}}, out, err) && contains(err, "500"));
  }
  {
    FakeServer srv("garbage");
    LocalHttpModel m("127.0.0.1", srv.port);
    std::string out, err;
    CHECK(!m.complete({{"user", "x"}}, out, err));
  }
  {  // nothing listening
    sock_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t l = sizeof a; ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
    const int port = ntohs(a.sin_port);
    closeSock(fd);
    LocalHttpModel m("127.0.0.1", port);
    std::string out, err;
    CHECK(!m.complete({{"user", "x"}}, out, err) && contains(err, "no language model"));
  }
}

void testAssistant() {
  const fs::path root = fs::temp_directory_path() / "wezt_assistant_test";
  fs::remove_all(root);
  fs::create_directories(root);
  const std::string data = (root / "data").string();
  const std::string dark = (root / "dark_mix.wav").string(), ref = (root / "bright ref.wav").string();
  writeNoise(dark, 8, 0.1, 3, 1);
  writeNoise(ref, 8, 0.7, 0, 2);

  Assistant a(data);
  CHECK(contains(a.say("help").text, "analyze"));
  CHECK(contains(a.say("").text, "listening"));
  CHECK(contains(a.say("blah blah").text, "help"));   // no model connected: tells the user what works

  // memory survives a restart
  CHECK(contains(a.say("remember that I make lo-fi hip hop around 80 bpm").text, "lo-fi"));
  {
    Assistant b(data);
    CHECK(b.notes().size() == 1 && contains(b.say("what do you remember").text, "lo-fi"));
    CHECK(contains(b.say("forget note 1").text, "Forgotten") && b.notes().empty());
    CHECK(contains(b.say("forget note 9").text, "no such note"));
  }
  Assistant c(data);
  CHECK(c.notes().size() == 0);   // the forget was saved too
  c.say("remember that I like a warm low end");

  // analysis
  auto r = c.say("analyze \"" + dark + "\"");
  CHECK(contains(r.text, "LUFS") && contains(r.text, "Stereo width"));
  CHECK(contains(c.say("analyze \"" + (root / "missing.wav").string() + "\"").text, "could not open"));
  CHECK(contains(Assistant(data + "2").say("analyze").text, "Which file"));

  // learning builds profiles; the default is "me", a name makes a reference
  CHECK(contains(c.say("advise \"" + dark + "\"").text, "no profile"));
  CHECK(contains(c.say("learn from \"" + ref + "\" as bright-ref").text, "bright-ref"));
  CHECK(contains(c.say("learn from \"" + dark + "\"").text, "\"me\""));
  CHECK(contains(c.say("profiles").text, "bright-ref") && contains(c.say("profiles").text, "me"));
  CHECK(contains(c.say("show profile bright-ref").text, "1 track"));
  CHECK(contains(c.say("how do I usually master").text, "Profile \"me\""));
  CHECK(contains(c.say("advise \"" + dark + "\" like ../../evil").text, "do not know a profile"));
  CHECK(!fs::exists(root / "evil.wprofile"));

  // advice, then learning from the answers
  r = c.say("advise \"" + dark + "\" like bright-ref");
  CHECK(contains(r.text, "1.") && contains(r.text, "accept"));
  CHECK(!c.pendingSuggestions().empty());
  const std::size_t nSug = c.pendingSuggestions().size();
  const std::string key0 = c.pendingSuggestions()[0].key();
  CHECK(contains(c.say("accept 1 with 2.5").text, "accepted 1"));
  CHECK(c.preferences().record.count(key0) && c.preferences().record.at(key0).first == 1);
  CHECK(contains(c.say("accept 99").text, "Which one"));
  if (nSug > 1) CHECK(contains(c.say("reject 2").text, "declined 1"));
  {
    Assistant d(data);   // learning persisted
    CHECK(d.preferences().record.count(key0));
  }

  // mastering to a delivery target, writing a new file and verifying it
  r = c.say("master \"" + dark + "\" to streaming");
  CHECK(contains(r.text, "saved"));
  const std::string out = (root / "dark_mix_master.wav").string();
  CHECK(fs::exists(out));
  AudioBuffer mb; double sr = 0; std::string err;
  CHECK(loadAudioFile(out, mb, sr, &err));
  const Features mf = analyze(mb, sr);
  NEAR(mf.lufs, -14.0, 0.6);
  CHECK(mf.truePeakDb <= -0.85f);
  CHECK(contains(c.say("master \"" + dark + "\" to streaming").text, "already exists"));   // never overwrites silently
  CHECK(contains(c.say("master \"" + dark + "\" to club as \"" + (root / "club.wav").string() + "\"").text, "saved"));
  CHECK(fs::exists(root / "club.wav"));
  CHECK(contains(c.say("master \"" + dark + "\" to club overwrite as \"" + dark + "\"").text, "already exists"));  // never the original
  CHECK(contains(c.say("master \"" + dark + "\" like nothing-here").text, "do not know a profile"));

  // session commands are handed to the app first
  c.setSessionHandler([](const std::string& t, std::string& rep) { if (t == "play") { rep = "Playing."; return true; } return false; });
  CHECK(c.say("play").text == "Playing.");
  CHECK(contains(c.say("sing me a song").text, "help"));

  // the language model: gets the notes and profile as context, can only speak
  auto fake = std::make_shared<FakeModel>();
  c.setModel(fake);
  auto lr = c.say("how should I EQ my kick?");
  CHECK(lr.usedModel && lr.text == "A kick around 60 Hz, then.");
  CHECK(fake->seen.size() == 2 && fake->seen[0].role == "system");
  CHECK(contains(fake->seen[0].content, "warm low end") && contains(fake->seen[0].content, "Profile \"me\"") && contains(fake->seen[0].content, "cannot run actions"));
  CHECK(!c.say("analyze \"" + dark + "\"").usedModel);   // commands never go to the model
  fake->fail = true;
  auto fr = c.say("tell me about compression");
  CHECK(!fr.usedModel && contains(fr.text, "not reachable") && contains(fr.text, "analyze"));
  fs::remove_all(root);
}

}  // namespace

int main() {
#ifdef _WIN32
  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  testLlmClient();
  testAssistant();
  std::printf(failures ? "FAILED\n" : "assistant tests passed\n");
  return failures;
}
