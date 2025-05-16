 /**
 * @file swi_tool.c
 * @brief Firmware tool for injecting commands to test the AT21CS11 EEPROM emulator.
 *
 * This project implements a testing tool for firmware that emulates the AT21CS11 EEPROM.
 * It accepts JSON-formatted commands over USB serial, parses them using jsmn, and leverages
 * the RP2040's dual-core capabilities: Core0 handles USB/JSON command processing, while
 * Core1 performs timing-critical bit-banging to emulate the EEPROM using open-drain GPIO.
 *
 * Supported JSON Commands:
 * - discoveryResponse
 *     - Command: {"command": "discoveryResponse"}
 *     - Expected Response: {"status": "success", "command": "discoveryResponse", "response": "ACK"}
 *
 * - txByte
 *     - Command: {"command": "txByte", "data": "0x55"}
 *       (Replace "0x55" with the byte value to transmit in hexadecimal format.)
 *     - Expected Response: {"status": "success", "command": "txByte", "response": "ACK"}
 *       (The response may also indicate "NACK" if transmission fails.)
 *
 * - rxByte
 *     - Command: {"command": "rxByte"}
 *     - Expected Response: {"status": "success", "command": "rxByte", "response": "0xYY"}
 *       (Where "0xYY" is the received byte in hexadecimal format.)
 *
 * - manufacturerId
 *     - Command: {"command": "manufacturerId", "dev_addr": "0x00"}
 *       (The "dev_addr" field specifies the device address.)
 *     - Expected Response: {"status": "success", "command": "manufacturerId", "response": "0x00XXXX"}
 *       (For example, 0x00D200 for AT21CS01 or 0x00D380 for AT21CS11; a manufacturer ID of 0 is considered an error.)
 *
 * - readBlock
 *     - Command: {"command": "readBlock", "dev_addr": "0x00", "start_addr": "0x00", "len": "0x10"}
 *       (The "dev_addr", "start_addr", and "len" fields specify the device address, the starting EEPROM address,
 *       and the number of bytes to read, respectively. All values are given as hexadecimal strings.)
 *     - Expected Response: {"status":"success","command":"readBlock","response":["0xXX", "0xXX", ...]}
 *       (A JSON array of hexadecimal strings representing the block data.)
 *
 * Implementation Details:
 * - EEPROM emulation is implemented using open-drain GPIO by dynamically switching the pin
 *   between input mode (to let the pull-up resistor drive it high) and output mode (to drive it low).
 * - Timing is achieved using a blocking delay function (soft_delay_us) that employs cycle counting,
 *   assuming a 125 MHz clock (approximately 8 ns per cycle). Global timing variables (time_bit, time_rd, etc.)
 *   are used for precise bit-banging and can be updated via the "setSpeed" command.
 * - Inter-core communication uses the FIFO interface: Core0 issues commands (using send_cmd())
 *   and Core1 processes them in a blocking fashion.
 *
 * Author: jjsch-dev
 * Date: 2025-04-10
 */


#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "jsmn.h"  // Ensure jsmn.h is in your include path

#define BUFFER_SIZE     256
#define SINGLE_WIRE_PIN 2   ///< GPIO pin used for EEPROM emulation (open-drain)
#define LED_PIN         25  ///< Onboard Pico LED (live indicator)

// Define command codes.
#define TX_BYTE     0x01
#define DISCOVERY   0x02
#define RX_BYTE     0x03  

// Define ack/nack sequence
#define SEND_ACK	0
#define SEND_NACK	1

// Timing constants for different speed settings.
// Prusa timings are used as a baseline.
#define T_PRUSA_LOW1_US   2
#define T_PRUSA_LOW0_US   10
#define T_PRUSA_RD_US     1
#define T_PRUSA_MRS_US    1
#define T_PRUSA_BIT_US    25

// Atmel timing constants for standard speed.
#define T_ATMEL_ST_LOW1_US    4
#define T_ATMEL_ST_LOW0_US    24
#define T_ATMEL_ST_RD_US      4
#define T_ATMEL_ST_MRS_US     2
#define T_ATMEL_ST_BIT_US     45
        
