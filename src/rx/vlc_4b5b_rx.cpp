#include <iostream>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <cstdint>
#include <array>

constexpr const char* DEFAULT_UART_PORT = "/dev/ttyAMA0";
constexpr size_t PAYLOAD_SIZE = 64;
constexpr int SERIAL_BAUD = B2000000;
constexpr uint8_t SYNC_WORD_1 = 0xAA;
constexpr uint8_t SYNC_WORD_2 = 0xD4;
constexpr size_t ENCODED_SIZE = 85; // 17 blocks * 5 bytes

enum RX_State { WAIT_SYNC1, WAIT_SYNC2, GET_ENCODED };

std::array<uint8_t, 32> generate_reverse_4b5b() {
    std::array<uint8_t, 32> rev;
    rev.fill(0xFF); // 0xFF marks an illegal channel-noise symbol
    const uint8_t map[16] = {
        0x1E, 0x09, 0x14, 0x15, 0x0A, 0x0B, 0x12, 0x13,
        0x0E, 0x0F, 0x16, 0x17, 0x1C, 0x1D, 0x1A, 0x1B
    };
    for (uint8_t i = 0; i < 16; ++i) rev[map[i]] = i;
    return rev;
}

/* * Decodes exactly 5 transmitted bytes (40 bits) back into 4 raw bytes (32 bits).
 * Returns false if an illegal 5-bit optical noise symbol is detected.
 */
bool decode_4b5b_block(const uint8_t* in5, uint8_t* out4, const std::array<uint8_t, 32>& rev_map) {
    uint64_t accum = 0;
    accum |= (static_cast<uint64_t>(in5[0]) << 32);
    accum |= (static_cast<uint64_t>(in5[1]) << 24);
    accum |= (static_cast<uint64_t>(in5[2]) << 16);
    accum |= (static_cast<uint64_t>(in5[3]) << 8);
    accum |= (static_cast<uint64_t>(in5[4]));

    for (int i = 0; i < 4; ++i) {
        uint8_t hi_sym = (accum >> (35 - i * 10)) & 0x1F;
        uint8_t lo_sym = (accum >> (30 - i * 10)) & 0x1F;
        
        uint8_t hi_nibble = rev_map[hi_sym];
        uint8_t lo_nibble = rev_map[lo_sym];
        
        if (hi_nibble == 0xFF || lo_nibble == 0xFF) return false; // Channel Noise Detected!
        out4[i] = (hi_nibble << 4) | lo_nibble;
    }
    return true;
}

uint16_t calculate_crc16(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

int setup_uart(const char* port) {
    int fd = open(port, O_RDONLY | O_NOCTTY);
    if (fd == -1) return -1;

    struct termios options;
    if (tcgetattr(fd, &options) == -1) return -1;
    if (cfsetispeed(&options, SERIAL_BAUD) == -1 || cfsetospeed(&options, SERIAL_BAUD) == -1) return -1;

    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8 | CREAD | CLOCAL;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_oflag &= ~OPOST;

    if (tcsetattr(fd, TCSANOW, &options) == -1) return -1;
    return fd;
}

int main(int argc, char* argv[]) {
    const char* uart_port = (argc > 1) ? argv[1] : DEFAULT_UART_PORT;
    const auto rev_4b5b_map = generate_reverse_4b5b();
    int serial_fd = setup_uart(uart_port);
    if (serial_fd == -1) return 1;

    std::cerr << "[RX] 2Mbps 4B5B Receiver Ready.\n";

    RX_State state = WAIT_SYNC1;
    std::vector<uint8_t> encoded_buffer;
    encoded_buffer.reserve(ENCODED_SIZE);
    
    uint8_t read_buf[4096];

    while (true) {
        ssize_t bytes_read = read(serial_fd, read_buf, sizeof(read_buf));
        if (bytes_read <= 0) continue;

        for (ssize_t i = 0; i < bytes_read; ++i) {
            uint8_t byte = read_buf[i];

            switch (state) {
                case WAIT_SYNC1:
                    if (byte == SYNC_WORD_1) state = WAIT_SYNC2;
                    break;
                case WAIT_SYNC2:
                    if (byte == SYNC_WORD_2) {
                        encoded_buffer.clear();
                        state = GET_ENCODED;
                    } else {
                        state = (byte == SYNC_WORD_1) ? WAIT_SYNC2 : WAIT_SYNC1;
                    }
                    break;
                case GET_ENCODED:
                    encoded_buffer.push_back(byte);
                    
                    if (encoded_buffer.size() == ENCODED_SIZE) {
                        std::vector<uint8_t> raw_packet;
                        raw_packet.reserve(68);
                        bool decode_success = true;

                        // Decode blocks of 5 bytes back to 4 bytes
                        for (size_t j = 0; j < ENCODED_SIZE; j += 5) {
                            uint8_t out4[4];
                            if (!decode_4b5b_block(&encoded_buffer[j], out4, rev_4b5b_map)) {
                                decode_success = false;
                                break;
                            }
                            raw_packet.insert(raw_packet.end(), out4, out4 + 4);
                        }

                        if (decode_success) {
                            // Extract headers and CRC
                            uint8_t payload_len = raw_packet[0];
                            uint16_t received_crc = (static_cast<uint16_t>(raw_packet[66]) << 8) | raw_packet[67];
                            
                            // Strip CRC for verification
                            raw_packet.pop_back();
                            raw_packet.pop_back();

                            if (calculate_crc16(raw_packet) == received_crc) {
                                // Robust output for M-JPEG pipeline
                                write(STDOUT_FILENO, raw_packet.data() + 2, payload_len);
                            } else {
                                decode_success = false;
                            }
                        }

                        // ERROR HANDLING: Zero Padding for M-JPEG Continuity
                        if (!decode_success) {
                            std::cerr << "[RX] 4B5B/CRC Error. Zero-padding to maintain stream continuity.\n";
                            std::vector<uint8_t> dummy_pad(PAYLOAD_SIZE, 0x00);
                            write(STDOUT_FILENO, dummy_pad.data(), dummy_pad.size());
                        }

                        state = WAIT_SYNC1;
                    }
                    break;
            }
        }
    }
    return 0;
}