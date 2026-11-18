#include "plugins/bridge/bridge_ipc.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
// ---------------------------------------------------------------------------------------------
// Windows (NOT yet compiled or run: no Windows toolchain was available when this was written)
// ---------------------------------------------------------------------------------------------
#include <windows.h>

namespace wezt::plugins::bridge {
namespace {
std::atomic<unsigned> g_counter{0};
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty() && w.back() == L'\0') w.pop_back();
  return w;
}
std::string quoteArg(const std::string& a) { return quoteWindowsArg(a); }
}  // namespace

struct IpcHost::Impl {
  HANDLE mapping = nullptr, req = nullptr, ack = nullptr;
  PROCESS_INFORMATION pi = {};
  void* view = nullptr;
  std::size_t bytes = 0;
  std::string shmName, reqName, ackName;
};

IpcHost::IpcHost() : impl_(std::make_unique<Impl>()) {}
IpcHost::~IpcHost() { shutdown(); }

bool IpcHost::create(std::size_t bytes, std::string* error) {
  auto& d = *impl_;
  const std::string base = "Local\\wezt-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(g_counter++);
  d.shmName = base + "-shm";
  d.reqName = base + "-req";
  d.ackName = base + "-ack";
  d.bytes = bytes;
  d.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(static_cast<unsigned long long>(bytes) >> 32),
                                 static_cast<DWORD>(bytes & 0xFFFFFFFFu), widen(d.shmName).c_str());
  if (!d.mapping) { if (error) *error = "CreateFileMapping failed"; return false; }
  d.view = MapViewOfFile(d.mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (!d.view) { if (error) *error = "MapViewOfFile failed"; return false; }
  std::memset(d.view, 0, 4096);
  d.req = CreateEventW(nullptr, FALSE, FALSE, widen(d.reqName).c_str());
  d.ack = CreateEventW(nullptr, FALSE, FALSE, widen(d.ackName).c_str());
  if (!d.req || !d.ack) { if (error) *error = "CreateEvent failed"; return false; }
  return true;
}

void* IpcHost::data() const { return impl_->view; }
void IpcHost::unlinkShm() {}  // named mappings vanish with their last handle

bool IpcHost::spawn(const std::string& exePath, const std::vector<std::string>& extra, std::string* error) {
  auto& d = *impl_;
  std::string cmd = quoteArg(exePath) + " --shm " + quoteArg(d.shmName) + " --size " + std::to_string(d.bytes) +
                    " --req " + quoteArg(d.reqName) + " --ack " + quoteArg(d.ackName) + " --parent " +
                    std::to_string(GetCurrentProcessId());
  for (auto& e : extra) cmd += " " + quoteArg(e);
  std::wstring wcmd = widen(cmd);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &d.pi)) {
    if (error) *error = "cannot start helper: " + exePath;
    return false;
  }
  return true;
}

bool IpcHost::signalHelper() { return SetEvent(impl_->req) != 0; }

bool IpcHost::waitHelper(int timeoutMs) {
  HANDLE h[2] = {impl_->ack, impl_->pi.hProcess};
  const DWORD n = impl_->pi.hProcess ? 2 : 1;
  DWORD r = WaitForMultipleObjects(n, h, FALSE, static_cast<DWORD>(timeoutMs));
  return r == WAIT_OBJECT_0;
}

bool IpcHost::helperAlive() {
  return impl_->pi.hProcess && WaitForSingleObject(impl_->pi.hProcess, 0) == WAIT_TIMEOUT;
}
long IpcHost::childPid() const { return static_cast<long>(impl_->pi.dwProcessId); }
void IpcHost::killChild() { if (impl_->pi.hProcess) TerminateProcess(impl_->pi.hProcess, 1); }

