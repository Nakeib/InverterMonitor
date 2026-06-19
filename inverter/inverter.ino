#include <Arduino.h>
#include <stdarg.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <vector>

#define DEVICE_ID "InverterMonitor v0.1\n"

const uint8_t RELAY_PIN_COUNT = 4;

// EEPROM layout constants
const int WIFI_SSID_MAX_LEN = 32;
const int WIFI_PASSWORD_MAX_LEN = 64;
const int MONITOR_HOST_MAX_LEN = 64;
const int MONITOR_PORT_MAX_LEN = 6;
const int RELAY_IP_ADDRESS_MAX_LEN = 15;
const int RELAY_IP_EEPROM_STRIDE = RELAY_IP_ADDRESS_MAX_LEN + 1;
const int WIFI_SSID_EEPROM_OFFSET = 0;
const int WIFI_PASSWORD_EEPROM_OFFSET = WIFI_SSID_EEPROM_OFFSET + WIFI_SSID_MAX_LEN + 1;
const int MONITOR_HOST_EEPROM_OFFSET = WIFI_PASSWORD_EEPROM_OFFSET + WIFI_PASSWORD_MAX_LEN + 1;
const int MONITOR_PORT_EEPROM_OFFSET = MONITOR_HOST_EEPROM_OFFSET + MONITOR_HOST_MAX_LEN + 1;
const int RS232_BAUD_EEPROM_OFFSET = MONITOR_PORT_EEPROM_OFFSET + MONITOR_PORT_MAX_LEN + 1;
const int SIMULATION_MODE_EEPROM_OFFSET = RS232_BAUD_EEPROM_OFFSET + sizeof(uint32_t);
const int TIMING_CONFIG_VALID_OFFSET = SIMULATION_MODE_EEPROM_OFFSET + 1;
const int CONTROL_SCAN_INTERVAL_EEPROM_OFFSET = TIMING_CONFIG_VALID_OFFSET + 1;
const int MONITOR_SCAN_INTERVAL_EEPROM_OFFSET = CONTROL_SCAN_INTERVAL_EEPROM_OFFSET + sizeof(uint32_t);
const int REGISTER_READ_SPACING_EEPROM_OFFSET = MONITOR_SCAN_INTERVAL_EEPROM_OFFSET + sizeof(uint32_t);
const int RELAY_IP_EEPROM_OFFSET = REGISTER_READ_SPACING_EEPROM_OFFSET + sizeof(uint32_t);
const int EEPROM_SIZE = RELAY_IP_EEPROM_OFFSET + RELAY_PIN_COUNT * RELAY_IP_EEPROM_STRIDE;
// Relay IP address storage follows timing config.

char wifiSSID[WIFI_SSID_MAX_LEN + 1];
char wifiPassword[WIFI_PASSWORD_MAX_LEN + 1];
char monitorHost[MONITOR_HOST_MAX_LEN + 1];
char monitorPortString[MONITOR_PORT_MAX_LEN + 1];
uint16_t monitorPort = 16670;
const unsigned long DEFAULT_RS232_BAUD_RATE = 9600UL;
unsigned long rs232BaudRate = DEFAULT_RS232_BAUD_RATE;
bool simulationMode = true;

// Relay configuration
bool relayStates[RELAY_PIN_COUNT] = {false, false, false, false};
char relayIpAddresses[RELAY_PIN_COUNT][RELAY_IP_ADDRESS_MAX_LEN + 1];

// Command line buffers for TCP and serial input.
const uint16_t MAX_COMMAND_LINE_LENGTH = 256;

String logCommandLine;
String monitorCommandLine;
String serialCommandLine;

// Scanning intervals and state for control and monitor registers.
const unsigned long WIFI_STATION_RETRY_INTERVAL_MS = 300000UL;
const unsigned long DEFAULT_CONTROL_SCAN_INTERVAL_MS = 60000UL;
const unsigned long DEFAULT_MONITOR_SCAN_INTERVAL_MS = 5000UL;
const unsigned long MONITOR_CONNECTION_CHECK_INTERVAL_MS = 60000UL;
unsigned long controlScanIntervalMs = DEFAULT_CONTROL_SCAN_INTERVAL_MS;
unsigned long monitorScanIntervalMs = DEFAULT_MONITOR_SCAN_INTERVAL_MS;
unsigned long nextWifiStationRetryTime = 0;
unsigned long nextControlScanTime = 0;
unsigned long nextMonitorScanTime = 0;
unsigned long nextMonitorConnectionCheckTime = 0;

size_t controlScanIndex = 0;
size_t monitorScanIndex = 0;
size_t controlRegisterCount = 0;
size_t monitorRegisterCount = 0;

// WiFi log server settings.
// Connect a TCP client to this port to receive console messages over WiFi.
const int MAX_WIFI_ATTEMPTS = 20;
const uint16_t LOG_SERVER_PORT = 12345;

WiFiServer logServer(LOG_SERVER_PORT);
WiFiClient logClient;
WiFiClient monitorClient;

// ESP8266 pins for PiSolar RS232 TTL
// D5/D6 are common choices on NodeMCU/ESP8266 boards.
// This converter does not use a DE/RE direction pin.
const uint8_t RS232_RX_PIN = D2; // GPIO14: receive from RS232 converter
const uint8_t RS232_TX_PIN = D1; // GPIO12: transmit to RS232 converter

SoftwareSerial rs232Serial(RS232_RX_PIN, RS232_TX_PIN);

// Control register descriptor used for pretty printing and read-only validation.
struct RegItem {
  uint16_t address;     // virtual register address
  uint8_t index;        // field index in the PiSolar response
  const char* writeCmd; // Optional command prefix for write operations, or nullptr if read-only
  const char* name;     // Human-readable register name
  const char* unit;     // Scaling unit string for formatting
};

// Control registers for inverter configuration and control.
String controlRegistersReadCmd = "QPIRI";
RegItem controlRegisters[] = {
  {100, 13, "PBT", "Battery type", ""}, // 00 - AGM, 01 - FLD, 02 - USR
  {101, 9, "PBCV", "Battery recharge voltage", "V"},
  {102, 23, "PBDV", "Battery redischarge voltage", "V"},
  {103, 10, "PSDV", "Battery under voltage", "V"},
  {104, 11, "PCVV", "Battery bulk voltage", "V"},
  {105, 12, "PBFT", "Battery float voltage", "V"}
};

// Monitor registers for real-time battery/PV measurements.
String monitorRegistersReadCmd = "QPIGS";
RegItem monitorRegisters[] = {
  {200, 9, nullptr, "Battery voltage", "V"},
  {201, 10, nullptr, "Battery charging current", "A"},
  {202, 16, nullptr, "Battery discharging current", "A"},
  {203, 14, nullptr, "PV voltage", "V"},
  {204, 20, nullptr, "PV input power", "W"},
  {205, 6, nullptr, "Load power", "W"},
};

// ===== Logging helpers =====
void printMessage(const String& message) {
  // Print to the local serial console.
  Serial.println(message);

  // Forward to any connected WiFi log client if already connected.
  if (logClient && logClient.connected()) {
    logClient.println(message);
    logClient.flush();
  }
}

