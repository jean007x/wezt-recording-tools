// wezt-mcp: the Wezt connector for AI assistants that speak the Model Context Protocol over stdio.
// Usage: wezt-mcp [work-folder]      (default: ~/.wezt/mcp)
// stdout carries protocol messages only; diagnostics go to stderr.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "mcp/server.h"

int main(int argc, char** argv) {
  std::string dir;
  if (argc > 1) dir = argv[1];
  else {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    dir = std::string(home ? home : ".") + "/.wezt/mcp";
  }
  wezt::mcp::Server server(dir);
  std::fprintf(stderr, "wezt-mcp ready; work folder %s\n", dir.c_str());
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const std::string reply = server.handle(line);
    if (!reply.empty()) { std::cout << reply << "\n"; std::cout.flush(); }
  }
  return 0;
}
