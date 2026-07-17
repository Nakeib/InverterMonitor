#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>

#define DEVICE_ID "TemperatureMonitor v0.1\n"

// OneWire bus pin for DS18B20 sensors.
// On ESP8266 boards like NodeMCU, D2 is GPIO4.
#define ONE_WIRE_BUS D2

// EEPROM layout constants
const int WIFI_SSID_MAX_LEN = 32;
const int WIFI_PASSWORD_MAX_LEN = 64;
const int MONITOR_HOST_MAX_LEN = 64;
const int MONITOR_PORT_MAX_LEN = 6;

const int WIFI_SSID_EEPROM_OFFSET = 0;
const int WIFI_PASSWORD_EEPROM_OFFSET = WIFI_SSID_EEPROM_OFFSET + WIFI_SSID_MAX_LEN + 1;
const int MONITOR_HOST_EEPROM_OFFSET = WIFI_PASSWORD_EEPROM_OFFSET + WIFI_PASSWORD_MAX_LEN + 1;
const int MONITOR_PORT_EEPROM_OFFSET = MONITOR_HOST_EEPROM_OFFSET + MONITOR_HOST_MAX_LEN + 1;

const int EEPROM_SIZE = MONITOR_PORT_EEPROM_OFFSET + MONITOR_PORT_MAX_LEN + 1;

char wifiSSID[WIFI_SSID_MAX_LEN + 1];
char wifiPassword[WIFI_PASSWORD_MAX_LEN + 1];
char monitorHost[MONITOR_HOST_MAX_LEN + 1];
char monitorPortString[MONITOR_PORT_MAX_LEN + 1];
uint16_t monitorPort = 16670;

// Scanning intervals and state for control and monitor registers.
const unsigned long WIFI_STATION_RETRY_INTERVAL_MS = 300000UL;
const unsigned long MONITOR_CONNECTION_CHECK_INTERVAL_MS = 60000UL;
const unsigned long TEMPERATURE_SCAN_INTERVAL_MS = 30000UL;

unsigned long nextWifiStationRetryTime = 0;
unsigned long nextMonitorConnectionCheckTime = 0;
unsigned long nextTemperatureScanTime = 0;

// WiFi log server settings.
// Connect a TCP client to this port to receive console messages over WiFi.
const int MAX_WIFI_ATTEMPTS = 20;
const uint16_t LOG_SERVER_PORT = 12345;

WiFiServer logServer(LOG_SERVER_PORT);
WiFiClient logClient;
WiFiClient monitorClient;

// Command line buffers for TCP and serial input.
const uint16_t MAX_COMMAND_LINE_LENGTH = 256;

String logCommandLine;
String monitorCommandLine;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

const uint16_t SENSOR_REGISTER_BASE = 10000;
const uint8_t MAX_TEMPERATURE_SENSORS = 4;
uint8_t connectedSensorCount = 0;
DeviceAddress connectedSensorAddresses[MAX_TEMPERATURE_SENSORS];

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

void printTemperature(uint16_t address, float value) {
  printMessage("%5u %-36s = %.2f", address, "Temperature", value);
  if (monitorClient && monitorClient.connected()) {
    char buffer[64];
    int len = snprintf(buffer, sizeof(buffer), "%u %.2f\n", address, value);
    if (len > 0) {
      monitorClient.write(reinterpret_cast<const uint8_t*>(buffer), len);
    }
  }
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

bool timeHasElapsed(unsigned long now, unsigned long targetTime) {
  return static_cast<long>(now - targetTime) >= 0;
}

// ===== Connection helpers =====
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
  Serial.println("WiFi connection failed, starting fallback AP 'TemperatureComm'");
  monitorClient.stop();
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 1, 1);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);
  WiFi.softAP("TemperatureComm", "12345678");
  Serial.print("Fallback AP started, IP address: ");
  Serial.println(WiFi.softAPIP());
  return false;
}

