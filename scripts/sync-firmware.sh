#!/bin/bash
# Copies the Tab5 firmware source into ./firmware (for the Docker image and the GitHub repository).
# Build output, downloaded components, local tools and the secrets files (Wi-Fi password, web PIN) stay out;
# example secrets files are written instead. Usage: scripts/sync-firmware.sh [path to the firmware project]
set -e
SRC="${1:-$HOME/esp/tab5panel}"
DST="$(cd "$(dirname "$0")/.." && pwd)/firmware"
[ -f "$SRC/CMakeLists.txt" ] || { echo "Nie znaleziono projektu Tab5 w $SRC"; exit 1; }
mkdir -p "$DST"
rsync -a --delete \
  --exclude build/ --exclude managed_components/ --exclude backup/ --exclude tools/ --exclude fonts_src/ --exclude .cache/ \
  --exclude main/studio/ --exclude 'sdkconfig.old' --exclude '*.bin' --exclude .DS_Store \
  --exclude main/wifi_secrets.h --exclude main/web_secrets.h \
  "$SRC/" "$DST/"
cat > "$DST/main/wifi_secrets.h.example" <<'H'
#pragma once

// Copy to wifi_secrets.h (or set WIFI_SSID / WIFI_PASSWORD in Tab5 Studio's .env)
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"
H
cat > "$DST/main/web_secrets.h.example" <<'H'
#pragma once

// PIN for the web page and API. Copy to web_secrets.h (or set WEB_PIN in Tab5 Studio's .env)
#define WEB_PIN "0000"
H
echo "Skopiowano kod Tab5 do $DST (bez haseł)."