void printMessage(const char* fmt, ...) {
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  String message(buffer);

  printMessage(message);
}

// Format raw register values using the register scaling unit.
String formatValue(uint16_t raw, const char* unit) {
  if (strcmp(unit, "0.1V") == 0) {
    // Raw value is 0.1 volts per step.
    return String(raw / 10.0, 1) + " V";
  }
  if (strcmp(unit, "0.01Hz") == 0) {
    // Raw value is 0.01 hertz per step.
    return String(raw / 100.0, 2) + " Hz";
  }
  if (strcmp(unit, "0.1A") == 0) {
    // Raw value is 0.1 amps per step.
    return String(raw / 10.0, 1) + " A";
  }
  if (strcmp(unit, "A") == 0) {
    return String(raw) + " A";
  }
  if (strcmp(unit, "V") == 0) {
    return String(raw) + " V";
  }
  if (strcmp(unit, "W") == 0) {
    return String(raw) + " W";
  }
  // If no scaling is known, return raw integer value.
  return String(raw);
}

String formatRawValue(uint16_t raw, const char* unit) {
  if (strcmp(unit, "0.1V") == 0 || strcmp(unit, "0.1A") == 0) {
    return String(raw / 10.0, 1);
  }
  if (strcmp(unit, "0.01Hz") == 0) {
    return String(raw / 100.0, 2);
  }
  return String(raw);
}

static uint16_t computePipsolarCrc(const char* msg, size_t length) {
  uint16_t crc = 0;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(static_cast<uint8_t>(msg[i])) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  uint8_t crc_low = crc & 0xFF;
  uint8_t crc_high = crc >> 8;
  if (crc_low == 0x28 || crc_low == 0x0D || crc_low == 0x0A) {
    crc_low++;
  }
  if (crc_high == 0x28 || crc_high == 0x0D || crc_high == 0x0A) {
    crc_high++;
  }
  return static_cast<uint16_t>((crc_high << 8) | crc_low);
}

static uint16_t computeModbusCrc(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x0001) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void sendModbusWakeup(Stream& serial) {
  const uint8_t wakeupCommand[] = {0x01, 0xAA, 0x06, 0xDE, 0xA2};
  while (serial.available()) {
    serial.read();
  }
  serial.write(wakeupCommand, sizeof(wakeupCommand));
  serial.flush();
}

bool readModbusRegister(Stream& serial, uint8_t slaveId, uint16_t registerAddress, uint16_t& value, unsigned long timeoutMs = 1000UL) {
  uint8_t request[8];
  request[0] = slaveId;
  request[1] = 0x03;
  request[2] = static_cast<uint8_t>(registerAddress >> 8);
  request[3] = static_cast<uint8_t>(registerAddress & 0xFF);
  request[4] = 0x00;
  request[5] = 0x01;
  uint16_t crc = computeModbusCrc(request, 6);
  request[6] = static_cast<uint8_t>(crc & 0xFF);
  request[7] = static_cast<uint8_t>(crc >> 8);

  String requestHex;
  for (size_t i = 0; i < sizeof(request); ++i) {
    char byteHex[4];
    snprintf(byteHex, sizeof(byteHex), "%02X", request[i]);
    if (i > 0) {
      requestHex += ' ';
    }
    requestHex += byteHex;
  }
  printMessage("readModbusRegister request: %s", requestHex.c_str());

  while (serial.available()) {
    serial.read();
  }
  serial.write(request, sizeof(request));
  serial.flush();

  unsigned long start = millis();
  uint8_t response[7];
  size_t pos = 0;
  while (millis() - start < timeoutMs) {
    if (serial.available() == 0) {
      delay(10);
      continue;
    }
    int nextByte = serial.read();
    if (nextByte < 0) {
      continue;
    }
    if (pos < sizeof(response)) {
      response[pos++] = static_cast<uint8_t>(nextByte);
    }
    if (pos == sizeof(response)) {
      break;
    }
  }

  String responseHex;
  for (size_t i = 0; i < pos; ++i) {
    char byteHex[4];
    snprintf(byteHex, sizeof(byteHex), "%02X", response[i]);
    if (i > 0) {
      responseHex += ' ';
    }
    responseHex += byteHex;
  }
  printMessage("readModbusRegister response (%u bytes): %s", static_cast<unsigned int>(pos), responseHex.c_str());

  if (pos != sizeof(response)) {
    printMessage("readModbusRegister error: expected 7 bytes, got %u", static_cast<unsigned int>(pos));
    return false;
  }

  uint16_t receivedCrc = static_cast<uint16_t>(response[5]) | (static_cast<uint16_t>(response[6]) << 8);
  uint16_t expectedCrc = computeModbusCrc(response, 5);
  if (receivedCrc != expectedCrc) {
    printMessage("readModbusRegister error: CRC mismatch received=0x%04X expected=0x%04X", receivedCrc, expectedCrc);
    return false;
  }

  if (response[0] != slaveId || response[1] != 0x03 || response[2] != 0x02) {
    printMessage("readModbusRegister error: invalid response header slave=0x%02X func=0x%02X byteCount=0x%02X", response[0], response[1], response[2]);
    return false;
  }

  value = static_cast<uint16_t>(response[3] << 8) | response[4];
  printMessage("readModbusRegister success: slave=%u address=%u value=0x%04X (%u)", slaveId, registerAddress, value, value);
  return true;
}

bool writeModbusRegister(Stream& serial, uint8_t slaveId, uint16_t registerAddress, uint16_t value, unsigned long timeoutMs = 1000UL) {
  uint8_t request[8];
  request[0] = slaveId;
  request[1] = 0x06;
  request[2] = static_cast<uint8_t>(registerAddress >> 8);
  request[3] = static_cast<uint8_t>(registerAddress & 0xFF);
  request[4] = static_cast<uint8_t>(value >> 8);
  request[5] = static_cast<uint8_t>(value & 0xFF);
  uint16_t crc = computeModbusCrc(request, 6);
  request[6] = static_cast<uint8_t>(crc & 0xFF);
  request[7] = static_cast<uint8_t>(crc >> 8);

  String requestHex;
  for (size_t i = 0; i < sizeof(request); ++i) {
    char byteHex[4];
    snprintf(byteHex, sizeof(byteHex), "%02X", request[i]);
    if (i > 0) {
      requestHex += ' ';
    }
    requestHex += byteHex;
  }
  printMessage("writeModbusRegister request: %s", requestHex.c_str());

  while (serial.available()) {
    serial.read();
  }
  serial.write(request, sizeof(request));
  serial.flush();

  unsigned long start = millis();
  uint8_t response[8];
  size_t pos = 0;
  while (millis() - start < timeoutMs) {
    if (serial.available() == 0) {
      delay(10);
      continue;
    }
    int nextByte = serial.read();
    if (nextByte < 0) {
      continue;
    }
    if (pos < sizeof(response)) {
      response[pos++] = static_cast<uint8_t>(nextByte);
    }
    if (pos == sizeof(response)) {
      break;
    }
  }

  String responseHex;
  for (size_t i = 0; i < pos; ++i) {
    char byteHex[4];
    snprintf(byteHex, sizeof(byteHex), "%02X", response[i]);
    if (i > 0) {
      responseHex += ' ';
    }
    responseHex += byteHex;
  }
  printMessage("writeModbusRegister response (%u bytes): %s", static_cast<unsigned int>(pos), responseHex.c_str());

  if (pos != sizeof(response)) {
    printMessage("writeModbusRegister error: expected 8 bytes, got %u", static_cast<unsigned int>(pos));
    return false;
  }

  uint16_t receivedCrc = static_cast<uint16_t>(response[6]) | (static_cast<uint16_t>(response[7]) << 8);
  uint16_t expectedCrc = computeModbusCrc(response, 6);
  if (receivedCrc != expectedCrc) {
    printMessage("writeModbusRegister error: CRC mismatch received=0x%04X expected=0x%04X", receivedCrc, expectedCrc);
    return false;
  }

  bool success = response[0] == slaveId && response[1] == 0x06 && response[2] == request[2] && response[3] == request[3] && response[4] == request[4] && response[5] == request[5];
  if (success) {
    printMessage("writeModbusRegister success: slave=%u address=%u value=0x%04X (%u)", slaveId, registerAddress, value, value);
  } else {
    printMessage("writeModbusRegister error: unexpected response header or payload");
  }
  return success;
}

