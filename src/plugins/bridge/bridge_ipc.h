#pragma once
// Cross-process primitives for the bridge: a shared memory region, a request/ack signal pair,
// and helper process management. POSIX uses shm_open + pipes + posix_spawn; Windows uses
// file mappings + events + CreateProcess. (macOS lacks sem_timedwait, hence pipes.)
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wezt::plugins::bridge {

using ArgMap = std::map<std::string, std::string>;

class IpcHost {
 public:
  IpcHost();
  ~IpcHost();
  IpcHost(const IpcHost&) = delete;
  IpcHost& operator=(const IpcHost&) = delete;

  bool create(std::size_t bytes, std::string* error);
  void* data() const;
  // Starts the helper executable with the IPC arguments plus `extra` command-line arguments.
  bool spawn(const std::string& exePath, const std::vector<std::string>& extra, std::string* error);
  bool signalHelper();            // request
  // Waits for the ack. false on timeout or helper exit. Timeout 0 only checks. Retries on EINTR.
  bool waitHelper(int timeoutMs);
  bool helperAlive();
  long childPid() const;          // 0 if none (diagnostics and tests)
  void killChild();               // kills only the helper process; pipes stay open (tests a real crash)
  // Removes the shared-memory name once the helper has attached, so a host that is later killed
  // cannot leave a 16 MB+ segment behind. Existing mappings stay valid.
  void unlinkShm();
  void shutdown();                // kills the helper if still running, releases everything

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class IpcHelper {
 public:
  IpcHelper();
  ~IpcHelper();
  IpcHelper(const IpcHelper&) = delete;
  IpcHelper& operator=(const IpcHelper&) = delete;

  bool attach(const ArgMap& args, std::string* error);
  void* data() const;
  std::size_t size() const;
  // true = a request arrived; false = timeout. `hostGone` is set if the host has died.
  bool waitRequest(int timeoutMs, bool* hostGone);
  bool signalDone();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

ArgMap parseArgs(int argc, char** argv);
std::string quoteWindowsArg(const std::string& arg);  // CreateProcess command-line quoting

}  // namespace wezt::plugins::bridge
