#pragma once

#include "registers.hpp"
#include "variables.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

void parseCommand(const std::string& command);

enum class ConditionInputType { Register, Variable, Relay, Time, Unknown };
enum class ConditionOperator { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, Unknown };
enum class CommandType { Send, Enable, Disable, SetVar, AddVar, SubVar, System, Unknown };

struct RuleCondition {
  ConditionInputType inputType = ConditionInputType::Unknown;
  uint32_t inputAddress = 0;
  ConditionOperator op = ConditionOperator::Unknown;
  std::string valueString;
  double valueNumber = 0.0;
  bool valueIsString = false;
};

struct RuleCommand {
  CommandType type = CommandType::Unknown;
  std::string data;
  uint32_t address = 0;
};

struct Rule {
  uint32_t id = 0;
  bool enabled = true;
  std::string name;
  std::vector<RuleCondition> conditions;
  std::vector<RuleCommand> commands;
};

inline std::vector<Rule> rules;
inline std::mutex rulesMutex;
inline std::map<uint32_t, bool> ruleLastState;
inline const char* RULES_FILE = "rules.json";

inline std::string lowerString(const std::string& value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

inline void skipWhitespace(const std::string& data, size_t& pos) {
  while (pos < data.size() && std::isspace(static_cast<unsigned char>(data[pos]))) {
    ++pos;
  }
}

inline bool consumeChar(const std::string& data, size_t& pos, char expected) {
  skipWhitespace(data, pos);
  if (pos < data.size() && data[pos] == expected) {
    ++pos;
    return true;
  }
  return false;
}

inline bool parseJsonString(const std::string& data, size_t& pos, std::string& out) {
  skipWhitespace(data, pos);
  if (pos >= data.size() || data[pos] != '"') {
    return false;
  }
  ++pos;
  out.clear();
  while (pos < data.size()) {
    char c = data[pos++];
    if (c == '"') {
      return true;
    }
    if (c == '\\' && pos < data.size()) {
      char escaped = data[pos++];
      switch (escaped) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(escaped); break;
      }
    } else {
      out.push_back(c);
    }
  }
  return false;
}

inline bool parseJsonNumberAsString(const std::string& data, size_t& pos, std::string& out) {
  skipWhitespace(data, pos);
  size_t start = pos;
  if (pos < data.size() && (data[pos] == '-' || data[pos] == '+')) {
    ++pos;
  }
  bool hasDigits = false;
  while (pos < data.size() && std::isdigit(static_cast<unsigned char>(data[pos]))) {
    hasDigits = true;
    ++pos;
  }
  if (pos < data.size() && data[pos] == '.') {
    ++pos;
    while (pos < data.size() && std::isdigit(static_cast<unsigned char>(data[pos]))) {
      hasDigits = true;
      ++pos;
    }
  }
  if (!hasDigits) {
    return false;
  }
  if (pos < data.size() && (data[pos] == 'e' || data[pos] == 'E')) {
    ++pos;
    if (pos < data.size() && (data[pos] == '+' || data[pos] == '-')) {
      ++pos;
    }
    bool expDigits = false;
    while (pos < data.size() && std::isdigit(static_cast<unsigned char>(data[pos]))) {
      expDigits = true;
      ++pos;
    }
    if (!expDigits) {
      return false;
    }
  }
  out = data.substr(start, pos - start);
  return true;
}

inline bool parseJsonBool(const std::string& data, size_t& pos, bool& out) {
  skipWhitespace(data, pos);
  if (data.compare(pos, 4, "true") == 0) {
    out = true;
    pos += 4;
    return true;
  }
  if (data.compare(pos, 5, "false") == 0) {
    out = false;
    pos += 5;
    return true;
  }
  return false;
}

