#include <cmath>
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

static constexpr uint8_t kSlaveId = 4;
static constexpr uint32_t kBaudRate = 19200;

struct SerialPort {
#ifdef WINDOWS_SERIAL
  HANDLE handle = INVALID_HANDLE_VALUE;
#else
  int fd = -1;
#endif

  bool openPort(const std::string& device) {
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
    dcb.BaudRate = CBR_19200;
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
    cfsetispeed(&options, B19200);
    cfsetospeed(&options, B19200);
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

uint16_t computeCrc(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int j = 0; j < 8; ++j) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void appendCrc(std::vector<uint8_t>& frame) {
  uint16_t crc = computeCrc(frame.data(), frame.size());
  frame.push_back(static_cast<uint8_t>(crc & 0xFF));
  frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
}

bool validCrc(const std::vector<uint8_t>& frame, size_t length) {
  if (length < 3) {
    return false;
  }
  uint16_t expected = computeCrc(frame.data(), length - 2);
  uint16_t actual = static_cast<uint16_t>(frame[length - 2]) |
                    (static_cast<uint16_t>(frame[length - 1]) << 8);
  return expected == actual;
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

uint16_t generateMonitorValue(uint16_t address) {
  using namespace std::chrono;
  auto now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  switch (address) {
    case 15205: {
      double value = 2200 + std::sin(now / 5000.0) * 50 + (std::rand() % 17 - 8);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 15206: {
      double value = 265 + std::sin(now / 100000.0) * 20 + (std::rand() % 5 - 2);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 15207: {
      double value = 150 + std::sin(now / 3000.0) * 40 + (std::rand() % 13 - 6);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 15208: {
      double value = 500 + std::sin(6.28 * now / 86400000.0) * 120 + (std::rand() % 41 - 20);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 25212: {
      double value = 800 + std::sin(now / 6000.0) * 100 + (std::rand() % 21 - 10);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    case 25274: {
      double value = 15 + std::sin(now / 10000.0) * 5 + (std::rand() % 5 - 2);
      return static_cast<uint16_t>(value < 0 ? 0 : value);
    }
    default:
      return 0;
  }
}

void printUsage(const std::string& programName) {
  std::cout << "Usage: " << programName << " <serial-port> [slave-id]\n";
  std::cout << "Example: " << programName << " COM3 4\n";
  std::cout << "Example: " << programName << " /dev/ttyUSB0 4\n";
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  std::srand(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));

  std::string device = argv[1];
  uint8_t slaveId = kSlaveId;
  if (argc >= 3) {
    int parsed = std::atoi(argv[2]);
    if (parsed < 1 || parsed > 247) {
      std::cerr << "Invalid slave id: " << argv[2] << "\n";
      return 1;
    }
    slaveId = static_cast<uint8_t>(parsed);
  }

  SerialPort serial;
  if (!serial.openPort(device)) {
    std::cerr << "Failed to open serial port: " << device << "\n";
    return 1;
  }

  std::cout << "Modbus RTU simulator running on " << device << " (slave id=" << static_cast<int>(slaveId)
            << ")\n";
  std::cout << "Press Ctrl+C to exit.\n";

  auto registers = makeRegisterMap();
  std::vector<uint8_t> rxBuffer;

  while (true) {
    auto bytes = serial.readAvailable();
    if (!bytes.empty()) {
      rxBuffer.insert(rxBuffer.end(), bytes.begin(), bytes.end());
    }

    while (rxBuffer.size() >= 8) {
      uint8_t address = rxBuffer[0];
      uint8_t function = rxBuffer[1];
      size_t expectedLength = 0;

      if (function == 0x03 || function == 0x06) {
        expectedLength = 8;
      } else {
        rxBuffer.erase(rxBuffer.begin());
        continue;
      }

      if (rxBuffer.size() < expectedLength) {
        break;
      }

      if (!validCrc(rxBuffer, expectedLength)) {
        rxBuffer.erase(rxBuffer.begin());
        continue;
      }

      if (address != slaveId) {
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + expectedLength);
        continue;
      }

      std::vector<uint8_t> response;
      response.push_back(address);
      response.push_back(function);

      uint16_t startAddress = static_cast<uint16_t>(rxBuffer[2] << 8 | rxBuffer[3]);
      uint16_t quantity = static_cast<uint16_t>(rxBuffer[4] << 8 | rxBuffer[5]);

      if (function == 0x03) {
        if (quantity == 0 || quantity > 125) {
          response[1] = 0x83;
          response.push_back(0x03);
        } else {
          bool valid = true;
          std::vector<uint8_t> payload;
          payload.reserve(quantity * 2 + 1);
          payload.push_back(static_cast<uint8_t>(quantity * 2));
          for (uint16_t offset = 0; offset < quantity; ++offset) {
            uint16_t addressValue = startAddress + offset;
            auto it = registers.find(addressValue);
            if (it == registers.end()) {
              valid = false;
              break;
            }
            uint16_t value = it->second.writable ? it->second.value : generateMonitorValue(addressValue);
            payload.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            payload.push_back(static_cast<uint8_t>(value & 0xFF));
          }
          if (!valid) {
            response[1] = 0x83;
            response.push_back(0x02);
          } else {
            response.insert(response.end(), payload.begin(), payload.end());
          }
        }
      } else if (function == 0x06) {
        auto it = registers.find(startAddress);
        if (it == registers.end()) {
          response[1] = 0x86;
          response.push_back(0x02);
        } else if (!it->second.writable) {
          response[1] = 0x86;
          response.push_back(0x03);
        } else {
          uint16_t value = static_cast<uint16_t>(rxBuffer[4] << 8 | rxBuffer[5]);
          it->second.value = value;
          response.push_back(rxBuffer[2]);
          response.push_back(rxBuffer[3]);
          response.push_back(rxBuffer[4]);
          response.push_back(rxBuffer[5]);
          std::cout << "WRITE " << startAddress << " = " << value << "\n";
        }
      }

      appendCrc(response);
      serial.writeBytes(response);
      rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + expectedLength);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  serial.closePort();
  return 0;
}
