# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-11-05

### Added
- Reliable file transfer over plain UDP with handshake, 256-packet sliding window and selective ACK bitmap.
- Adaptive retransmission using Jacobson/Karels RTT estimation with Karn's rule and RTO backoff, plus AIMD congestion control.
- End-to-end integrity with a from-scratch SHA-256 verified by the receiver before the file is published.
- Resume of interrupted transfers from a `.part` file and a small state file.
- `ImpairedChannel` loss, duplication, delay and reordering simulator, and in-process `rudp selftest`.
- `rudp` command line tool (`send`, `recv`, `selftest`, `hash`) with a progress bar, and the `rudp_bench` throughput benchmark.
- Unit tests run through CTest, with CI on GCC, Clang, ASan/UBSan and macOS.

[Unreleased]: https://github.com/bekirhankarakurt/reliable-udp-transfer/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/bekirhankarakurt/reliable-udp-transfer/releases/tag/v1.0.0