inline bool parseJsonValueAsString(const std::string& data, size_t& pos, std::string& out, bool& outIsString) {
  skipWhitespace(data, pos);
  if (pos >= data.size()) {
    return false;
  }
  if (data[pos] == '"') {
    outIsString = true;
    return parseJsonString(data, pos, out);
  }
  if (data[pos] == '-' || data[pos] == '+' || std::isdigit(static_cast<unsigned char>(data[pos]))) {
    outIsString = false;
    return parseJsonNumberAsString(data, pos, out);
  }
  bool boolValue = false;
  if (parseJsonBool(data, pos, boolValue)) {
    outIsString = false;
    out = boolValue ? "true" : "false";
    return true;
  }
  if (data.compare(pos, 4, "null") == 0) {
    outIsString = false;
    out.clear();
    pos += 4;
    return true;
  }
  return false;
}

inline int parseTimeStringToSeconds(const std::string& value) {
  std::istringstream iss(value);
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  char colon1 = 0;
  char colon2 = 0;
  if (!(iss >> hours >> colon1 >> minutes)) {
    return -1;
  }
  if (colon1 != ':') {
    return -1;
  }
  if (iss >> colon2 >> seconds) {
    if (colon2 != ':') {
      return -1;
    }
  } else {
    seconds = 0;
  }
  if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59 || seconds < 0 || seconds > 59) {
    return -1;
  }
  return hours * 3600 + minutes * 60 + seconds;
}

inline std::tm utcTimeFromTimeT(std::time_t value) {
  std::tm result{};
#ifdef _WIN32
  gmtime_s(&result, &value);
#else
  gmtime_r(&value, &result);
#endif
  return result;
}

inline ConditionInputType parseConditionInputType(const std::string& value) {
  const std::string lowered = lowerString(value);
  if (lowered == "register") return ConditionInputType::Register;
  if (lowered == "variable") return ConditionInputType::Variable;
  if (lowered == "relay") return ConditionInputType::Relay;
  if (lowered == "time") return ConditionInputType::Time;
  return ConditionInputType::Unknown;
}

inline ConditionOperator parseConditionOperator(const std::string& value) {
  const std::string lowered = lowerString(value);
  if (lowered == "equal") return ConditionOperator::Equal;
  if (lowered == "notequal") return ConditionOperator::NotEqual;
  if (lowered == "less") return ConditionOperator::Less;
  if (lowered == "lessequal") return ConditionOperator::LessEqual;
  if (lowered == "greater") return ConditionOperator::Greater;
  if (lowered == "greaterequal") return ConditionOperator::GreaterEqual;
  return ConditionOperator::Unknown;
}

inline CommandType parseCommandType(const std::string& value) {
  const std::string lowered = lowerString(value);
  if (lowered == "send") return CommandType::Send;
  if (lowered == "enable") return CommandType::Enable;
  if (lowered == "disable") return CommandType::Disable;
  if (lowered == "setvar") return CommandType::SetVar;
  if (lowered == "addvar") return CommandType::AddVar;
  if (lowered == "subvar") return CommandType::SubVar;
  if (lowered == "system") return CommandType::System;
  return CommandType::Unknown;
}

inline void skipJsonValue(const std::string& data, size_t& pos) {
  skipWhitespace(data, pos);
  if (pos >= data.size()) {
    return;
  }
  if (data[pos] == '{') {
    ++pos;
    while (pos < data.size()) {
      skipJsonValue(data, pos);
      skipWhitespace(data, pos);
      if (pos < data.size() && data[pos] == ',') {
        ++pos;
        continue;
      }
      if (pos < data.size() && data[pos] == '}') {
        ++pos;
        break;
      }
      ++pos;
    }
    return;
  }
  if (data[pos] == '[') {
    ++pos;
    while (pos < data.size()) {
      skipJsonValue(data, pos);
      skipWhitespace(data, pos);
      if (pos < data.size() && data[pos] == ',') {
        ++pos;
        continue;
      }
      if (pos < data.size() && data[pos] == ']') {
        ++pos;
        break;
      }
      ++pos;
    }
    return;
  }
  if (data[pos] == '"') {
    std::string unused;
    parseJsonString(data, pos, unused);
    return;
  }
  if (data.compare(pos, 4, "true") == 0 || data.compare(pos, 5, "false") == 0 || data.compare(pos, 4, "null") == 0) {
    bool boolValue = false;
    parseJsonBool(data, pos, boolValue);
    if (data.compare(pos, 4, "null") == 0) {
      pos += 4;
    }
    return;
  }
  std::string number;
  if (parseJsonNumberAsString(data, pos, number)) {
    return;
  }
  ++pos;
}

