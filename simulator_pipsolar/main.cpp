#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef WINDOWS_SERIAL
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#endif

static constexpr uint32_t kBaudRate = 9600;

#ifndef WINDOWS_SERIAL
static bool getTermiosBaud(uint32_t baud, speed_t& speed) {
  switch (baud) {
    case 300: speed = B300; return true;
    case 1200: speed = B1200; return true;
    case 2400: speed = B2400; return true;
    case 4800: speed = B4800; return true;
    case 9600: speed = B9600; return true;
    case 19200: speed = B19200; return true;
    case 38400: speed = B38400; return true;
    case 57600: speed = B57600; return true;
    case 115200: speed = B115200; return true;
    default: return false;
  }
}
#endif

struct SerialPort {
#ifdef WINDOWS_SERIAL
  HANDLE handle = INVALID_HANDLE_VALUE;
#else
  int fd = -1;
#endif

  bool openPort(const std::string& device, uint32_t baudRate = kBaudRate) {
#ifdef WINDOWS_SERIAL
    std::string path = device;
    if (path.rfind("COM", 0) == 0 && path.size() > 4) {
      path = "\\\\.\\" + path;
    }
    handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(handle, &dcb)) {
      closePort();
      return false;
    }
    dcb.BaudRate = static_cast<DWORD>(baudRate);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(handle, &dcb)) {
      closePort();
      return false;
    }

    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(handle, &timeouts)) {
      closePort();
      return false;
    }
#else
    fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
      return false;
    }
    struct termios options;
    if (tcgetattr(fd, &options) != 0) {
      closePort();
      return false;
    }
    speed_t speed;
    if (!getTermiosBaud(baudRate, speed)) {
      closePort();
      return false;
    }
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);
    options.c_cflag = (options.c_cflag & ~CSIZE) | CS8;
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CRTSCTS;
    options.c_cflag |= CLOCAL | CREAD;
    options.c_lflag = 0;
    options.c_iflag = 0;
    options.c_oflag = 0;
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 1;
    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &options) != 0) {
      closePort();
      return false;
    }
#endif
    return true;
  }

  void closePort() {
#ifdef WINDOWS_SERIAL
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
      handle = INVALID_HANDLE_VALUE;
    }
#else
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
#endif
  }

  bool writeBytes(const std::vector<uint8_t>& bytes) {
#ifdef WINDOWS_SERIAL
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
      return false;
    }
    return written == bytes.size();
#else
    ssize_t written = ::write(fd, bytes.data(), bytes.size());
    return written == static_cast<ssize_t>(bytes.size());
#endif
  }

  std::vector<uint8_t> readAvailable() {
    std::vector<uint8_t> result;
#ifdef WINDOWS_SERIAL
    DWORD toRead = 0;
    COMSTAT status;
    DWORD errors = 0;
    if (!ClearCommError(handle, &errors, &status)) {
      return result;
    }
    toRead = status.cbInQue;
    if (toRead == 0) {
      return result;
    }
    result.resize(toRead);
    DWORD read = 0;
    if (!ReadFile(handle, result.data(), toRead, &read, nullptr)) {
      result.clear();
      return result;
    }
    result.resize(read);
#else
    uint8_t buffer[256];
    ssize_t readBytes = ::read(fd, buffer, sizeof(buffer));
    if (readBytes > 0) {
      result.insert(result.end(), buffer, buffer + readBytes);
    }
#endif
    return result;
  }
};

