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

const uint16_t PV_POWER_ADDRESS = 15208;
const uint16_t PV_VOLTAGE_ADDRESS = 15205;
const uint16_t BATTERY_VOLTAGE_ADDRESS = 15206;
const uint16_t BATTERY_CURRENT_ADDRESS = 25274;
const uint16_t LOAD_CURRENT_ADDRESS = 25212;

std::vector<int> clients;
std::mutex clientsMutex;
std::map<std::string, std::chrono::steady_clock::time_point> validSessions;
std::mutex sessionsMutex;
std::atomic<bool> running(true);

std::map<uint8_t, RelayMetadata> relayMetadata = {
  {1, {"Relay 1", ""}},
  {2, {"Relay 2", ""}},
  {3, {"Relay 3", ""}},
  {4, {"Relay 4", ""}}
};

const std::map<uint16_t, RegisterMetadata> registerMetadata = {
  {20101, {"Inverter offgrid work enable", "", 1.0}},
  {20102, {"Inverter output voltage set", "0.1V", 0.1}},
  {20103, {"Inverter output frequency set", "0.01Hz", 0.01}},
  {20104, {"Inverter search mode enable", "", 1.0}},
  {20108, {"Inverter discharge to grid enable", "", 1.0}},
  {20109, {"Energy use mode", "", 1.0}},
  {20111, {"Grid protect standard", "", 1.0}},
  {20112, {"SolarUse Aim", "", 1.0}},
  {20113, {"Inverter max discharger current", "0.1A", 0.1}},
  {20118, {"Battery stop discharging voltage", "0.1V", 0.1}},
  {20119, {"Battery stop charging voltage", "0.1V", 0.1}},
  {10103, {"Float voltage", "0.1V", 0.1}},
  {10104, {"Absorption voltage", "0.1V", 0.1}},
  {10105, {"Battery low voltage (PV/PH)", "0.1V", 0.1}},
  {10107, {"Battery high voltage (PV/PH)", "0.1V", 0.1}},
  {20125, {"Grid max charger current set", "0.1A", 0.1}},
  {20127, {"Battery low voltage", "0.1V", 0.1}},
  {20128, {"Battery high voltage", "0.1V", 0.1}},
  {20132, {"Max Combine charger current", "0.1A", 0.1}},
  {20142, {"System setting", "", 1.0}},
  {20143, {"Charger source priority", "", 1.0}},
  {20144, {"Solar power balance", "", 1.0}},
  {15205, {"PV voltage", "0.1V", 0.1}},
  {15206, {"Battery voltage", "0.1V", 0.1}},
  {15207, {"PV charger current", "0.1A", 0.1}},
  {15208, {"PV charger power", "W", 1.0}},
  {25212, {"Load current", "0.1A", 0.1}},
  {25274, {"Battery current (negative for charging)", "A", 1.0}}
};

int main(int argc, char* argv[]) {
  uint16_t port = MONITOR_SERVER_PORT;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::stoi(argv[1]));
  }

  int listenSocket = createListeningSocket(port);
  if (listenSocket < 0) {
    return 1;
  }

  std::cout << "TCP monitor server listening on port " << port << "\n";
  std::cout << "Type commands in this console to send them to connected clients. Type 'quit' or 'exit' to stop." << std::endl;

  int commandListenSocket = createListeningSocket(COMMAND_SERVER_PORT);
  if (commandListenSocket < 0) {
    std::cerr << "Unable to start HTTP command server on port " << COMMAND_SERVER_PORT << std::endl;
  } else {
    std::cout << "HTTP command server listening on port " << COMMAND_SERVER_PORT << "\n";
  }

  int authListenSocket = createListeningSocket(AUTHORIZATION_SERVER_PORT);
  if (authListenSocket < 0) {
    std::cerr << "Unable to start authorization server on port " << AUTHORIZATION_SERVER_PORT << std::endl;
  } else {
    std::cout << "Authorization server listening on port " << AUTHORIZATION_SERVER_PORT << "\n";
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

