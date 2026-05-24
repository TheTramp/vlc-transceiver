#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <vector>

constexpr const char* UART_PORT = "/dev/ttyAMA0";
constexpr size_t PAYLOAD_SIZE = 64;
constexpr int SERIAL_BAUD = B2000000;
constexpr uint8_t SYNC_WORD_1 = 0xAA;
constexpr uint8_t SYNC_WORD_2 = 0xD4;

/* ANSI X3T9.5 4B5B Encoding Table (Prioritizes DC Balance and Clock Transitions) */
const uint8_t MAP_4B5B[16] = {
    0x1E, 0x09, 0x14, 0x15, 0x0A, 0x0B, 0x12, 0x13,
    0x0E, 0x0F, 0x16, 0x17, 0x1C, 0x1D, 0x1A, 0x1B
};

/* * Encodes exactly 4 raw bytes (32 bits) into 5 transmitted bytes (40 bits).
 * Time Complexity: O(1) per block. Space: O(1).
 */
void encode_4b5b_block(const uint8_t* in4, uint8_t* out5) {
    uint64_t accum = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t hi = (in4[i] >> 4) & 0x0F;
        uint8_t lo = in4[i] & 0x0F;
        accum = (accum << 5) | MAP_4B5B[hi];
        accum = (accum << 5) | MAP_4B5B[lo];
    }
    
    // Unpack 40-bit accumulator into 5 distinct 8-bit UART bytes
    out5[0] = static_cast<uint8_t>((accum >> 32) & 0xFF);
    out5[1] = static_cast<uint8_t>((accum >> 24) & 0xFF);
    out5[2] = static_cast<uint8_t>((accum >> 16) & 0xFF);
    out5[3] = static_cast<uint8_t>((accum >> 8) & 0xFF);
    out5[4] = static_cast<uint8_t>(accum & 0xFF);
}

uint16_t calculate_crc16(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

ssize_t read_all(int fd, uint8_t* buffer, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t result = read(fd, buffer + total, count - total);
        if (result < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (result == 0) break;
        total += static_cast<size_t>(result);
    }
    return static_cast<ssize_t>(total);
}

bool write_all(int fd, const uint8_t* buffer, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t result = write(fd, buffer + total, count - total);
        if (result < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += static_cast<size_t>(result);
    }
    return true;
}

int setup_uart() {
    int fd = open(UART_PORT, O_RDWR | O_NOCTTY);
    if (fd == -1) return -1;

    struct termios options;
    if (tcgetattr(fd, &options) != 0) return -1;

    cfsetispeed(&options, SERIAL_BAUD);
    cfsetospeed(&options, SERIAL_BAUD);

    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8 | CREAD | CLOCAL;

    options.c_iflag &= ~(IXON | IXOFF | IXANY | BRKINT | INPCK | ISTRIP | IGNCR | ICRNL);
    options.c_oflag &= ~OPOST;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG | IEXTEN);
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &options) != 0) return -1;

    return fd;
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    int serial_fd = setup_uart();
    if (serial_fd == -1) {
        std::cerr << "[TX] Failed to open UART port\n";
        return 1;
    }

    std::cerr << "[TX] 2Mbps 4B5B Transmitter Ready. (Theoretical Throughput: 1.6 Mbps)\n";
    
    uint8_t stdin_buf[PAYLOAD_SIZE] = {};
    uint8_t sequence = 0;

    while (true) {
        ssize_t bytes_read = read_all(STDIN_FILENO, stdin_buf, PAYLOAD_SIZE);
        if (bytes_read <= 0) break;

        std::vector<uint8_t> raw_packet;
        raw_packet.reserve(2 + PAYLOAD_SIZE + 2); // Len(1) + Seq(1) + Payload(64) + CRC(2) = 68 bytes
        
        raw_packet.push_back(static_cast<uint8_t>(bytes_read));
        raw_packet.push_back(sequence++);
        raw_packet.insert(raw_packet.end(), stdin_buf, stdin_buf + bytes_read);
        
        if (bytes_read < static_cast<ssize_t>(PAYLOAD_SIZE)) {
            raw_packet.insert(raw_packet.end(), PAYLOAD_SIZE - bytes_read, 0x00);
        }

        uint16_t crc = calculate_crc16(raw_packet);
        raw_packet.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
        raw_packet.push_back(static_cast<uint8_t>(crc & 0xFF));

        // 68 bytes raw / 4 = 17 blocks. 17 * 5 = 85 bytes encoded.
        std::vector<uint8_t> tx_buffer;
        tx_buffer.reserve(2 + 85); 
        
        tx_buffer.push_back(SYNC_WORD_1);
        tx_buffer.push_back(SYNC_WORD_2);

        // Apply 4B5B compression algorithm block-by-block
        for (size_t i = 0; i < raw_packet.size(); i += 4) {
            uint8_t out5[5];
            encode_4b5b_block(&raw_packet[i], out5);
            tx_buffer.insert(tx_buffer.end(), out5, out5 + 5);
        }

        if (!write_all(serial_fd, tx_buffer.data(), tx_buffer.size())) break;
    }
    
    close(serial_fd);
    return 0;
}