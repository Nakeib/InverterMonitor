#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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

struct RegisterMetadata {
  const char* name;
  const char* unit;
  double multiplier;
};

struct RelayMetadata {
  const char* name;
  std::string ipAddress;
};

extern const uint16_t MONITOR_SERVER_PORT;
extern const uint16_t COMMAND_SERVER_PORT;
extern const uint16_t AUTHORIZATION_SERVER_PORT;
extern const int BACKLOG;
extern const std::size_t RECV_BUFFER_SIZE;

extern const uint16_t PV_POWER_ADDRESS;
extern const uint16_t PV_VOLTAGE_ADDRESS;
extern const uint16_t BATTERY_VOLTAGE_ADDRESS;
extern const uint16_t BATTERY_CURRENT_ADDRESS;
extern const uint16_t LOAD_CURRENT_ADDRESS;

extern std::vector<int> clients;
extern std::mutex clientsMutex;
extern std::map<std::string, std::chrono::steady_clock::time_point> validSessions;
extern std::mutex sessionsMutex;
extern std::atomic<bool> running;

extern std::map<uint8_t, RelayMetadata> relayMetadata;
extern const std::map<uint16_t, RegisterMetadata> registerMetadata;

std::string trimString(const std::string& value);
void sendHttpResponse(int clientSocket, const std::string& status, const std::string& body);
std::string generateSessionId();
void removeExpiredSessions();

std::string jsonEscape(const std::string& input);
const RelayMetadata* getRelayMetadata(uint16_t address);
double getScaledRegisterValue(uint16_t address, int16_t value);

bool saveRegisterValues(const std::map<uint16_t, int16_t>& lastValues);
bool appendPowerValue(int16_t value);
bool appendBatteryValue(int16_t value);
bool appendBatteryCurrentValue(int16_t value);
bool appendLoadCurrentValue(int16_t value);
bool appendPVVoltageValue(int16_t value);

inline std::string trimString(const std::string& value) {
  const std::string whitespace = " \t\r\n";
  const size_t start = value.find_first_not_of(whitespace);
  if (start == std::string::npos) {
    return "";
  }
  const size_t end = value.find_last_not_of(whitespace);
  return value.substr(start, end - start + 1);
}

inline void sendHttpResponse(int clientSocket, const std::string& status, const std::string& body) {
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

inline std::string jsonEscape(const std::string& input) {
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

inline const RelayMetadata* getRelayMetadata(uint16_t address) {
  auto it = relayMetadata.find(static_cast<uint8_t>(address));
  return it != relayMetadata.end() ? &it->second : nullptr;
}

inline double getScaledRegisterValue(uint16_t address, int16_t value) {
  const RelayMetadata* relayInfo = getRelayMetadata(address);
  if (relayInfo) {
    return static_cast<double>(value);
  }
  auto metadataIt = registerMetadata.find(address);
  const double multiplier = metadataIt != registerMetadata.end() ? metadataIt->second.multiplier : 1.0;
  return static_cast<double>(value) * multiplier;
}

inline bool saveRegisterValues(const std::map<uint16_t, int16_t>& lastValues) {
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
    const RelayMetadata* relayInfo = getRelayMetadata(address);
    auto metadataIt = registerMetadata.find(address);
    std::string name;
    if (relayInfo) {
      name = jsonEscape(relayInfo->name);
      if (!relayInfo->ipAddress.empty()) {
        name += " (";
        name += jsonEscape(relayInfo->ipAddress);
        name += ")";
      }
    } else {
      name = metadataIt != registerMetadata.end() ? jsonEscape(metadataIt->second.name) : "";
    }
    const double adjustedValue = getScaledRegisterValue(address, value);
    out << "  {\"address\":" << address
        << ",\"name\":\"" << name << "\""
        << ",\"value\":" << adjustedValue << "}";
  }
  out << "\n]\n";
  return out.good();
}

inline bool appendPowerValue(int16_t value) {
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

inline bool appendBatteryValue(int16_t value) {
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

inline bool appendBatteryCurrentValue(int16_t value) {
  std::ofstream out("batterycurr.dat", std::ofstream::app);
  if (!out) {
    std::cerr << "Unable to open batterycurr.dat for appending" << std::endl;
    return false;
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  char timestamp[64];
  if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTime)) == 0) {
    std::cerr << "Unable to format timestamp for batterycurr.dat" << std::endl;
    return false;
  }

  const double scaledValue = getScaledRegisterValue(BATTERY_CURRENT_ADDRESS, value);
  out << timestamp << " " << scaledValue << "\n";
  return out.good();
}

inline bool appendLoadCurrentValue(int16_t value) {
  std::ofstream out("loadcurr.dat", std::ofstream::app);
  if (!out) {
    std::cerr << "Unable to open loadcurr.dat for appending" << std::endl;
    return false;
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  char timestamp[64];
  if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTime)) == 0) {
    std::cerr << "Unable to format timestamp for loadcurr.dat" << std::endl;
    return false;
  }

  const double scaledValue = getScaledRegisterValue(LOAD_CURRENT_ADDRESS, value);
  out << timestamp << " " << scaledValue << "\n";
  return out.good();
}

inline bool appendPVVoltageValue(int16_t value) {
  std::ofstream out("pvvoltage.dat", std::ofstream::app);
  if (!out) {
    std::cerr << "Unable to open pvvoltage.dat for appending" << std::endl;
    return false;
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  char timestamp[64];
  if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTime)) == 0) {
    std::cerr << "Unable to format timestamp for pvvoltage.dat" << std::endl;
    return false;
  }

  const double scaledValue = getScaledRegisterValue(PV_VOLTAGE_ADDRESS, value);
  out << timestamp << " " << scaledValue << "\n";
  return out.good();
}