uint16_t computePipsolarCrc(const uint8_t* data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
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

uint16_t generateMonitorValue(uint16_t address) {
  using namespace std::chrono;
  auto now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  switch (address) {
    case 200: {
      double value = 2200 + std::sin(now / 5000.0) * 50 + (std::rand() % 17 - 8);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 201: {
      double value = 265 + std::sin(now / 100000.0) * 20 + (std::rand() % 5 - 2);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 202: {
      double value = 150 + std::sin(now / 3000.0) * 40 + (std::rand() % 13 - 6);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 203: {
      double value = 500 + std::sin(6.28 * now / 86400000.0) * 120 + (std::rand() % 41 - 20);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 204: {
      double value = 800 + std::sin(now / 6000.0) * 100 + (std::rand() % 21 - 10);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 205: {
      double value = 15 + std::sin(now / 10000.0) * 5 + (std::rand() % 5 - 2);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 206: {
      double value = 40 + std::sin(now / 7000.0) * 8 + (std::rand() % 9 - 4);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    default:
      return 0;
  }
}

std::string toHexString(const std::vector<uint8_t>& data) {
  std::string result;
  result.reserve(data.size() * 3);
  char buffer[4];
  for (size_t i = 0; i < data.size(); ++i) {
    if (i > 0) {
      result.push_back(' ');
    }
    snprintf(buffer, sizeof(buffer), "%02X", data[i]);
    result += buffer;
  }
  return result;
}

std::string makePipsolarResponse(const std::string& command) {
  if (command == "QPIGS") {
    std::vector<std::string> fields(20, "0");
    fields[4] = std::to_string(generateMonitorValue(206)); // field 5: load power
    fields[7] = std::to_string(generateMonitorValue(200)); // field 8: battery voltage
    fields[8] = std::to_string(generateMonitorValue(201)); // field 9: battery charging current
    fields[12] = std::to_string(generateMonitorValue(203)); // field 13: PV voltage
    fields[14] = std::to_string(generateMonitorValue(202)); // field 15: battery discharging current
    fields[18] = std::to_string(generateMonitorValue(205)); // field 19: PV input power

    std::string response = "(";
    for (size_t i = 0; i < fields.size(); ++i) {
      if (i > 0) {
        response += ' ';
      }
      response += fields[i];
    }
    response += ")";
    return response;
  }
  if (command == "QPIRI") {
    return "(0 0 0 0 0 0 0 275 210 250 240 0 0 0 0 0 0 0 0 0 0 0)";
  }
  return "(0)";
}

bool validPipsolarFrame(const std::vector<uint8_t>& frame, size_t length) {
  if (length < 4) {
    return false;
  }
  uint16_t receivedCrc = (static_cast<uint16_t>(frame[length - 3]) << 8) |
                         static_cast<uint16_t>(frame[length - 2]);
  uint16_t expectedCrc = computePipsolarCrc(frame.data(), length - 3);
  return receivedCrc == expectedCrc;
}

struct RegisterInfo {
  uint16_t value;
  bool writable;
};

std::map<uint16_t, RegisterInfo> makeRegisterMap() {
  std::map<uint16_t, RegisterInfo> registers = {
    {20101, {0, true}},
    {20102, {2300, true}},
    {20103, {5000, true}},
    {20104, {0, true}},
    {20108, {0, true}},
    {20109, {1, true}},
    {20111, {0, true}},
    {20112, {0, true}},
    {20113, {100, true}},
    {20118, {270, true}},
    {20119, {270, true}},
    {10103, {270, true}},
    {10104, {282, true}},
    {10105, {170, true}},
    {10107, {300, true}},
    {20125, {600, true}},
    {20127, {170, true}},
    {20128, {300, true}},
    {20132, {600, true}},
    {20142, {0, true}},
    {20143, {0, true}},
    {20144, {0, true}},
    {15205, {2200, false}},
    {15206, {265, false}},
    {15207, {150, false}},
    {15208, {500, false}},
    {25212, {800, false}},
    {25274, {15, false}},
  };
  return registers;
}

void printUsage(const std::string& programName) {
  std::cout << "Usage: " << programName << " <serial-port> [baud]\n";
  std::cout << "Example: " << programName << " COM3 9600\n";
  std::cout << "Example: " << programName << " /dev/ttyUSB0 9600\n";
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  std::srand(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));

  std::string device = argv[1];
  uint32_t baudRate = kBaudRate;
  if (argc >= 3) {
    char* endPtr = nullptr;
    unsigned long parsed = std::strtoul(argv[2], &endPtr, 10);
    if (endPtr == argv[2] || parsed == 0) {
      std::cerr << "Invalid baud rate: " << argv[2] << "\n";
      return 1;
    }
    baudRate = static_cast<uint32_t>(parsed);
  }

  SerialPort serial;
  if (!serial.openPort(device, baudRate)) {
    std::cerr << "Failed to open serial port: " << device << " at baud " << baudRate << "\n";
    return 1;
  }

  std::cout << "PiSolar simulator running on " << device << "\n";
  std::cout << "Press Ctrl+C to exit.\n";

  std::vector<uint8_t> rxBuffer;

  while (true) {
    auto bytes = serial.readAvailable();
    if (!bytes.empty()) {
      rxBuffer.insert(rxBuffer.end(), bytes.begin(), bytes.end());
    }

    auto frameEnd = std::find(rxBuffer.begin(), rxBuffer.end(), static_cast<uint8_t>(0x0D));
    while (frameEnd != rxBuffer.end()) {
      size_t frameLength = std::distance(rxBuffer.begin(), frameEnd) + 1;
      std::vector<uint8_t> requestBytes(rxBuffer.begin(), rxBuffer.begin() + frameLength);
      if (frameLength >= 4 && validPipsolarFrame(rxBuffer, frameLength)) {
        std::string command(reinterpret_cast<const char*>(rxBuffer.data()), frameLength - 3);
        std::cout << "RX: " << toHexString(requestBytes) << "\n";

        std::string responseText = makePipsolarResponse(command);
        std::vector<uint8_t> response(responseText.begin(), responseText.end());
        uint16_t crc = computePipsolarCrc(response.data(), response.size());
        response.push_back(static_cast<uint8_t>(crc >> 8));
        response.push_back(static_cast<uint8_t>(crc & 0xFF));
        response.push_back(0x0D);
        std::cout << "TX: " << toHexString(response) << "\n";
        serial.writeBytes(response);
      } else {
        std::cout << "RX invalid frame raw: " << toHexString(requestBytes) << "\n";
      }
      rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + frameLength);
      frameEnd = std::find(rxBuffer.begin(), rxBuffer.end(), static_cast<uint8_t>(0x0D));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  serial.closePort();
  return 0;
}
