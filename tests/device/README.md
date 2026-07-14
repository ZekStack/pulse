# Device smoke tests

Compile and run `PulseSmoke` on:

- classic ESP32 with `PulseStackType::Internal`;
- a PSRAM-capable ESP32 with `PulseStackType::Psram`;
- `PulseStackType::Auto` with PSRAM;
- `PulseStackType::Auto` without PSRAM.

Record the board, Arduino ESP32 version, build system, configured stack type, actual stack type, and serial result. A successful run prints `PULSE_SMOKE_PASS`.

The GitHub workflow compiles the smoke sketch across the supported board matrix. Physical runtime results remain a release-checklist requirement when no hardware runner is connected.