inline bool parseRuleCondition(const std::string& data, size_t& pos, RuleCondition& condition) {
  if (!consumeChar(data, pos, '{')) {
    return false;
  }
  while (true) {
    skipWhitespace(data, pos);
    if (pos >= data.size()) {
      return false;
    }
    if (data[pos] == '}') {
      ++pos;
      return true;
    }
    std::string key;
    if (!parseJsonString(data, pos, key)) {
      return false;
    }
    if (!consumeChar(data, pos, ':')) {
      return false;
    }
    if (lowerString(key) == "inputtype") {
      std::string value;
      bool isString = false;
      if (!parseJsonValueAsString(data, pos, value, isString)) {
        return false;
      }
      condition.inputType = parseConditionInputType(value);
    } else if (lowerString(key) == "inputaddress") {
      std::string value;
      bool isString = false;
      if (!parseJsonValueAsString(data, pos, value, isString)) {
        return false;
      }
      condition.inputAddress = static_cast<uint32_t>(std::stoul(value));
    } else if (lowerString(key) == "operator") {
      std::string value;
      bool isString = false;
      if (!parseJsonValueAsString(data, pos, value, isString)) {
        return false;
      }
      condition.op = parseConditionOperator(value);
    } else if (lowerString(key) == "value") {
      std::string value;
      bool isString = false;
      if (!parseJsonValueAsString(data, pos, value, isString)) {
        return false;
      }
      condition.valueIsString = isString;
      if (isString) {
        condition.valueString = value;
        if (condition.inputType == ConditionInputType::Time) {
          int seconds = parseTimeStringToSeconds(value);
          condition.valueNumber = seconds >= 0 ? static_cast<double>(seconds) : 0.0;
        } else {
          condition.valueNumber = std::stod(value);
        }
      } else {
        condition.valueString.clear();
        condition.valueNumber = std::stod(value);
      }
    } else {
      skipJsonValue(data, pos);
    }
    skipWhitespace(data, pos);
    if (pos < data.size() && data[pos] == ',') {
      ++pos;
      continue;
    }
  }
}

inline bool parseRuleCommand(const std::string& data, size_t& pos, RuleCommand& command) {
  if (!consumeChar(data, pos, '{')) {
    return false;
  }
  while (true) {
    skipWhitespace(data, pos);
    if (pos >= data.size()) {
      return false;
    }
    if (data[pos] == '}') {
      ++pos;
      return true;
    }
    std::string key;
    if (!parseJsonString(data, pos, key)) {
      return false;
    }
    if (!consumeChar(data, pos, ':')) {
      return false;
    }
    const std::string loweredKey = lowerString(key);
    if (loweredKey == "type") {
      std::string value;
      bool isString = false;
      if (!parseJsonValueAsString(data, pos, value, isString)) {
        return false;
      }
      command.type = parseCommandType(value);
    } else if (loweredKey == "data") {
      std::string value;
      bool isString = false;
      if (!parseJsonValueAsString(data, pos, value, isString)) {
        return false;
      }
      command.data = value;
    } else if (loweredKey == "address") {
      std::string value;
      bool isString = false;
      if (!parseJsonValueAsString(data, pos, value, isString)) {
        return false;
      }
      command.address = static_cast<uint32_t>(std::stoul(value));
    } else {
      skipJsonValue(data, pos);
    }
    skipWhitespace(data, pos);
    if (pos < data.size() && data[pos] == ',') {
      ++pos;
      continue;
    }
  }
}

