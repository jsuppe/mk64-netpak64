/**
 * netpak_sc64.h — SC64 USB transport backend for the NetPak64 driver.
 *
 * On a real console with a SummerCart64 there is no Domain-2 device; instead a
 * PC-side bridge daemon (np64_bridge.py) terminates the NetPak64 device
 * semantics and relays to np64-relay over UDP. This backend keeps a local
 * "register shadow" of the device in RDRAM and exchanges compact message
 * frames with the bridge over the cart's USB link (vendored UNFLoader usb.c).
 *
 * netpak_mk64.c stays the single driver: its np_read/np_write/np_dma seam
 * dispatches here when netpak_init() detects an SC64 instead of the Dom2
 * device. Register offsets and semantics follow netpak-spec.md v0.9 §3-§5.
 */
#ifndef NETPAK_SC64_H
#define NETPAK_SC64_H

#include <ultra64.h>

/** Probe for an SC64 + live bridge: usb_initialize, cart check, then a
 *  HELLO/WELCOME handshake with the bridge daemon (bounded wait).
 *  Returns 0 when the bridge answered, negative otherwise. */
s32 np_sc64_detect(void);

/** Register-shadow accessors mirroring the Dom2 np_read/np_write/np_dma
 *  contract. Reads of CMD_STATUS / STATUS / RX_COUNT / EPOCH pump the USB
 *  ingest so the driver's existing poll loops make progress. */
u32 np_sc64_read(u32 off);
void np_sc64_write(u32 off, u32 v);
void np_sc64_dma(s32 dir, u32 cart_off, void *dram, u32 len);

#endif
