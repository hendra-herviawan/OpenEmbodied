# MZ01-C3-LCD firmware binaries — 2026-09-13

Prebuilt images for the **Zuowei MZ01-C3-LCD** (EZTECH X1, factory board id
`zuowei-c3-realtime-lcd-id`, ESP32-C3, 8 MB flash).

| File | Build |
|------|-------|
| `xiaozhi.bin` + `bootloader.bin` / `partition-table.bin` / `ota_data_initial.bin` / `flash_args` | Working build: standard-xiaozhi websocket protocol for a self-hosted xiaozhi-server-go, Bahasa Indonesia locale and voice prompts, official LVGL UI, graceful chat exit, on-screen volume indicator. Daily-driver build, hardware-verified. |
| `mz01-prboard-xiaozhi.bin` | Upstream-candidate build of the `pr-board` branch (clean board profile, stock Coze protocol, no protocol bridge). Boot-tested on hardware. |

## Flashing (app-only, preserves NVS)

WiFi credentials and any server URL stored in NVS survive this:

```
esptool.py --chip esp32c3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 bootloader.bin 0x8000 partition-table.bin \
  0xd000 ota_data_initial.bin 0x100000 xiaozhi.bin
```

`flash_args` carries the same offsets for `write_flash @flash_args`.

## Caveats

- `xiaozhi.bin` points at the author's self-hosted server
  (`ws://192.168.31.220:8000`, NVS namespace `websocket`). For your own
  server, update the NVS `websocket/url` key or rebuild from the `mz01`
  branch (see the board kit description in the commit history).
- `mz01-prboard-xiaozhi.bin` uses the stock fork (Coze) protocol — it
  demonstrates the upstreamable board profile; see issue #5.
- Partition table matches the factory MZ01 layout
  (`partitions_mz01_8M.csv`); OTA slots at 0x100000 / 0x490000.
