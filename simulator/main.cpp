#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

static const int WIFI_SSID_MAX_LEN = 32;
static const int WIFI_PASSWORD_MAX_LEN = 64;
static const int WIFI_SSID_EEPROM_OFFSET = 0;
static const int WIFI_PASSWORD_EEPROM_OFFSET = WIFI_SSID_EEPROM_OFFSET + WIFI_SSID_MAX_LEN + 1;
static const int MONITOR_HOST_MAX_LEN = 64;
static const int MONITOR_PORT_MAX_LEN = 6;
static const int MONITOR_HOST_EEPROM_OFFSET = WIFI_PASSWORD_EEPROM_OFFSET + WIFI_PASSWORD_MAX_LEN + 1;
static const int MONITOR_PORT_EEPROM_OFFSET = MONITOR_HOST_EEPROM_OFFSET + MONITOR_HOST_MAX_LEN + 1;
static const int EEPROM_SIZE = MONITOR_PORT_EEPROM_OFFSET + MONITOR_PORT_MAX_LEN + 1;

static const uint16_t DEFAULT_MONITOR_PORT = 12345;
static const char DEFAULT_MONITOR_HOST[] = "127.0.0.1";
static const unsigned long UPDATE_INTERVAL_MS = 2000;

struct RegItem {
  uint16_t address;
  const char* name;
  const char* unit;
};

static const RegItem controlRegisters[] = {
  {20101, "Inverter offgrid work enable", ""},
  {20102, "Inverter output voltage set", "0.1V"},
  {20103, "Inverter output frequency set", "0.01Hz"},
  {20104, "Inverter search mode enable", ""},
  {20108, "Inverter discharge to grid enable", ""},
  {20109, "Energy use mode", ""},
  {20111, "Grid protect standard", ""},
  {20112, "SolarUse Aim", ""},
  {20113, "Inverter max discharger current", "0.1A"},
  {20118, "Battery stop discharging voltage", "0.1V"},
  {20119, "Battery stop charging voltage", "0.1V"},
  {10103, "Float voltage", "0.1V"},
  {10104, "Absorption voltage", "0.1V"},
  {10105, "Battery low voltage (PV/PH)", "0.1V"},
  {10107, "Battery high voltage (PV/PH)", "0.1V"},
  {20125, "Grid max charger current set", "0.1A"},
  {20127, "Battery low voltage", "0.1V"},
  {20128, "Battery high voltage", "0.1V"},
  {20132, "Max Combine charger current", "0.1A"},
  {20142, "System setting", ""},
  {20143, "Charger source priority", ""},
  {20144, "Solar power balance", ""}
};

static const RegItem monitorRegisters[] = {
  {15205, "PV voltage", "0.1V"},
  {15206, "Battery voltage", "0.1V"},
  {15207, "PV charger current", "0.1A"},
  {15208, "PV charger power", "W"},
  {25210, "Inverter current", "0.1A"},
  {25274, "Battery current", "A"}
};

struct EEPROMFile {
  std::filesystem::path path;
  EEPROMFile(const std::filesystem::path& filePath) : path(filePath) {
    if (!std::filesystem::exists(path)) {
      std::ofstream out(path, std::ios::binary);
      std::vector<uint8_t> buffer(EEPROM_SIZE, 0xFF);
      out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    }
  }

  bool readBytes(int offset, int count, uint8_t* dest) {
    if (offset < 0 || count < 0 || offset + count > EEPROM_SIZE) {
      return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return false;
    }
    in.seekg(offset, std::ios::beg);
    in.read(reinterpret_cast<char*>(dest), count);
    return in.good();
  }

  bool writeBytes(int offset, int count, const uint8_t* src) {
    if (offset < 0 || count < 0 || offset + count > EEPROM_SIZE) {
      return false;
    }
    std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) {
      return false;
    }
    io.seekp(offset, std::ios::beg);
    io.write(reinterpret_cast<const char*>(src), count);
    return io.good();
  }
};

static EEPROMFile* eeprom = nullptr;
static char wifiSSID[WIFI_SSID_MAX_LEN + 1] = "";
static char wifiPassword[WIFI_PASSWORD_MAX_LEN + 1] = "";
static char monitorHost[MONITOR_HOST_MAX_LEN + 1] = "";
static char monitorPortString[MONITOR_PORT_MAX_LEN + 1] = "";
static uint16_t monitorPort = 0;
static std::atomic<bool> running(true);
static std::unordered_map<uint16_t, uint16_t> controlRegisterValues;
static std::mt19937 randomEngine(static_cast<unsigned long>(std::chrono::steady_clock::now().time_since_epoch().count()));
static std::uniform_real_distribution<double> noiseDistribution(-1.0, 1.0);

