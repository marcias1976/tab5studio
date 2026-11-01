#!/bin/bash
# Runs Tab5 Studio on this computer without Docker. Needs a .venv with flask, pillow and fonttools
# and ESP-IDF 5.4. Override any of these before running, e.g. IDF_EXPORT=~/esp-idf/export.sh ./run-local.sh
cd "$(dirname "$0")"
export FIRMWARE_DIR="${FIRMWARE_DIR:-$(pwd)/firmware}"
export IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
export DATA_DIR="${DATA_DIR:-$(pwd)/data}"
exec .venv/bin/python app/server.py
