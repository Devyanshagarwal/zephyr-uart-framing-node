# zephyr-uart-framing-node

A Zephyr RTOS port of the [ringbuf-parser](https://github.com/Devyanshagarwal/Ring-buffer-SPI-I2C-protocol-parser-in-C) library — a byte-at-a-time framing state machine designed for the ISR-to-thread hand-off pattern common on UART, SPI, and I2C receive paths in embedded firmware.

The upstream C11 version uses lock-free ring buffers with acquire/release atomics. This port uses Zephyr's native primitives (`k_msgq`, `k_thread`, `k_work`, logging subsystem) and follows the Zephyr application layout so it drops into any `west` workspace.

## Frame format

```
[SOF=0x7E] [LEN] [PAYLOAD ...LEN bytes...] [CRC16_HI] [CRC16_LO]
```

The CRC is CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`) computed over `LEN || PAYLOAD`.

## What this demonstrates

- **Zephyr application layout** — `west.yml` manifest, `prj.conf`, `Kconfig`, `CMakeLists.txt`
- **Zephyr kernel primitives** — `K_MSGQ_DEFINE`, `k_thread_create`, `K_THREAD_STACK_DEFINE`, `irq_lock`/`irq_unlock` for stats snapshotting
- **Zephyr logging subsystem** — `LOG_MODULE_REGISTER`, per-module log level via Kconfig
- **ztest suite** — coverage of wrap-around, partial-frame delivery, garbage resync, and CRC-error paths, plus a standard CRC-16/CCITT-FALSE test vector (`0x29B1` for `"123456789"`)
- **CI via GitHub Actions** — build for `native_sim` and run the ztest suite in every push

## Layout

```
zephyr-uart-framing-node/
├── src/
│   ├── framer.h            # Public API
│   ├── framer.c            # Parser state machine + msgq hand-off
│   └── main.c              # Demo: producer + byte-shovel + consumer
├── tests/
│   └── framer/             # ztest suite
│       ├── src/test_framer.c
│       ├── prj.conf
│       ├── testcase.yaml
│       └── CMakeLists.txt
├── .github/workflows/ci.yml
├── CMakeLists.txt
├── prj.conf
├── Kconfig
├── west.yml
└── README.md
```

## Building

Requires a Zephyr toolchain and `west`. If you already have a Zephyr workspace, drop this repo into it. Otherwise, initialize a workspace pointing at this repo as the manifest:

```bash
west init -m https://github.com/Devyanshagarwal/zephyr-uart-framing-node --mr main workspace
cd workspace
west update
west zephyr-export
```

Build the demo application for `native_sim`:

```bash
cd zephyr-uart-framing-node
west build -b native_sim -p auto .
./build/zephyr/zephyr.exe
```

Expected output (abbreviated):

```
[00:00:00.000,000] <inf> main: zephyr-uart-framing-node starting
[00:00:00.100,000] <inf> main: RX frame len=4 first=0xde last=0xef
[00:00:00.150,000] <inf> main: RX frame len=5 first=0x48 last=0x4f
[00:00:00.200,000] <inf> main: stats: ok=2 crc_err=0 resyncs=1 dropped=4
[00:00:00.250,000] <inf> main: RX frame len=6 first=0x57 last=0x21
[00:00:00.250,000] <inf> main: stats: ok=3 crc_err=1 resyncs=1 dropped=4
```

## Running the tests

```bash
$ZEPHYR_BASE/scripts/twister -T tests -p native_sim --inline-logs -v
```

Or standalone with west:

```bash
west build -b native_sim -p auto tests/framer
./build/zephyr/zephyr.exe
```

## Roadmap

- Wire the framer into a real UART driver via `uart_callback_set` and the async API (currently the demo uses an in-process producer thread).
- Add a Bluetooth LE `nRF52840dk_nrf52840` target that publishes decoded frames as NUS notifications.
- Add a native_posix telemetry publisher over MQTT for cloud-backend prototyping.

## License

Apache-2.0. See [LICENSE](LICENSE).
