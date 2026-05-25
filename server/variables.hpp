#pragma once

#include <cstdint>
#include <map>
#include <mutex>

inline std::map<uint32_t, double> variables;
inline std::mutex variablesMutex;

inline void setVariable(uint32_t key, double value) {
  std::lock_guard<std::mutex> lock(variablesMutex);
  variables[key] = value;
}

inline double getVariable(uint32_t key, double defaultValue = 0.0) {
  std::lock_guard<std::mutex> lock(variablesMutex);
  auto it = variables.find(key);
  return it != variables.end() ? it->second : defaultValue;
}

inline void addToVariable(uint32_t key, double valueToAdd) {
  std::lock_guard<std::mutex> lock(variablesMutex);
  variables[key] += valueToAdd;
}

inline void subtractFromVariable(uint32_t key, double valueToSubtract) {
  std::lock_guard<std::mutex> lock(variablesMutex);
  variables[key] -= valueToSubtract;
}
