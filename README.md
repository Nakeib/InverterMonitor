# InverterMonitor

InverterMonitor is a collection of firmware and simulation tools for building a WiFi-enabled RS485/Modbus inverter monitoring system.

## Project Structure

- `inverter/`
  - `inverter.ino` - ESP8266 firmware for the inverter monitor device.
  - Connects to a configured WiFi network or falls back to an access point.
  - Polls inverter control and monitor registers over RS485/Modbus.
  - Forwards console logs over TCP to a WiFi client.
  - Accepts remote commands via serial or TCP command lines.

- `server/`
  - `main.cpp` - TCP monitor server and optional HTTP command/auth server.
  - Accepts monitor connections from devices and clients.
  - Supports interactive console command broadcasting.
  - Supports authorized HTTP command delivery using `auth.dat` and session tokens.
  - `CMakeLists.txt` - build configuration for `inverter_monitor_server`.

- `simulator_modbus/`
  - `main.cpp` - emulator for inverter Modbus register access.
  - Provides a fake register set for development and firmware testing.
  - `CMakeLists.txt` - build configuration for `simulator_modbus`.

- `simulator_monitor/`
  - `main.cpp` - monitor client simulator that connects to the server as if it were the ESP8266 monitor.
  - Sends simulated PV/battery register values periodically.
  - Accepts TCP configuration commands for WiFi and monitor target settings.
  - Stores settings in a local EEPROM file.
  - `CMakeLists.txt` - build configuration for `inverter_monitor_simulator`.

- `LICENSE` - MIT License.

## Features

- ESP8266 firmware with RS485/Modbus read and write support.
- Remote command control via serial and TCP.
- WiFi log forwarding on port `12345`.
- Server-side monitor client handling and command distribution.
- Authorization layer for HTTP command submission.
- Standalone simulators for inverter registers and monitor behavior.

## Subproject Details

### `inverter/` (ESP8266 firmware)

The firmware runs on ESP8266 hardware and supports:

- WiFi station mode to connect to a configured AP.
- Fallback access point `InverterComm` when the configured WiFi is unavailable.
- RS485-based Modbus register polling of inverter control and monitor values.
- A TCP log server on port `12345`.
- Incoming command parsing from serial or TCP.

#### User interaction

Users can connect to the device over serial or to the TCP log server and send newline-terminated commands.

#### Supported commands

- `WIFI [ssid] [password]`
  - Save WiFi credentials into EEPROM and reconnect.
- `HOST [address] [port]`
  - Save the monitor server target host and port into EEPROM.
- `SIM [ON|OFF|1|0]`
  - Enable or disable register simulation mode.
- `CTRL_SCAN_MS [value]`
  - Set the control register scan interval in milliseconds.
- `MON_SCAN_MS [value]`
  - Set the monitor register scan interval in milliseconds.
- `REG_SPACING_MS [value]`
  - Set the delay between individual register reads.
- `RELAY [number] [ON|OFF|1|0]`
  - Turn relay output 1-4 on or off.
- `[register] [uintValue]`
  - Write a raw Modbus register value by register address.

### `server/` (Monitor server)

The server hosts three main interfaces:

- Monitor connection server on port `16670` (default).
- HTTP command server on port `16671`.
- HTTP authorization server on port `16672`.

It also supports direct console input and a file-based command queue.

#### User interaction

- Type commands directly into the server console and press enter.
- Create a `command.dat` file containing one command per line; the server will broadcast those commands and then delete the file.
- Use HTTP endpoints to obtain a session token and submit commands remotely.

#### HTTP command flow

1. POST `/authorize` to port `16672` with a password in the request body.
   - Password is read from `auth.dat`.
   - A successful response returns a session token.
2. POST `/command` to port `16671` with body:
   - `<sessionId> <command>`
   - The server validates the session token and broadcasts the command to connected monitor clients.

#### Command broadcast

Commands sent from console, `command.dat`, or the HTTP API are forwarded to all connected clients. The server logs the command and counts connected monitor clients.

### `simulator_modbus/` (Modbus emulator)

This binary emulates inverter register behavior for firmware testing.

- It responds to Modbus requests using a fixed register map.
- On Windows, it can be built with `WINDOWS_SERIAL` to enable serial port support.
- No direct command syntax is required; it is intended as a backend emulator.

### `simulator_monitor/` (Monitor client simulator)

This simulator behaves like a monitor client that connects to the monitor server and sends periodic register updates.

#### User interaction

- Run with optional CLI arguments:
  - `--eeprom <path>` to specify the EEPROM file.
  - `--monitor-host <host>` to set the server host.
  - `--monitor-port <port>` to set the server port.
- It stores host and port configuration in the EEPROM file.
- It accepts TCP commands from the connected server to update:
  - `WIFI [ssid] [password]`
  - `HOST [address] [port]`
  - `[register] [uintValue]`

## Building

Each component can be built independently using CMake.

### Linux / macOS

```bash
cd server
cmake -S . -B build
cmake --build build

cd ../simulator_modbus
cmake -S . -B build
cmake --build build

cd ../simulator_monitor
cmake -S . -B build
cmake --build build
```

### Windows

For `simulator_modbus`, define `WINDOWS_SERIAL` when building in CMake to enable Windows serial support.

```powershell
cd simulator_modbus
cmake -S . -B build
cmake --build build
```

## Usage

### ESP8266 firmware

- Load `inverter/inverter.ino` onto an ESP8266 board.
- Configure WiFi and monitor server settings using commands over serial or TCP.
- The firmware will poll Modbus registers and send operation logs to connected WiFi clients.

### Server

- Run the server binary from `server/build`.
- Connect monitor clients to port `16670`.
- Use the server console or HTTP API to dispatch commands.
- Place a `command.dat` file next to the server executable to send queued commands.

### Simulator setup

- Run `simulator_modbus` when you need a fake inverter device.
- Run `simulator_monitor` to emulate the monitor client and connect it to the server.

## Notes

- `inverter.ino` uses `ESP8266WiFi`, `EEPROM`, `SoftwareSerial`, and `ModbusMaster`.
- The firmware stores WiFi credentials, monitor host/port, simulation mode, and timing settings in EEPROM.
- `server` supports interactive console commands, HTTP command submission, and file-based commands.
- `simulator_monitor` persists connection info in a local EEPROM file and sends periodic monitor register updates.

## License

This project is licensed under the MIT License. See `LICENSE` for details.