bool sendPipsolarCommand(Stream& serial, const char* command, String& response) {
  while (serial.available()) {
    serial.read();
  }

  size_t commandLength = strlen(command);
  uint16_t crc = computePipsolarCrc(command, commandLength);

  size_t writeLength = commandLength + 3; // CRC high, CRC low, CR
  std::vector<uint8_t> writeBuffer;
  writeBuffer.reserve(512);
  writeBuffer.insert(writeBuffer.end(), reinterpret_cast<const uint8_t*>(command), reinterpret_cast<const uint8_t*>(command) + commandLength);
  writeBuffer.push_back(static_cast<uint8_t>(crc >> 8));
  writeBuffer.push_back(static_cast<uint8_t>(crc & 0xFF));
  writeBuffer.push_back(0x0D);

  String hexLog;
  for (size_t i = 0; i < writeLength; ++i) {
    if (i > 0) {
      hexLog += ' ';
    }
    char byteHex[4];
    snprintf(byteHex, sizeof(byteHex), "%02X", writeBuffer[i]);
    hexLog += byteHex;
  }
  printMessage("sendPipsolarCommand write buffer (%u bytes): %s", static_cast<unsigned int>(writeLength), hexLog.c_str());

  serial.write(writeBuffer.data(), writeLength);
  serial.flush();

  unsigned long start = millis();
  uint8_t buffer[200];
  size_t pos = 0;
  while (millis() - start < 2000) {
    if (serial.available() == 0) {
      delay(10);
      continue;
    }
    int received = serial.read();
    if (received < 0) {
      continue;
    }
    if (pos >= sizeof(buffer)) {
      printMessage("ERROR response buffer overflow");
      return false;
    }
    buffer[pos++] = static_cast<uint8_t>(received);
    if (buffer[pos - 1] == 0x0D) {
      break;
    }
  }

  if (pos < 3 || buffer[pos - 1] != 0x0D) {
    printMessage("ERROR response too short or missing CR [size: %d]", pos);
    return false;
  }

  uint16_t receivedCrc = (static_cast<uint16_t>(buffer[pos - 3]) << 8) | buffer[pos - 2];
  uint16_t expectedCrc = computePipsolarCrc(reinterpret_cast<const char*>(buffer), pos - 3);
  if (receivedCrc != expectedCrc) {
    printMessage("ERROR CRC mismatch");
    return false;
  }

  char temp[200];
  memcpy(temp, buffer, pos - 3);
  temp[pos - 3] = '\0';
  response = String(temp);
  return true;
}

bool parsePipsolarField(const String& response, uint8_t fieldIndex, String& fieldValue) {
  if (fieldIndex == 0 || response.length() == 0) {
    return false;
  }

  auto isWhitespace = [&](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };

  bool isParenList = false;
  int start = 0;
  if (response[0] == '(') {
    isParenList = true;
    start = 1;
  } else if (response.length() > 5) {
    start = 5;
  } else {
    return false;
  }

  while (start < response.length() && isWhitespace(response[start])) {
    start++;
  }

  for (uint8_t index = 1; index < fieldIndex; ++index) {
    if (isParenList) {
      while (start < response.length() && response[start] != ')' && !isWhitespace(response[start])) {
        start++;
      }
      while (start < response.length() && isWhitespace(response[start])) {
        start++;
      }
      if (start < response.length() && response[start] == ')') {
        return false;
      }
    } else {
      int commaPos = response.indexOf(',', start);
      if (commaPos < 0) {
        return false;
      }
      start = commaPos + 1;
    }
  }

  if (start >= response.length()) {
    return false;
  }

  int end = start;
  if (isParenList) {
    while (end < response.length() && response[end] != ')' && !isWhitespace(response[end])) {
      end++;
    }
  } else {
    end = response.indexOf(',', start);
    if (end < 0) {
      end = response.length();
    }
  }

  fieldValue = response.substring(start, end);
  fieldValue.trim();
  return fieldValue.length() > 0;
}

uint16_t parsePipsolarValue(const String& fieldValue, const char* unit) {
  if (strcmp(unit, "0.1V") == 0 || strcmp(unit, "0.1A") == 0) {
    float scaled = fieldValue.toFloat() * 10.0f;
    return static_cast<uint16_t>(scaled + 0.5f);
  }
  if (strcmp(unit, "0.01Hz") == 0) {
    float scaled = fieldValue.toFloat() * 100.0f;
    return static_cast<uint16_t>(scaled + 0.5f);
  }
  return static_cast<uint16_t>(fieldValue.toInt());
}

bool sendPipsolarWriteCommand(const String& command) {
  String response;
  if (!sendPipsolarCommand(rs232Serial, command.c_str(), response)) {
    return false;
  }
  if (response.startsWith("(ACK") || response.startsWith("(NAK")) {
    return true;
  }
  return false;
}

bool buildPipsolarWriteCommand(const RegItem& reg, String& value, String& command) {
  if (reg.writeCmd == nullptr) {
    return false;
  }

  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%s%s", reg.writeCmd, value.c_str());
  command = buffer;
  return true;
}

// ===== EEPROM helpers =====
void readEEPROMString(int start, int maxLen, char* dest) {
  for (int i = 0; i < maxLen; ++i) {
    uint8_t c = EEPROM.read(start + i);
    if (c == 0 || c == 255) {
      dest[i] = '\0';
      return;
    }
    dest[i] = static_cast<char>(c);
  }
  dest[maxLen - 1] = '\0';
}

bool loadWiFiCredentialsFromEEPROM() {
  readEEPROMString(WIFI_SSID_EEPROM_OFFSET, WIFI_SSID_MAX_LEN + 1, wifiSSID);
  readEEPROMString(WIFI_PASSWORD_EEPROM_OFFSET, WIFI_PASSWORD_MAX_LEN + 1, wifiPassword);

  if (wifiSSID[0] == '\0') {
    return false;
  }
  return true;
}