void IpcHost::shutdown() {
  auto& d = *impl_;
  if (d.pi.hProcess) {
    if (WaitForSingleObject(d.pi.hProcess, 300) == WAIT_TIMEOUT) TerminateProcess(d.pi.hProcess, 1);
    CloseHandle(d.pi.hProcess);
    CloseHandle(d.pi.hThread);
    d.pi = {};
  }
  if (d.view) { UnmapViewOfFile(d.view); d.view = nullptr; }
  for (HANDLE* h : {&d.mapping, &d.req, &d.ack})
    if (*h) { CloseHandle(*h); *h = nullptr; }
}

struct IpcHelper::Impl {
  HANDLE mapping = nullptr, req = nullptr, ack = nullptr, parent = nullptr;
  void* view = nullptr;
  std::size_t bytes = 0;
};
IpcHelper::IpcHelper() : impl_(std::make_unique<Impl>()) {}
IpcHelper::~IpcHelper() {
  auto& d = *impl_;
  if (d.view) UnmapViewOfFile(d.view);
  for (HANDLE h : {d.mapping, d.req, d.ack, d.parent}) if (h) CloseHandle(h);
}
bool IpcHelper::attach(const ArgMap& a, std::string* error) {
  auto& d = *impl_;
  auto get = [&](const char* k) { auto it = a.find(k); return it == a.end() ? std::string() : it->second; };
  d.bytes = static_cast<std::size_t>(std::stoull(get("size")));
  d.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, widen(get("shm")).c_str());
  if (!d.mapping) { if (error) *error = "cannot open shared memory"; return false; }
  d.view = MapViewOfFile(d.mapping, FILE_MAP_ALL_ACCESS, 0, 0, d.bytes);
  d.req = OpenEventW(EVENT_ALL_ACCESS, FALSE, widen(get("req")).c_str());
  d.ack = OpenEventW(EVENT_ALL_ACCESS, FALSE, widen(get("ack")).c_str());
  if (!d.view || !d.req || !d.ack) { if (error) *error = "cannot open IPC objects"; return false; }
  const std::string parent = get("parent");
  if (!parent.empty()) d.parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoul(parent)));
  return true;
}
void* IpcHelper::data() const { return impl_->view; }
std::size_t IpcHelper::size() const { return impl_->bytes; }
bool IpcHelper::waitRequest(int timeoutMs, bool* hostGone) {
  if (hostGone) *hostGone = false;
  HANDLE h[2] = {impl_->req, impl_->parent};
  const DWORD r = WaitForMultipleObjects(impl_->parent ? 2 : 1, h, FALSE, static_cast<DWORD>(timeoutMs));
  if (r == WAIT_OBJECT_0 + 1 && hostGone) *hostGone = true;  // host process exited
  return r == WAIT_OBJECT_0;
}
bool IpcHelper::signalDone() { return SetEvent(impl_->ack) != 0; }

}  // namespace wezt::plugins::bridge

