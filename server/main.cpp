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

static const uint16_t MONITOR_SERVER_PORT = 16670;
static const uint16_t COMMAND_SERVER_PORT = 16671;
static const uint16_t AUTHORIZATION_SERVER_PORT = 16672;
static const int BACKLOG = 5;
static const size_t RECV_BUFFER_SIZE = 1024;

static const uint16_t PV_POWER_ADDRESS = 15208;
static const uint16_t BATTERY_VOLTAGE_ADDRESS = 15206;

std::vector<int> clients;
std::mutex clientsMutex;
std::map<std::string, std::chrono::steady_clock::time_point> validSessions;
std::mutex sessionsMutex;
std::atomic<bool> running(true);

struct RegisterMetadata {
  const char* name;
  const char* unit;
  double multiplier;
};

static const std::map<uint16_t, RegisterMetadata> registerMetadata = {
  {1, {"Relay 1", "", 1.0}},
  {2, {"Relay 2", "", 1.0}},
  {3, {"Relay 3", "", 1.0}},
  {4, {"Relay 4", "", 1.0}},
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
  {25210, {"Inverter current", "0.1A", 0.1}},
  {25274, {"Battery current", "A", 1.0}}
};

int createListeningSocket(uint16_t port) {
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

void closeClient(int clientSocket) {
  if (clientSocket >= 0) {
    close(clientSocket);
  }
}

void removeClient(int clientSocket) {
  std::lock_guard<std::mutex> lock(clientsMutex);
  clients.erase(std::remove(clients.begin(), clients.end(), clientSocket), clients.end());
  closeClient(clientSocket);
}

void broadcastToClients(const std::string& message) {
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

static std::string trimString(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  const size_t start = value.find_first_not_of(whitespace);
  if (start == std::string::npos) {
    return "";
  }
  const size_t end = value.find_last_not_of(whitespace);
  return value.substr(start, end - start + 1);
}

static void sendHttpResponse(int clientSocket, const std::string& status, const std::string& body) {
  std::ostringstream response;
  response << "HTTP/1.1 " << status << "\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
           << "Access-Control-Allow-Headers: Content-Type\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
  const std::string responseStr = response.str();
  send(clientSocket, responseStr.c_str(), static_cast<ssize_t>(responseStr.size()), MSG_NOSIGNAL);
}

static std::string generateSessionId() {
  std::random_device rd;
  std::mt19937_64 rng(rd());
  std::uniform_int_distribution<uint64_t> dist;
  const size_t bytes = 64;
  std::string result;
  result.reserve(bytes * 2);
  for (size_t i = 0; i < bytes / 8; ++i) {
    uint64_t value = dist(rng);
    for (int j = 0; j < 8; ++j) {
      uint8_t byte = static_cast<uint8_t>((value >> ((7 - j) * 8)) & 0xFF);
      const char* hexDigits = "0123456789abcdef";
      result.push_back(hexDigits[byte >> 4]);
      result.push_back(hexDigits[byte & 0x0F]);
    }
  }
  return result;
}

static void removeExpiredSessions() {
  constexpr auto SESSION_LIFETIME = std::chrono::minutes(5);
  static auto lastCleanup = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (now - lastCleanup < std::chrono::minutes(1)) {
    return;
  }
  lastCleanup = now;

  std::lock_guard<std::mutex> lock(sessionsMutex);
  for (auto it = validSessions.begin(); it != validSessions.end(); ) {
    if (now - it->second > SESSION_LIFETIME) {
      std::cout << "Session expired: " << it->first << "\n";
      it = validSessions.erase(it);
    } else {
      ++it;
    }
  }
}

static void handleCommandRequest(int clientSocket) {
  char buffer[RECV_BUFFER_SIZE];
  ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
  if (bytesRead <= 0) {
    return;
  }
  buffer[bytesRead] = '\0';
  std::string request(buffer, static_cast<size_t>(bytesRead));

  const size_t headerEnd = request.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Missing headers");
    return;
  }

  std::string requestLine;
  {
    const size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
      sendHttpResponse(clientSocket, "400 Bad Request", "Invalid request line");
      return;
    }
    requestLine = request.substr(0, lineEnd);
  }

  std::istringstream requestLineStream(requestLine);
  std::string method;
  std::string path;
  requestLineStream >> method >> path;
  if (method == "OPTIONS") {
    sendHttpResponse(clientSocket, "204 No Content", "");
    return;
  }
  if (method != "POST" || path != "/command") {
    sendHttpResponse(clientSocket, "404 Not Found", "Not found");
    return;
  }

  size_t contentLength = 0;
  std::istringstream headerStream(request.substr(request.find("\r\n") + 2, headerEnd));
  std::string headerLine;
  while (std::getline(headerStream, headerLine)) {
    if (!headerLine.empty() && headerLine.back() == '\r') {
      headerLine.pop_back();
    }
    const size_t colonPos = headerLine.find(':');
    if (colonPos != std::string::npos) {
      std::string name = trimString(headerLine.substr(0, colonPos));
      std::string value = trimString(headerLine.substr(colonPos + 1));
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);
      if (name == "content-length") {
        contentLength = static_cast<size_t>(std::stoul(value));
      }
    }
  }

  std::string body = request.substr(headerEnd + 4);
  while (body.size() < contentLength) {
    bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
      break;
    }
    buffer[bytesRead] = '\0';
    body.append(buffer, static_cast<size_t>(bytesRead));
  }
  if (body.size() > contentLength) {
    body.resize(contentLength);
  }
  body = trimString(body);

  if (body.empty()) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Empty command");
    return;
  }

  const size_t sessionSep = body.find(' ');
  if (sessionSep == std::string::npos) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Missing session id");
    return;
  }

  const std::string sessionId = body.substr(0, sessionSep);
  const std::string command = trimString(body.substr(sessionSep + 1));
  if (command.empty()) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Missing command");
    return;
  }

  removeExpiredSessions();

  {
    std::lock_guard<std::mutex> lock(sessionsMutex);
    auto it = validSessions.find(sessionId);
    if (it == validSessions.end()) {
      sendHttpResponse(clientSocket, "401 Unauthorized", "Invalid session");
      std::cout << "Command rejected: invalid session " << sessionId << "\n";
      return;
    }
    it->second = std::chrono::steady_clock::now();
  }

  std::string commandToSend = command;
  if (commandToSend.back() != '\n') {
    commandToSend.push_back('\n');
  }
  broadcastToClients(commandToSend);
  std::cout << "Broadcasted HTTP command from session " << sessionId << " to " << clients.size() << " client(s): " << command << "\n";

  sendHttpResponse(clientSocket, "200 OK", "OK");
}