bool loadMonitorServerFromEEPROM() {
  readEEPROMString(MONITOR_HOST_EEPROM_OFFSET, MONITOR_HOST_MAX_LEN + 1, monitorHost);
  readEEPROMString(MONITOR_PORT_EEPROM_OFFSET, MONITOR_PORT_MAX_LEN + 1, monitorPortString);
  monitorPort = static_cast<uint16_t>(strtoul(monitorPortString, nullptr, 10));

  if (monitorHost[0] == '\0' || monitorPort == 0) {
    monitorHost[0] = '\0';
    monitorPortString[0] = '\0';
    monitorPort = 0;
    return false;
  }
  return true;
}

bool loadSimulationModeFromEEPROM() {
  uint8_t storedValue = EEPROM.read(SIMULATION_MODE_EEPROM_OFFSET);
  if (storedValue == 0xFF) {
    simulationMode = true;
    return false;
  }
  simulationMode = (storedValue != 0);
  return true;
}

bool loadRelayIpAddressesFromEEPROM() {
  bool anyLoaded = false;
  for (uint8_t i = 0; i < RELAY_PIN_COUNT; ++i) {
    int offset = RELAY_IP_EEPROM_OFFSET + i * RELAY_IP_EEPROM_STRIDE;
    readEEPROMString(offset, RELAY_IP_EEPROM_STRIDE, relayIpAddresses[i]);
    if (relayIpAddresses[i][0] != '\0') {
      anyLoaded = true;
    }
  }
  return anyLoaded;
}

void writeEEPROMString(int start, int maxLen, const char* src) {
  int i = 0;
  while (i < maxLen && src[i] != '\0') {
    EEPROM.write(start + i, static_cast<uint8_t>(src[i]));
    ++i;
  }
  EEPROM.write(start + i, 0);
  while (++i <= maxLen) {
    EEPROM.write(start + i, 0);
  }
}

bool saveWiFiCredentialsToEEPROM(const char* ssid, const char* password) {
  if (strlen(ssid) > WIFI_SSID_MAX_LEN || strlen(password) > WIFI_PASSWORD_MAX_LEN) {
    return false;
  }

  writeEEPROMString(WIFI_SSID_EEPROM_OFFSET, WIFI_SSID_MAX_LEN, ssid);
  writeEEPROMString(WIFI_PASSWORD_EEPROM_OFFSET, WIFI_PASSWORD_MAX_LEN, password);
  return EEPROM.commit();
}

bool saveMonitorServerToEEPROM(const char* host, const char* portString) {
  if (strlen(host) > MONITOR_HOST_MAX_LEN || strlen(portString) > MONITOR_PORT_MAX_LEN) {
    return false;
  }

  writeEEPROMString(MONITOR_HOST_EEPROM_OFFSET, MONITOR_HOST_MAX_LEN, host);
  writeEEPROMString(MONITOR_PORT_EEPROM_OFFSET, MONITOR_PORT_MAX_LEN, portString);
  return EEPROM.commit();
}

bool saveRelayIpAddressToEEPROM(uint8_t relayIndex, const char* ip) {
  if (relayIndex >= RELAY_PIN_COUNT || strlen(ip) > RELAY_IP_ADDRESS_MAX_LEN) {
    return false;
  }
  int offset = RELAY_IP_EEPROM_OFFSET + relayIndex * RELAY_IP_EEPROM_STRIDE;
  writeEEPROMString(offset, RELAY_IP_ADDRESS_MAX_LEN, ip);
  return EEPROM.commit();
}

bool parseIpAddress(const String& ipText, char* dest, int maxLen) {
  if (ipText.length() == 0 || ipText.length() > maxLen) {
    return false;
  }

  int parts[4] = {0, 0, 0, 0};
  int partIndex = 0;
  unsigned long value = 0;
  bool hasDigit = false;

  for (unsigned int i = 0; i < ipText.length(); ++i) {
    char c = ipText[i];
    if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      if (value > 255) {
        return false;
      }
      hasDigit = true;
    } else if (c == '.') {
      if (!hasDigit || partIndex >= 3) {
        return false;
      }
      parts[partIndex++] = static_cast<int>(value);
      value = 0;
      hasDigit = false;
    } else {
      return false;
    }
  }

  if (!hasDigit || partIndex != 3) {
    return false;
  }
  parts[3] = static_cast<int>(value);

  int len = snprintf(dest, maxLen + 1, "%u.%u.%u.%u", parts[0], parts[1], parts[2], parts[3]);
  return len > 0 && len <= maxLen;
}

void printRelayIpAddresses() {
  for (uint8_t i = 0; i < RELAY_PIN_COUNT; ++i) {
    if (relayIpAddresses[i][0] != '\0') {
      printMessage("Relay %u IP = %s", static_cast<unsigned int>(i + 1), relayIpAddresses[i]);
    } else {
      printMessage("Relay %u IP = <unset>", static_cast<unsigned int>(i + 1));
    }
  }
}

uint32_t readEEPROMUint32(int start) {
  uint32_t value = 0;
  value |= static_cast<uint32_t>(EEPROM.read(start));
  value |= static_cast<uint32_t>(EEPROM.read(start + 1)) << 8;
  value |= static_cast<uint32_t>(EEPROM.read(start + 2)) << 16;
  value |= static_cast<uint32_t>(EEPROM.read(start + 3)) << 24;
  return value;
}