#else
// ---------------------------------------------------------------------------------------------
// POSIX (Linux, macOS)
// ---------------------------------------------------------------------------------------------
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace wezt::plugins::bridge {
namespace {
std::atomic<unsigned> g_counter{0};
}

struct IpcHost::Impl {
  int shmFd = -1;
  int reqPipe[2] = {-1, -1};  // host writes [1], helper reads [0]
  int ackPipe[2] = {-1, -1};  // helper writes [1], host reads [0]
  pid_t child = -1;
  void* view = nullptr;
  std::size_t bytes = 0;
  std::string shmName;
};

IpcHost::IpcHost() : impl_(std::make_unique<Impl>()) {}
IpcHost::~IpcHost() { shutdown(); }

namespace {
void setCloexec(int fd) {
  const int flags = fcntl(fd, F_GETFD);
  if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
}  // namespace

bool IpcHost::create(std::size_t bytes, std::string* error) {
  auto& d = *impl_;
  // A helper that died leaves the request pipe without a reader. Writing to it would then raise
  // SIGPIPE, whose default action kills the whole host: exactly what the bridge exists to prevent.
  // Ignore it process-wide; write() then just returns EPIPE, which signalHelper() reports.
  static std::once_flag sigpipeOnce;
  std::call_once(sigpipeOnce, [] { std::signal(SIGPIPE, SIG_IGN); });
  d.bytes = bytes;
  d.shmName = "/wezt-" + std::to_string(getpid()) + "-" + std::to_string(g_counter++);
  d.shmFd = shm_open(d.shmName.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (d.shmFd < 0) { if (error) *error = "shm_open failed"; return false; }
  if (ftruncate(d.shmFd, static_cast<off_t>(bytes)) != 0) { if (error) *error = "ftruncate failed"; return false; }
  d.view = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, d.shmFd, 0);
  if (d.view == MAP_FAILED) { d.view = nullptr; if (error) *error = "mmap failed"; return false; }
  std::memset(d.view, 0, 4096);
  if (pipe(d.reqPipe) != 0 || pipe(d.ackPipe) != 0) { if (error) *error = "pipe failed"; return false; }
  // The host's own ends must not leak into the helper (or into any other helper spawned later):
  // if the helper held the write end of its own request pipe it would never see EOF when the host
  // dies, and would live on as an orphan holding the plugin open. The helper's two ends stay
  // inheritable; they are closed in the host right after spawn.
  setCloexec(d.shmFd);
  setCloexec(d.reqPipe[1]);
  setCloexec(d.ackPipe[0]);
  return true;
}

void IpcHost::unlinkShm() {
  auto& d = *impl_;
  if (!d.shmName.empty()) { shm_unlink(d.shmName.c_str()); d.shmName.clear(); }  // mappings stay valid
}

void* IpcHost::data() const { return impl_->view; }

bool IpcHost::spawn(const std::string& exePath, const std::vector<std::string>& extra, std::string* error) {
  auto& d = *impl_;
  std::vector<std::string> args = {exePath, "--shm", d.shmName, "--size", std::to_string(d.bytes),
                                   "--req", std::to_string(d.reqPipe[0]), "--ack", std::to_string(d.ackPipe[1])};
  for (auto& e : extra) args.push_back(e);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  // The pipe ends the helper needs are inherited because pipe() does not set CLOEXEC.
  int rc = posix_spawn(&d.child, exePath.c_str(), nullptr, nullptr, argv.data(), environ);
  if (rc != 0) {
    d.child = -1;
    if (error) *error = "cannot start helper: " + exePath;
    return false;
  }
  // Parent no longer needs the helper's ends.
  close(d.reqPipe[0]); d.reqPipe[0] = -1;
  close(d.ackPipe[1]); d.ackPipe[1] = -1;
  return true;
}

bool IpcHost::signalHelper() {
  const char b = 1;
  ssize_t r;
  do { r = write(impl_->reqPipe[1], &b, 1); } while (r < 0 && errno == EINTR);
  return r == 1;
}

bool IpcHost::waitHelper(int timeoutMs) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    pollfd p = {impl_->ackPipe[0], POLLIN, 0};
    const int left = static_cast<int>(std::max<long long>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count()));
    const int r = poll(&p, 1, left);
    if (r < 0) {
      if (errno == EINTR) continue;  // a signal is not a timeout: wait out the rest of the time
      return false;
    }
    if (r == 0) return false;
    if (!(p.revents & POLLIN)) return false;  // hangup = helper died
    char b;
    ssize_t n;
    do { n = read(impl_->ackPipe[0], &b, 1); } while (n < 0 && errno == EINTR);
    return n == 1;
  }
}

long IpcHost::childPid() const { return static_cast<long>(impl_->child > 0 ? impl_->child : 0); }
void IpcHost::killChild() {
  auto& d = *impl_;
  if (d.child > 0) kill(d.child, SIGKILL);
}

bool IpcHost::helperAlive() {
  auto& d = *impl_;
  if (d.child <= 0) return false;
  int status = 0;
  const pid_t r = waitpid(d.child, &status, WNOHANG);
  if (r == 0) return true;
  d.child = -1;  // reaped
  return false;
}