// Atmel timing constants for high speed.
#define T_ATMEL_HI_LOW1_US    1
#define T_ATMEL_HI_LOW0_US    10
#define T_ATMEL_HI_RD_US      1
#define T_ATMEL_HI_MRS_US     1
#define T_ATMEL_HI_BIT_US     15

// Global timing variables.
// These are used by the bit-banging functions to generate precise delays.
// Note: Since operations are blocking, these values remain constant during a single transaction.
#define time_bit        T_PRUSA_BIT_US
#define time_rd         T_PRUSA_RD_US
#define time_mrs        T_PRUSA_MRS_US
#define time_low1       T_PRUSA_LOW1_US
#define time_low0       T_PRUSA_LOW0_US
#define tx_one_btime    (T_PRUSA_BIT_US - T_PRUSA_LOW1_US)
#define tx_zero_btime   (T_PRUSA_BIT_US - T_PRUSA_LOW0_US)
#define rd_btime        (T_PRUSA_BIT_US - T_PRUSA_RD_US - T_PRUSA_MRS_US)

/**
 * @brief Sets the single-wire pin to a high state.
 *
 * In open-drain emulation, setting the pin as input releases the line so that
 * the pull-up resistor can pull it high.
 */
static inline void sio_set_high(void) {
    gpio_set_dir(SINGLE_WIRE_PIN, GPIO_IN);
}

/**
 * @brief Sets the single-wire pin to a low state.
 *
 * Configures the pin as an output, forcing the line low.
 * It is assumed that the output register is preset to 0.
 */
static inline void sio_set_low(void) {
    gpio_set_dir(SINGLE_WIRE_PIN, GPIO_OUT);
}

/**
 * @brief Reads the current logic level of the single-wire pin.
 *
 * Configures the pin as an input and returns its state.
 *
 * @return The logic level (0 = low, 1 = high).
 */
static inline uint8_t sio_get_value(void) {
    gpio_set_dir(SINGLE_WIRE_PIN, GPIO_IN);
    return gpio_get(SINGLE_WIRE_PIN);
}

/* Low-level protocol function declarations */
uint8_t discovery_response(void);
uint8_t tx_byte(uint8_t data_byte);
uint8_t rx_byte(uint8_t ack);
void tx_one(void);
void tx_zero(void);
uint8_t read_bit(void);
uint8_t ack_nack(void);
uint8_t read_byte(uint8_t ack);

/**
 * @brief Busy-wait delay in microseconds using cycle counting.
 *
 * Converts the desired delay in microseconds to the equivalent number of CPU cycles,
 * taking into account the clock speed.
 *
 * For the original Pico (125 MHz), each cycle is ~8 ns.
 * For the Pico 2 (150 MHz), each cycle is ~6.67 ns.
 *
 * Adjust the calibration constant (-7) as needed for your application.
 *
 * @param __us Delay duration in microseconds.
 */
#ifdef PICO2
void soft_delay_us(double __us) {
    // Pico 2: 6.67 ns per cycle (150 MHz)
    uint32_t __count = (uint32_t)(__us / 0.00667) - 7;  
    busy_wait_at_least_cycles(__count);
}
#else
void soft_delay_us(double __us) {
    // Original Pico: 8 ns per cycle (125 MHz)
    uint32_t __count = (uint32_t)(__us / 0.008) - 7;
    busy_wait_at_least_cycles(__count);
}
#endif

/**
 * @brief Performs the EEPROM discovery response sequence.
 *
 * Executes a series of pin toggles to simulate the discovery response.
 *
 * @return 0x00 if ACK is observed, or 0xFF if NACK is detected.
 */
uint8_t discovery_response(void) {
    uint8_t temp;
    
    sio_set_high();
    soft_delay_us(200);  // tHTSS (Standard Speed)
    sio_set_low();
    soft_delay_us(150); //(500);  // tRESET (Standard Speed)
    sio_set_high();
    soft_delay_us(100); //(20);   // tRRT

    sio_set_low();
    soft_delay_us(1); // tDRR
    sio_set_high();
    soft_delay_us(3); //(2);    // tMSDR
    temp = (sio_get_value() == 0) ? 0x00 : 0xFF;
    soft_delay_us(150); //(21);   // tDACK delay
    return temp;
}