void writeEEPROMUint32(int start, uint32_t value) {
  EEPROM.write(start, static_cast<uint8_t>(value & 0xFF));
  EEPROM.write(start + 1, static_cast<uint8_t>((value >> 8) & 0xFF));
  EEPROM.write(start + 2, static_cast<uint8_t>((value >> 16) & 0xFF));
  EEPROM.write(start + 3, static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool timeHasElapsed(unsigned long now, unsigned long targetTime) {
  return static_cast<long>(now - targetTime) >= 0;
}

bool loadTimingConfigFromEEPROM() {
  if (EEPROM.read(TIMING_CONFIG_VALID_OFFSET) != 0xAA) {
    controlScanIntervalMs = DEFAULT_CONTROL_SCAN_INTERVAL_MS;
    monitorScanIntervalMs = DEFAULT_MONITOR_SCAN_INTERVAL_MS;
    return false;
  }

  uint32_t controlValue = readEEPROMUint32(CONTROL_SCAN_INTERVAL_EEPROM_OFFSET);
  uint32_t monitorValue = readEEPROMUint32(MONITOR_SCAN_INTERVAL_EEPROM_OFFSET);

  controlScanIntervalMs = (controlValue == 0) ? DEFAULT_CONTROL_SCAN_INTERVAL_MS : controlValue;
  monitorScanIntervalMs = (monitorValue == 0) ? DEFAULT_MONITOR_SCAN_INTERVAL_MS : monitorValue;
  return true;
}

bool saveTimingConfigToEEPROM(uint32_t controlInterval,
                              uint32_t monitorInterval) {
  EEPROM.write(TIMING_CONFIG_VALID_OFFSET, 0xAA);
  writeEEPROMUint32(CONTROL_SCAN_INTERVAL_EEPROM_OFFSET, controlInterval);
  writeEEPROMUint32(MONITOR_SCAN_INTERVAL_EEPROM_OFFSET, monitorInterval);
  return EEPROM.commit();
}

bool saveSimulationModeToEEPROM(bool enabled) {
  EEPROM.write(SIMULATION_MODE_EEPROM_OFFSET, enabled ? 1 : 0);
  return EEPROM.commit();
}

bool loadRs232BaudRateFromEEPROM() {
  uint32_t storedBaud = readEEPROMUint32(RS232_BAUD_EEPROM_OFFSET);
  if (storedBaud == 0 || storedBaud == 0xFFFFFFFFUL) {
    rs232BaudRate = DEFAULT_RS232_BAUD_RATE;
    return false;
  }
  rs232BaudRate = storedBaud;
  return true;
}

bool saveRs232BaudRateToEEPROM(uint32_t baudRate) {
  writeEEPROMUint32(RS232_BAUD_EEPROM_OFFSET, baudRate);
  return EEPROM.commit();
}

// ===== Connection helpers =====
void resetScanTime() {
  nextControlScanTime = 0;
  nextMonitorScanTime = 0;
  controlScanIndex = 0;
  monitorScanIndex = 0;
}

bool connectToMonitorServer() {
  if (monitorHost[0] == '\0' || monitorPort == 0) {
    return false;
  }
  if (monitorClient && monitorClient.connected()) {
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (monitorClient.connect(monitorHost, monitorPort)) {
    monitorClient.setNoDelay(true);
    Serial.printf("Connected to monitor server %s:%u\n", monitorHost, monitorPort);
    return true;
  }

  Serial.printf("Failed to connect to monitor server %s:%u\n", monitorHost, monitorPort);
  monitorClient.stop();
  return false;
}

bool connectToWiFi() {
  if (logClient && logClient.connected()) {
    logClient.stop();
  }
  if (monitorClient && monitorClient.connected()) {
    monitorClient.stop();
  }

  WiFi.disconnect(true);

  if (wifiSSID[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID, wifiPassword);

    Serial.printf("Connecting to WiFi SSID '%s'...\n", wifiSSID);
    int wifiAttempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifiAttempts < MAX_WIFI_ATTEMPTS) {
      delay(500);
      Serial.print('.');
      wifiAttempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.print("WiFi connected, IP address: ");
      Serial.println(WiFi.localIP());
      connectToMonitorServer();
      return true;
    }
  }

  nextWifiStationRetryTime = millis() + WIFI_STATION_RETRY_INTERVAL_MS;

  Serial.println();
  Serial.println("WiFi connection failed, starting fallback AP 'InverterComm'");
  monitorClient.stop();
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 1, 1);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);
  WiFi.softAP("InverterComm", "12345678");
  Serial.print("Fallback AP started, IP address: ");
  Serial.println(WiFi.softAPIP());
  return false;
}

// ===== PiSolar register access =====
size_t readControlRegisters() {
  const String command = controlRegistersReadCmd;
  size_t count = 0;

  if (simulationMode) {
    for (size_t index = 0; index < controlRegisterCount; ++index) {
      const RegItem& reg = controlRegisters[index];
      printRegisterResult(reg, "0");
      ++count;
    }
    return count;
  }

  String response;
  if (!sendPipsolarCommand(rs232Serial, command.c_str(), response)) {
    for (size_t index = 0; index < controlRegisterCount; ++index) {
      const RegItem& reg = controlRegisters[index];
      printMessage("%5u %-36s => READ ERROR", reg.address, reg.name);
      ++count;
    }
    return count;
  }

  for (size_t index = 0; index < controlRegisterCount; ++index) {
    const RegItem& reg = controlRegisters[index];
    String fieldValue;
    if (parsePipsolarField(response, reg.index, fieldValue)) {
      printRegisterResult(reg, fieldValue);
    } else {
      printMessage("%5u %-36s => PARSE ERROR", reg.address, reg.name);
    }
    ++count;
  }
  return count;
}

size_t readMonitorRegisters() {
  const String command = monitorRegistersReadCmd;
  size_t count = 0;

  if (simulationMode) {
    unsigned long now = millis();
    for (size_t index = 0; index < monitorRegisterCount; ++index) {
      const RegItem& reg = monitorRegisters[index];
      uint16_t value = 0;
      switch (reg.address) {
        case 200:
          value = static_cast<uint16_t>(max(0L, static_cast<long>(518 + std::sin(now / 5000.0) * 4) + random(-2, 3)));
          break;
        case 201:
          value = static_cast<uint16_t>(max(0L, static_cast<long>(4 + std::sin(now / 8000.0) * 2) + random(-1, 2)));
          break;
        case 202:
          value = static_cast<uint16_t>(max(0L, static_cast<long>(9 + std::sin(now / 6000.0) * 3) + random(-2, 3)));
          break;
        case 203:
          value = static_cast<uint16_t>(max(0L, static_cast<long>(2438 + std::sin(now / 7000.0) * 15) + random(-5, 6)));
          break;
        case 205:
          value = static_cast<uint16_t>(max(0L, static_cast<long>(302 + std::sin(now / 2000.0) * 30) + random(-20, 21)));
          break;
        case 206:
          value = static_cast<uint16_t>(max(0L, static_cast<long>(0 + std::sin(now / 12000.0) * 10) + random(-5, 6)));
          break;
        default:
          value = 0;
          break;
      }
      printRegisterResult(reg, formatRawValue(value, reg.unit));
      ++count;
    }
    return count;
  }

  String response;
  if (!sendPipsolarCommand(rs232Serial, command.c_str(), response)) {
    for (size_t index = 0; index < monitorRegisterCount; ++index) {
      const RegItem& reg = monitorRegisters[index];
      printMessage("%5u %-36s => READ ERROR", reg.address, reg.name);
      ++count;
    }
    return count;
  }

  for (size_t index = 0; index < monitorRegisterCount; ++index) {
    const RegItem& reg = monitorRegisters[index];
    String fieldValue;
    if (parsePipsolarField(response, reg.index, fieldValue)) {
      printRegisterResult(reg, fieldValue);
    } else {
      printMessage("%5u %-36s => PARSE ERROR", reg.address, reg.name);
    }
    ++count;
  }
  return count;
}

const RegItem* findRegister(uint16_t address) {
  for (const RegItem& reg : controlRegisters) {
    if (reg.address == address) {
      return &reg;
    }
  }
  for (const RegItem& reg : monitorRegisters) {
    if (reg.address == address) {
      return &reg;
    }
  }
  return nullptr;
}

bool setRelayState(uint8_t relayIndex, bool active) {
  if (relayIndex >= RELAY_PIN_COUNT) {
    return false;
  }
  relayStates[relayIndex] = active;
  return true;
}

String relayStateText(bool active) {
  return active ? String("ON") : String("OFF");
}