void readEEPROMString(int start, int maxLen, char* dest) {
  std::vector<uint8_t> buffer(maxLen, 0xFF);
  if (!eeprom->readBytes(start, maxLen, buffer.data())) {
    dest[0] = '\0';
    return;
  }

  for (int i = 0; i < maxLen; ++i) {
    if (buffer[i] == 0 || buffer[i] == 0xFF) {
      dest[i] = '\0';
      return;
    }
    dest[i] = static_cast<char>(buffer[i]);
  }
  dest[maxLen - 1] = '\0';
}

void writeEEPROMString(int start, int maxLen, const char* src) {
  std::vector<uint8_t> buffer(maxLen, 0);
  int i = 0;
  while (i < maxLen && src[i] != '\0') {
    buffer[i] = static_cast<uint8_t>(src[i]);
    ++i;
  }
  if (i < maxLen) {
    buffer[i] = 0;
  }
  eeprom->writeBytes(start, maxLen, buffer.data());
}

bool loadMonitorServerFromEEPROM() {
  readEEPROMString(MONITOR_HOST_EEPROM_OFFSET, MONITOR_HOST_MAX_LEN + 1, monitorHost);
  readEEPROMString(MONITOR_PORT_EEPROM_OFFSET, MONITOR_PORT_MAX_LEN + 1, monitorPortString);
  monitorPort = static_cast<uint16_t>(std::strtoul(monitorPortString, nullptr, 10));
  return monitorHost[0] != '\0' && monitorPort != 0;
}

bool loadWiFiCredentialsFromEEPROM() {
  readEEPROMString(WIFI_SSID_EEPROM_OFFSET, WIFI_SSID_MAX_LEN + 1, wifiSSID);
  readEEPROMString(WIFI_PASSWORD_EEPROM_OFFSET, WIFI_PASSWORD_MAX_LEN + 1, wifiPassword);
  return wifiSSID[0] != '\0' && wifiPassword[0] != '\0';
}

bool saveWiFiCredentialsToEEPROM(const char* ssid, const char* password) {
  if (std::strlen(ssid) > WIFI_SSID_MAX_LEN || std::strlen(password) > WIFI_PASSWORD_MAX_LEN) {
    return false;
  }
  writeEEPROMString(WIFI_SSID_EEPROM_OFFSET, WIFI_SSID_MAX_LEN, ssid);
  writeEEPROMString(WIFI_PASSWORD_EEPROM_OFFSET, WIFI_PASSWORD_MAX_LEN, password);
  return true;
}

bool saveMonitorServerToEEPROM(const char* host, const char* portString) {
  if (std::strlen(host) > MONITOR_HOST_MAX_LEN || std::strlen(portString) > MONITOR_PORT_MAX_LEN) {
    return false;
  }
  writeEEPROMString(MONITOR_HOST_EEPROM_OFFSET, MONITOR_HOST_MAX_LEN, host);
  writeEEPROMString(MONITOR_PORT_EEPROM_OFFSET, MONITOR_PORT_MAX_LEN, portString);
  return true;
}

bool isWritableControlRegister(uint16_t address) {
  for (const RegItem& reg : controlRegisters) {
    if (reg.address == address) {
      return true;
    }
  }
  return false;
}

void trimString(std::string& value) {
  const char* whitespace = " \t\r\n";
  const auto start = value.find_first_not_of(whitespace);
  if (start == std::string::npos) {
    value.clear();
    return;
  }
  const auto end = value.find_last_not_of(whitespace);
  value = value.substr(start, end - start + 1);
}