inline bool parseRuleObject(const std::string& data, size_t& pos, Rule& rule) {
  if (!consumeChar(data, pos, '{')) {
    return false;
  }
  while (true) {
    skipWhitespace(data, pos);
    if (pos >= data.size()) {
      return false;
    }
    if (data[pos] == '}') {
      ++pos;
      return true;
    }
    std::string key;
    if (!parseJsonString(data, pos, key)) {
      return false;
    }
    if (!consumeChar(data, pos, ':')) {
      return false;
    }
    const std::string loweredKey = lowerString(key);
    if (loweredKey == "id") {
      std::string value;
      bool isString = false;
      if (!parseJsonValueAsString(data, pos, value, isString)) {
        return false;
      }
      rule.id = static_cast<uint32_t>(std::stoul(value));
    } else if (loweredKey == "enabled") {
      bool boolValue = false;
      if (!parseJsonBool(data, pos, boolValue)) {
        return false;
      }
      rule.enabled = boolValue;
    } else if (loweredKey == "name") {
      std::string value;
      if (!parseJsonString(data, pos, value)) {
        return false;
      }
      rule.name = value;
    } else if (loweredKey == "conditions") {
      if (!consumeChar(data, pos, '[')) {
        return false;
      }
      rule.conditions.clear();
      while (true) {
        skipWhitespace(data, pos);
        if (pos >= data.size()) {
          return false;
        }
        if (data[pos] == ']') {
          ++pos;
          break;
        }
        RuleCondition condition;
        if (!parseRuleCondition(data, pos, condition)) {
          return false;
        }
        rule.conditions.push_back(condition);
        skipWhitespace(data, pos);
        if (pos < data.size() && data[pos] == ',') {
          ++pos;
          continue;
        }
      }
    } else if (loweredKey == "commands") {
      if (!consumeChar(data, pos, '[')) {
        return false;
      }
      rule.commands.clear();
      while (true) {
        skipWhitespace(data, pos);
        if (pos >= data.size()) {
          return false;
        }
        if (data[pos] == ']') {
          ++pos;
          break;
        }
        RuleCommand command;
        if (!parseRuleCommand(data, pos, command)) {
          return false;
        }
        rule.commands.push_back(command);
        skipWhitespace(data, pos);
        if (pos < data.size() && data[pos] == ',') {
          ++pos;
          continue;
        }
      }
    } else {
      skipJsonValue(data, pos);
    }
    skipWhitespace(data, pos);
    if (pos < data.size() && data[pos] == ',') {
      ++pos;
      continue;
    }
  }
}

inline bool parseRulesArray(const std::string& data, std::vector<Rule>& outRules) {
  size_t pos = 0;
  skipWhitespace(data, pos);
  if (!consumeChar(data, pos, '[')) {
    return false;
  }
  outRules.clear();
  while (true) {
    skipWhitespace(data, pos);
    if (pos >= data.size()) {
      return false;
    }
    if (data[pos] == ']') {
      ++pos;
      return true;
    }
    Rule rule;
    if (!parseRuleObject(data, pos, rule)) {
      return false;
    }
    if (rule.id == 0) {
      uint32_t nextId = 1;
      for (const auto& existing : outRules) {
        nextId = std::max(nextId, existing.id + 1);
      }
      rule.id = nextId;
    }
    outRules.push_back(rule);
    skipWhitespace(data, pos);
    if (pos < data.size() && data[pos] == ',') {
      ++pos;
      continue;
    }
  }
}

inline bool loadRules() {
  std::ifstream in(RULES_FILE);
  if (!in) {
    std::cerr << "Unable to open " << RULES_FILE << " for reading. Starting with no rules." << std::endl;
    std::lock_guard<std::mutex> lock(rulesMutex);
    rules.clear();
    ruleLastState.clear();
    return true;
  }
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<Rule> loaded;
  if (!parseRulesArray(content, loaded)) {
    std::cerr << "Failed to parse " << RULES_FILE << " - ignoring corrupt rules" << std::endl;
    return false;
  }
  std::lock_guard<std::mutex> lock(rulesMutex);
  rules = std::move(loaded);
  ruleLastState.clear();
  for (const auto& rule : rules) {
    std::cout << "Loaded rule " << rule.id << " ('" << rule.name << "')"
              << " enabled=" << (rule.enabled ? "true" : "false") << "\n";
    ruleLastState[rule.id] = false;
  }
  return true;
}