void printRelaysState() {
  char buffer[32];

  for (uint8_t i = 0; i < RELAY_PIN_COUNT; ++i) {
    printMessage("R%u: %s [%s]", static_cast<unsigned int>(i + 1), relayStateText(relayStates[i]).c_str(), relayIpAddresses[i][0] != '\0' ? relayIpAddresses[i] : "no IP");
    if (monitorClient && monitorClient.connected()) {
      int len = snprintf(buffer, sizeof(buffer), "R%u %u %s\n", i + 1, relayStates[i] ? 1 : 0, relayIpAddresses[i][0] != '\0' ? relayIpAddresses[i] : "no_ip");
      if (len > 0) {
        monitorClient.write(reinterpret_cast<const uint8_t*>(buffer), len);
      }
    }
  }
}

// ===== Command parsing =====
void handleCommand(const String& rawLine) {
  String line = rawLine;
  line.trim();
  if (line.length() == 0) {
    return;
  }

  if (line.startsWith("WIFI ")) {
    String remainder = line.substring(5);
    remainder.trim();
    int splitIndex = remainder.indexOf(' ');
    if (splitIndex < 0) {
      printMessage("TCP CMD: invalid WIFI format, expected 'WIFI [ssid] [password]'");
      return;
    }

    String newSsid = remainder.substring(0, splitIndex);
    String newPass = remainder.substring(splitIndex + 1);
    newSsid.trim();
    newPass.trim();

    if (newSsid.length() == 0 || newPass.length() == 0) {
      printMessage("TCP CMD: invalid WIFI format, expected 'WIFI [ssid] [password]'");
      return;
    }
    if (newSsid.length() > WIFI_SSID_MAX_LEN || newPass.length() > WIFI_PASSWORD_MAX_LEN) {
      printMessage("TCP CMD: WIFI SSID or password too long");
      return;
    }

    strcpy(wifiSSID, newSsid.c_str());
    strcpy(wifiPassword, newPass.c_str());
    if (!saveWiFiCredentialsToEEPROM(wifiSSID, wifiPassword)) {
      printMessage("TCP CMD: failed to save WiFi credentials to EEPROM");
      return;
    }

    printMessage("TCP CMD: WIFI credentials updated, reconnecting...");
    connectToWiFi();
    resetScanTime();
    return;
  }

  if (line.startsWith("HOST ")) {
    String remainder = line.substring(5);
    remainder.trim();
    int splitIndex = remainder.indexOf(' ');
    if (splitIndex < 0) {
      printMessage("TCP CMD: invalid HOST format, expected 'HOST [address] [port]'");
      return;
    }

    String newHost = remainder.substring(0, splitIndex);
    String newPort = remainder.substring(splitIndex + 1);
    newHost.trim();
    newPort.trim();

    if (newHost.length() == 0 || newPort.length() == 0) {
      printMessage("TCP CMD: invalid HOST format, expected 'HOST [address] [port]'");
      return;
    }
    if (newHost.length() > MONITOR_HOST_MAX_LEN || newPort.length() > MONITOR_PORT_MAX_LEN) {
      printMessage("TCP CMD: monitor host or port too long");
      return;
    }

    char* endPtr;
    unsigned long rawPort = strtoul(newPort.c_str(), &endPtr, 10);
    if (endPtr == newPort.c_str() || rawPort == 0 || rawPort > 0xFFFF) {
      printMessage("TCP CMD: invalid HOST port");
      return;
    }

    strcpy(monitorHost, newHost.c_str());
    strcpy(monitorPortString, newPort.c_str());
    monitorPort = static_cast<uint16_t>(rawPort);

    if (!saveMonitorServerToEEPROM(monitorHost, monitorPortString)) {
      printMessage("TCP CMD: failed to save monitor host to EEPROM");
      return;
    }

    printMessage("TCP CMD: monitor host updated to %s:%s", monitorHost, monitorPortString);
    connectToMonitorServer();
    return;
  }

  if (line.startsWith("BAUD ")) {
    String valueText = line.substring(5);
    valueText.trim();
    if (valueText.length() == 0) {
      printMessage("TCP CMD: invalid BAUD format, expected 'BAUD [baud]' ");
      return;
    }

    char* endPtr;
    unsigned long baud = strtoul(valueText.c_str(), &endPtr, 10);
    if (endPtr == valueText.c_str() || baud < 300UL || baud > 2000000UL) {
      printMessage("TCP CMD: invalid BAUD value");
      return;
    }

    rs232BaudRate = baud;
    rs232Serial.begin(rs232BaudRate);
    if (!saveRs232BaudRateToEEPROM(rs232BaudRate)) {
      printMessage("TCP CMD: failed to save RS232 baud rate to EEPROM");
      return;
    }

    printMessage("TCP CMD: BAUD %lu", rs232BaudRate);
    return;
  }

  if (line.startsWith("SIM ")) {
    String valueText = line.substring(4);
    valueText.trim();
    if (valueText.length() == 0) {
      printMessage("TCP CMD: invalid SIM format, expected 'SIM [ON|OFF|1|0]'");
      return;
    }

    bool enabled;
    if (valueText.equalsIgnoreCase("ON") || valueText == "1") {
      enabled = true;
    } else if (valueText.equalsIgnoreCase("OFF") || valueText == "0") {
      enabled = false;
    } else {
      printMessage("TCP CMD: invalid SIM value, expected ON/OFF/1/0");
      return;
    }

    simulationMode = enabled;
    if (!saveSimulationModeToEEPROM(simulationMode)) {
      printMessage("TCP CMD: failed to save simulation mode to EEPROM");
      return;
    }

    printMessage("TCP CMD: SIM %s", simulationMode ? "ON" : "OFF");
    return;
  }

  if (line.startsWith("CTRL_SCAN_MS")) {
    String valueText = line.substring(12);
    valueText.trim();
    if (valueText.length() == 0) {
      printMessage("TCP CMD: invalid CTRL_SCAN_MS format, expected 'CTRL_SCAN_MS [value]' ");
      return;
    }

    char* endPtr;
    unsigned long rawValue = strtoul(valueText.c_str(), &endPtr, 10);
    if (endPtr == valueText.c_str() || rawValue == 0) {
      printMessage("TCP CMD: invalid CTRL_SCAN_MS value");
      return;
    }

    controlScanIntervalMs = rawValue;
    if (!saveTimingConfigToEEPROM(controlScanIntervalMs, monitorScanIntervalMs)) {
      printMessage("TCP CMD: failed to save timing config to EEPROM");
      return;
    }

    printMessage("TCP CMD: CTRL_SCAN_MS %lu", controlScanIntervalMs);
    return;
  }

  if (line.startsWith("MON_SCAN_MS")) {
    String valueText = line.substring(11);
    valueText.trim();
    if (valueText.length() == 0) {
      printMessage("TCP CMD: invalid MON_SCAN_MS format, expected 'MON_SCAN_MS [value]' ");
      return;
    }

    char* endPtr;
    unsigned long rawValue = strtoul(valueText.c_str(), &endPtr, 10);
    if (endPtr == valueText.c_str() || rawValue == 0) {
      printMessage("TCP CMD: invalid MON_SCAN_MS value");
      return;
    }

    monitorScanIntervalMs = rawValue;
    if (!saveTimingConfigToEEPROM(controlScanIntervalMs, monitorScanIntervalMs)) {
      printMessage("TCP CMD: failed to save timing config to EEPROM");
      return;
    }

    printMessage("TCP CMD: MON_SCAN_MS %lu", monitorScanIntervalMs);
    return;
  }

  if (line.startsWith("RELAY ")) {
    String remainder = line.substring(6);
    remainder.trim();
    int splitIndex = remainder.indexOf(' ');
    if (splitIndex < 0) {
      printMessage("TCP CMD: invalid RELAY format, expected 'RELAY [number] [state]' ");
      return;
    }

    String relayText = remainder.substring(0, splitIndex);
    String stateText = remainder.substring(splitIndex + 1);
    relayText.trim();
    stateText.trim();

    if (relayText.length() == 0 || stateText.length() == 0) {
      printMessage("TCP CMD: invalid RELAY format, expected 'RELAY [number] [state]' ");
      return;
    }

    char* endPtr;
    unsigned long relayNumber = strtoul(relayText.c_str(), &endPtr, 10);
    if (endPtr == relayText.c_str() || relayNumber < 1 || relayNumber > RELAY_PIN_COUNT) {
      printMessage("TCP CMD: invalid relay number, expected 1-%u", RELAY_PIN_COUNT);
      return;
    }

    bool active;
    if (stateText.equalsIgnoreCase("ON") || stateText == "1") {
      active = true;
    } else if (stateText.equalsIgnoreCase("OFF") || stateText == "0") {
      active = false;
    } else {
      printMessage("TCP CMD: invalid relay state, expected ON/OFF or 1/0");
      return;
    }

    uint8_t relayIndex = static_cast<uint8_t>(relayNumber - 1);
    if (!setRelayState(relayIndex, active)) {
      printMessage("TCP CMD: failed to set relay %u", relayNumber);
      return;
    }

    printMessage("TCP CMD: RELAY %u %s", relayNumber, relayStateText(active).c_str());
    return;
  }

  if (line.startsWith("SETIP ")) {
    String remainder = line.substring(6);
    remainder.trim();
    if (remainder.length() == 0) {
      printMessage("TCP CMD: invalid SETIP format, expected 'SETIP [relayNumber]' or 'SETIP [relayNumber] [ip]' ");
      return;
    }

    int splitIndex = remainder.indexOf(' ');
    String relayText = (splitIndex < 0) ? remainder : remainder.substring(0, splitIndex);
    String ipText = (splitIndex < 0) ? String() : remainder.substring(splitIndex + 1);
    relayText.trim();
    ipText.trim();

    if (relayText.length() == 0) {
      printMessage("TCP CMD: invalid SETIP format, expected 'SETIP [relayNumber]' or 'SETIP [relayNumber] [ip]' ");
      return;
    }

    char* endPtr;
    unsigned long relayNumber = strtoul(relayText.c_str(), &endPtr, 10);
    if (endPtr == relayText.c_str() || relayNumber < 1 || relayNumber > RELAY_PIN_COUNT) {
      printMessage("TCP CMD: invalid relay number, expected 1-%u", RELAY_PIN_COUNT);
      return;
    }

    uint8_t relayIndex = static_cast<uint8_t>(relayNumber - 1);
    if (ipText.length() == 0) {
      relayIpAddresses[relayIndex][0] = '\0';
      if (!saveRelayIpAddressToEEPROM(relayIndex, "")) {
        printMessage("TCP CMD: failed to clear relay IP in EEPROM");
        return;
      }
      printMessage("TCP CMD: SETIP %u cleared", relayNumber);
      return;
    }

    char ipBuffer[RELAY_IP_ADDRESS_MAX_LEN + 1];
    if (!parseIpAddress(ipText, ipBuffer, RELAY_IP_ADDRESS_MAX_LEN)) {
      printMessage("TCP CMD: invalid IP address format, expected IPv4 like 192.168.1.100");
      return;
    }

    strcpy(relayIpAddresses[relayIndex], ipBuffer);
    if (!saveRelayIpAddressToEEPROM(relayIndex, ipBuffer)) {
      printMessage("TCP CMD: failed to save relay IP to EEPROM");
      return;
    }

    printMessage("TCP CMD: SETIP %u %s", relayNumber, ipBuffer);
    return;
  }

  if (line.startsWith("READPIP ")) {
    String commandStr = line.substring(8);
    commandStr.trim();
    if (commandStr.length() == 0) {
      printMessage("TCP CMD: invalid READPIP format, expected 'READPIP [commandStr]'");
      return;
    }

    if (simulationMode) {
      printMessage("TCP CMD: simulation mode enabled, skipping READPIP");
      return;
    }

    String response;
    if (sendPipsolarCommand(rs232Serial, commandStr.c_str(), response)) {
      printMessage("READPIP response: %s", response.c_str());
    } else {
      printMessage("TCP CMD: READPIP failed for command '%s'", commandStr.c_str());
    }
    return;
  }

  if (line == "WAKEUPMOD") {
    sendModbusWakeup(rs232Serial);
    printMessage("TCP CMD: WAKEUPMOD sent");
    return;
  }

  if (line.startsWith("READMOD ")) {
    String remainder = line.substring(8);
    remainder.trim();
    int splitIndex = remainder.indexOf(' ');
    if (splitIndex < 0) {
      printMessage("TCP CMD: invalid READMOD format, expected 'READMOD [slaveId] [address]'");
      return;
    }

    String slaveText = remainder.substring(0, splitIndex);
    String addrText = remainder.substring(splitIndex + 1);
    slaveText.trim();
    addrText.trim();

    if (slaveText.length() == 0 || addrText.length() == 0) {
      printMessage("TCP CMD: invalid READMOD format, expected 'READMOD [slaveId] [address]'");
      return;
    }

    char* endPtr;
    unsigned long rawSlave = strtoul(slaveText.c_str(), &endPtr, 10);
    if (endPtr == slaveText.c_str() || rawSlave == 0 || rawSlave > 0xFF) {
      printMessage("TCP CMD: invalid READMOD slaveId");
      return;
    }

    unsigned long rawAddr = strtoul(addrText.c_str(), &endPtr, 10);
    if (endPtr == addrText.c_str() || rawAddr > 0xFFFF) {
      printMessage("TCP CMD: invalid READMOD address");
      return;
    }

    if (simulationMode) {
      printMessage("TCP CMD: simulation mode enabled, skipping READMOD");
      return;
    }

    uint8_t slaveId = static_cast<uint8_t>(rawSlave);
    uint16_t address = static_cast<uint16_t>(rawAddr);
    uint16_t value = 0;
    if (readModbusRegister(rs232Serial, slaveId, address, value)) {
      printMessage("READMOD response: slave=%u address=%u value=%u (0x%04X)", slaveId, address, value, value);
    } else {
      printMessage("TCP CMD: READMOD failed for slave=%u address=%u", slaveId, address);
    }
    return;
  }

  int splitIndex = line.indexOf(' ');
  if (splitIndex < 0) {
    printMessage("TCP CMD: invalid format, expected '[regAddr] [uintValue]'");
    return;
  }

  String addrText = line.substring(0, splitIndex);
  String valueText = line.substring(splitIndex + 1);
  addrText.trim();
  valueText.trim();
  if (addrText.length() == 0 || valueText.length() == 0) {
    printMessage("TCP CMD: invalid format, expected '[regAddr] [uintValue]'");
    return;
  }

  char* endPtr;
  unsigned long rawAddr = strtoul(addrText.c_str(), &endPtr, 10);
  if (endPtr == addrText.c_str() || rawAddr > 0xFFFF) {
    printMessage("TCP CMD: invalid register address");
    return;
  }

  uint16_t address = static_cast<uint16_t>(rawAddr);
  
  if (address <= RELAY_PIN_COUNT) {
    uint8_t relayIndex = static_cast<uint8_t>(address - 1);
    unsigned long value = strtoul(valueText.c_str(), &endPtr, 10);
    setRelayState(relayIndex, value != 0);
    printMessage("TCP CMD: RELAY %u %s", address, relayStateText(relayStates[relayIndex]).c_str());
  } else {
    const RegItem* reg = findRegister(address);
    if (!reg) {
      printMessage("TCP CMD: unknown register " + String(address));
      return;
    }

    // For now control registers are treated as read-only.
    // The legacy PiSolar write path is left in code for future use, but is disabled here.
    printMessage("TCP CMD: register %u is read-only", address);
    return;

    String command;
    if (!buildPipsolarWriteCommand(*reg, valueText, command)) {
      printMessage("TCP CMD: unable to build write command for register %u", address);
      return;
    }

    if (simulationMode) {
      printMessage("TCP CMD: wrote %u = %s (simulation)", address, valueText.c_str());
      controlScanIndex = 0;
      nextControlScanTime = millis();
      return;
    }

    if (sendPipsolarWriteCommand(command)) {
      unsigned long now = millis();
      printMessage("TCP CMD: wrote %u = %s", address, valueText.c_str());
      controlScanIndex = 0;
      nextControlScanTime = now;
    } else {
      printMessage("TCP CMD: write failed for %u", address);
    }
  }
}

