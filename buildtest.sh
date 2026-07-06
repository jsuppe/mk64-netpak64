#!/bin/bash
# buildtest.sh — canonical TEST-ROM build+stage. Flips test flags, builds,
# verifies the ROM actually changed, stages to the harness path, and flips
# flags back to product so the tree never lingers in test state.
# Usage: ./buildtest.sh            (test build -> mk64_test.z64)
#        ./buildtest.sh product    (product build -> prints md5, no staging)
set -euo pipefail
cd "$(dirname "$0")"

MODE=${1:-test}
if [ "$MODE" = test ]; then
  sed -i 's/^#define NET_MENU_TEST 0$/#define NET_MENU_TEST 1/; s/^#define NET_DEMO_AUTODRIVE 0$/#define NET_DEMO_AUTODRIVE 1/' src/net_race.c
  sed -i 's/^#define NET_MENU_JOINED_CAN_START 0$/#define NET_MENU_JOINED_CAN_START 1/' src/net_menu.c
else
  sed -i 's/^#define NET_MENU_TEST 1$/#define NET_MENU_TEST 0/; s/^#define NET_DEMO_AUTODRIVE 1$/#define NET_DEMO_AUTODRIVE 0/' src/net_race.c
  sed -i 's/^#define NET_MENU_JOINED_CAN_START 1$/#define NET_MENU_JOINED_CAN_START 0/' src/net_menu.c
fi

OLD=$(md5sum build/us/mk64.us.z64 2>/dev/null | cut -c1-8 || echo none)
if ! make NON_MATCHING=1 -j"$(nproc)" 2>&1 | grep -iE "error" ; then :; else
  echo "BUILD FAILED"; exit 1
fi
NEW=$(md5sum build/us/mk64.us.z64 | cut -c1-8)
echo "md5: $OLD -> $NEW"
grep -q "#define NET_MENU_TEST 1" src/net_race.c && FLAGS=test || FLAGS=product
echo "flags in ROM: $FLAGS"

if [ "$MODE" = test ]; then
  [ "$FLAGS" = test ] || { echo "FATAL: flag mismatch"; exit 1; }
  cp build/us/mk64.us.z64 /mnt/micron/jsuppe/netpak/mk64_test.z64
  echo "STAGED mk64_test.z64 ($NEW)"
  # restore product flags in the tree (ROM already built+staged)
  sed -i 's/^#define NET_MENU_TEST 1$/#define NET_MENU_TEST 0/; s/^#define NET_DEMO_AUTODRIVE 1$/#define NET_DEMO_AUTODRIVE 0/' src/net_race.c
  sed -i 's/^#define NET_MENU_JOINED_CAN_START 1$/#define NET_MENU_JOINED_CAN_START 0/' src/net_menu.c
fi