void handleServerCommand(const std::string& rawLine) {
  std::string line = rawLine;
  trimString(line);
  if (line.empty()) {
    return;
  }

  if (line.rfind("WIFI ", 0) == 0) {
    std::string remainder = line.substr(5);
    trimString(remainder);
    auto splitIndex = remainder.find(' ');
    if (splitIndex == std::string::npos) {
      std::cout << "TCP CMD: invalid WIFI format, expected 'WIFI [ssid] [password]'" << std::endl;
      return;
    }

    std::string newSsid = remainder.substr(0, splitIndex);
    std::string newPass = remainder.substr(splitIndex + 1);
    trimString(newSsid);
    trimString(newPass);

    if (newSsid.empty() || newPass.empty()) {
      std::cout << "TCP CMD: invalid WIFI format, expected 'WIFI [ssid] [password]'" << std::endl;
      return;
    }
    std::strncpy(wifiSSID, newSsid.c_str(), sizeof(wifiSSID) - 1);
    wifiSSID[sizeof(wifiSSID) - 1] = '\0';
    std::strncpy(wifiPassword, newPass.c_str(), sizeof(wifiPassword) - 1);
    wifiPassword[sizeof(wifiPassword) - 1] = '\0';

    if (!saveWiFiCredentialsToEEPROM(wifiSSID, wifiPassword)) {
      std::cout << "TCP CMD: failed to save WiFi credentials to EEPROM" << std::endl;
      return;
    }

    std::cout << "TCP CMD: WIFI credentials updated to: " << wifiSSID << " / " << wifiPassword << std::endl;
    return;
  }

  if (line.rfind("HOST ", 0) == 0) {
    std::string remainder = line.substr(5);
    trimString(remainder);
    auto splitIndex = remainder.find(' ');
    if (splitIndex == std::string::npos) {
      std::cout << "TCP CMD: invalid HOST format, expected 'HOST [address] [port]'" << std::endl;
      return;
    }

    std::string newHost = remainder.substr(0, splitIndex);
    std::string newPort = remainder.substr(splitIndex + 1);
    trimString(newHost);
    trimString(newPort);

    if (newHost.empty() || newPort.empty()) {
      std::cout << "TCP CMD: invalid HOST format, expected 'HOST [address] [port]'" << std::endl;
      return;
    }

    unsigned long rawPort = std::stoul(newPort);
    if (rawPort == 0 || rawPort > 0xFFFF) {
      std::cout << "TCP CMD: invalid HOST port" << std::endl;
      return;
    }

    std::strncpy(monitorHost, newHost.c_str(), sizeof(monitorHost) - 1);
    monitorHost[sizeof(monitorHost) - 1] = '\0';
    std::snprintf(monitorPortString, sizeof(monitorPortString), "%u", static_cast<uint16_t>(rawPort));
    monitorPort = static_cast<uint16_t>(rawPort);

    if (!saveMonitorServerToEEPROM(monitorHost, monitorPortString)) {
      std::cout << "TCP CMD: failed to save monitor host to EEPROM" << std::endl;
      return;
    }

    std::cout << "TCP CMD: monitor host updated to " << monitorHost << ":" << monitorPort << std::endl;
    return;
  }

  auto splitIndex = line.find(' ');
  if (splitIndex == std::string::npos) {
    std::cout << "TCP CMD: invalid format, expected '[regAddr] [uintValue]'" << std::endl;
    return;
  }

  std::string addrText = line.substr(0, splitIndex);
  std::string valueText = line.substr(splitIndex + 1);
  trimString(addrText);
  trimString(valueText);
  if (addrText.empty() || valueText.empty()) {
    std::cout << "TCP CMD: invalid format, expected '[regAddr] [uintValue]'" << std::endl;
    return;
  }

  unsigned long rawAddr = 0;
  unsigned long rawValue = 0;
  try {
    rawAddr = std::stoul(addrText);
    rawValue = std::stoul(valueText);
  } catch (...) {
    std::cout << "TCP CMD: invalid register address or value" << std::endl;
    return;
  }

  if (rawAddr > 0xFFFF || rawValue > 0xFFFF) {
    std::cout << "TCP CMD: invalid register address or value" << std::endl;
    return;
  }

  uint16_t address = static_cast<uint16_t>(rawAddr);
  uint16_t value = static_cast<uint16_t>(rawValue);
  if (!isWritableControlRegister(address)) {
    std::cout << "TCP CMD: unknown register " << address << std::endl;
    return;
  }

  controlRegisterValues[address] = value;
  std::cout << "TCP CMD: wrote " << address << " = " << value << std::endl;
}

void processServerCommands(int sock) {
  std::string commandLine;
  char buffer[128];

  while (running.load()) {
    ssize_t bytesRead = recv(sock, buffer, sizeof(buffer), 0);
    if (bytesRead <= 0) {
      if (bytesRead < 0) {
        std::perror("recv");
      }
      running.store(false);
      break;
    }

    for (ssize_t i = 0; i < bytesRead; ++i) {
      char c = buffer[i];
      if (c == '\r') {
        continue;
      }
      if (c == '\n') {
        handleServerCommand(commandLine);
        commandLine.clear();
        continue;
      }
      commandLine.push_back(c);
    }
  }
}

int createSocketAndConnect(const std::string& host, uint16_t port) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    std::perror("socket");
    return -1;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    hostent* he = gethostbyname(host.c_str());
    if (!he || he->h_addr_list[0] == nullptr) {
      std::cerr << "Failed to resolve host " << host << std::endl;
      close(sock);
      return -1;
    }
    std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
  }

  if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("connect");
    close(sock);
    return -1;
  }

  return sock;
}

