#pragma once

#include "rules.hpp"
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

inline std::string jsonEscapeLocal(const std::string& input) {
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

inline std::string rulesToJson(const std::vector<Rule>& rules) {
  std::ostringstream out;
  out << "[";
  for (size_t index = 0; index < rules.size(); ++index) {
    const Rule& rule = rules[index];
    out << "{"
        << "\"id\":" << rule.id << ","
        << "\"enabled\":" << (rule.enabled ? "true" : "false") << ","
        << "\"name\":\"" << jsonEscapeLocal(rule.name) << "\",";
    out << "\"conditions\":[";
    for (size_t ci = 0; ci < rule.conditions.size(); ++ci) {
      const RuleCondition& condition = rule.conditions[ci];
      out << "{";
      out << "\"inputType\":\"";
      switch (condition.inputType) {
        case ConditionInputType::Register: out << "register"; break;
        case ConditionInputType::Variable: out << "variable"; break;
        case ConditionInputType::Relay: out << "relay"; break;
        case ConditionInputType::Time: out << "time"; break;
        default: out << "unknown"; break;
      }
      out << "\",";
      out << "\"inputAddress\":" << condition.inputAddress << ",";
      out << "\"operator\":\"";
      switch (condition.op) {
        case ConditionOperator::Equal: out << "equal"; break;
        case ConditionOperator::NotEqual: out << "notEqual"; break;
        case ConditionOperator::Less: out << "less"; break;
        case ConditionOperator::LessEqual: out << "lessEqual"; break;
        case ConditionOperator::Greater: out << "greater"; break;
        case ConditionOperator::GreaterEqual: out << "greaterEqual"; break;
        default: out << "unknown"; break;
      }
      out << "\",";
      if (condition.valueInputType != ConditionInputType::Unknown) {
        out << "\"valueType\":\"";
        switch (condition.valueInputType) {
          case ConditionInputType::Register: out << "register"; break;
          case ConditionInputType::Variable: out << "variable"; break;
          case ConditionInputType::Relay: out << "relay"; break;
          case ConditionInputType::Time: out << "time"; break;
          default: out << "unknown"; break;
        }
        out << "\",";
        out << "\"valueAddress\":" << condition.valueInputAddress;
      } else {
        out << "\"value\":";
        if (condition.valueIsString) {
          out << "\"" << jsonEscapeLocal(condition.valueString) << "\"";
        } else {
          out << condition.valueNumber;
        }
      }
      out << "}";
      if (ci + 1 < rule.conditions.size()) {
        out << ",";
      }
    }
    out << "],";
    out << "\"commands\":[";
    for (size_t ci = 0; ci < rule.commands.size(); ++ci) {
      const RuleCommand& command = rule.commands[ci];
      out << "{";
      out << "\"type\":\"";
      switch (command.type) {
        case CommandType::Send: out << "send"; break;
        case CommandType::Enable: out << "enable"; break;
        case CommandType::Disable: out << "disable"; break;
        case CommandType::SetVar: out << "setVar"; break;
        case CommandType::AddVar: out << "addVar"; break;
        case CommandType::SubVar: out << "subVar"; break;
        case CommandType::System: out << "system"; break;
        default: out << "unknown"; break;
      }
      out << "\"";
      if (!command.data.empty()) {
        out << ",\"data\":\"" << jsonEscapeLocal(command.data) << "\"";
      }
      if (command.address > 0) {
        out << ",\"address\":" << command.address;
      }
      out << "}";
      if (ci + 1 < rule.commands.size()) {
        out << ",";
      }
    }
    out << "]";
    out << "}";
    if (index + 1 < rules.size()) {
      out << ",";
    }
  }
  out << "]";
  return out.str();
}

inline bool handleHttpReqCommand(const std::string& command, std::string& responseBody, bool authorized = false) {
  std::string trimmed = command;
  if (!trimmed.empty() && trimmed.back() == '\n') {
    trimmed.pop_back();
  }

  std::istringstream iss(trimmed);
  std::string token;
  if (!(iss >> token)) {
    return false;
  }

  if (token == "GETRULES") {
    responseBody = rulesToJson(getRulesSnapshot());
    return true;
  }

  if (token == "RELOADRULES") {
    if (!authorized) {
      return false;
    }
    if (loadRules()) {
      responseBody = "OK";
      return true;
    }
    responseBody = "Failed to reload rules";
    return false;
  }

  if (token == "ENABLERULE" || token == "DISABLERULE") {
    if (!authorized) {
      return false;
    }
    unsigned long ruleNo = 0;
    if (!(iss >> ruleNo) || ruleNo == 0) {
      return false;
    }
    const bool result = (token == "ENABLERULE") ? enableRule(static_cast<uint32_t>(ruleNo))
                                                  : disableRule(static_cast<uint32_t>(ruleNo));
    if (result) {
      responseBody = "OK";
      return true;
    }
    responseBody = std::string("Failed to ") + (token == "ENABLERULE" ? "enable" : "disable") + " rule";
    return false;
  }

  if (token == "EXECUTERULE") {
    if (!authorized) {
      return false;
    }
    unsigned long ruleNo = 0;
    if (!(iss >> ruleNo) || ruleNo == 0) {
      return false;
    }
    if (executeRuleCommands(static_cast<uint32_t>(ruleNo))) {
      responseBody = "OK";
      return true;
    }
    responseBody = "Failed to execute rule";
    return false;
  }

  if (token == "SETVAR") {
    if (!authorized) {
      return false;
    }
    unsigned long address = 0;
    double value = 0.0;
    if (!(iss >> address >> value)) {
      return false;
    }
    setVariable(static_cast<uint32_t>(address), value);
    responseBody = "OK";
    return true;
  }

  if (token != "REQ") {
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
      case 4: return std::string("loadpower.dat");
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

  size_t contentLength = 0;
  std::string contentType;
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
      } else if (name == "content-type") {
        contentType = value;
      }
    }
  }

  if (method == "POST" && path == "/upload_firmware") {
    if (contentLength == 0 || contentType.find("multipart/form-data") == std::string::npos) {
      sendHttpResponse(clientSocket, "400 Bad Request", "Invalid upload request");
      return;
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

    const std::string boundaryPrefix = "boundary=";
    const size_t boundaryPos = contentType.find(boundaryPrefix);
    if (boundaryPos == std::string::npos) {
      sendHttpResponse(clientSocket, "400 Bad Request", "Missing boundary");
      return;
    }
    std::string boundary = "--" + contentType.substr(boundaryPos + boundaryPrefix.size());
    if (!boundary.empty() && boundary.back() == '\r') {
      boundary.pop_back();
    }

    auto extractPart = [&](const std::string& data, const std::string& fieldName, std::string& outValue, std::string& outFilename) {
      const std::string fieldSearch = "name=\"" + fieldName + "\"";
      size_t partPos = data.find(fieldSearch);
      if (partPos == std::string::npos) {
        return false;
      }
      size_t headerStart = data.rfind(boundary, partPos);
      if (headerStart == std::string::npos) {
        return false;
      }
      size_t headerEndPos = data.find("\r\n\r\n", partPos);
      if (headerEndPos == std::string::npos) {
        return false;
      }
      size_t contentStart = headerEndPos + 4;
      size_t nextBoundary = data.find(boundary, contentStart);
      if (nextBoundary == std::string::npos) {
        return false;
      }
      outValue = data.substr(contentStart, nextBoundary - contentStart - 2);
      const std::string filenameSearch = "filename=\"";
      size_t filenamePos = data.rfind(filenameSearch, partPos);
      if (filenamePos != std::string::npos && filenamePos < partPos) {
        size_t filenameEnd = data.find('"', filenamePos + filenameSearch.size());
        if (filenameEnd != std::string::npos) {
          outFilename = data.substr(filenamePos + filenameSearch.size(), filenameEnd - (filenamePos + filenameSearch.size()));
        }
      }
      return true;
    };

    std::string sessionId;
    std::string fileName;
    std::string fileContent;
    if (!extractPart(body, "sessionId", sessionId, fileName)) {
      sendHttpResponse(clientSocket, "400 Bad Request", "Missing sessionId field");
      return;
    }
    if (!extractPart(body, "firmware", fileContent, fileName)) {
      sendHttpResponse(clientSocket, "400 Bad Request", "Missing firmware file");
      return;
    }
    sessionId = trimString(sessionId);
    if (sessionId.empty()) {
      sendHttpResponse(clientSocket, "400 Bad Request", "Invalid sessionId");
      return;
    }

    removeExpiredSessions();
    {
      std::lock_guard<std::mutex> lock(sessionsMutex);
      auto it = validSessions.find(sessionId);
      if (it == validSessions.end()) {
        sendHttpResponse(clientSocket, "401 Unauthorized", "Invalid session");
        std::cout << "Firmware upload rejected: invalid session " << sessionId << "\n";
        return;
      }
      it->second = std::chrono::steady_clock::now();
    }

    const std::string outputFile = "firmware.bin";
    std::ofstream out(outputFile, std::ios::binary);
    if (!out) {
      sendHttpResponse(clientSocket, "500 Internal Server Error", "Unable to save firmware");
      return;
    }
    out.write(fileContent.data(), static_cast<std::streamsize>(fileContent.size()));
    if (!out) {
      sendHttpResponse(clientSocket, "500 Internal Server Error", "Write failed");
      return;
    }
    out.close();

    std::cout << "Firmware uploaded: " << outputFile << " (" << fileContent.size() << " bytes)\n";
    sendHttpResponse(clientSocket, "200 OK", "Firmware uploaded successfully");
    return;
  }

  if (method != "POST" || path != "/command") {
    sendHttpResponse(clientSocket, "404 Not Found", "Not found");
    return;
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
  if (body.rfind("REQ", 0) == 0 || body == "REQ")
  {
    if (handleHttpReqCommand(body, responseBody, false)) {
      sendHttpResponse(clientSocket, "200 OK", responseBody);
      return;
    }
    sendHttpResponse(clientSocket, "400 Bad Request", "Invalid REQ command");
    return;
  }
  else if (body == "GETRULES") {
    if (handleHttpReqCommand(body, responseBody, false)) {
      sendHttpResponse(clientSocket, "200 OK", responseBody);
      return;
    }
    sendHttpResponse(clientSocket, "400 Bad Request", "Invalid GETRULES command");
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

  if (handleHttpReqCommand(command, responseBody, true)) {
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