/**
 * @brief Transmits a logic '1' bit.
 *
 * Uses the global timing variables to determine the low pulse width and overall bit duration.
 */
void tx_one(void) {
    sio_set_low();
    soft_delay_us(time_low1);
    sio_set_high();
    soft_delay_us(tx_one_btime); 
}

/**
 * @brief Transmits a logic '0' bit.
 *
 * Uses the global timing variables to determine the low pulse width and overall bit duration.
 */
void tx_zero(void) {
    sio_set_low(); 
    soft_delay_us(time_low0);
    sio_set_high();
    soft_delay_us(tx_zero_btime); 
}

/**
 * @brief Reads a single bit from the bus.
 *
 * Performs the required toggling and timing to read one bit from the EEPROM interface.
 *
 * @return The read bit (0 or 1).
 */
uint8_t read_bit(void) {
    sio_set_low();
    soft_delay_us(time_rd);         // Read delay period.
    sio_set_high();
    soft_delay_us(time_mrs);        // Minimum recovery time.
    uint8_t temp = sio_get_value() & 0x01;
    soft_delay_us(rd_btime);
    sio_set_high();
    return temp;
}

/**
 * @brief Receives the ACK/NACK bit after transmission.
 *
 * Reads one bit to determine if an ACK (0x00) or NACK (0xFF) was received.
 *
 * @return 0x00 on ACK, 0xFF on NACK.
 */
uint8_t ack_nack(void) {
    return read_bit() ? 0xFF : 0x00;
}

void stop_con() {
    soft_delay_us(500);
}

/**
 * @brief Transmits a byte bit-by-bit and then retrieves the ACK/NACK response.
 *
 * Iterates over each bit in the byte, transmitting using tx_one() or tx_zero()
 * based on the bit's value. After sending the byte, it reads the ACK/NACK.
 *
 * @param data_byte The byte to transmit.
 * @return The ACK/NACK code: 0x00 indicates an ACK; 0xFF indicates a NACK.
 */
uint8_t tx_byte(uint8_t data_byte) {
    for (uint8_t ii = 0; ii < 8; ii++) {
        if (data_byte & 0x80) {
            tx_one();
        } else {
            tx_zero();
        }
        data_byte <<= 1;
    }
    return ack_nack();
}

/**
 * @brief Receives a byte bit-by-bit.
 *
 * Reads 8 bits from the bus using read_bit() and combines them to form a byte.
 *
 * @return The byte received from the bus.
 */
uint8_t read_byte(uint8_t ack) {
    uint8_t data_byte = 0;
    uint8_t temp;
    
    for (int8_t ii = 0; ii < 8; ii++) {
        temp = read_bit();
        data_byte = (data_byte << 1) | temp;
    }
    
    if(ack) {
        tx_one();
    } else {
        tx_zero();
    }
    return data_byte;
}

/**
 * @brief Alias for read_byte to receive a byte.
 *
 * @return The byte received from the bus.
 */
uint8_t rx_byte(uint8_t ack) {
    return read_byte(ack);
}

/**
 * @brief Initializes the single-wire pin for open-drain operation.
 *
 * Configures the pin as an input with an internal pull-up and sets the drive strength.
 * The output register is set to 0 so that switching to output immediately drives the line low.
 */
