#include <Arduino.h>
#include <stdarg.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <ModbusMaster.h>

#define SIMULATION_MODE

const int WIFI_SSID_MAX_LEN = 32;
const int WIFI_PASSWORD_MAX_LEN = 64;
const int WIFI_SSID_EEPROM_OFFSET = 0;
const int WIFI_PASSWORD_EEPROM_OFFSET = WIFI_SSID_EEPROM_OFFSET + WIFI_SSID_MAX_LEN + 1;
const int MONITOR_HOST_MAX_LEN = 64;
const int MONITOR_PORT_MAX_LEN = 6;
const int MONITOR_HOST_EEPROM_OFFSET = WIFI_PASSWORD_EEPROM_OFFSET + WIFI_PASSWORD_MAX_LEN + 1;
const int MONITOR_PORT_EEPROM_OFFSET = MONITOR_HOST_EEPROM_OFFSET + MONITOR_HOST_MAX_LEN + 1;
const int EEPROM_SIZE = MONITOR_PORT_EEPROM_OFFSET + MONITOR_PORT_MAX_LEN + 1;

char wifiSSID[WIFI_SSID_MAX_LEN + 1];
char wifiPassword[WIFI_PASSWORD_MAX_LEN + 1];
char monitorHost[MONITOR_HOST_MAX_LEN + 1];
char monitorPortString[MONITOR_PORT_MAX_LEN + 1];
uint16_t monitorPort = 0;

const uint8_t RELAY_PIN_COUNT = 4;
const uint8_t relayPins[RELAY_PIN_COUNT] = {D1, D2, D3, D4};
bool relayStates[RELAY_PIN_COUNT] = {false, false, false, false};

const int MAX_WIFI_ATTEMPTS = 20;

const uint16_t MAX_TCP_LINE_LENGTH = 64;
String logCommandLine;
String monitorCommandLine;
String serialCommandLine;

const unsigned long CONTROL_SCAN_INTERVAL_MS = 60000UL;
const unsigned long MONITOR_SCAN_INTERVAL_MS = 5000UL;
const unsigned long REGISTER_READ_SPACING_MS = 120UL;
const unsigned long MONITOR_CONNECTION_CHECK_INTERVAL_MS = 60000UL;

unsigned long nextControlScanTime = 0;
unsigned long nextMonitorScanTime = 0;
unsigned long nextMonitorConnectionCheckTime = 0;

size_t controlScanIndex = 0;
size_t monitorScanIndex = 0;
size_t controlRegisterCount = 0;
size_t monitorRegisterCount = 0;

// WiFi log server settings.
// Connect a TCP client to this port to receive console messages over WiFi.
const uint16_t LOG_SERVER_PORT = 12345;
WiFiServer logServer(LOG_SERVER_PORT);
WiFiClient logClient;
WiFiClient monitorClient;

// ESP8266 pins for RS485 TTL
// D5/D6 are common choices on NodeMCU/ESP8266 boards.
// This converter does not use a DE/RE direction pin.
const uint8_t RS485_RX_PIN = D5; // GPIO14: receive from RS485 converter
const uint8_t RS485_TX_PIN = D6; // GPIO12: transmit to RS485 converter

SoftwareSerial rs485Serial(RS485_RX_PIN, RS485_TX_PIN);
ModbusMaster modbus;

// Register access permissions.
enum AccessType {
  ACCESS_RO,
  ACCESS_RW
};

// Control register descriptor used for pretty printing and RW/RO validation.
struct RegItem {
  uint16_t address;    // Modbus register address from the inverter spec
  const char* name;    // Human-readable register name
  const char* unit;    // Scaling unit string for formatting
  AccessType access;   // Read-only or read/write register access
};

