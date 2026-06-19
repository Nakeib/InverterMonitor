#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "server_monitor.hpp"
#include "server_command.hpp"
#include "server_authorization.hpp"

const uint16_t MONITOR_SERVER_PORT = 16670;
const uint16_t COMMAND_SERVER_PORT = 16671;
const uint16_t AUTHORIZATION_SERVER_PORT = 16672;
const int BACKLOG = 5;
const size_t RECV_BUFFER_SIZE = 1024;

std::vector<int> clients;
std::mutex clientsMutex;
std::map<std::string, std::chrono::steady_clock::time_point> validSessions;
std::mutex sessionsMutex;
std::atomic<bool> running(true);


int main(int argc, char* argv[]) {
  uint16_t port = MONITOR_SERVER_PORT;
  uint16_t commandPort = COMMAND_SERVER_PORT;
  uint16_t authPort = AUTHORIZATION_SERVER_PORT;

  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }
  if (argc > 2) {
    commandPort = static_cast<uint16_t>(std::stoi(argv[2]));
  }
  if (argc > 3) {
    authPort = static_cast<uint16_t>(std::stoi(argv[3]));
  }

  int listenSocket = createListeningSocket(port);
  if (listenSocket < 0) {
    return 1;
  }

  std::cout << "TCP monitor server listening on port " << port << "\n";
  std::cout << "Type commands in this console to send them to connected clients. Type 'quit' or 'exit' to stop." << std::endl;

  int commandListenSocket = createListeningSocket(commandPort);
  if (commandListenSocket < 0) {
    std::cerr << "Unable to start HTTP command server on port " << commandPort << std::endl;
  } else {
    std::cout << "HTTP command server listening on port " << commandPort << "\n";
  }

  int authListenSocket = createListeningSocket(authPort);
  if (authListenSocket < 0) {
    std::cerr << "Unable to start authorization server on port " << authPort << std::endl;
  } else {
    std::cout << "Authorization server listening on port " << authPort << "\n";
  }

  std::thread serverThread(serverLoop, listenSocket);
  std::thread commandThread;
  if (commandListenSocket >= 0) {
    commandThread = std::thread(commandServerLoop, commandListenSocket);
  }
  std::thread authThread;
  if (authListenSocket >= 0) {
    authThread = std::thread(authorizationServerLoop, authListenSocket);
  }
  std::thread inputThread(consoleLoop);

  inputThread.join();
  running.store(false);
  serverThread.join();
  if (commandThread.joinable()) {
    commandThread.join();
  }
  if (authThread.joinable()) {
    authThread.join();
  }

  {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (int clientSocket : clients) {
      closeClient(clientSocket);
    }
    clients.clear();
  }

  closeClient(listenSocket);
  if (commandListenSocket >= 0) {
    closeClient(commandListenSocket);
  }
  if (authListenSocket >= 0) {
    closeClient(authListenSocket);
  }

  std::cout << "Server stopped." << std::endl;
  return 0;
}

