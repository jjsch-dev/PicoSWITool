# PicoSWITool

A firmware tool for testing SWI (Single-Wire Interface) EEPROM emulators, specifically designed for the AT21CS11. This tool runs on Raspberry Pi Pico boards (RP2040 and RP2040) and facilitates sending commands and receiving responses to verify the functionality of an EEPROM emulation.

## Table of Contents

* [Features](#features)
* [Hardware Requirements](#hardware-requirements)
* [Software Requirements](#software-requirements)
* [Installation](#installation)
* [Usage](#usage)
    * [JSON Command Format](#json-command-format)
    * [Command Details](#command-details)
* [Implementation Details](#implementation-details)
* [Timing](#timing)
* [Contributing](#contributing)
* [License](#license)
* [Author](#author)

## Features

* Sends commands to an SWI EEPROM emulator.
* Receives and parses responses.
* Supports various AT21CS11 commands (discovery, read/write, etc.).
* Utilizes the RP2040's dual-core architecture for efficient timing and USB communication.
* Communicates via USB serial using JSON commands.
* Provides feedback via JSON responses.

## Hardware Requirements

* Raspberry Pi Pico (RP2040) or Raspberry Pi Pico W (RP2040)
* Device with an AT21CS11 SWI EEPROM interface or an emulator of it.

## Software Requirements

* Raspberry Pi Pico SDK
* ARM GCC compiler

## Installation

1.  Ensure you have the Raspberry Pi Pico SDK installed and the `PICO_SDK_PATH` environment variable set[cite: 1, 2].
2.  Clone this repository.
3.  Navigate to the project directory.
4.  Create a `build` directory: `mkdir build`
5.  Navigate to the `build` directory: `cd build`
6.  Run `cmake ..` to generate the build files.
7.  Build the project using `make`.
8.  The resulting `.uf2` file can be flashed to the Raspberry Pi Pico.

## Usage

The tool communicates via USB serial. Send JSON-formatted commands to the Pico, and it will respond with JSON-formatted responses.

### JSON Command Format

Commands are sent as JSON objects with a "command" field and any necessary data fields.

### Command Details

Here's a breakdown of the supported commands:

* **discoveryResponse**
    * Command: `{"command": "discoveryResponse"}`
    * Response: `{"status": "success", "command": "discoveryResponse", "response": "ACK"}` or `{"status": "success", "command": "discoveryResponse", "response": "NACK"}`
* **txByte**
    * Command: `{"command": "txByte", "data": "0x55"}`
    * Response: `{"status": "success", "command": "txByte", "response": "ACK"}` or `{"status": "success", "command": "txByte", "response": "NACK"}`
* **rxByte**
    * Command: `{"command": "rxByte"}`
    * Response: `{"status": "success", "command": "rxByte", "response": "0xYY"}` (where `0xYY` is the received byte)
* **manufacturerId**
    * Command: `{"command": "manufacturerId", "dev_addr": "0x00"}`
    * Response: `{"status": "success", "command": "manufacturerId", "response": "0x00XXXX"}` (e.g., `0x00D200` for AT21CS01, `0x00D380` for AT21CS11) or `{"status":"error","command":"manufacturerId","response":"Error: Manufacturer ID is zero"}`
* **readBlock**
    * Command: `{"command": "readBlock", "dev_addr": "0x00", "start_addr": "0x00", "len": "0x10"}`
    * Response: `{"status":"success","command":"readBlock","response":["0xXX", "0xXX", ...]}`

## Implementation Details

* The code is designed for the Raspberry Pi Pico (RP2040) and uses the Pico SDK[cite: 1, 2].
* It leverages the RP2040's dual-core capability: Core 0 handles USB communication and command parsing, while Core 1 manages the time-sensitive SWI communication.
* SWI EEPROM emulation is achieved through open-drain GPIO control.
* JSON parsing is done using the `jsmn` library.
* The `CMakeLists.txt` file is used to build the project[cite: 3].
* USB serial communication is enabled, and UART is disabled[cite: 2].

## Timing

The code includes timing constants optimized for SWI communication. These constants may need to be adjusted based on the specific EEPROM device or emulator being used. The code defines different timing presets (Prusa, Atmel Standard, Atmel High Speed).

## Contributing

(Add your contributing guidelines here)

## License

(Add your license information here)

## Author

jjsch-dev
