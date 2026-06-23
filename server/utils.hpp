#pragma once

#include "registers.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <ctime>
#include <vector>

extern const uint16_t MONITOR_SERVER_PORT;
extern const uint16_t COMMAND_SERVER_PORT;
extern const uint16_t AUTHORIZATION_SERVER_PORT;
extern const int BACKLOG;
extern const std::size_t RECV_BUFFER_SIZE;

extern std::vector<int> clients;
extern std::mutex clientsMutex;
extern std::map<std::string, std::chrono::steady_clock::time_point> validSessions;
extern std::mutex sessionsMutex;
extern std::atomic<bool> running;

std::string trimString(const std::string& value);
void sendHttpResponse(int clientSocket, const std::string& status, const std::string& body, const std::string& contentType = std::string("text/plain"));
std::string generateSessionId();
void removeExpiredSessions();

std::vector<std::pair<std::string, double>> readDataFilePointsInRange(
    const std::string& filename,
    const std::string& fromTimestamp,
    const std::string& toTimestamp,
    std::size_t maxPoints);

inline std::string trimString(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  const size_t start = value.find_first_not_of(whitespace);
  if (start == std::string::npos) {
    return "";
  }
  const size_t end = value.find_last_not_of(whitespace);
  return value.substr(start, end - start + 1);
}

inline void sendHttpResponse(int clientSocket, const std::string& status, const std::string& body, const std::string& contentType) {
  std::ostringstream response;
  response << "HTTP/1.1 " << status << "\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           << "Access-Control-Allow-Headers: Content-Type\r\n"
           << "Content-Type: " << contentType << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
  const std::string responseStr = response.str();
  send(clientSocket, responseStr.c_str(), static_cast<ssize_t>(responseStr.size()), MSG_NOSIGNAL);
}

inline std::string generateSessionId() {
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

inline void removeExpiredSessions() {
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

inline std::vector<std::pair<std::string, double>> readDataFilePointsInRange(
    const std::string& filename,
    const std::string& fromTimestamp,
    const std::string& toTimestamp,
    std::size_t maxPoints) {
  std::vector<std::pair<std::string, double>> points;
  if (filename.empty() || fromTimestamp.empty() || toTimestamp.empty() || maxPoints == 0) {
    return points;
  }

  std::ifstream in(filename);
  if (!in) {
    std::cerr << "Unable to open " << filename << " for reading" << std::endl;
    return points;
  }

  std::vector<std::pair<std::string, double>> filtered;
  std::string line;
  while (std::getline(in, line)) {
    const std::string trimmed = trimString(line);
    if (trimmed.empty()) {
      continue;
    }

    std::istringstream ss(trimmed);
    std::string timestamp;
    double value = 0.0;
    if (!(ss >> timestamp >> value)) {
      continue;
    }

    if (timestamp < fromTimestamp || timestamp > toTimestamp) {
      continue;
    }

    filtered.emplace_back(timestamp, value);
  }

  if (filtered.empty()) {
    return points;
  }
  if (filtered.size() <= maxPoints) {
    return filtered;
  }

  if (maxPoints == 1) {
    return { filtered.back() };
  }

  points.reserve(maxPoints);
  const double step = static_cast<double>(filtered.size() - 1) / static_cast<double>(maxPoints - 1);
  for (std::size_t index = 0; index < maxPoints; ++index) {
    std::size_t selectedIndex = static_cast<std::size_t>(std::round(index * step));
    if (selectedIndex >= filtered.size()) {
      selectedIndex = filtered.size() - 1;
    }
    points.push_back(filtered[selectedIndex]);
  }

  return points;
}

