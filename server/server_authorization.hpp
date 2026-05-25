#pragma once

#include "utils.hpp"
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

void handleAuthorizeRequest(int clientSocket);
void authorizationServerLoop(int listenSocket);

inline void handleAuthorizeRequest(int clientSocket) {
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

inline void authorizationServerLoop(int listenSocket) {
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
