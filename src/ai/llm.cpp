#include "ai/llm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
static const sock_t kBadSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
static void closeSock(sock_t s) { ::close(s); }
static const sock_t kBadSock = -1;
#endif

namespace wezt::ai {
namespace {

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  return o;
}

void appendUtf8(std::string& o, unsigned cp) {
  if (cp < 0x80) o += static_cast<char>(cp);
  else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
  else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// Reads the JSON string starting at s[i] == '"'. Returns false if it is not terminated.
bool readJsonString(const std::string& s, std::size_t i, std::string& out) {
  out.clear();
  if (i >= s.size() || s[i] != '"') return false;
  for (++i; i < s.size(); ++i) {
    char c = s[i];
    if (c == '"') return true;
    if (c != '\\') { out += c; continue; }
    if (++i >= s.size()) return false;
    switch (s[i]) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        auto hex4 = [&](std::size_t p, unsigned& v) {
          if (p + 4 > s.size()) return false;
          char* end = nullptr;
          const std::string h = s.substr(p, 4);
          v = static_cast<unsigned>(std::strtoul(h.c_str(), &end, 16));
          return end == h.c_str() + 4;
        };
        unsigned cp = 0;
        if (!hex4(i + 1, cp)) return false;
        i += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
          unsigned lo = 0;
          if (hex4(i + 3, lo) && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
        }
        appendUtf8(out, cp);
        break;
      }
      default: out += s[i];  // \" \\ \/
    }
  }
  return false;
}

// Decodes "Transfer-Encoding: chunked".
bool dechunk(const std::string& in, std::string& out) {
  out.clear();
  std::size_t p = 0;
  for (;;) {
    const std::size_t eol = in.find("\r\n", p);
    if (eol == std::string::npos) return false;
    const unsigned long n = std::strtoul(in.substr(p, eol - p).c_str(), nullptr, 16);
    p = eol + 2;
    if (n == 0) return true;
    if (p + n > in.size()) return false;
    out.append(in, p, n);
    p += n + 2;
  }
}

}  // namespace

bool LocalHttpModel::isLoopback(const std::string& h) { return h == "127.0.0.1" || h == "localhost" || h == "::1"; }

std::string LocalHttpModel::buildRequestBody(const std::string& model, const std::vector<ChatMessage>& chat) {
  std::string b = "{\"model\":\"" + jsonEscape(model) + "\",\"stream\":false,\"temperature\":0.4,\"messages\":[";
  for (std::size_t i = 0; i < chat.size(); ++i) {
    if (i) b += ",";
    b += "{\"role\":\"" + jsonEscape(chat[i].role) + "\",\"content\":\"" + jsonEscape(chat[i].content) + "\"}";
  }
  return b + "]}";
}

bool LocalHttpModel::parseReply(const std::string& body, std::string& text, std::string& error) {
  const std::size_t msg = body.find("\"message\"");
  std::size_t key = body.find("\"content\"", msg == std::string::npos ? 0 : msg);
  if (key == std::string::npos) {
    const std::size_t err = body.find("\"error\"");
    error = err != std::string::npos ? "the model server reported an error" : "the reply had no message";
    return false;
  }
  std::size_t p = body.find(':', key);
  if (p == std::string::npos) { error = "malformed reply"; return false; }
  ++p;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\n' || body[p] == '\t')) ++p;
  if (!readJsonString(body, p, text)) { error = "malformed or empty reply"; return false; }
  return true;
}

bool LocalHttpModel::complete(const std::vector<ChatMessage>& chat, std::string& reply, std::string& error) {
  if (!isLoopback(host_)) { error = "refusing to connect to \"" + host_ + "\": the language model must run on this computer"; return false; }
  if (port_ < 1 || port_ > 65535) { error = "bad port"; return false; }
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { error = "network stack unavailable"; return false; }
#endif
  const bool v6 = host_ == "::1";
  sock_t s = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
  if (s == kBadSock) { error = "cannot create a socket"; return false; }
#ifdef _WIN32
  DWORD tmo = static_cast<DWORD>(timeout_) * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
#else
  timeval tv{timeout_, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
  int rc;
  if (v6) {
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6; a.sin6_port = htons(static_cast<unsigned short>(port_));
    inet_pton(AF_INET6, "::1", &a.sin6_addr);
    rc = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
  } else {
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(static_cast<unsigned short>(port_));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);  // "localhost" is pinned to the loopback address, never resolved
    rc = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
  }
  if (rc != 0) { closeSock(s); error = "no language model is running on port " + std::to_string(port_); return false; }

  const std::string body = buildRequestBody(model_, chat);
  const std::string req = "POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nAccept: application/json\r\nConnection: close\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
  for (std::size_t sent = 0; sent < req.size();) {
#ifdef _WIN32
    const int n = ::send(s, req.data() + sent, static_cast<int>(req.size() - sent), 0);
#else
    const long n = ::send(s, req.data() + sent, req.size() - sent, 0);
#endif
    if (n <= 0) { closeSock(s); error = "could not send the request"; return false; }
    sent += static_cast<std::size_t>(n);
  }
  std::string resp;
  char buf[8192];
  constexpr std::size_t kMax = 4u << 20;
  for (;;) {
#ifdef _WIN32
    const int n = ::recv(s, buf, sizeof buf, 0);
#else
    const long n = ::recv(s, buf, sizeof buf, 0);
#endif
    if (n < 0) { closeSock(s); error = "the model took too long to answer"; return false; }
    if (n == 0) break;
    resp.append(buf, static_cast<std::size_t>(n));
    if (resp.size() > kMax) { closeSock(s); error = "reply too large"; return false; }
  }
  closeSock(s);

  const std::size_t hdrEnd = resp.find("\r\n\r\n");
  if (hdrEnd == std::string::npos || resp.compare(0, 5, "HTTP/") != 0) { error = "not an HTTP reply"; return false; }
  const std::string head = resp.substr(0, hdrEnd);
  std::string payload = resp.substr(hdrEnd + 4);
  const std::size_t sp = head.find(' ');
  const int status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
  if (status != 200) { error = "the model server answered with status " + std::to_string(status); return false; }
  std::string lower = head;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower.find("transfer-encoding: chunked") != std::string::npos) {
    std::string d;
    if (!dechunk(payload, d)) { error = "damaged chunked reply"; return false; }
    payload = std::move(d);
  }
  return parseReply(payload, reply, error);
}

}  // namespace wezt::ai