void processTcpCommands(WiFiClient& client, String& commandLine) {
  if (!client || !client.connected()) {
    return;
  }

  while (client.available()) {
    char c = client.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      handleCommand(commandLine);
      commandLine = "";
      continue;
    }
    if (commandLine.length() < MAX_COMMAND_LINE_LENGTH) {
      commandLine += c;
    }
  }
}

void processSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      handleCommand(serialCommandLine);
      serialCommandLine = "";
      continue;
    }
    if (serialCommandLine.length() < MAX_COMMAND_LINE_LENGTH) {
      serialCommandLine += c;
    }
  }
}

void printRegisterResult(const RegItem& reg, const String& value) {
  printMessage("%5u %-36s = %s", reg.address, reg.name, value.c_str());
  if (monitorClient && monitorClient.connected()) {
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%u %s\n", reg.address, value.c_str());
    if (len > 0) {
      monitorClient.write(reinterpret_cast<const uint8_t*>(buffer), len);
    }
  }
}

// ===== Arduino lifecycle =====
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.printf("ESP8266 Inverter PiSolar RS232 Control Register Reader\n");

  EEPROM.begin(EEPROM_SIZE);
  if (loadWiFiCredentialsFromEEPROM()) {
    Serial.println("Loaded WiFi credentials from EEPROM.");
  } else {
    Serial.println("Using default WiFi credentials.");
  }

  if (loadMonitorServerFromEEPROM()) {
    Serial.printf("Loaded monitor server %s:%s from EEPROM.\n", monitorHost, monitorPortString);
  } else {
    Serial.println("No monitor server configured in EEPROM.");
  }

  if (loadSimulationModeFromEEPROM()) {
    Serial.printf("Loaded simulation mode %s from EEPROM.\n", simulationMode ? "ON" : "OFF");
  } else {
    Serial.printf("No simulation mode configured in EEPROM, defaulting to %s.\n", simulationMode ? "ON" : "OFF");
  }

  if (loadRs232BaudRateFromEEPROM()) {
    Serial.printf("Loaded RS232 baud rate %lu from EEPROM.\n", rs232BaudRate);
  } else {
    Serial.printf("Using default RS232 baud rate %lu.\n", rs232BaudRate);
  }

  if (loadTimingConfigFromEEPROM()) {
    Serial.printf("Loaded timing config from EEPROM: control=%lu ms, monitor=%lu ms\n",
                  controlScanIntervalMs, monitorScanIntervalMs);
  } else {
    Serial.printf("No timing config in EEPROM, using defaults: control=%lu ms, monitor=%lu ms\n",
                  controlScanIntervalMs, monitorScanIntervalMs);
  }

  if (loadRelayIpAddressesFromEEPROM()) {
    printRelayIpAddresses();
  } else {
    Serial.println("No relay IP addresses configured in EEPROM.");
  }

  connectToWiFi();

  if (simulationMode) {
    randomSeed(micros());
  }

  // Start the TCP log server for clients that want console output over WiFi.
  logServer.begin();
  Serial.printf("WiFi log server listening on port %u\n", LOG_SERVER_PORT);

  // Initialize relay outputs and set them off by default.
  for (uint8_t i = 0; i < RELAY_PIN_COUNT; ++i) {
    relayStates[i] = false;
  }

  pinMode(RS232_RX_PIN, INPUT);

  // Start TTL serial for the RS232 PiSolar converter.
  rs232Serial.begin(rs232BaudRate);

  controlRegisterCount = sizeof(controlRegisters) / sizeof(controlRegisters[0]);
  monitorRegisterCount = sizeof(monitorRegisters) / sizeof(monitorRegisters[0]);
}

