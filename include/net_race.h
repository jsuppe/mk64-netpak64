/**
 * net_race.h — NetPak64 racing netcode (milestone A: kart position replication).
 *
 * Peer-to-peer, own-kart-authoritative snapshot replication over the NetPak64
 * device (see netpak.h). The local kart is always player slot 0 (rendered
 * full-screen by mk64 as usual); every remote peer is driven as a "puppet" in
 * slots 1..3. Each frame we broadcast a compact snapshot of slot 0 on ch0
 * (unreliable, newest-wins) and apply the latest snapshot from each peer to its
 * mapped puppet slot right where per-player simulation would otherwise run.
 *
 * The whole thing is a no-op unless the NetPak64 device is present, so a stock
 * ROM is unaffected.
 */
#ifndef NET_RACE_H
#define NET_RACE_H

#include <ultra64.h>
#include "common_structs.h"

/** Reset all replication state (call on race entry / epoch change). */
void net_race_reset(void);

/** Demo aid: force the local kart to auto-drive during a networked race so the
 *  paired instance can see it move (no-op unless built with NET_DEMO_AUTODRIVE).
 *  Call from the game loop AFTER read_controllers(). */
void net_race_autodrive(void);

/** Test-build overlay: draw the autodrive's injected inputs (stick bar + A/B
 *  lights) over the race view. Call right after net_lockstep_cam_pop() while
 *  the master display list is open. No-op unless NET_DEMO_AUTODRIVE. */
void net_autodrive_overlay(void);

/** Returns true exactly once, when the NetPak64 device is connected and it is
 *  time to auto-start the networked race (see net_race.c). The caller then
 *  performs the menu-bypass launch. Always false with no device. */
bool net_race_autostart_check(void);

/** Per-rendered-frame pump: drain inbound snapshots, broadcast the local kart.
 *  Assumes netpak_poll() already ran this frame. Safe to call every frame in
 *  any gamestate; only does work while racing with the device present. */
void net_race_frame(void);

/* Debug: dense per-frame in-race state sampler (call from race_logic_loop). */
void net_race_debug_tick(void);

/* Lockstep: exchange inputs and drive all player karts from the shared buffer.
 * Call once per frame AFTER read_controllers during an online race; a no-op
 * otherwise. Only active when built with NET_LOCKSTEP. */
void net_lockstep_tick(void);

/* Path B (isolated sim RNG): snapshot the sim's private RNG state after the sim
 * update and before rendering. Call from race_logic_loop between the sim and the
 * render. No-op unless an online lockstep race is active. */
void net_lockstep_rng_save(void);

/* Run the HUMAN drive handler for lockstep slots 1..N-1 (1P-mode GP only runs
 * it for player 0, leaving every remote player's kart parked at the grid).
 * Call from race_logic_loop's sim block right after
 * handle_a_press_for_all_players_during_race(). No-op offline. */
void net_lockstep_drive_humans(void);

/* Camera retarget: the slot the local 1P viewport/camera should follow. Returns
 * the local node's slot during an online lockstep race, 0 otherwise. Render-only. */
s32 net_lockstep_local_slot(void);

/* Robust local chase-cam. cam_push (call AFTER the sim, BEFORE the 1P render) runs
 * MK64's real camera-follow for the local kart against a persistent local-camera
 * context, isolated from the sim; cam_pop (call AFTER the render) restores the sim's
 * camera state so determinism is preserved. No-op offline / for the host slot. */
void net_lockstep_cam_push(void);
void net_lockstep_cam_pop(void);

/* Zero the CPU-AI residue block BEFORE course load (start-state agreement that
 * keeps the real CPU karts functional). Call from the online race launch. */
void net_lockstep_prerace_clear(void);

/* TEMP diag: drawn course chunk vs local kart section (test builds). */
void net_render_cull_diag(void);

/* Live desync indicator: flashing red square once peer sim-hashes mismatch.
 * Call right after net_lockstep_cam_pop() while the display list is open. */
void net_lockstep_desync_indicator(void);
void net_lockstep_stall_indicator(void);

/* Stall gate: true when the lockstep tick could not assemble every player's input
 * for the frame about to be simulated, so the sim must be held this render-frame
 * (both consoles freeze in sync rather than desyncing on a mismatched input).
 * race_logic_loop skips the sim update while this is set. Always false outside an
 * online lockstep race. */
bool net_lockstep_stalled(void);

/**
 * Puppet hook, called at the top of the per-player update (func_80028E70).
 * If `playerId` is a remote-driven slot with a fresh snapshot, overwrites the
 * kart's transform from the network and returns true — the caller must then
 * return, skipping local simulation for that slot. Returns false for the local
 * slot 0, unmapped slots, or before any snapshot has arrived (slot keeps its
 * normal CPU behavior until its peer starts sending).
 */
bool net_race_apply_puppet(Player* player, s32 playerId);

#endif /* NET_RACE_H */