static void handleAuthorizeRequest(int clientSocket) {
  char clientIp[INET_ADDRSTRLEN] = "unknown";
  sockaddr_storage addr{};
  socklen_t addrLen = sizeof(addr);
  if (getpeername(clientSocket, reinterpret_cast<sockaddr*>(&addr), &addrLen) == 0) {
    if (addr.ss_family == AF_INET) {
      inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in&>(addr).sin_addr, clientIp, sizeof(clientIp));
    } else if (addr.ss_family == AF_INET6) {
      inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6&>(addr).sin6_addr, clientIp, sizeof(clientIp));
    }
  }

  char buffer[RECV_BUFFER_SIZE];
  ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
  if (bytesRead <= 0) {
    return;
  }
  buffer[bytesRead] = '\0';
  std::string request(buffer, static_cast<size_t>(bytesRead));

  const size_t headerEnd = request.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Missing headers");
    std::cout << "Authorization failed from " << clientIp << ": missing headers\n";
    return;
  }

  std::string requestLine;
  {
    const size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
      sendHttpResponse(clientSocket, "400 Bad Request", "Invalid request line");
      std::cout << "Authorization failed from " << clientIp << ": invalid request line\n";
      return;
    }
    requestLine = request.substr(0, lineEnd);
  }

  std::istringstream requestLineStream(requestLine);
  std::string method;
  std::string path;
  requestLineStream >> method >> path;
  if (method == "OPTIONS") {
    sendHttpResponse(clientSocket, "204 No Content", "");
    return;
  }
  if (method != "POST" || path != "/authorize") {
    sendHttpResponse(clientSocket, "404 Not Found", "Not found");
    std::cout << "Authorization failed from " << clientIp << ": unsupported method/path " << method << " " << path << "\n";
    return;
  }

  size_t contentLength = 0;
  std::istringstream headerStream(request.substr(request.find("\r\n") + 2, headerEnd));
  std::string headerLine;
  while (std::getline(headerStream, headerLine)) {
    if (!headerLine.empty() && headerLine.back() == '\r') {
      headerLine.pop_back();
    }
    const size_t colonPos = headerLine.find(':');
    if (colonPos != std::string::npos) {
      std::string name = trimString(headerLine.substr(0, colonPos));
      std::string value = trimString(headerLine.substr(colonPos + 1));
      std::transform(name.begin(), name.end(), name.begin(), ::tolower);
      if (name == "content-length") {
        contentLength = static_cast<size_t>(std::stoul(value));
      }
    }
  }

  std::string body = request.substr(headerEnd + 4);
  while (body.size() < contentLength) {
    bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
      break;
    }
    buffer[bytesRead] = '\0';
    body.append(buffer, static_cast<size_t>(bytesRead));
  }
  if (body.size() > contentLength) {
    body.resize(contentLength);
  }
  body = trimString(body);

  if (body.empty()) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Empty password");
    std::cout << "Authorization failed from " << clientIp << ": empty password\n";
    return;
  }

  std::ifstream authFile("auth.dat");
  if (!authFile) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Missing authorization file");
    std::cout << "Authorization failed from " << clientIp << ": missing auth.dat\n";
    return;
  }

  std::string storedPassword;
  if (!std::getline(authFile, storedPassword)) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Unable to read authorization password");
    std::cout << "Authorization failed from " << clientIp << ": unable to read auth.dat\n";
    return;
  }
  storedPassword = trimString(storedPassword);

  if (body != storedPassword) {
    sendHttpResponse(clientSocket, "401 Unauthorized", "Invalid password");
    std::cout << "Authorization failed from " << clientIp << ": invalid password\n";
    return;
  }

  const std::string sessionId = generateSessionId();
  {
    std::lock_guard<std::mutex> sessionLock(sessionsMutex);
    validSessions[sessionId] = std::chrono::steady_clock::now();
  }

  sendHttpResponse(clientSocket, "200 OK", sessionId);
  std::cout << "Authorization success from " << clientIp << ", session " << sessionId << "\n";
}

