// SquachWatch-CYD — per-device alert suppression ("IGNORE")
#pragma once
#include <stdint.h>
#include "state.h"

// A short list of MACs whose detections should never raise the full-screen
// ALERT. The point is the devices you own: your own AirTag in your own
// pocket and your own doorbell camera are true positives every time, and a
// detector that shouts about them constantly is one you stop reading.
//
// Ignored devices are still scanned, still counted and still written to the
// LOG -- only the alert is suppressed. That way the list is recoverable
// (you can see the device and un-ignore it) and the counters stay honest.
//
// Storage is a single packed NVS blob of 7-byte records -- six bytes of
// MAC and one of DetectionType -- not one key per device. NVS allocates in
// 32-byte entries, so a key each would cost several times the space and
// make enumeration a series of lookups; the whole list at MAX entries is
// 448 bytes, which is nothing against the 20KB partition.
//
// The type is stored rather than looked up because the log it came from is
// a short ring: mute a device, leave it a while, and the detection that
// told you what it was has long since scrolled out. Without the type the
// list is a column of MACs, which tells you that you muted something but
// not what.
namespace IgnoreList {

    static const uint8_t MAX = 64;

    // Loads from NVS. Safe to call more than once.
    void begin();

    bool contains(const uint8_t* mac);

    // Returns false if the list is full or the MAC is already on it.
    // Persists immediately -- an ignore that did not survive a reboot
    // would be worse than no ignore at all.
    bool add(const uint8_t* mac, DetectionType type = DetectionType::UNKNOWN);

    bool remove(const uint8_t* mac);

    // A tracker that rotates its address took a new one (see the engine's
    // handover): the entry follows it, in RAM only. Writing NVS every
    // fifteen minutes per tag would wear the flash for nothing -- after a
    // restart the tag has rotated again anyway. False when `from` is not on
    // the list.
    bool rekey(const uint8_t* from, const uint8_t* to);
    // IGNORE on a device of a type that rotates its address: rather than a
    // new entry every time the same tag comes back under a new address (one
    // per restart, filling the list), an entry of the same type whose
    // address is no longer around is reused. `live` says whether an address
    // is currently being heard. Same return as add().
    bool addRotating(const uint8_t* mac, DetectionType type, bool (*live)(const uint8_t* mac));

    uint8_t count();

    // nullptr when idx is out of range. Points into the module's own
    // storage; valid until the next add/remove/clear.
    const uint8_t* macAt(uint8_t idx);

    // UNKNOWN for out-of-range, and for anything muted before the type was
    // recorded or muted from the raw scanner, which classifies nothing.
    DetectionType typeAt(uint8_t idx);

    void clear();

    // SNOOZE: the same thing as IGNORE for one device -- no full-screen
    // alert, still scanned, counted and logged -- but in RAM only, so it
    // lasts until the board restarts and not a moment longer. For the
    // doorbell across the street that keeps coming and going on a walk
    // past, where IGNORE would be a decision about forever.
    //
    // It used to mute the whole TYPE for an hour, and more than the alert:
    // it switched detection of that type off, so nothing was logged either.
    // Snoozing a neighbour's AirTag silenced every AirTag, which is the one
    // thing a tracker detector must never quietly do.
    //
    // Thirty-two of them; when full, the oldest snooze makes room.
    void snooze(const uint8_t* mac);
    bool snoozed(const uint8_t* mac);
    // What every alert gate asks: ignored for good, or snoozed for now.
    bool silenced(const uint8_t* mac);
}