// ===== Temperature sensor helpers =====
void printAddress(const DeviceAddress deviceAddress) {
  for (uint8_t i = 0; i < 8; i++) {
    if (deviceAddress[i] < 16) Serial.print("0");
    Serial.print(deviceAddress[i], HEX);
  }
}

int compareSensorAddresses(const void* a, const void* b) {
  const uint8_t* lhs = reinterpret_cast<const uint8_t*>(a);
  const uint8_t* rhs = reinterpret_cast<const uint8_t*>(b);
  for (uint8_t i = 0; i < 8; ++i) {
    if (lhs[i] < rhs[i]) return -1;
    if (lhs[i] > rhs[i]) return 1;
  }
  return 0;
}

void detectTemperatureSensors() {
  connectedSensorCount = 0;
  memset(connectedSensorAddresses, 0, sizeof(connectedSensorAddresses));
  uint8_t totalDevices = sensors.getDeviceCount();
  printMessage("OneWire bus reports %u device(s).", totalDevices);

  for (uint8_t index = 0; index < totalDevices && connectedSensorCount < MAX_TEMPERATURE_SENSORS; ++index) {
    DeviceAddress deviceAddress;
    if (!sensors.getAddress(deviceAddress, index)) {
      continue;
    }
    if (!sensors.validAddress(deviceAddress)) {
      continue;
    }
    memcpy(connectedSensorAddresses[connectedSensorCount++], deviceAddress, sizeof(DeviceAddress));
  }

  if (connectedSensorCount > 1) {
    qsort(connectedSensorAddresses, connectedSensorCount, sizeof(DeviceAddress), compareSensorAddresses);
  }

  if (connectedSensorCount == 0) {
    printMessage("No DS18B20 sensors found on the OneWire bus.");
    return;
  }

  printMessage("Detected %u DS18B20 sensor(s):", connectedSensorCount);
  for (uint8_t i = 0; i < connectedSensorCount; ++i) {
    Serial.printf("  register %u -> ", SENSOR_REGISTER_BASE + i);
    printAddress(connectedSensorAddresses[i]);
    Serial.println();
  }

  if (totalDevices > MAX_TEMPERATURE_SENSORS) {
    printMessage("Warning: only first %u sensors are mapped.", MAX_TEMPERATURE_SENSORS);
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

    nextTemperatureScanTime = 0; // Force immediate temperature scan after WiFi reconnect
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

  if (line == "DETECT") {
    printMessage("TCP CMD: DETECT received, scanning for temperature sensors...");
    detectTemperatureSensors();
    return;
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

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("Temperature monitor starting...");

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

  // Start the TCP log server for clients that want console output over WiFi.
  logServer.begin();
  Serial.printf("WiFi log server listening on port %u\n", LOG_SERVER_PORT);

  sensors.begin();
  sensors.setResolution(12);
  detectTemperatureSensors();
}

float readTemperature(const DeviceAddress deviceAddress) {
  if (!sensors.isConnected(deviceAddress)) {
    return NAN;
  }

  sensors.requestTemperaturesByAddress(deviceAddress);
  float tempC = sensors.getTempC(deviceAddress);
  return tempC;
}

void loop() {
  unsigned long now = millis();

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

  if (nextTemperatureScanTime == 0 || timeHasElapsed(now, nextTemperatureScanTime)) {
    printMessage("Reading temperature sensors...");
    nextTemperatureScanTime = now + TEMPERATURE_SCAN_INTERVAL_MS;

    if (connectedSensorCount == 0) {
      printMessage("No temperature sensors available for reading.");
    } else {
      sensors.requestTemperatures();
      for (uint8_t sensorIndex = 0; sensorIndex < connectedSensorCount; ++sensorIndex) {
        float temperature = sensors.getTempC(connectedSensorAddresses[sensorIndex]);
        printTemperature(SENSOR_REGISTER_BASE + sensorIndex, temperature);
      }
    }
  }
}