static void authorizationServerLoop(int listenSocket) {
  while (running.load()) {
    removeExpiredSessions();
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenSocket, &readSet);
    timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int ready = select(listenSocket + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::perror("select");
      break;
    }

    if (!FD_ISSET(listenSocket, &readSet)) {
      continue;
    }

    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    int clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    if (clientSocket < 0) {
      std::perror("accept");
      continue;
    }

    handleAuthorizeRequest(clientSocket);
    closeClient(clientSocket);
  }
}

static void commandServerLoop(int listenSocket) {
  while (running.load()) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenSocket, &readSet);
    timeval timeout{};
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int ready = select(listenSocket + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::perror("select");
      break;
    }

    if (!FD_ISSET(listenSocket, &readSet)) {
      continue;
    }

    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    int clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
    if (clientSocket < 0) {
      std::perror("accept");
      continue;
    }

    handleCommandRequest(clientSocket);
    closeClient(clientSocket);
  }
}

static std::string jsonEscape(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (char c : input) {
    switch (c) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\b': output += "\\b"; break;
      case '\f': output += "\\f"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output += c; break;
    }
  }
  return output;
}

static double getScaledRegisterValue(uint16_t address, uint16_t value) {
  auto metadataIt = registerMetadata.find(address);
  const double multiplier = metadataIt != registerMetadata.end() ? metadataIt->second.multiplier : 1.0;
  return static_cast<double>(value) * multiplier;
}