// Inverter control registers selected from the HTML register map.
// The address values are the decimal register addresses from the inverter spec.
// The unit strings describe the scaling for each raw register value.
RegItem controlRegisters[] = {
  {20101, "Inverter offgrid work enable", "", ACCESS_RW},   // 0 = OFF, 1 = ON
  {20102, "Inverter output voltage set", "0.1V", ACCESS_RW}, // 2200-2400 => 220.0-240.0 V
  {20103, "Inverter output frequency set", "0.01Hz", ACCESS_RW}, // 5000 = 50.00 Hz, 6000 = 60.00 Hz
  {20104, "Inverter search mode enable", "", ACCESS_RW}, // 0 = OFF, 1 = ON
  {20108, "Inverter discharge to grid enable", "", ACCESS_RW}, // 0 = OFF, 1 = ON (48V only)
  {20109, "Energy use mode", "", ACCESS_RW}, // 48V: 1=SBU,2=SUB,3=UTI,4=SOL; EP: 1=BAU,3=UTI,4=BOU; 12/24V: 1=SBU,3=UTI,4=SOL or 1=BU,3=UTI
  {20111, "Grid protect standard", "", ACCESS_RW}, // 0=VDE4105, 1=UPS, 2=home, 3=GEN
  {20112, "SolarUse Aim", "", ACCESS_RW}, // PV/PH: 0=LBU,1=BLU; EP: 0=LB,1=LU
  {20113, "Inverter max discharger current", "0.1A", ACCESS_RW}, // 10-217 => 1.0-21.7 A
  {20118, "Battery stop discharging voltage", "0.1V", ACCESS_RW}, // 220-290 => 22.0-29.0 V
  {20119, "Battery stop charging voltage", "0.1V", ACCESS_RW}, // 220-290 => 22.0-29.0 V (default 27.0 V)
  {10103, "Float voltage", "0.1V", ACCESS_RW}, // 240-292 => 24.0-29.2 V (default 27.0 V)
  {10104, "Absorption voltage", "0.1V", ACCESS_RW}, // 240-292 => 24.0-29.2 V (default 28.2 V)
  {10105, "Battery low voltage (PV/PH)", "0.1V", ACCESS_RW}, // 170-220 => 17.0-22.0 V (default 17.0 V)
  {10107, "Battery high voltage (PV/PH)", "0.1V", ACCESS_RW}, // 290-300 => 29.0-30.0 V (default 30.0 V)
  {20125, "Grid max charger current set", "0.1A", ACCESS_RW}, // 10-800 => 1.0-80.0 A (default 60.0 A)
  {20127, "Battery low voltage", "0.1V", ACCESS_RW}, // 170-220 => 17.0-22.0 V (default 17.0 V)
  {20128, "Battery high voltage", "0.1V", ACCESS_RW}, // 290-300 => 29.0-30.0 V (default 30.0 V)
  {20132, "Max Combine charger current", "0.1A", ACCESS_RW}, // 10-1400 => 1.0-140.0 A (default 60.0 A)
  {20142, "System setting", "", ACCESS_RW}, // bit field; see System setting bit frame
  {20143, "Charger source priority", "", ACCESS_RW}, // 0=Solar first,2=Solar+Utility,3=Only Solar; EP: 2=Utility enable,3=Utility disable
  {20144, "Solar power balance", "", ACCESS_RW} // 0=SBD, 1=SBE
};

// Monitor registers for real-time battery/PV measurements.
RegItem monitorRegisters[] = {
  {15205, "PV voltage", "0.1V", ACCESS_RO},      // 0.0-150.0 V
  {15206, "Battery voltage", "0.1V", ACCESS_RO}, // 0.0-80.0 V
  {15207, "PV charger current", "0.1A", ACCESS_RO}, // 0.0-90.0 A
  {15208, "PV charger power", "W", ACCESS_RO}, // 0-5000 W
  {25210, "Inverter current", "0.1A", ACCESS_RO}, // 0.0-?? A
  {25274, "Battery current", "A", ACCESS_RO}     // raw amps
};

void preTransmission() {
  // No DE pin is available on this converter.
  // The adapter must manage direction automatically.
}

void postTransmission() {
  // No DE pin is available on this converter.
}

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
  char buffer[128];
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
  // If no scaling is known, return raw integer value.
  return String(raw);
}

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
  WiFi.disconnect(true);

  resetScanTime();

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

