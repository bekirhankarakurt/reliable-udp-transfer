# reliable-udp-transfer

Reliable, integrity-checked file transfer over plain UDP in C++17: sliding window, selective ACK, adaptive retransmission, AIMD congestion control and resume - with a built-in loss/latency simulator so you can test it without root or `tc`.

[![CI](https://github.com/bekirhankarakurt/reliable-udp-transfer/actions/workflows/ci.yml/badge.svg)](https://github.com/bekirhankarakurt/reliable-udp-transfer/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg)

## Why / what

- **Transport internals, no dependencies.** Handshake, 256-packet sliding window, SACK bitmap, Jacobson/Karels RTT estimation (with Karn's rule), RTO backoff and AIMD are all implemented in the library - no Boost, no OpenSSL.
- **End-to-end integrity.** A from-scratch SHA-256 (FIPS 180-4 vectors tested) is computed by the sender, carried in the handshake and verified by the receiver before the file is published.
- **Survives interruption.** Receiver keeps a `.part` file plus a small state file; a re-run resumes from the last contiguous chunk instead of starting over.
- **Testable without a network.** `ImpairedChannel` injects loss, duplication, delay and reordering on any channel; the whole protocol also runs in-process over an in-memory link (`rudp selftest`).
- **Clean engineering.** RAII throughout, no raw `new`/`delete` in the library, zero warnings under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, CI on GCC, Clang, macOS and ASan/UBSan.

## Demo

Real output from a 20 MB transfer on loopback (`recv` in one terminal, `send` in the other):

```text
$ rudp recv 9000 --out ./incoming
listening on udp/9000
received  ./incoming/firmware.bin  19.07 MiB
          packets 16667  duplicates 0
          sha256 776145953bfae57c6f99b31b03a101178ac7ab207333517298c204f2a6b755bc (verified)

$ rudp send firmware.bin 127.0.0.1:9000
sent      firmware.bin  19.07 MiB  in 0.39 s  (48.9 MiB/s)
          packets 16667  retransmits 0  timeouts 0  fast-losses 0  srtt 1.74 ms
          sha256 776145953bfae57c6f99b31b03a101178ac7ab207333517298c204f2a6b755bc
```

Same file with 3% loss and 5 ms delay injected by the simulator:

```text
$ rudp send firmware.bin 127.0.0.1:9001 --sim-loss 0.03 --sim-delay 5
sent      firmware.bin  19.07 MiB  in 18.84 s  (1.0 MiB/s)
          packets 17235  retransmits 568  timeouts 88  fast-losses 433  srtt 5.49 ms
          sha256 776145953bfae57c6f99b31b03a101178ac7ab207333517298c204f2a6b755bc
```

Resume: the sender is interrupted (Ctrl-C) mid-transfer, then re-run:

```text
$ rudp send log.bin 127.0.0.1:9200 --sim-delay 10 --window 8      # Ctrl-C after 3 s
send failed: cancelled: cancelled
receive failed: timeout: sender stopped responding; progress saved for resume

$ rudp send log.bin 127.0.0.1:9201
sent      log.bin  7.63 MiB  in 0.12 s  (65.6 MiB/s)
          packets 4469  retransmits 0  timeouts 0  fast-losses 0  srtt 1.60 ms  resumed-chunks 2198
```

Fully in-process, 5% loss + 2% duplicates + reordering, no sockets:

```text
$ rudp selftest --size-mb 8 --sim-loss 0.05 --sim-dup 0.02 --sim-delay 5 --sim-jitter 4
selftest  8.00 MiB over in-memory link: loss 5%  dup 2%  delay 5 ms  jitter 4 ms  seed 1
sent      payload.bin  8.00 MiB  in 25.74 s  (0.3 MiB/s)
          packets 7457  retransmits 466  timeouts 79  fast-losses 439  srtt 15.10 ms
          receiver sha256 e0f3e1f02ebcc815730af879b327d1fe99a07842ee3104c860c7aae2d677f8ea  duplicates 201
result    PASS (hashes match)
```

## Quick start (60 seconds)

```bash
git clone https://github.com/bekirhankarakurt/reliable-udp-transfer.git
cd reliable-udp-transfer
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure      # optional, ~10 s
./build/rudp selftest                           # no network needed
```

Two terminals:

```bash
./build/rudp recv 9000 --out ./incoming
./build/rudp send ./some-file.bin 127.0.0.1:9000
```

## Requirements and build options

| Requirement | Version |
|---|---|
| C++ compiler | GCC >= 9 or Clang >= 10 (C++17, `<filesystem>`) |
| CMake | >= 3.16 |
| OS | Linux, macOS (POSIX sockets) |
| Runtime deps | none (only pthreads) |

| CMake option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | Standard CMake build type |
| `RUDP_BUILD_TESTS` | `ON` | Build the CTest suite |
| `RUDP_BUILD_BENCH` | `ON` | Build `rudp_bench` |
| `RUDP_SANITIZE` | `OFF` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `RUDP_WERROR` | `OFF` | Treat warnings as errors (CI turns it on) |

## Usage

```text
rudp send <file> <host:port> [options]
rudp recv <port> [options]
rudp selftest [options]
rudp hash <file>
```

| Option | Applies to | Default | Meaning |
|---|---|---|---|
| `--chunk BYTES` | send, selftest | 1200 | Payload bytes per packet (1..60000) |
| `--window PACKETS` | send, selftest | 256 | Max packets in flight (1..256) |
| `--out DIR` | recv | `.` | Output directory |
| `--wait SECONDS` | recv | forever | Give up if no sender appears |
| `--timeout SECONDS` | send, recv | 10 / 15 | Give up after this much silence |
| `--linger SECONDS` | recv | 1 | Keep answering FIN after completion |
| `--size-mb N` | selftest | 8 | Size of the generated test file |
| `--sim-loss P` | all | 0 | Drop probability (0..0.9) of datagrams received by this process |
| `--sim-dup P` | all | 0 | Duplicate probability |
| `--sim-delay MS` | all | 0 | Added one-way delay |
| `--sim-jitter MS` | all | 0 | Random extra delay (causes reordering) |
| `--seed N` | all | 1 | Simulator seed for reproducible runs |
| `--no-progress` | all | auto | Never draw the progress bar (it is drawn only on a TTY) |

Exit codes: `0` success, `1` transfer failed, `2` usage error. `Ctrl-C` cancels cleanly and saves resume state.

### Recipes

**1. Send a file to another machine**

```bash
# on the receiver (open udp/9000 in the firewall)
rudp recv 9000 --out /data/incoming
# on the sender
rudp send release.tar.gz 192.168.1.20:9000
```

**2. Resume after an interruption** - run the same two commands again. The receiver detects `release.tar.gz.part` and its state file and only the missing chunks are sent (`resumed-chunks` in the summary). Resume is skipped automatically if the file content changed.

**3. Rehearse a bad link without `tc`** - the simulator impairs datagrams *received* by the process it is given to, so use it on both ends for a symmetric link:

```bash
rudp recv 9000 --sim-loss 0.05 --sim-delay 20 --seed 7 &
rudp send big.iso 127.0.0.1:9000 --sim-loss 0.05 --sim-delay 20 --seed 8
```

**4. Cross-check integrity independently**

```bash
rudp hash incoming/release.tar.gz && sha256sum incoming/release.tar.gz
```

**5. Constrained or fragile path** - smaller packets avoid IP fragmentation; a smaller window limits bursts:

```bash
rudp send telemetry.dat 10.0.0.5:9000 --chunk 512 --window 64
```

## Architecture

```text
  rudp CLI (apps/rudp.cpp)            bench / tests
        |                                  |
        v                                  v
  +--------------------- transfer API (transfer.hpp) ----------------------+
  |  send_file()                                   receive_file()          |
  |  SendSession                                   ReceiveSession          |
  |   - SYN handshake, FIN close                    - SYN-ACK, resume state|
  |   - window: Pending/InFlight/Lost/Done          - write chunk at offset|
  |   - RttEstimator (Jacobson/Karels, Karn)        - cumulative + SACK    |
  |   - CongestionController (AIMD)                 - SHA-256 verify, rename|
  +--------------------------------+---------------------------------------+
                                   |  Channel: send(datagram) / receive(timeout)
          +------------------------+-----------------------+
          v                        v                       v
     UdpChannel             ImpairedChannel          MemoryChannel pair
   (POSIX sockets)     (loss/dup/delay/jitter)     (in-process, lossless)
```

Transfer flow: `SYN(name,size,chunk,sha256)` -> `SYN-ACK(resume offset)` -> `DATA`/`ACK` stream -> `FIN` -> `FIN-ACK(ok | hash mismatch)`.

Design trade-offs:

| Decision | Why | Cost |
|---|---|---|
| Single-threaded, poll-driven session per side | No locks, deterministic, easy to reason about and sanitize | One transfer per `recv`; run several receivers on different ports for parallelism |
| 256-bit SACK bitmap per ACK, window capped at 256 | Precise loss detection with a fixed-size ACK (48 B) | Caps throughput at 256 x chunk / RTT (~15 MiB/s at 20 ms RTT) |
| Loss inferred from SACK gaps (3 later chunks) plus RTO | Recovers without waiting for timers | Retransmissions are timer-recovered only, so loss of a retransmit costs an RTO |
| AIMD, one window cut per loss episode | Simple, fair, easy to verify | Matches Reno behaviour; no CUBIC/BBR-style probing |
| ACK per burst of up to 16 data packets | Far fewer ACKs and wake-ups | Slightly coarser RTT sampling |
| Resume = contiguous prefix only | State file is 52 bytes and always consistent | Chunks received after the first gap are re-sent |
| Hash after the last chunk, before publishing | Corrupt data never appears under the final name | Large files pay one extra read pass on each side |
| Impairment applied on receive | Needs no timers or threads, works on any `Channel` | Simulated delay is per direction and per process |

### Benchmark vs TCP

`rudp_bench 64 4` (container VM, 4 vCPU; loopback rows move 64 MiB, simulated rows 4 MiB). rudp numbers include SHA-256 on both ends; TCP has no integrity pass. The last column is the Mathis steady-state bound for TCP Reno (MSS 1460 B) - a **model**, not a measurement, because inducing loss on real TCP needs `tc`/root.

| Scenario | MiB/s | seconds | TCP model |
|---|---:|---:|---:|
| TCP, loopback | 1048.0 | 0.06 | - |
| rudp, UDP loopback | 42.7 | 1.50 | - |
| rudp, 20 ms RTT, 0% loss | 5.7 | 0.70 | - |
| rudp, 20 ms RTT, 1% loss | 0.6 | 6.51 | 0.85 |
| rudp, 20 ms RTT, 5% loss | 0.2 | 17.57 | 0.38 |

Reading it honestly: kernel TCP on loopback is a memory copy and wins by more than an order of magnitude over a user-space, per-packet protocol. Under loss, rudp lands close to the Reno model, as expected from AIMD; the simulated 20 ms RTT rows are also limited by thread wake-up latency of the in-process link. The value here is a transparent, testable implementation, not beating the kernel.

## Wire format

All integers are big-endian. Every packet starts with a 12-byte header.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | magic | `0x5255` ("RU") |
| 2 | 1 | type | 1 SYN, 2 SYN-ACK, 3 DATA, 4 ACK, 5 FIN, 6 FIN-ACK |
| 3 | 1 | flags | bit 0: failed (SYN-ACK rejected, FIN-ACK hash mismatch) |
| 4 | 4 | session | random id chosen by the sender |
| 8 | 4 | seq | DATA: chunk index; ACK: cumulative ack (next chunk expected); SYN-ACK: chunks already held |

| Packet | Payload |
|---|---|
| SYN | file size (8), chunk size (4), SHA-256 (32), name length (2), name |
| DATA | raw chunk bytes (last chunk may be shorter) |
| ACK | trigger chunk (4), SACK bitmap (32); bit *i* = chunk `cumulative + 1 + i` received |
| SYN-ACK, FIN, FIN-ACK | empty |

Receiver resume state (`<name>.part.state`, 52 bytes): magic, SHA-256, file size, chunk size, number of leading chunks on disk.

## Testing

```bash
ctest --test-dir build --output-on-failure
```

| Suite | Covers |
|---|---|
| `sha256` | NIST vectors, million-`a`, padding boundaries, file hashing |
| `packet` | Header/SYN/ACK round trips, endianness, malformed input |
| `protocol` | Jacobson/Karels maths, backoff clamps, AIMD slow start / increase / decrease |
| `progress` | Bar and byte formatting, throttled reporter |
| `transfer` | Sizes 0..300 kB, odd chunk/window, 10-25% loss, duplicates + reordering, corrupted data detected, resume + stale state, timeouts and cancellation |
| `udp` | Real loopback UDP transfer |

Sanitizer run: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DRUDP_SANITIZE=ON && cmake --build build-asan -j && ctest --test-dir build-asan`.

## Troubleshooting / FAQ

| Symptom | Likely cause / fix |
|---|---|
| `send failed: timeout: no response from receiver` | Receiver not running, wrong port, or firewall blocks UDP. Start `recv` first; open the port. |
| `bind: Address already in use` | Another process owns the port; pick another. |
| Transfer is slow across the internet | Window caps throughput at `window x chunk / RTT`; lower RTT or raise `--chunk` (watch MTU, ~1400 max without fragmentation). |
| `receive failed: timeout ... progress saved for resume` | Sender vanished. Re-run both sides; transfer continues from the saved prefix. |
| `integrity failure` | Received bytes do not hash to the sender's SHA-256; the `.part` file is discarded. Re-run. |
| Stray `*.part` / `*.part.state` files | Left by an interrupted transfer on purpose; delete them to force a fresh start. |
| Is it encrypted or authenticated? | No. SHA-256 detects corruption, not tampering. Use it on trusted networks or tunnel it. |
| IPv6? | Not yet (IPv4 only). |

## Project layout

```text
.
├── CMakeLists.txt            # library, CLI, bench, tests; warning/sanitizer options
├── apps/rudp.cpp             # CLI: send | recv | selftest | hash
├── bench/bench.cpp           # throughput vs TCP, simulated lossy links
├── include/rudp/
│   ├── channel.hpp           # Channel interface, ImpairedChannel, memory link
│   ├── congestion.hpp        # AIMD controller
│   ├── packet.hpp            # wire format encode/decode
│   ├── progress.hpp          # progress bar
│   ├── rtt.hpp               # Jacobson/Karels estimator
│   ├── sha256.hpp            # SHA-256
│   ├── transfer.hpp          # send_file / receive_file / run_loopback
│   └── udp_socket.hpp        # POSIX UDP channel
├── src/                      # implementations (sender.cpp, receiver.cpp, ...)
├── tests/                    # dependency-free tests run by CTest
└── .github/workflows/ci.yml  # GCC, Clang, ASan+UBSan, macOS
```

## Roadmap

- [x] Handshake, sliding window, selective ACK
- [x] Jacobson/Karels RTT and retransmit timers
- [x] AIMD congestion control
- [x] Resume on interruption
- [x] SHA-256 end-to-end verification
- [x] Progress bar, sender/receiver CLI
- [x] Loss/latency simulation without `tc`
- [ ] Fast retransmit of lost retransmissions (avoid RTO)
- [ ] Multi-file / directory transfer
- [ ] IPv6
- [ ] Authenticated encryption (pre-shared key)

## License

MIT - see [LICENSE](LICENSE).

Bekirhan Karakurt - github.com/bekirhankarakurt
