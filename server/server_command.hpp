#pragma once

#include "utils.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

void handleCommandRequest(int clientSocket);
void commandServerLoop(int listenSocket);

inline bool handleHttpReqCommand(const std::string& command, std::string& responseBody) {
  std::string trimmed = command;
  if (!trimmed.empty() && trimmed.back() == '\n') {
    trimmed.pop_back();
  }

  std::istringstream iss(trimmed);
  std::string token;
  if (!(iss >> token) || token != "REQ") {
    return false;
  }

  unsigned chartNo = 0;
  std::size_t pointsCount = 0;
  std::string fromTimestamp;
  std::string toTimestamp;
  if (!(iss >> chartNo >> pointsCount >> fromTimestamp >> toTimestamp)) {
    return false;
  }

  if (!toTimestamp.empty() && toTimestamp.back() == '|') {
    toTimestamp.pop_back();
  }

  const std::string filename = [&]() {
    switch (chartNo) {
      case 1: return std::string("power.dat");
      case 2: return std::string("battery.dat");
      case 3: return std::string("batterycurr.dat");
      case 4: return std::string("loadcurr.dat");
      case 5: return std::string("pvvoltage.dat");
      default: return std::string();
    }
  }();

  if (filename.empty() || pointsCount == 0) {
    return false;
  }

  if (fromTimestamp.empty()) {
    fromTimestamp = "0000-01-01T00:00:00Z";
  }
  if (toTimestamp.empty()) {
    toTimestamp = "9999-12-31T23:59:59Z";
  }

  auto points = readDataFilePointsInRange(filename, fromTimestamp, toTimestamp, pointsCount);
  responseBody.clear();
  for (const auto& point : points) {
    responseBody += point.first + " " + std::to_string(point.second) + "\n";
  }
  return true;
}

inline void handleCommandRequest(int clientSocket) {
  std::vector<char> buffer;
  buffer.reserve(RECV_BUFFER_SIZE);
  ssize_t bytesRead = recv(clientSocket, buffer.data(), RECV_BUFFER_SIZE - 1, 0);
  if (bytesRead <= 0) {
    return;
  }
  std::string request(buffer.data(), static_cast<size_t>(bytesRead));

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
    bytesRead = recv(clientSocket, buffer.data(), RECV_BUFFER_SIZE - 1, 0);
    if (bytesRead <= 0) {
      break;
    }
    body.append(buffer.data(), static_cast<size_t>(bytesRead));
  }
  if (body.size() > contentLength) {
    body.resize(contentLength);
  }
  body = trimString(body);

  if (body.empty()) {
    sendHttpResponse(clientSocket, "400 Bad Request", "Empty command");
    return;
  }

  std::string responseBody;
  if (body.rfind("REQ ", 0) == 0 || body == "REQ") {
    if (handleHttpReqCommand(body, responseBody)) {
      sendHttpResponse(clientSocket, "200 OK", responseBody);
      return;
    }
    sendHttpResponse(clientSocket, "400 Bad Request", "Invalid REQ command");
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

  if (handleHttpReqCommand(command, responseBody)) {
    sendHttpResponse(clientSocket, "200 OK", responseBody);
    return;
  }

  parseCommand(command);
  std::cout << "Broadcasted HTTP command from session " << sessionId << " to " << clients.size() << " client(s): " << command << "\n";

  sendHttpResponse(clientSocket, "200 OK", "OK");
}

inline void commandServerLoop(int listenSocket) {
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
