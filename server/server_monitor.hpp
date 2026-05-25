#pragma once

#include "registers.hpp"
#include "rules.hpp"
#include "utils.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int createListeningSocket(uint16_t port);
void closeClient(int clientSocket);
void removeClient(int clientSocket);
void broadcastToClients(const std::string& message);
void parseCommand(const std::string& command);

bool updateRelayStateWithIp(const std::string& line, std::map<uint16_t, int16_t>& lastValues);
bool updateAddressValue(const std::string& line, std::map<uint16_t, int16_t>& lastValues);

void serverLoop(int listenSocket);
void consoleLoop();

inline int createListeningSocket(uint16_t port) {
  int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (listenSocket < 0) {
    std::perror("socket");
    return -1;
  }

  int enableReuse = 1;
  if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &enableReuse, sizeof(enableReuse)) < 0) {
    std::perror("setsockopt");
    close(listenSocket);
    return -1;
  }

  sockaddr_in serverAddr{};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(port);

  if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
    std::perror("bind");
    close(listenSocket);
    return -1;
  }

  if (listen(listenSocket, BACKLOG) < 0) {
    std::perror("listen");
    close(listenSocket);
    return -1;
  }

  return listenSocket;
}

inline void closeClient(int clientSocket) {
  if (clientSocket >= 0) {
    close(clientSocket);
  }
}

inline void removeClient(int clientSocket) {
  std::lock_guard<std::mutex> lock(clientsMutex);
  clients.erase(std::remove(clients.begin(), clients.end(), clientSocket), clients.end());
  closeClient(clientSocket);
}

inline void broadcastToClients(const std::string& message) {
  std::lock_guard<std::mutex> lock(clientsMutex);
  auto it = clients.begin();
  while (it != clients.end()) {
    const int clientSocket = *it;
    ssize_t sent = send(clientSocket, message.c_str(), message.size(), MSG_NOSIGNAL);
    if (sent < 0) {
      std::cerr << "Failed to send to client " << clientSocket << ": " << std::strerror(errno) << "\n";
      closeClient(clientSocket);
      it = clients.erase(it);
    } else {
      ++it;
    }
  }
}

inline void parseCommand(const std::string& command) {
  std::string trimmed = command;
  if (!trimmed.empty() && trimmed.back() == '\n') {
    trimmed.pop_back();
  }

  std::istringstream iss(trimmed);
  std::string token;
  if (iss >> token && token == "RELAY") {
    unsigned relayIndex;
    std::string state;
    if (iss >> relayIndex >> state) {
      std::cout << "Relay switch command received: index=" << relayIndex
                << " state=" << state << "\n";
      std::string systemCommand = "python3 plug_switch.py --index " + std::to_string(relayIndex) +
                                  " --state " + state;
      int rc = std::system(systemCommand.c_str());
      if (rc != 0) {
        std::cerr << "Failed to execute relay helper command: " << systemCommand
                  << " (exit=" << rc << ")\n";
      }
    }
  }

  std::string outgoing = command;
  if (outgoing.empty() || outgoing.back() != '\n') {
    outgoing.push_back('\n');
  }
  broadcastToClients(outgoing);
}

inline bool updateRelayStateWithIp(const std::string& line,
                                   std::map<uint16_t, int16_t>& lastValues) {
  if (line.empty() || line[0] != 'R') {
    return false;
  }

  std::istringstream iss(line.substr(1));
  unsigned long relayNumber = 0;
  unsigned long stateValue = 0;
  std::string ipAddress;
  if (!(iss >> relayNumber >> stateValue >> ipAddress)) {
    return false;
  }
  if (relayNumber == 0 || relayNumber > 4 || stateValue > 1) {
    return false;
  }

  uint8_t relayIndex = static_cast<uint8_t>(relayNumber);
  uint16_t address = static_cast<uint16_t>(relayIndex);
  int16_t value = static_cast<int16_t>(stateValue);
  auto it = lastValues.find(address);
  if (it == lastValues.end() || it->second != value) {
    lastValues[address] = value;
    std::cout << "Relay state changed: " << relayIndex << " = " << value << std::endl;
  }

  auto relayIt = relayMetadata.find(relayIndex);
  if (relayIt != relayMetadata.end() && relayIt->second.ipAddress != ipAddress) {
    relayIt->second.ipAddress = ipAddress;
    std::cout << "Relay " << relayIndex << " IP updated: " << ipAddress << std::endl;
  }
  return true;
}