void init_open_drain_swi_pin(void) {
    gpio_init(SINGLE_WIRE_PIN);
    gpio_set_drive_strength(SINGLE_WIRE_PIN, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_dir(SINGLE_WIRE_PIN, GPIO_IN);
    gpio_pull_up(SINGLE_WIRE_PIN);
    gpio_put(SINGLE_WIRE_PIN, 0);
}

/**
 * @brief Entry function for Core1.
 *
 * Core1 runs this function to process timing-critical commands received via FIFO.
 * It waits for a command from Core0, disables interrupts for precision,
 * executes the corresponding operation (transmission, reception, or discovery),
 * and sends back an acknowledgment.
 */
void core1_entry(void) {
    init_open_drain_swi_pin();
 
    while (true) {
        // Retrieve a command from Core0 via the FIFO.
        uint32_t item = multicore_fifo_pop_blocking();
        uint8_t ack;
        uint8_t cmd = (item >> 24) & 0xFF;
        uint8_t data = item & 0xFF;
         
        // Disable interrupts to perform a precise, timing-critical operation.
        uint32_t irq_status = save_and_disable_interrupts();
        switch (cmd) {
            case TX_BYTE:
                ack = tx_byte(data);
                break;
            case DISCOVERY:
                ack = discovery_response();
                break;
            case RX_BYTE:
                ack = rx_byte(data);
                break;
            default:
                ack = 0xFF;  // Unknown command error.
                break;
        }
        restore_interrupts(irq_status);
        // Send the ACK or response back to Core0.
        multicore_fifo_push_blocking(ack);
    }
}

#define OPCODE_EEPROM_ACCESS        0xA0    /* Read/Write the contents of the main memory array. */
#define OPCODE_SEC_REG_ACCESS       0xB0    /* Read/Write the contents of the Security register. */
#define OPCODE_LOCK_SEC_REG         0X20    /* Permanently lock the contents of the Security register. */
#define OPCODE_ROM_ZONE_REG_ACCESS  0x70    /* Inhibit further modification to a zone of the EEPROM array. */
#define OPCODE_FREEZE_ROM           0x10    /* Permanently lock the current state of the ROM Zone registers. */
#define OPCODE_MANUFACTURER_ID      0xC0    /* Query manufacturer and density of device. */
#define OPCODE_STANDARD_SPEED       0xD0    /* Switch to Standard Speed mode operation (AT21CS01 only
                                               command, the AT21CS11 will NACK this command). */
#define OPCODE_HIGH_SPEED           0xE0    /* Switch to High-Speed mode operation (AT21CS01 power‑on default.
                                               AT21CS11 will ACK this command). */
#define	RW_BIT                      0x01    /* The last bit of the opcode set Read (1) or Write (0) operation. */

/**
 * @brief Sends a command (with associated data) to Core1 and waits for a response.
 *
 * Encodes the command and data into a 32-bit value: the upper 8 bits represent the command,
 * and the lower 8 bits represent the data. The function then waits for the response from Core1.
 *
 * @param cmd  The command code (8-bit).
 * @param data The accompanying data (8-bit).
 * @return The acknowledgment (8-bit) received from Core1.
 */
uint8_t send_cmd(uint8_t cmd, uint8_t data) {  
    multicore_fifo_push_blocking((cmd << 24) | data);
    return (uint8_t)multicore_fifo_pop_blocking();
}

/**
 * Return manufacturer device ID
 * 0x00D200 for AT21CS01
 * 0x00D380 for AT21CS11
 *
 * @return Manufacturer ID
 */
uint32_t read_mfr_id(uint8_t dev_addr) {
    uint32_t id = 0;
    uint8_t ack = send_cmd(DISCOVERY, 0);
    if( !ack ) {
        ack = send_cmd(TX_BYTE, OPCODE_MANUFACTURER_ID | dev_addr | RW_BIT);
		
        if(!ack) {
            id = id | (send_cmd(RX_BYTE, SEND_ACK) << 16);
            id = id | (send_cmd(RX_BYTE, SEND_ACK) << 8);
            id = id | (send_cmd(RX_BYTE, SEND_NACK) << 0);
        }
    }	    
    return id;
}

int load_address(uint8_t dev_addr, uint8_t data_addr) {
    // Check address is in range
    if (data_addr > 128) { 
        return -1;
    }
    
    // Address device, return if device didn't ack
    if (send_cmd(TX_BYTE, OPCODE_EEPROM_ACCESS | dev_addr | 0)) {
        return -2;
    }
    
    // Select write address in device. Return if device didn't ack.
    if (send_cmd(TX_BYTE, data_addr)) { 
        return -3;
    }
    return 1;
}