void readControlRegisters(size_t index) {
  const RegItem& reg = controlRegisters[index];
#ifdef SIMULATION_MODE
  printRegisterResult(reg, 0);
#else
  uint8_t result = modbus.readHoldingRegisters(reg.address, 1);
  if (result == modbus.ku8MBSuccess) {
    uint16_t value = modbus.getResponseBuffer(0);
    printRegisterResult(reg, value);
  } else {
    printMessage("%5u %-36s => READ ERROR 0x%02X", reg.address, reg.name, result);
  }
#endif
}

void readMonitorRegisters(size_t index) {
  const RegItem& reg = monitorRegisters[index];
  uint8_t result = modbus.ku8MBSuccess;
  uint16_t value = 0;
#ifdef SIMULATION_MODE
  unsigned long now = millis();
  long rawValue = 0;
  switch (reg.address) {
    case 15205:
      rawValue = static_cast<long>(2200 + std::sin(now / 5000.0) * 50);
      rawValue += random(-8, 9);
      break;
    case 15206:
      rawValue = static_cast<long>(265 + std::sin(now / 100000.0) * 20);
      rawValue += random(-2, 2);
      break;
    case 15207:
      rawValue = static_cast<long>(150 + std::sin(now / 3000.0) * 40);
      rawValue += random(-6, 7);
      break;
    case 15208:
      rawValue = static_cast<long>(500 + std::sin(6.28 * now / 86400000.0) * 120);
      rawValue += random(-20, 21);
      break;
    case 25210:
      rawValue = static_cast<long>(800 + std::sin(now / 6000.0) * 100);
      rawValue += random(-10, 11);
      break;
    case 25274:
      rawValue = static_cast<long>(15 + std::sin(now / 10000.0) * 5);
      rawValue += random(-2, 3);
      break;
    default:
      rawValue = 0;
      break;
  }
  value = rawValue > 0 ? static_cast<uint16_t>(rawValue) : 0;
#else
  result = modbus.readHoldingRegisters(reg.address, 1);
  if (result == modbus.ku8MBSuccess) {
    value = modbus.getResponseBuffer(0);
  } else {
    printMessage("%5u %-36s => READ ERROR 0x%02X", reg.address, reg.name, result);
  }
#endif
  printRegisterResult(reg, value);
}

