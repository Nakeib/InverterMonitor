#pragma once

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
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

inline const uint16_t PV_POWER_ADDRESS = 204;
inline const uint16_t PV_VOLTAGE_ADDRESS = 203;
inline const uint16_t BATTERY_VOLTAGE_ADDRESS = 200;
inline const uint16_t BATTERY_CHARGE_CURRENT_ADDRESS = 201;
inline const uint16_t BATTERY_DISCHARGE_CURRENT_ADDRESS = 202;
inline const uint16_t LOAD_POWER_ADDRESS = 205;
inline const uint16_t TEMPERATURE_START_ADDRESS = 10000;

inline std::map<uint8_t, RelayMetadata> relayMetadata = {
  {1, {"Relay 1", ""}},
  {2, {"Relay 2", ""}},
  {3, {"Relay 3", ""}},
  {4, {"Relay 4", ""}}
};

inline const std::map<uint16_t, RegisterMetadata> registerMetadata = {
  {100, {"Battery type", "", 1.0}},
  {101, {"Battery recharge voltage", "V", 1.0}},
  {102, {"Battery redischarge voltage", "V", 1.0}},
  {103, {"Battery under voltage", "V", 1.0}},
  {104, {"Battery bulk voltage", "V", 1.0}},
  {105, {"Battery float voltage", "V", 1.0}},
  {200, {"Battery voltage", "V", 1.0}},
  {201, {"Battery charging current", "A", 1.0}},
  {202, {"Battery discharging current", "A", 1.0}},
  {203, {"PV voltage", "V", 1.0}},
  {204, {"PV input power", "W", 1.0}},
  {205, {"Load power", "W", 1.0}},
  {10000, {"Temperature 1", "C", 1.0}},
  {10001, {"Temperature 2", "C", 1.0}},
  {10002, {"Temperature 3", "C", 1.0}},
  {10003, {"Temperature 4", "C", 1.0}},
};

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

inline double getScaledRegisterValue(uint16_t address, float value) {
  const RelayMetadata* relayInfo = getRelayMetadata(address);
  if (relayInfo) {
    return static_cast<double>(value);
  }
  auto metadataIt = registerMetadata.find(address);
  const double multiplier = metadataIt != registerMetadata.end() ? metadataIt->second.multiplier : 1.0;
  return static_cast<double>(value) * multiplier;
}

inline bool saveRegisterValues(const std::map<uint16_t, float>& lastValues) {
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

inline bool appendBatteryValue(float value) {
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
  out << timestamp << " " << std::fixed << std::setprecision(1) << scaledValue << "\n";
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

  const double scaledValue = getScaledRegisterValue(BATTERY_CHARGE_CURRENT_ADDRESS, value);
  out << timestamp << " " << scaledValue << "\n";
  return out.good();
}

inline bool appendLoadPowerValue(int16_t value) {
  std::ofstream out("loadpower.dat", std::ofstream::app);
  if (!out) {
    std::cerr << "Unable to open loadpower.dat for appending" << std::endl;
    return false;
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  char timestamp[64];
  if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTime)) == 0) {
    std::cerr << "Unable to format timestamp for loadpower.dat" << std::endl;
    return false;
  }

  const double scaledValue = getScaledRegisterValue(LOAD_POWER_ADDRESS, value);
  out << timestamp << " " << scaledValue << "\n";
  return out.good();
}

inline bool appendPVVoltageValue(float value) {
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
  out << timestamp << " " << std::fixed << std::setprecision(1) << scaledValue << "\n";
  return out.good();
}

inline bool appendTemperatureValue(uint16_t sensorIndex, float value) {
  std::ostringstream filename;
  filename << "temperature" << sensorIndex << ".dat";
  std::ofstream out(filename.str(), std::ofstream::app);
  if (!out) {
    std::cerr << "Unable to open " << filename.str() << " for appending" << std::endl;
    return false;
  }

  auto now = std::chrono::system_clock::now();
  std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  char timestamp[64];
  if (std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTime)) == 0) {
    std::cerr << "Unable to format timestamp for " << filename.str() << std::endl;
    return false;
  }

  out << timestamp << " " << std::fixed << std::setprecision(1) << static_cast<double>(value) << "\n";
  return out.good();
}