static bool saveRegisterValues(const std::map<uint16_t, uint16_t>& lastValues) {
  std::ofstream out("registers.json", std::ofstream::trunc);
  if (!out) {
    std::cerr << "Unable to write registers.json" << std::endl;
    return false;
  }

  out << "[\n";
  bool firstEntry = true;
  for (const auto& [address, value] : lastValues) {
    if (!firstEntry) {
      out << ",\n";
    }
    firstEntry = false;
    auto metadataIt = registerMetadata.find(address);
    const std::string name = metadataIt != registerMetadata.end() ? jsonEscape(metadataIt->second.name) : "";
    const double adjustedValue = getScaledRegisterValue(address, value);
    out << "  {\"address\":" << address
        << ",\"name\":\"" << name << "\""
        << ",\"value\":" << adjustedValue << "}";
  }
  out << "\n]\n";
  return out.good();
}

static bool appendPowerValue(uint16_t value) {
  std::ofstream out("power.dat", std::ofstream::app);
  if (!out) {
    std::cerr << "Unable to open power.dat for appending" << std::endl;
    return false;
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  char timestamp[64];
  if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTime)) == 0) {
    std::cerr << "Unable to format timestamp for power.dat" << std::endl;
    return false;
  }

  const double scaledValue = getScaledRegisterValue(PV_POWER_ADDRESS, value);
  out << timestamp << " " << scaledValue << "\n";
  return out.good();
}

static bool appendBatteryValue(uint16_t value) {
  std::ofstream out("battery.dat", std::ofstream::app);
  if (!out) {
    std::cerr << "Unable to open battery.dat for appending" << std::endl;
    return false;
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  char timestamp[64];
  if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTime)) == 0) {
    std::cerr << "Unable to format timestamp for battery.dat" << std::endl;
    return false;
  }

  const double scaledValue = getScaledRegisterValue(BATTERY_VOLTAGE_ADDRESS, value);
  out << timestamp << " " << scaledValue << "\n";
  return out.good();
}

static bool processCommandFile() {
  std::ifstream in("command.dat");
  if (!in) {
    return false;
  }

  std::string line;
  bool sentAny = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }

    std::string command = line + "\n";
    broadcastToClients(command);
    std::cout << "Sent command.dat line to " << clients.size() << " client(s): " << line << "\n";
    sentAny = true;
  }
  in.close();
  std::remove("command.dat");
  return sentAny;
}

bool updateAddressValue(const std::string& line, std::map<uint16_t, uint16_t>& lastValues) {
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
  uint16_t value = static_cast<uint16_t>(rawValue);
  auto it = lastValues.find(address);
  if (it == lastValues.end() || it->second != value) {
    lastValues[address] = value;
    std::cout << "Value changed: " << address << " = " << value << std::endl;
  }
  return true;
}

void serverLoop(int listenSocket) {
  std::map<int, std::string> partialLines;
  std::map<uint16_t, uint16_t> lastValues;
  auto lastSaveTime = std::chrono::steady_clock::now();

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
    {
      std::lock_guard<std::mutex> lock(clientsMutex);
      for (int clientSocket : clients) {
        if (!FD_ISSET(clientSocket, &readSet)) {
          continue;
        }

        char buffer[RECV_BUFFER_SIZE];
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) {
          staleClients.push_back(clientSocket);
        } else {
          std::string& pending = partialLines[clientSocket];
          pending.append(buffer, static_cast<size_t>(bytesRead));

          size_t newlinePos = 0;
          while ((newlinePos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newlinePos);
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            if (!line.empty()) {
              updateAddressValue(line, lastValues);
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

    processCommandFile();

    auto now = std::chrono::steady_clock::now();
    if (now - lastSaveTime >= std::chrono::seconds(5)) {
      saveRegisterValues(lastValues);
      auto powerIt = lastValues.find(PV_POWER_ADDRESS);
      if (powerIt != lastValues.end()) {
        appendPowerValue(powerIt->second);
      }
      auto batteryIt = lastValues.find(BATTERY_VOLTAGE_ADDRESS);
      if (batteryIt != lastValues.end()) {
        appendBatteryValue(batteryIt->second);
      }
      lastSaveTime = now;
    }
  }
}

void consoleLoop() {
  std::string line;
  while (running.load() && std::getline(std::cin, line)) {
    if (line == "quit" || line == "exit") {
      running.store(false);
      break;
    }

    if (!line.empty()) {
      std::string command = line + "\n";
      broadcastToClients(command);
      std::cout << "Sent command to " << clients.size() << " client(s): " << line << "\n";
    }
  }

  running.store(false);
}

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