const RegItem* findRegister(uint16_t address) {
  for (const RegItem& reg : controlRegisters) {
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
  digitalWrite(relayPins[relayIndex], active ? HIGH : LOW);
  relayStates[relayIndex] = active;
  return true;
}

String relayStateText(bool active) {
  return active ? String("ON") : String("OFF");
}

void printRelaysState() {
  char buffer[32];

  for (uint8_t i = 0; i < RELAY_PIN_COUNT; ++i) {
    printMessage("R%u: %s", static_cast<unsigned int>(i + 1), relayStateText(relayStates[i]).c_str());
    if (monitorClient && monitorClient.connected()) {
      int len = snprintf(buffer, sizeof(buffer), "%u %u\n", i + 1, relayStates[i] ? 1 : 0);
      if (len > 0) {
        monitorClient.write(reinterpret_cast<const uint8_t*>(buffer), len);
      }
    }
  }
}

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

  unsigned long rawValue = strtoul(valueText.c_str(), &endPtr, 10);
  if (endPtr == valueText.c_str() || rawValue > 0xFFFF) {
    printMessage("TCP CMD: invalid register value");
    return;
  }

  uint16_t address = static_cast<uint16_t>(rawAddr);
  uint16_t value = static_cast<uint16_t>(rawValue);

  if (address <= RELAY_PIN_COUNT) {
     uint8_t relayIndex = static_cast<uint8_t>(address - 1);
     setRelayState(relayIndex, value != 0);
     printMessage("TCP CMD: RELAY %u %s", address, relayStateText(relayStates[relayIndex]).c_str());
  }
  else {
    const RegItem* reg = findRegister(address);
    if (!reg) {
      printMessage("TCP CMD: unknown register " + String(address));
      return;
    }
    if (reg->access != ACCESS_RW) {
      printMessage("TCP CMD: register " + String(address) + " is read-only");
      return;
    }

    uint8_t result = modbus.writeSingleRegister(address, value);
    if (result == modbus.ku8MBSuccess) {
      unsigned long now = millis();
      printMessage("TCP CMD: wrote " + String(address) + " = " + String(value));
      controlScanIndex = 0;
      nextControlScanTime = now;
    } else {
      printMessage("TCP CMD: write failed for %u, error 0x%02X\n", address, result);
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
    if (commandLine.length() < MAX_TCP_LINE_LENGTH) {
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
    if (serialCommandLine.length() < MAX_TCP_LINE_LENGTH) {
      serialCommandLine += c;
    }
  }
}

void printRegisterResult(const RegItem& reg, uint16_t value) {
  String formatted = formatValue(value, reg.unit);
  if (strlen(reg.unit) > 0) {
    printMessage("%5u %-36s = %-12s (%s)", reg.address, reg.name, formatted.c_str(), reg.unit);
  } else {
    printMessage("%5u %-36s = %s", reg.address, reg.name, formatted.c_str());
  }
  if (monitorClient && monitorClient.connected()) {
    char buffer[32];
    int len = snprintf(buffer, sizeof(buffer), "%u %u\n", reg.address, value);
    if (len > 0) {
      monitorClient.write(reinterpret_cast<const uint8_t*>(buffer), len);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.printf("ESP8266 Inverter Modbus RTU Control Register Reader");

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

  connectToWiFi();

#ifdef SIMULATION_MODE
  randomSeed(micros());
#endif

  // Start the TCP log server for clients that want console output over WiFi.
  logServer.begin();
  Serial.printf("WiFi log server listening on port %u\n", LOG_SERVER_PORT);

  // Initialize relay outputs and set them off by default.
  for (uint8_t i = 0; i < RELAY_PIN_COUNT; ++i) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
    relayStates[i] = false;
  }

  // Start TTL serial for the RS485 converter at 19200 baud.
  rs485Serial.begin(19200);

  // Modbus slave ID 4 as specified by the inverter sheet.
  modbus.begin(4, rs485Serial);
  modbus.preTransmission(preTransmission);
  modbus.postTransmission(postTransmission);

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
      Serial.println("WiFi log client connected.");
    }
  }
  processTcpCommands(logClient, logCommandLine);

  if (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED && now >= nextMonitorConnectionCheckTime) {
    nextMonitorConnectionCheckTime = now + MONITOR_CONNECTION_CHECK_INTERVAL_MS;
    if (!monitorClient.connected()) {
      connectToMonitorServer();
    }
  }
  processTcpCommands(monitorClient, monitorCommandLine);

  if (controlScanIndex >= controlRegisterCount && (nextControlScanTime == 0 || now >= nextControlScanTime)) {
    printMessage("Reading control registers...");
    controlScanIndex = 0;
    nextControlScanTime = now;
  }

  if (controlScanIndex < controlRegisterCount && now >= nextControlScanTime) {
    readControlRegisters(controlScanIndex);
    controlScanIndex++;
    if (controlScanIndex < controlRegisterCount) {
      nextControlScanTime = now + REGISTER_READ_SPACING_MS;
    } else {
      nextControlScanTime = now + CONTROL_SCAN_INTERVAL_MS;
    }
  }

  if (monitorScanIndex >= monitorRegisterCount && (nextMonitorScanTime == 0 || now >= nextMonitorScanTime)) {
    printMessage("Reading monitor registers...");
    monitorScanIndex = 0;
    nextMonitorScanTime = now;
  }

  if (monitorScanIndex < monitorRegisterCount && now >= nextMonitorScanTime) {
    printRelaysState();

    readMonitorRegisters(monitorScanIndex);
    monitorScanIndex++;
    if (monitorScanIndex < monitorRegisterCount) {
      nextMonitorScanTime = now + REGISTER_READ_SPACING_MS;
    } else {
      nextMonitorScanTime = now + MONITOR_SCAN_INTERVAL_MS;
    }
  }
}