void loop() {
  unsigned long now = millis();

  processSerialCommands();

  // Accept a new WiFi log client if one is waiting.
  if (!logClient || !logClient.connected()) {
    WiFiClient client = logServer.available();
    if (client) {
      logClient = client;
      logClient.setNoDelay(true);
      logClient.printf("%s", DEVICE_ID);
      Serial.println("WiFi log client connected.");
    }
  }
  processTcpCommands(logClient, logCommandLine);

  if (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED && timeHasElapsed(now, nextMonitorConnectionCheckTime)) {
    nextMonitorConnectionCheckTime = now + MONITOR_CONNECTION_CHECK_INTERVAL_MS;
    if (!monitorClient.connected()) {
      connectToMonitorServer();
    }
  }
  processTcpCommands(monitorClient, monitorCommandLine);

  if (nextMonitorScanTime == 0 || timeHasElapsed(now, nextMonitorScanTime)) {
    printMessage("Reading monitor registers...");
    monitorScanIndex = 0;
    nextMonitorScanTime = now + monitorScanIntervalMs;

    readMonitorRegisters();
  }

  if (nextControlScanTime == 0 || timeHasElapsed(now, nextControlScanTime)) {
    printMessage("Reading control registers...");
    controlScanIndex = 0;
    nextControlScanTime = now + controlScanIntervalMs;

    readControlRegisters();
  }
}
