# High-Speed Visible Light Communication (VLC) Transceiver

## Project Overview

This repository contains the source code, benchmarking scripts, and hardware documentation for an undergraduate engineering project on **Visible Light Communication (Li-Fi)**.

The goal of this project was to demonstrate high-speed optical wireless networking by building a real-time, 720p video-capable transceiver using discrete analog components, completely bypassing the need for expensive FPGAs or Analog-to-Digital Converters (ADCs).

```
 ┌─────────────────┐      Optical Channel       ┌─────────────────┐
 │   Transmitter   ├───────────────────────────>│    Receiver     │
 │ (Raspberry Pi)  │          (Li-Fi)           │ (Raspberry Pi)  │
 └─────────────────┘                            └─────────────────┘
```

---

## Key Engineering Achievements

*   **Hardware Architecture:** Designed a custom Analog Front-End (AFE) utilizing an OPA380 transimpedance amplifier and an LMV7219 high-speed comparator for nanosecond signal quantization.
*   **Line Coding:** Implemented a highly optimized C++ 4B5B encoding algorithm on the Raspberry Pi baseband to ensure DC-balance, achieving 80% spectral efficiency and preventing LED flicker.
*   **Error Resilience:** Utilized a Motion-JPEG (M-JPEG) video pipeline to achieve zero temporal error propagation across the noisy optical channel.
*   **Empirical Performance:** Sustained a verified **1.17 Mbps** payload throughput at a physical baud rate of **2 Mbps**, maintaining a highly stable link up to **1.1 meters**.

---

## System Architecture

Below is the high-level representation of the optical transceiver link:

![High-Level System Architecture](hardware/High-Level%20System%20Architecture.png)

### Hardware Configuration

The system operates using a simplex optical link between two Raspberry Pi 4B computers.

| Component / Stage | Transmitter (TX) Node | Receiver (RX) Node |
| :--- | :--- | :--- |
| **Processor** | Raspberry Pi 4B (UART @ 2 Mbps) | Raspberry Pi 4B (UART @ 2 Mbps) |
| **Active Drivers / Sensors** | TC4420 High-Speed MOSFET Driver | BPW34 Silicon PIN Photodiode |
| **Power Stage / Amp** | IRLZ44N Logic-Level MOSFET | OPA380 Precision High-Speed TIA |
| **Quantization / Source** | 10W High-Power White LED | LMV7219 High-Speed Voltage Comparator |

> [!NOTE]
> Detailed circuit block diagrams, layout schematics, and physical photos are located in the [hardware/](hardware/) directory.

---

## Repository Structure

*   [`src/tx/`](src/tx/): C++ source code for the 4B5B transmission node and FFmpeg pipe handling.
    *   [`vlc_4b5b_tx.cpp`](src/tx/vlc_4b5b_tx.cpp): Transmitter baseband transceiver controller.
*   [`src/rx/`](src/rx/): C++ source code for the receiver node, CRC-16 validation, and synchronization logic.
    *   [`vlc_4b5b_rx.cpp`](src/rx/vlc_4b5b_rx.cpp): Receiver baseband decoder and frame-boundary parser.
*   [`benchmarks/`](benchmarks/): Benchmarking code for throughput validation.
    *   [`vlc_benchmark_tx.cpp`](benchmarks/vlc_benchmark_tx.cpp)
    *   [`vlc_benchmark_rx.cpp`](benchmarks/vlc_benchmark_rx.cpp)
*   [`hardware/`](hardware/): System architecture diagrams, encoding tables, and hardware implementation files.
*   [`docs/`](docs/): Contains the academic thesis papers and slides.
    *   [`VLC System Design.pdf`](docs/VLC%20System%20Design.pdf): Complete thesis documentation.
    *   [`VLC System Design Thesis.pptx`](docs/VLC%20System%20Design%20Thesis.pptx): Project defense presentation slides.

---

## Getting Started

### Prerequisites
*   A Linux environment (designed specifically for **Raspberry Pi OS**).
*   Standard compilation tools (`g++`).
*   `ffmpeg` and `ffplay` installed for streaming video validation.

### Compilation

The transceiver code is highly optimized and does not require external libraries. Simply compile the C++ source files using standard optimization flags:

```bash
# Compile the Transmitter
g++ -O3 -Wall src/tx/vlc_4b5b_tx.cpp -o vlc_tx

# Compile the Receiver
g++ -O3 -Wall src/rx/vlc_4b5b_rx.cpp -o vlc_rx
```

---

## Running the Video Pipeline

Make sure both nodes have their UART interfaces configured at `2,000,000` baud (defaulting to `/dev/ttyAMA0`).

### 1. Launch the Receiver
On the receiver Raspberry Pi, start the baseband receiver executable and pipe its raw video stream into `ffplay` for low-latency playback:

```bash
./vlc_rx | ffplay -f mpegts -an -sn -infbuf -sync ext -framedrop -x 1280 -y 720 -i -
```

### 2. Launch the Transmitter
On the transmitter Raspberry Pi, capture/generate a video input source via `ffmpeg`, compress it to low-latency H.264, encapsulate it into an MPEG-TS format, and pipe it directly to the transceiver:

```bash
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=15 \
       -vcodec libx264 -preset ultrafast -tune zerolatency \
       -b:v 250k -maxrate 250k -bufsize 30k -pix_fmt yuv420p \
       -g 15 -f mpegts - | ./vlc_tx
```

---

## Line Coding & Timing Details

For details on the ANSI X3T9.5 4B5B translation map and clock synchronizations, consult [`4B5B encoding.png`](hardware/4B5B%20encoding.png).

## Hardware Demonstration
Watch the 1-minute real-time demonstration of the VLC system streaming video via the 10W LED: 
[Click here to watch the demo on YouTube](https://youtube.com/shorts/aaMfWNTAqAo)