inline bool saveRules(const std::vector<Rule>& currentRules) {
  std::ofstream out(RULES_FILE, std::ofstream::trunc);
  if (!out) {
    std::cerr << "Unable to write " << RULES_FILE << std::endl;
    return false;
  }
  out << "[\n";
  for (size_t index = 0; index < currentRules.size(); ++index) {
    const Rule& rule = currentRules[index];
    out << "  {\n";
    out << "    \"id\":" << rule.id << ",\n";
    out << "    \"enabled\":" << (rule.enabled ? "true" : "false") << ",\n";
    out << "    \"name\":\"" << jsonEscape(rule.name) << "\",\n";
    out << "    \"conditions\":[\n";
    for (size_t ci = 0; ci < rule.conditions.size(); ++ci) {
      const RuleCondition& condition = rule.conditions[ci];
      out << "      {\n";
      out << "        \"inputType\":\"" << jsonEscape([&]() {
        switch (condition.inputType) {
          case ConditionInputType::Register: return std::string("register");
          case ConditionInputType::Variable: return std::string("variable");
          case ConditionInputType::Relay: return std::string("relay");
          case ConditionInputType::Time: return std::string("time");
          default: return std::string("unknown");
        }
      }()) << "\",\n";
      out << "        \"inputAddress\":" << condition.inputAddress << ",\n";
      out << "        \"operator\":\"" << jsonEscape([&]() {
        switch (condition.op) {
          case ConditionOperator::Equal: return std::string("equal");
          case ConditionOperator::NotEqual: return std::string("notEqual");
          case ConditionOperator::Less: return std::string("less");
          case ConditionOperator::LessEqual: return std::string("lessEqual");
          case ConditionOperator::Greater: return std::string("greater");
          case ConditionOperator::GreaterEqual: return std::string("greaterEqual");
          default: return std::string("unknown");
        }
      }()) << "\",\n";
      out << "        \"value\":";
      if (condition.valueIsString) {
        out << "\"" << jsonEscape(condition.valueString) << "\"";
      } else {
        out << condition.valueNumber;
      }
      out << "\n      }";
      if (ci + 1 < rule.conditions.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << "    ],\n";
    out << "    \"commands\":[\n";
    for (size_t ci = 0; ci < rule.commands.size(); ++ci) {
      const RuleCommand& command = rule.commands[ci];
      out << "      {\n";
      out << "        \"type\":\"" << jsonEscape([&]() {
        switch (command.type) {
          case CommandType::Send: return std::string("send");
          case CommandType::Enable: return std::string("enable");
          case CommandType::Disable: return std::string("disable");
          case CommandType::SetVar: return std::string("setVar");
          case CommandType::AddVar: return std::string("addVar");
          case CommandType::SubVar: return std::string("subVar");
          case CommandType::System: return std::string("system");
          default: return std::string("unknown");
        }
      }()) << "\",\n";
      if (!command.data.empty()) {
        if (command.type == CommandType::Send || command.type == CommandType::System) {
          out << "        \"data\":\"" << jsonEscape(command.data) << "\"";
        } else {
          bool numericData = true;
          for (char c : command.data) {
            if (c != '.' && c != '-' && c != '+' && !std::isdigit(static_cast<unsigned char>(c))) {
              numericData = false;
              break;
            }
          }
          if (numericData && !command.data.empty()) {
            out << "        \"data\":" << command.data;
          } else {
            out << "        \"data\":\"" << jsonEscape(command.data) << "\"";
          }
        }
        if (command.address > 0) {
          out << ",\n";
        } else {
          out << "\n";
        }
      }
      if (command.address > 0) {
        out << "        \"address\":" << command.address << "\n";
      }
      out << "      }";
      if (ci + 1 < rule.commands.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << "    ]\n";
    out << "  }";
    if (index + 1 < currentRules.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "]\n";
  return out.good();
}

inline bool addRule(const Rule& rule) {
  std::lock_guard<std::mutex> lock(rulesMutex);
  Rule newRule = rule;
  if (newRule.id == 0) {
    uint32_t nextId = 1;
    for (const auto& existing : rules) {
      nextId = std::max(nextId, existing.id + 1);
    }
    newRule.id = nextId;
  } else {
    for (const auto& existing : rules) {
      if (existing.id == newRule.id) {
        return false;
      }
    }
  }
  rules.push_back(newRule);
  ruleLastState[newRule.id] = false;
  return saveRules(rules);
}

inline bool deleteRule(uint32_t ruleId) {
  std::lock_guard<std::mutex> lock(rulesMutex);
  auto it = std::remove_if(rules.begin(), rules.end(), [&](const Rule& r) { return r.id == ruleId; });
  if (it == rules.end()) {
    return false;
  }
  rules.erase(it, rules.end());
  ruleLastState.erase(ruleId);
  return saveRules(rules);
}

inline bool setRuleEnabledInternal(uint32_t ruleId, bool enabled) {
  for (auto& rule : rules) {
    if (rule.id == ruleId) {
      if (rule.enabled == enabled) {
        return false;
      }
      rule.enabled = enabled;
      if (!enabled) {
        ruleLastState[ruleId] = false;
      }
      return true;
    }
  }
  return false;
}

inline bool setRuleEnabled(uint32_t ruleId, bool enabled) {
  std::lock_guard<std::mutex> lock(rulesMutex);
  if (!setRuleEnabledInternal(ruleId, enabled)) {
    return false;
  }
  return saveRules(rules);
}

inline bool getConditionInputValue(const RuleCondition& condition,
                                   const std::map<uint16_t, int16_t>& lastValues,
                                   double& outValue) {
  switch (condition.inputType) {
    case ConditionInputType::Register: {
      auto it = lastValues.find(static_cast<uint16_t>(condition.inputAddress));
      if (it == lastValues.end()) {
        return false;
      }
      outValue = getScaledRegisterValue(static_cast<uint16_t>(condition.inputAddress), it->second);
      return true;
    }
    case ConditionInputType::Variable: {
      outValue = getVariable(condition.inputAddress);
      return true;
    }
    case ConditionInputType::Relay: {
      auto it = lastValues.find(static_cast<uint16_t>(condition.inputAddress));
      if (it == lastValues.end()) {
        return false;
      }
      outValue = static_cast<double>(it->second);
      return true;
    }
    case ConditionInputType::Time: {
      auto now = std::chrono::system_clock::now();
      std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
      std::tm utcTime = utcTimeFromTimeT(nowTime);
      outValue = utcTime.tm_hour * 3600 + utcTime.tm_min * 60 + utcTime.tm_sec;
      return true;
    }
    default:
      return false;
  }
}

inline bool evaluateConditionValue(double inputValue, const RuleCondition& condition) {
  const double compareValue = condition.valueNumber;
  switch (condition.op) {
    case ConditionOperator::Equal:
      return std::abs(inputValue - compareValue) < 1e-6;
    case ConditionOperator::NotEqual:
      return std::abs(inputValue - compareValue) >= 1e-6;
    case ConditionOperator::Less:
      return inputValue < compareValue;
    case ConditionOperator::LessEqual:
      return inputValue <= compareValue;
    case ConditionOperator::Greater:
      return inputValue > compareValue;
    case ConditionOperator::GreaterEqual:
      return inputValue >= compareValue;
    default:
      return false;
  }
}

inline void executeRuleCommand(const RuleCommand& command, bool& modifiedRules) {
  std::string commandTypeName;
  switch (command.type) {
    case CommandType::Send: commandTypeName = "send"; break;
    case CommandType::Enable: commandTypeName = "enable"; break;
    case CommandType::Disable: commandTypeName = "disable"; break;
    case CommandType::SetVar: commandTypeName = "setVar"; break;
    case CommandType::AddVar: commandTypeName = "addVar"; break;
    case CommandType::SubVar: commandTypeName = "subVar"; break;
    case CommandType::System: commandTypeName = "system"; break;
    default: commandTypeName = "unknown"; break;
  }
  std::cout << "Executing rule command: type=" << commandTypeName;
  if (!command.data.empty()) {
    std::cout << " data='" << command.data << "'";
  }
  if (command.address > 0) {
    std::cout << " address=" << command.address;
  }
  std::cout << std::endl;

  switch (command.type) {
    case CommandType::Send:
      parseCommand(command.data);
      break;
    case CommandType::Enable: {
      uint32_t targetId = 0;
      try {
        targetId = static_cast<uint32_t>(std::stoul(command.data));
      } catch (...) {
        targetId = 0;
      }
      if (targetId > 0 && setRuleEnabledInternal(targetId, true)) {
        modifiedRules = true;
      }
      break;
    }
    case CommandType::Disable: {
      uint32_t targetId = 0;
      try {
        targetId = static_cast<uint32_t>(std::stoul(command.data));
      } catch (...) {
        targetId = 0;
      }
      if (targetId > 0 && setRuleEnabledInternal(targetId, false)) {
        modifiedRules = true;
      }
      break;
    }
    case CommandType::SetVar: {
      double value = 0.0;
      try {
        value = std::stod(command.data);
      } catch (...) {
        value = 0.0;
      }
      setVariable(command.address, value);
      break;
    }
    case CommandType::AddVar: {
      double value = 0.0;
      try {
        value = std::stod(command.data);
      } catch (...) {
        value = 0.0;
      }
      addToVariable(command.address, value);
      break;
    }
    case CommandType::SubVar: {
      double value = 0.0;
      try {
        value = std::stod(command.data);
      } catch (...) {
        value = 0.0;
      }
      subtractFromVariable(command.address, value);
      break;
    }
    case CommandType::System:
      if (!command.data.empty()) {
        int rc = std::system(command.data.c_str());
        if (rc != 0) {
          std::cerr << "System command failed: " << command.data << " (exit=" << rc << ")" << std::endl;
        }
      }
      break;
    default:
      break;
  }
}

inline void evaluateRules(const std::map<uint16_t, int16_t>& lastValues) {
  std::lock_guard<std::mutex> lock(rulesMutex);
  bool rulesModified = false;
  for (auto& rule : rules) {
    if (!rule.enabled) {
      ruleLastState[rule.id] = false;
      continue;
    }
    bool conditionsMet = true;
    for (const auto& condition : rule.conditions) {
      double inputValue = 0.0;
      if (!getConditionInputValue(condition, lastValues, inputValue)) {
        conditionsMet = false;
        break;
      }
      if (!evaluateConditionValue(inputValue, condition)) {
        conditionsMet = false;
        break;
      }
    }
    bool previouslyMet = ruleLastState[rule.id];
    if (conditionsMet && !previouslyMet) {
      for (const auto& command : rule.commands) {
        executeRuleCommand(command, rulesModified);
      }
    }
    ruleLastState[rule.id] = conditionsMet;
  }
  if (rulesModified) {
    saveRules(rules);
  }
}

inline bool enableRule(uint32_t ruleId) {
  return setRuleEnabled(ruleId, true);
}

inline bool disableRule(uint32_t ruleId) {
  return setRuleEnabled(ruleId, false);
}

inline bool executeRuleCommands(uint32_t ruleId) {
  std::lock_guard<std::mutex> lock(rulesMutex);
  for (const auto& rule : rules) {
    if (rule.id == ruleId) {
      bool rulesModified = false;
      for (const auto& command : rule.commands) {
        executeRuleCommand(command, rulesModified);
      }
      if (rulesModified) {
        return saveRules(rules);
      }
      return true;
    }
  }
  return false;
}

inline std::vector<Rule> getRulesSnapshot() {
  std::lock_guard<std::mutex> lock(rulesMutex);
  return rules;
}
