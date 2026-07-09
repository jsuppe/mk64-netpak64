#!/bin/bash
# Flash the current build to the SummerCart64 over USB, then power-cycle the N64.
# Usage: ./flashcart.sh [rom]   (default: build/us/mk64.us.z64)
set -e
ROM="${1:-build/us/mk64.us.z64}"
DEPLOYER=~/dev/sc64/SummerCart64/sw/deployer/target/release/sc64deployer
md5sum "$ROM"
"$DEPLOYER" upload "$ROM" --save-type eeprom4k
echo "Uploaded. Power-cycle the console to boot."
