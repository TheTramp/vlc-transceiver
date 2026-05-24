#include <iostream>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <cstdint>
#include <array>
#include <chrono>
#include <iomanip>

constexpr const char* DEFAULT_UART_PORT = "/dev/ttyAMA0";
constexpr size_t PAYLOAD_SIZE = 64;
constexpr int SERIAL_BAUD = B2000000;
constexpr uint8_t SYNC_WORD_1 = 0xAA;
constexpr uint8_t SYNC_WORD_2 = 0xD4;
constexpr size_t ENCODED_SIZE = 85; 

enum RX_State { WAIT_SYNC1, WAIT_SYNC2, GET_ENCODED };

std::array<uint8_t, 32> generate_reverse_4b5b() {
    std::array<uint8_t, 32> rev;
    rev.fill(0xFF); 
    const uint8_t map[16] = {
        0x1E, 0x09, 0x14, 0x15, 0x0A, 0x0B, 0x12, 0x13,
        0x0E, 0x0F, 0x16, 0x17, 0x1C, 0x1D, 0x1A, 0x1B
    };
    for (uint8_t i = 0; i < 16; ++i) rev[map[i]] = i;
    return rev;
}

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
        
        if (hi_nibble == 0xFF || lo_nibble == 0xFF) return false; 
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

/* Kernighan's Algorithm to count bit errors in O(N) time */
int count_bit_errors(const uint8_t* received, const uint8_t* expected, size_t len) {
    int errors = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t diff = received[i] ^ expected[i];
        while (diff) {
            diff &= (diff - 1); 
            errors++;
        }
    }
    return errors;
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
    
    // Set a 0.1 second timeout so read() doesn't block forever if the laser is disconnected
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 1; 

    if (tcsetattr(fd, TCSANOW, &options) == -1) return -1;
    return fd;
}

int main(int argc, char* argv[]) {
    const char* uart_port = (argc > 1) ? argv[1] : DEFAULT_UART_PORT;
    const auto rev_4b5b_map = generate_reverse_4b5b();
    int serial_fd = setup_uart(uart_port);
    if (serial_fd == -1) return 1;

    std::cerr << "[RX] Waiting for laser connection...\n";

    RX_State state = WAIT_SYNC1;
    std::vector<uint8_t> encoded_buffer;
    encoded_buffer.reserve(ENCODED_SIZE);
    uint8_t read_buf[4096];

    // The Golden Payload for BER comparison
    uint8_t golden_payload[PAYLOAD_SIZE];
    for(size_t i = 0; i < PAYLOAD_SIZE; ++i) golden_payload[i] = 0x55;

    // Metrics Tracking
    uint64_t total_payload_bits = 0;
    uint64_t bit_errors = 0;
    uint32_t good_packets = 0;
    uint32_t corrupted_packets = 0;
    uint32_t lost_packets = 0;
    int32_t last_seq = -1;
    
    bool test_started = false;
    auto start_time = std::chrono::steady_clock::now();
    const double TEST_DURATION_SECONDS = 180.0; // 3 minutes

    while (true) {
        if (test_started) {
            auto current_time = std::chrono::steady_clock::now();
            std::chrono::duration<double> elapsed = current_time - start_time;
            if (elapsed.count() >= TEST_DURATION_SECONDS) {
                break; // 3 minutes are up!
            }
        }

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
                        if (!test_started) {
                            std::cerr << "[RX] Connection Acquired! Starting 3-minute test...\n";
                            start_time = std::chrono::steady_clock::now();
                            test_started = true;
                        }

                        std::vector<uint8_t> raw_packet;
                        raw_packet.reserve(68);
                        bool decode_success = true;

                        for (size_t j = 0; j < ENCODED_SIZE; j += 5) {
                            uint8_t out4[4];
                            if (!decode_4b5b_block(&encoded_buffer[j], out4, rev_4b5b_map)) {
                                decode_success = false;
                                break;
                            }
                            raw_packet.insert(raw_packet.end(), out4, out4 + 4);
                        }

                        if (decode_success) {
                            uint8_t seq = raw_packet[1];
                            uint16_t received_crc = (static_cast<uint16_t>(raw_packet[66]) << 8) | raw_packet[67];
                            
                            // Check for lost packets using sequence number jump
                            if (last_seq != -1) {
                                int diff = (seq - last_seq) & 0xFF; // Accounts for 0-255 rollover
                                if (diff > 1) {
                                    lost_packets += (diff - 1);
                                }
                            }
                            last_seq = seq;

                            raw_packet.pop_back();
                            raw_packet.pop_back();

                            if (calculate_crc16(raw_packet) == received_crc) {
                                good_packets++;
                            } else {
                                corrupted_packets++;
                            }

                            // Calculate BER on the payload (Bytes 2 through 65)
                            int errors = count_bit_errors(raw_packet.data() + 2, golden_payload, PAYLOAD_SIZE);
                            bit_errors += errors;
                            total_payload_bits += (PAYLOAD_SIZE * 8);

                        } else {
                            // If 4B5B fails, the packet is corrupted by physical channel noise
                            corrupted_packets++;
                            // Sequence number is unreadable, assume it increments by 1
                            if (last_seq != -1) last_seq = (last_seq + 1) & 0xFF;
                        }

                        state = WAIT_SYNC1;
                    }
                    break;
            }
        }
    }

    // --- MATHEMATICAL ANALYSIS & CSV GENERATION ---
    uint32_t total_expected_packets = good_packets + corrupted_packets + lost_packets;
    double per_percentage = (total_expected_packets == 0) ? 100.0 : 
        (static_cast<double>(corrupted_packets + lost_packets) / total_expected_packets) * 100.0;
    
    double ber = (total_payload_bits == 0) ? 1.0 : 
        static_cast<double>(bit_errors) / static_cast<double>(total_payload_bits);
    
    // Throughput formula: (Good Packets * Payload Bits) / Total Time
    double throughput_bps = (static_cast<double>(good_packets) * PAYLOAD_SIZE * 8.0) / TEST_DURATION_SECONDS;
    double throughput_kbps = throughput_bps / 1000.0;

    std::cerr << "\n[TEST COMPLETE] Raw Data Collection Generated.\n";
    std::cerr << "Distance (cm),Ambient Light (Lux),Total Bits,Bit Errors,BER,Total Packets,Lost Packets,PER (%),Throughput (kbps)\n";
    
    std::cout << std::fixed << std::setprecision(8);
    std::cout << "ENTER_DIST,ENTER_LUX," 
              << total_payload_bits << "," 
              << bit_errors << "," 
              << ber << "," 
              << total_expected_packets << "," 
              << (corrupted_packets + lost_packets) << "," 
              << std::setprecision(4) << per_percentage << "," 
              << throughput_kbps << "\n";

    return 0;
}