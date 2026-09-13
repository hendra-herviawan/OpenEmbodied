#!/bin/bash
# Flash mz01-c3-lcd build onto the ESP32-C3 (EZTECH X1) via native USB
# Usage: ./flash-mz01.sh          (erase NVS = fresh WiFi/server config)
#        KEEP_NVS=1 ./flash-mz01.sh
set -e
cd "$(dirname "$0")"
source ~/esp/esp-idf/export.sh >/dev/null 2>&1
PORT=${PORT:-/dev/ttyACM0}

if [ "${KEEP_NVS:-0}" != "1" ]; then
    echo ">> Erasing NVS (WiFi/server config will be reset). Use KEEP_NVS=1 to skip."
    idf.py -B build-mz01 -p "$PORT" erase-flash
fi

# App goes to ota_0 (no factory partition in partitions_8M.csv; otadata erased -> boots ota_0)
idf.py -B build-mz01 -p "$PORT" flash
echo ">> Done. Monitoring serial (Ctrl+] to exit):"
idf.py -B build-mz01 -p "$PORT" monitor
