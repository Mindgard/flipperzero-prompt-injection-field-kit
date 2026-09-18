#pragma once

/*
 * Delivery channel table — the single description of what each channel
 * is, what it can carry, and how it is named.
 *
 * Three separate enumerations of this concept used to be kept in step by
 * hand: PifkProtocol (settings.json), the QuickDeployIndex rows
 * (the UI), and the bridge's EXEC verb strings.  They disagreed — the
 * protocol enum knew about five channels while the UI offered twelve.
 * Everything reads this table instead.
 *
 * The capacity limits themselves stay in the headers next to the code
 * that enforces them, and are referenced here, so the payload list's
 * idea of what fits cannot drift from the executor's.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <gui/icon.h>

/* This header deliberately does not include pifk_app.h: it is
 * included *by* code that pifk_app.h's consumers use, and the
 * payload type there is an anonymous struct typedef that cannot be
 * forward-declared.  The eligibility API therefore takes the payload's
 * text directly, and the one function that needs the database takes it
 * as void* (see pifk_channel_eligible_count). */

/* ── Channel identity ────────────────────────────────────────── *
 *
 * Nothing persists these today — the settings.json default_protocol they
 * were designed to be compatible with turned out to be written by the
 * settings screen and read by nobody, so it was removed rather than
 * renamed.
 *
 * They are still append-only, because the ids are what a saved scene
 * state and any future persisted default would name, and because slot 3
 * records a retired channel (Sub-GHz, which never executed anything).
 * Reordering to close that gap would be a silent change of meaning for
 * no gain.
 */
typedef enum {
    PifkChannelBadUsb = 0,
    PifkChannelNfcFile = 1,
    PifkChannelBleBeacon = 2,
    PifkChannelReserved3 = 3, /* was Sub-GHz; do not reuse */
    PifkChannelGpio = 4,
    PifkChannelGpioCapture = 5,
    PifkChannelQr = 6,
    PifkChannelUsbDesc = 7,
    PifkChannelBleGatt = 8,
    PifkChannelNfcText = 9,
    PifkChannelNfcUrl = 10,
    PifkChannelI2cWrite = 11,
    PifkChannelIdCount = 12,
} PifkChannelId;

/* ── Groups ──────────────────────────────────────────────────── *
 *
 * Named for what the operator must physically do to the target, because
 * that is the question they can answer by looking at it.  "USB /
 * Wireless / Screen / Wires" is answerable on sight; "HID / RF /
 * Optical / Serial" requires already knowing the answer.
 *
 * Display order, unlike PifkChannelId, is free to change: nothing
 * persists a group index.
 */
typedef enum {
    PifkGroupUsb,
    PifkGroupWireless,
    PifkGroupScreen,
    PifkGroupWires,
    PifkGroupCount,
} PifkChannelGroup;

typedef struct {
    PifkChannelId id;
    PifkChannelGroup group;

    /* What the channel does, in the operator's terms ("Type it"), and
     * the technology underneath it ("BadUSB").  Two fields because the
     * first is what you choose by and the second is what you report. */
    const char* label;
    const char* tech;

    /* The bridge's EXEC verb, without the "EXEC " prefix.  NULL for a
     * channel the bridge does not expose. */
    const char* verb;

    /* What this channel is, what the target has to do for it to work,
     * and what it cannot tell you.  Shown by pressing Right on the
     * channel's row.
     *
     * Held here rather than in the scene because it is a property of the
     * channel, and the scene that lists channels is not the only place
     * that wants it — the payload-first "Deploy via..." screen lists the
     * same channels and needs the same explanations. */
    const char* help;

    /* Glyph drawn at the start of the row, 9x9.  Held in the table for
     * the same reason as the help text: the payload-first "Deploy
     * via..." screen lists the same channels and wants the same marks. */
    const Icon* icon;

    /* Largest payload the channel can carry, in bytes. */
    size_t max_bytes;

    /* Characters this channel cannot represent and drops silently, or
     * NULL if it carries arbitrary bytes.
     *
     * A function pointer rather than an `ascii_only` flag because the
     * two channels that lose characters lose *different* ones: BadUSB
     * drops anything hid_ascii_to_key() cannot map, which includes some
     * ASCII, while USB string descriptors drop only bytes >= 0x80. A
     * single boolean would make this filter disagree with the executor
     * for exactly the payloads where the difference matters. Each
     * channel keeps its own predicate and the table points at it. */
    size_t (*count_lossy_chars)(const char* text);

    /* Keeps running after the execute call returns, and is stopped on
     * scene exit.  Was previously discoverable only by reading a
     * comment and cross-checking four stop calls in on_exit. */
    bool persists_after_exec;

    /* Ignores the selected payload entirely — a diagnostic rather than
     * a delivery.  Listed apart from the delivery channels so the menu
     * does not imply it sends anything. */
    bool is_diagnostic;
} PifkChannel;

/* ── Table access ────────────────────────────────────────────── */

/* Every delivery channel, in display order within each group.
 * Diagnostics (bus scan, loopback) are not in here; they are scene
 * actions under their group, not payload destinations. */
const PifkChannel* pifk_channels(size_t* count);

/* NULL for PifkChannelReserved3 or an out-of-range id, so a stale
 * settings.json cannot select a channel that does not exist. */
const PifkChannel* pifk_channel_by_id(PifkChannelId id);

/* Case-insensitive match on the EXEC verb.  NFCEMUURL must be tested
 * before NFCEMU by any caller doing prefix matching — the bridge's
 * existing ordering already does. */
const PifkChannel* pifk_channel_by_verb(const char* verb);

const char* pifk_group_name(PifkChannelGroup group);

/* What the operator must do to the target for this group to work.
 * Shown as the group screen's header, where it answers "is this the
 * right group?" without costing a row. */
const char* pifk_group_precondition(PifkChannelGroup group);

/* Number of channels in a group, for the "(2)" count on the main menu. */
size_t pifk_group_channel_count(PifkChannelGroup group);

/* Glyph for a group's row on the main menu. */
const Icon* pifk_group_icon(PifkChannelGroup group);

/* ── Eligibility ─────────────────────────────────────────────── *
 *
 * The whole point of the table.  A 128x64 monochrome screen has no room
 * for a validation layer, so the only affordable validation is not
 * offering the invalid option: payload lists filter on this rather than
 * rejecting a selection afterwards.
 */

bool pifk_channel_accepts_text(const PifkChannel* ch, const char* text);

/* Number of payloads in the database this channel can carry.  Shown in
 * the list header — an operator not told that 27 of 45 were filtered
 * out will conclude the library is small. */
size_t pifk_channel_eligible_count(const PifkChannel* ch, const void* payload_db);

/* Why a payload was excluded, for the payload-first path where the
 * channel list is filtered against a fixed payload and the operator
 * needs to know what happened to the missing entries.  NULL if the
 * channel accepts it. */
const char* pifk_channel_reject_reason(const PifkChannel* ch, const char* text);
