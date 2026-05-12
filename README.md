ESP8266 Volt Sinus Pro Ultra 6000 Project
========================================

This repository contains three main components:

1. `inverter/` - ESP8266 firmware for inverter control, WiFi, RS485 communication, and monitor forwarding.
2. `server/` - native C++ server and browser UI for monitoring register values, charting history, and sending commands.
3. `simulator/` - native C++ simulator that connects to the monitor server and sends simulated inverter register values.

Additional documentation files are included for the inverter protocol and register spec.

---

Folder Contents
---------------

inverter/
~~~~~~~~~
- `inverter.ino`
  - ESP8266 sketch for inverter interaction.
  - Provides TCP-based command handling, WiFi setup, EEPROM storage, RS485-to-Modbus access, and monitor forwarding.
  - Controls include relay switching, Modbus register writes, and runtime configuration of the monitor server endpoint.

server/
~~~~~~~
- `main.cpp`
  - Native server application implementing:
    - monitor server input for register updates
    - HTTP command server for sending device commands
    - HTTP authorization server for admin login
    - register and history persistence
  - Generates `registers.json`, `power.dat`, and `battery.dat` for the browser UI.
- `index.html`
  - Browser interface for viewing registers, PV power history, battery voltage history, and issuing commands.
- `CMakeLists.txt`
  - Build script for the native server application.

simulator/
~~~~~~~~~~~
- `main.cpp`
  - Native simulator application that:
    - connects to the monitor server over TCP
    - sends synthetic monitor register measurements on a fixed interval
    - listens for simple text commands from the server
  - Uses a local EEPROM-style file to remember monitor host/port settings.
- `CMakeLists.txt`
  - Build script for the native simulator application.

---

Build and Run
-------------

### Server

1. Enter the `server/` folder.
2. Create a build directory and configure:
   ```bash
   mkdir build
   cd build
   cmake ..
   cmake --build .
   ```
3. Run the built executable:
   ```bash
   ./inverter_monitor_server
   ```

### Simulator

1. Enter the `simulator/` folder.
2. Build the simulator:
   ```bash
   mkdir build
   cd build
   cmake ..
   cmake --build .
   ```
3. Run the simulator with the monitor server address:
   ```bash
   ./inverter_monitor_simulator --monitor-host 127.0.0.1 --monitor-port 16670
   ```
4. Optional EEPROM file path:
   ```bash
   ./inverter_monitor_simulator --eeprom simulator_eeprom.bin
   ```

### Inverter Firmware

- The `inverter/inverter.ino` sketch is intended for ESP8266 boards with the Arduino/ESP8266 core installed.
- It requires `ESP8266WiFi`, `EEPROM`, `SoftwareSerial`, and `ModbusMaster` libraries.
- Load the sketch into an ESP8266 board and power it.

---

User Interaction and Control
----------------------------

### Web UI (`server/index.html`)

The browser UI provides the following controls and displays:

- Register monitor table
  - Loads `registers.json` and displays address, name, and current value.
- Common settings
  - `Max points` controls the number of points shown in charts.
  - `Timezone` adjusts label conversion for timestamps.
- PV Power History
  - `From day` / `To day` filters PV power history from `power.dat`.
- Battery Voltage History
  - `From day` / `To day` filters battery voltage history from `battery.dat`.
- Admin authorization
  - Enter the admin password and click `Authorize`.
  - A session token is returned and displayed as authorized state.
  - The web UI shows a live session countdown timer.
- Command sending
  - Enter a text command and click `Submit` to send it to the HTTP command server.
  - Command requests are authorized using the active admin session.
  - If authorization expires or is rejected, the UI returns to `Unauthorized`.

### HTTP Authorization and Command Flow

The server uses an `auth.dat` file for the admin password. The browser UI must:

- POST the password to `/authorize` on port `16672`.
- Receive a session ID and remain authorized for the session lifetime.
- POST commands to `/command` on port `16671` using the session ID prefix.

### Inverter TCP Command Interface

The `inverter/` firmware can receive TCP text commands on its network socket. Supported direct commands include:

- `WIFI [ssid] [password]`
  - Update saved WiFi credentials and reconnect.
- `HOST [address] [port]`
  - Update the monitor server host and port and reconnect.
- `RELAY [number] [ON|OFF|1|0]`
  - Toggle relay outputs 1-4.
- `[regAddr] [uintValue]`
  - Write a raw 16-bit register value to the inverter if the address is writable.

Additional inverter behavior:
- WiFi credentials are stored in EEPROM.
- Monitor host and port are stored in EEPROM.
- If the board cannot connect to configured WiFi, it falls back to a local AP mode [InverterComm | 12345678 / 192.168.1.1:12345].
- It also opens a WiFi log server port for console messages.

### Simulator Interaction

The simulator sends synthetic monitor register values to the monitor server and accepts simple responses.

- Connects to the monitor server host and port supplied by CLI or EEPROM config.
- Sends repeated register updates for PV voltage, battery voltage, PV charger current, PV charger power, inverter current, and battery current.
- Listens for incoming text responses or commands from the monitor server.

---

Notes
-----

- The project includes protocol documentation in the root file names starting with `PH1800 PV1800 EP1800 PV3500 EP3500 RS485 Modbud RTU communication Protocol1.4.15`.
- The native server and simulator use POSIX sockets and pthreads, so they are meant for Linux/macOS or a compatible environment.
- The ESP8266 firmware uses the Arduino-style `inverter.ino` sketch and is intended for deployment to a compatible board.
