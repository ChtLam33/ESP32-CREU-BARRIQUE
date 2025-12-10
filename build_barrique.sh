#!/usr/bin/env bash
set -e

arduino-cli compile \
  --fqbn esp32:esp32:esp32c3 \
    --output-dir build-barrique \
      sketch_dec5a8TESTESPC3