uint16_t simulateValue(uint16_t address, unsigned long timestampMs) {
  auto noise = noiseDistribution(randomEngine);
  double baseValue = 0.0;
  double maxNoise = 0.0;

  switch (address) {
    case 15205:
      baseValue = 2200 + (std::sin(timestampMs / 5000.0) * 50);
      maxNoise = 8.0;
      break;
    case 15206:
      baseValue = 520 + (std::sin(timestampMs / 7000.0) * 20);
      maxNoise = 4.0;
      break;
    case 15207:
      baseValue = 150 + (std::sin(timestampMs / 3000.0) * 40);
      maxNoise = 6.0;
      break;
    case 15208:
      baseValue = 500 + (std::sin(6.28 * timestampMs / 86400000.0) * 120);
      maxNoise = 18.0;
      break;
    case 25210:
      baseValue = 800 + (std::sin(timestampMs / 6000.0) * 100);
      maxNoise = 12.0;
      break;
    case 25274:
      baseValue = 15 + (std::sin(timestampMs / 10000.0) * 5);
      maxNoise = 1.5;
      break;
    default:
      return 0;
  }

  double randomized = baseValue + (noise * maxNoise);
  if (randomized < 0.0) {
    randomized = 0.0;
  }
  return static_cast<uint16_t>(std::round(randomized));
}

bool sendMonitorValue(int sock, uint16_t address, uint16_t value) {
  char buffer[32];
  int len = std::snprintf(buffer, sizeof(buffer), "%u %u\n", address, value);
  if (len <= 0) {
    return false;
  }
  ssize_t sent = send(sock, buffer, len, 0);
  return sent == len;
}

int main(int argc, char* argv[]) {
  std::string eepromPath = "simulator_eeprom.bin";
  std::string host = DEFAULT_MONITOR_HOST;
  uint16_t port = DEFAULT_MONITOR_PORT;
  bool hostArgProvided = false;
  bool portArgProvided = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--eeprom" && i + 1 < argc) {
      eepromPath = argv[++i];
    } else if (arg == "--monitor-host" && i + 1 < argc) {
      host = argv[++i];
      hostArgProvided = true;
    } else if (arg == "--monitor-port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::stoi(argv[++i]));
      portArgProvided = true;
    } else {
      std::cerr << "Usage: " << argv[0] << " [--eeprom <path>] [--monitor-host <host>] [--monitor-port <port>]" << std::endl;
      return 1;
    }
  }

  EEPROMFile file(eepromPath);
  eeprom = &file;

  bool loadedFromEeprom = loadMonitorServerFromEEPROM();
  if (!loadedFromEeprom) {
    std::strncpy(monitorHost, host.c_str(), sizeof(monitorHost) - 1);
    monitorHost[sizeof(monitorHost) - 1] = '\0';
    std::snprintf(monitorPortString, sizeof(monitorPortString), "%u", port);
    monitorPort = port;
    saveMonitorServerToEEPROM(monitorHost, monitorPortString);
  } else if (hostArgProvided || portArgProvided) {
    if (hostArgProvided) {
      std::strncpy(monitorHost, host.c_str(), sizeof(monitorHost) - 1);
      monitorHost[sizeof(monitorHost) - 1] = '\0';
    }
    if (portArgProvided) {
      std::snprintf(monitorPortString, sizeof(monitorPortString), "%u", port);
      monitorPort = port;
    }
    saveMonitorServerToEEPROM(monitorHost, monitorPortString);
    std::cout << "Updated monitor server from CLI args: " << monitorHost << ":" << monitorPort << std::endl;
  } else {
    std::cout << "Loaded monitor server from EEPROM: " << monitorHost << ":" << monitorPort << std::endl;
  }

  int sock = createSocketAndConnect(monitorHost, monitorPort);
  if (sock < 0) {
    std::cerr << "Unable to connect to monitor server " << monitorHost << ":" << monitorPort << std::endl;
    return 1;
  }

  std::cout << "Connected to monitor server " << monitorHost << ":" << monitorPort << std::endl;
  std::cout << "Using EEPROM file: " << eepromPath << std::endl;

  std::thread commandThread(processServerCommands, sock);

  while (running.load()) {
    auto now = std::chrono::steady_clock::now();
    unsigned long timestampMs = static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

    for (const auto& reg : monitorRegisters) {
      if (!running.load()) {
        break;
      }
      uint16_t value = simulateValue(reg.address, timestampMs);
      if (!sendMonitorValue(sock, reg.address, value)) {
        std::cerr << "Failed to send value for register " << reg.address << std::endl;
        running.store(false);
        break;
      }
      std::cout << "Sent " << reg.address << " = " << value << "\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!running.load()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(UPDATE_INTERVAL_MS));
  }

  close(sock);
  if (commandThread.joinable()) {
    commandThread.join();
  }

  return 0;
}