/**
 * Read byte from EEPROM
 * This function return int to allow for error states to be returned.
 * Should be cast to uint8_t for further procesing.
 *
 * @param data_addr Address to read(0-127)
 * @return negative number if error occurred, data on address otherwise
 */
int read_eeprom(uint8_t dev_addr, uint8_t data_addr) {
    int data;
    
    // Load data address into Address Pointer.
    int res = load_address(dev_addr, data_addr); 
    if (res < 0) {
        return res - 5;
    }
    
    stop_con(); //-- wait 500uS
    
    // Address device, return if device didn't ack    
    if (send_cmd(TX_BYTE, OPCODE_EEPROM_ACCESS | dev_addr | RW_BIT) ) { 
        return -5;
    }
    
    data = (int)send_cmd(RX_BYTE, SEND_NACK); // Ready byte from bus

    stop_con(); // Give EEPROM some extra time. Reduces errors.
    return data;
}

int verified_read(uint8_t dev_addr, uint8_t data_addr) {
    int data[3];

    data[0] = read_eeprom(dev_addr, data_addr);
    data[1] = read_eeprom(dev_addr, data_addr);

    if (data[0] == data[1]) {
        return data[0];
    }

    // data mismatch, we need 3rd data to find out which one is correct
    data[2] = read_eeprom(dev_addr, data_addr);

    if (data[1] == data[2]) {
        return data[1];
    } else if (data[2] == data[0]) {
        return data[2];
    }    
    return -1;
}

/**
 * @brief Reads multiple bytes of data from the EEPROM.
 *
 * This function reads a block of data starting at data_addr from the EEPROM.
 * It first checks that the requested block does not exceed the memory range
 * (assuming valid addresses 0 to 127). Then it sends a DISCOVERY command to verify
 * the presence of the EEPROM and proceeds to read each byte using verified_read(),
 * which reads the same EEPROM address multiple times for verification.
 *
 * @param dev_addr The device address for EEPROM access.
 * @param data_addr The starting address in the EEPROM (0-127).
 * @param buffer A pointer to a buffer to store the read data.
 * @param len The number of bytes to read.
 * @return 1 on success, or a negative error code if an error occurs.
 */
int read_block(uint8_t dev_addr, uint8_t data_addr, uint8_t *buffer, uint8_t len) {
    // Validate that the block is within EEPROM address range (0 to 127)
    if (data_addr + len > 128) {
        return -1; // Block exceeds available memory.
    }
    
    // Issue a DISCOVERY command to confirm the device is present.
    if (send_cmd(DISCOVERY, 0)) {
        return -2; // Device did not acknowledge.
    }
    
    // Read each byte in the block using verified_read() which does multiple readings.
    for (int i = 0; i < len; i++) {
        int res = verified_read(dev_addr, data_addr + i);
        if (res < 0) {
            return -3; // Error occurred during EEPROM read.
        }
        buffer[i] = (uint8_t) res;
    }
    return 1; // Success.
}


/**
 * @brief Compares a JSON token with a given string.
 *
 * This helper function checks if the provided token (obtained using jsmn) is a string
 * that exactly matches the given null-terminated string.
 *
 * @param json The entire JSON string.
 * @param tok  The token to compare.
 * @param s    The null-terminated string to compare against.
 * @return 0 if the token matches the string; otherwise, -1.
 */