void IpcHost::shutdown() {
  auto& d = *impl_;
  if (d.child > 0) {
    for (int i = 0; i < 30; ++i) {  // up to ~300 ms to exit on its own
      int status = 0;
      if (waitpid(d.child, &status, WNOHANG) != 0) { d.child = -1; break; }
      usleep(10000);
    }
    if (d.child > 0) {
      kill(d.child, SIGKILL);
      waitpid(d.child, nullptr, 0);
      d.child = -1;
    }
  }
  if (d.view) { munmap(d.view, d.bytes); d.view = nullptr; }
  if (d.shmFd >= 0) { close(d.shmFd); d.shmFd = -1; }
  unlinkShm();
  for (int* fd : {&d.reqPipe[0], &d.reqPipe[1], &d.ackPipe[0], &d.ackPipe[1]})
    if (*fd >= 0) { close(*fd); *fd = -1; }
}

struct IpcHelper::Impl {
  int shmFd = -1, reqFd = -1, ackFd = -1;
  void* view = nullptr;
  std::size_t bytes = 0;
};
IpcHelper::IpcHelper() : impl_(std::make_unique<Impl>()) {}
IpcHelper::~IpcHelper() {
  auto& d = *impl_;
  if (d.view) munmap(d.view, d.bytes);
  if (d.shmFd >= 0) close(d.shmFd);
}

bool IpcHelper::attach(const ArgMap& a, std::string* error) {
  auto& d = *impl_;
  auto get = [&](const char* k) { auto it = a.find(k); return it == a.end() ? std::string() : it->second; };
  if (get("shm").empty() || get("size").empty() || get("req").empty() || get("ack").empty()) {
    if (error) *error = "missing IPC arguments";
    return false;
  }
  d.bytes = static_cast<std::size_t>(std::stoull(get("size")));
  d.reqFd = std::stoi(get("req"));
  d.ackFd = std::stoi(get("ack"));
  d.shmFd = shm_open(get("shm").c_str(), O_RDWR, 0600);
  if (d.shmFd < 0) { if (error) *error = "cannot open shared memory"; return false; }
  d.view = mmap(nullptr, d.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, d.shmFd, 0);
  if (d.view == MAP_FAILED) { d.view = nullptr; if (error) *error = "mmap failed"; return false; }
  return true;
}

void* IpcHelper::data() const { return impl_->view; }
std::size_t IpcHelper::size() const { return impl_->bytes; }

bool IpcHelper::waitRequest(int timeoutMs, bool* hostGone) {
  if (hostGone) *hostGone = false;
  pollfd p = {impl_->reqFd, POLLIN, 0};
  const int r = poll(&p, 1, timeoutMs);
  if (r <= 0) return false;
  if (p.revents & POLLIN) {
    char b;
    if (read(impl_->reqFd, &b, 1) == 1) return true;
  }
  if (hostGone) *hostGone = true;  // hangup / EOF: the host closed its end
  return false;
}

bool IpcHelper::signalDone() {
  const char b = 1;
  return write(impl_->ackFd, &b, 1) == 1;
}

}  // namespace wezt::plugins::bridge
#endif

namespace wezt::plugins::bridge {
// Quotes one argument for CreateProcess following the rules CommandLineToArgvW / the MSVC runtime use:
// a backslash is literal unless it precedes a quote, in which case each one must be doubled, and a
// quote inside the argument must be escaped. Pure string logic, so it is tested on every platform.
std::string quoteWindowsArg(const std::string& a) {
  std::string out = "\"";
  std::size_t bs = 0;
  for (char ch : a) {
    if (ch == '\\') { ++bs; continue; }
    if (ch == '"') { out.append(bs * 2 + 1, '\\'); out.push_back('"'); }
    else { out.append(bs, '\\'); out.push_back(ch); }
    bs = 0;
  }
  out.append(bs * 2, '\\');  // trailing backslashes sit right before the closing quote
  out.push_back('"');
  return out;
}

ArgMap parseArgs(int argc, char** argv) {
  ArgMap m;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k.rfind("--", 0) == 0) k = k.substr(2);
    m[k] = argv[i + 1];
  }
  return m;
}
}  // namespace wezt::plugins::bridge