inline bool updateAddressValue(const std::string& line, std::map<uint16_t, int16_t>& lastValues) {
  std::istringstream iss(line);
  unsigned long rawAddr = 0;
  unsigned long rawValue = 0;
  if (!(iss >> rawAddr >> rawValue)) {
    return false;
  }
  if (rawAddr > 0xFFFF || rawValue > 0xFFFF) {
    return false;
  }

  uint16_t address = static_cast<uint16_t>(rawAddr);
  int16_t value = static_cast<int16_t>(rawValue);
  auto it = lastValues.find(address);
  if (it == lastValues.end() || it->second != value) {
    lastValues[address] = value;
    std::cout << "Value changed: " << address << " = " << value << std::endl;
  }
  return true;
}

inline void serverLoop(int listenSocket) {
  std::map<int, std::string> partialLines;
  std::map<uint16_t, int16_t> lastValues;
  auto lastSaveTime = std::chrono::steady_clock::now();
  loadRules();

  while (running.load()) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenSocket, &readSet);
    int maxFd = listenSocket;

    {
      std::lock_guard<std::mutex> lock(clientsMutex);
      for (int clientSocket : clients) {
        FD_SET(clientSocket, &readSet);
        maxFd = std::max(maxFd, clientSocket);
      }
    }

    timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::perror("select");
      break;
    }

    if (FD_ISSET(listenSocket, &readSet)) {
      sockaddr_in clientAddr{};
      socklen_t clientLen = sizeof(clientAddr);
      int clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
      if (clientSocket >= 0) {
        char clientIp[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        std::cout << "Accepted client " << clientSocket << " from " << clientIp << ":" << ntohs(clientAddr.sin_port) << "\n";
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.push_back(clientSocket);
      } else {
        std::perror("accept");
      }
    }

    std::vector<int> staleClients;
    bool valuesChanged = false;
    {
      std::lock_guard<std::mutex> lock(clientsMutex);
      for (int clientSocket : clients) {
        if (!FD_ISSET(clientSocket, &readSet)) {
          continue;
        }

        std::vector<char> buffer;
        buffer.reserve(RECV_BUFFER_SIZE);
        ssize_t bytesRead = recv(clientSocket, buffer.data(), RECV_BUFFER_SIZE - 1, 0);
        if (bytesRead <= 0) {
          staleClients.push_back(clientSocket);
        } else {
          std::string& pending = partialLines[clientSocket];
          pending.append(buffer.data(), static_cast<size_t>(bytesRead));

          size_t newlinePos = 0;
          while ((newlinePos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newlinePos);
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            if (!line.empty()) {
              if (updateRelayStateWithIp(line, lastValues)) {
                valuesChanged = true;
              } else if (updateAddressValue(line, lastValues)) {
                valuesChanged = true;
              }
            }
            pending.erase(0, newlinePos + 1);
          }
        }
      }
    }

    for (int clientSocket : staleClients) {
      std::cout << "Client " << clientSocket << " disconnected." << std::endl;
      removeClient(clientSocket);
    }

    if (valuesChanged) {
      evaluateRules(lastValues);
    }

    auto now = std::chrono::steady_clock::now();
    if (now - lastSaveTime >= std::chrono::seconds(5)) {
      saveRegisterValues(lastValues);
      auto powerIt = lastValues.find(PV_POWER_ADDRESS);
      if (powerIt != lastValues.end()) {
        appendPowerValue(powerIt->second);
      }
      auto pvVoltageIt = lastValues.find(PV_VOLTAGE_ADDRESS);
      if (pvVoltageIt != lastValues.end()) {
        appendPVVoltageValue(pvVoltageIt->second);
      }
      auto batteryIt = lastValues.find(BATTERY_VOLTAGE_ADDRESS);
      if (batteryIt != lastValues.end()) {
        appendBatteryValue(batteryIt->second);
      }
      auto batteryCurrentIt = lastValues.find(BATTERY_CURRENT_ADDRESS);
      if (batteryCurrentIt != lastValues.end()) {
        appendBatteryCurrentValue(batteryCurrentIt->second);
      }
      auto loadCurrentIt = lastValues.find(LOAD_CURRENT_ADDRESS);
      if (loadCurrentIt != lastValues.end()) {
        appendLoadCurrentValue(loadCurrentIt->second);
      }
      lastSaveTime = now;
    }
  }
}

inline void consoleLoop() {
  std::string line;
  while (running.load() && std::getline(std::cin, line)) {
    if (line == "quit" || line == "exit") {
      running.store(false);
      break;
    }

    if (!line.empty()) {
      std::string command = line + "\n";
      parseCommand(command);
      std::cout << "Sent command to " << clients.size() << " client(s): " << line << "\n";
    }
  }

  running.store(false);
}