int jsoneq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING &&
        (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

/**
 * @brief Parses a JSON string using jsmn and dispatches commands.
 *
 * Extracts the "command", "data", "dev_addr", "start_addr", and "len" fields from the JSON string.
 * For the "readBlock" command, it uses dev_addr, start_addr, and len fields to specify the EEPROM block
 * to read. Other commands are dispatched as before. Responses are printed in JSON format.
 *
 * @param json_str The JSON command string.
 */
void handle_command(char *json_str) {
    jsmn_parser parser;
    jsmntok_t tokens[30];  // Increased token count to accommodate additional fields.
    jsmn_init(&parser);
    int token_count = jsmn_parse(&parser, json_str, strlen(json_str), tokens, 30);
    if (token_count < 0) {
        printf("{\"status\":\"error\",\"command\":\"parse\",\"response\":\"Failed to parse JSON\"}\n");
        return;
    }
    if (token_count < 1 || tokens[0].type != JSMN_OBJECT) {
        printf("{\"status\":\"error\",\"command\":\"parse\",\"response\":\"JSON object expected\"}\n");
        return;
    }
    
    char command[64] = {0};
    char data[64] = {0};
    char dev_addr_str[32] = {0};
    char start_addr_str[32] = {0};
    char len_str[32] = {0};
    
    // Iterate over tokens to extract expected fields.
    for (int i = 1; i < token_count; i++) {
        if (jsoneq(json_str, &tokens[i], "command") == 0) {
            int length = tokens[i + 1].end - tokens[i + 1].start;
            if (length < (int)sizeof(command)) {
                strncpy(command, json_str + tokens[i + 1].start, length);
                command[length] = '\0';
            }
            i++; // Skip value token.
        }
        else if (jsoneq(json_str, &tokens[i], "data") == 0) {
            int length = tokens[i + 1].end - tokens[i + 1].start;
            if (length < (int)sizeof(data)) {
                strncpy(data, json_str + tokens[i + 1].start, length);
                data[length] = '\0';
            }
            i++; // Skip value token.
        }
        else if (jsoneq(json_str, &tokens[i], "dev_addr") == 0) {
            int length = tokens[i + 1].end - tokens[i + 1].start;
            if (length < (int)sizeof(dev_addr_str)) {
                strncpy(dev_addr_str, json_str + tokens[i + 1].start, length);
                dev_addr_str[length] = '\0';
            }
            i++; // Skip value token.
        }
        else if (jsoneq(json_str, &tokens[i], "start_addr") == 0) {
            int length = tokens[i + 1].end - tokens[i + 1].start;
            if (length < (int)sizeof(start_addr_str)) {
                strncpy(start_addr_str, json_str + tokens[i + 1].start, length);
                start_addr_str[length] = '\0';
            }
            i++; // Skip value token.
        }
        else if (jsoneq(json_str, &tokens[i], "len") == 0) {
            int length = tokens[i + 1].end - tokens[i + 1].start;
            if (length < (int)sizeof(len_str)) {
                strncpy(len_str, json_str + tokens[i + 1].start, length);
                len_str[length] = '\0';
            }
            i++; // Skip value token.
        }
    }
    
    // Dispatch commands based on the parsed "command" field.
    if (strcmp(command, "discoveryResponse") == 0) {
        uint8_t ack = send_cmd(DISCOVERY, 0);
        const char *status_str = (ack == 0x00) ? "ACK" : "NACK";
        printf("{\"status\":\"success\",\"command\":\"discoveryResponse\",\"response\":\"%s\"}\n", status_str);
    }
    else if (strcmp(command, "txByte") == 0) {
        uint8_t data_val = 0;
        unsigned int temp_val = 0;
        if (strlen(data) > 0) {
            if (sscanf(data, "0x%x", &temp_val) == 1) {
                data_val = (uint8_t)temp_val;
            }
        }
        uint8_t ack = send_cmd(TX_BYTE, data_val);  
        const char *ack_str = (ack == 0x00) ? "ACK" : "NACK";
        printf("{\"status\":\"success\",\"command\":\"txByte\",\"response\":\"%s\"}\n", ack_str);
    }
    else if (strcmp(command, "rxByte") == 0) {
        uint8_t received = send_cmd(RX_BYTE, 0);  
        printf("{\"status\":\"success\",\"command\":\"rxByte\",\"response\":\"0x%02X\"}\n", received);
    }
    else if (strcmp(command, "manufacturerId") == 0) {
        uint8_t dev_addr = 0;
        unsigned int temp_val = 0;
        if (strlen(dev_addr_str) > 0) {
            if (sscanf(dev_addr_str, "0x%x", &temp_val) == 1) {
                dev_addr = (uint8_t)temp_val;
            }
        }
        uint32_t received = read_mfr_id(dev_addr);  
        /* If the manufacturer ID equals zero, that is considered an error. */
        if (received == 0) {
            printf("{\"status\":\"error\",\"command\":\"manufacturerId\",\"response\":\"Error: Manufacturer ID is zero\"}\n");
        } else {
            printf("{\"status\":\"success\",\"command\":\"manufacturerId\",\"response\":\"0x%08X\"}\n", received);
        }
    }
    else if (strcmp(command, "readBlock") == 0) {
        // Use parsed values from additional fields; if not provided, use defaults.
        unsigned int dev_addr = 0;
        unsigned int start_addr = 0;
        unsigned int block_len = 10;  // default length

        if (strlen(dev_addr_str) > 0) {
            sscanf(dev_addr_str, "0x%x", &dev_addr);
        }
        if (strlen(start_addr_str) > 0) {
            sscanf(start_addr_str, "0x%x", &start_addr);
        }
        if (strlen(len_str) > 0) {
            sscanf(len_str, "0x%x", &block_len);
        }
        
        // Allocate buffer based on block length.
        uint8_t *read_buffer = malloc(block_len);
        if (!read_buffer) {
            printf("{\"status\":\"error\",\"command\":\"readBlock\",\"response\":\"Memory allocation error\"}\n");
            return;
        }
        
        int result = read_block((uint8_t)dev_addr, (uint8_t)start_addr, read_buffer, (uint8_t)block_len);
        if (result < 0) {
            printf("{\"status\":\"error\",\"command\":\"readBlock\",\"response\":\"Error %d\"}\n", result);
        } else {
            // Build a JSON array with the values, inserting a newline after every 8 entries.
		    printf("{\"status\":\"success\",\"command\":\"readBlock\",\"response\":[\n");
		    for (unsigned int i = 0; i < block_len; i++) {
		        printf("\"0x%02X\"", read_buffer[i]);
		        if (i < block_len - 1) {
		            // Insert a comma after each value.
		            if ((i + 1) % 8 == 0) {
		                // After every 8 values, print a newline.
		                printf(",\n");
		            } else {
		                printf(", ");
		            }
		        }
		    }
		    printf("\n]}\n");
        }
        free(read_buffer);
    }
    else {
        printf("{\"status\":\"error\",\"command\":\"unknown\",\"response\":\"Invalid Command\"}\n");
    }
}

/**
 * @brief Main entry point.
 *
 * Initializes STDIO and the onboard LED. Waits for USB connection before displaying
 * the splash message, then launches Core1 for timing-critical operations. The main loop
 * reads JSON commands from USB serial, dispatches them, and toggles the LED as an activity indicator.
 *
 * @return int 0 on exit.
 */
int main(void) {
    stdio_init_all();
    char buffer[BUFFER_SIZE];
    int index = 0;

    // Initialize the onboard LED.
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    
    // Wait for USB to be connected before printing the splash message.
    while (!stdio_usb_connected()) {
        sleep_ms(100);
        gpio_xor_mask(1 << LED_PIN);
    }

    // Splash message indicating the tool is ready.
    printf("\n"
           "******************************************\n"
           "*   AT21CS11 Pico JSON Command Tool      *\n"
           "*                                        *\n"
           "*  Firmware Interface Test Utility Ready *\n"
           "*                                        *\n"
           "*  Inject commands via USB serial to     *\n"
           "*  emulate and test AT21CS11 EEPROMs.    *\n"
           "******************************************\n\n");

    // Launch Core1 for timing-critical bit-banging.
    multicore_launch_core1(core1_entry);
    
    // Main loop: read JSON commands from USB serial and process them,
    // while toggling the LED to indicate activity.
    while (true) {
        int ch = getchar_timeout_us(250000);
        if (ch != PICO_ERROR_TIMEOUT) {
            putchar(ch);  // Optionally echo received characters.
            if (ch == '\n' || ch == '\r') {
                buffer[index] = '\0';
                if (index > 0) {
                    handle_command(buffer);
                    index = 0;
                }
            } else {
                if (index < BUFFER_SIZE - 1) {
                    buffer[index++] = (char)ch;
                }
            }
        }
        // Toggle the LED as a live indicator.
        gpio_xor_mask(1 << LED_PIN);
    }
    return 0;
}

