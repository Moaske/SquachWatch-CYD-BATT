// Service-data trackers -- trackerServiceData() in src/signatures.cpp
//
// Google Find My Device tags and Samsung SmartTags put their identity in
// service data only, never in the service UUID list. The interesting cases
// are the near-misses: a plain Eddystone beacon shares 0xFEAA and is not a
// tracker, and the same bytes inside some other field must not match.
#include "signatures.h"
#include "test_util.h"
#include <cstring>

int main() {
    // A Chipolo ONE Point-shaped advert: Flags, then FMDN service data --
    // 0xFEAA, frame 0x40, a 20-byte EID, the hashed flags byte.
    uint8_t fmdn[29] = {
        0x02, 0x01, 0x06,                    // Flags
        0x19, 0x16, 0xAA, 0xFE, 0x40,        // Service Data (25 bytes): 0xFEAA, FMDN frame
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,   // EID
        0x5A                                 // hashed flags
    };
    bool f = false, u = false;

    suite("Google Find My Device");
    ck("FMDN frame is a Google tag", trackerServiceData(fmdn, sizeof fmdn, &f, &u) == DetectionType::GOOGLE_TAG);
    ck("...and flagged as a real FMDN frame", f);
    ck("...with protection off", !u);

    uint8_t t[29];
    memcpy(t, fmdn, 29); t[7] = 0x41;
    ck("frame 0x41 matches too", trackerServiceData(t, 29, &f, &u) == DetectionType::GOOGLE_TAG && f && u);

    suite("Not a tracker");
    memcpy(t, fmdn, 29); t[7] = 0x00;    // Eddystone-UID
    ck("Eddystone-UID frame is not FMDN", trackerServiceData(t, 29, &f, &u) == DetectionType::UNKNOWN && !f);
    memcpy(t, fmdn, 29); t[7] = 0x10;    // Eddystone-URL
    ck("Eddystone-URL frame is not FMDN", trackerServiceData(t, 29, &f, &u) == DetectionType::UNKNOWN);
    memcpy(t, fmdn, 29); t[5] = 0xAB;
    ck("another service's data", trackerServiceData(t, 29, &f, &u) == DetectionType::UNKNOWN);
    memcpy(t, fmdn, 29); t[4] = 0xFF;    // the same bytes, as manufacturer data
    ck("the bytes inside manufacturer data", trackerServiceData(t, 29, &f, &u) == DetectionType::UNKNOWN);

    suite("Malformed");
    ck("null", trackerServiceData(nullptr, 10, &f, &u) == DetectionType::UNKNOWN);
    ck("truncated structure", trackerServiceData(fmdn, 6, &f, &u) == DetectionType::UNKNOWN);
    ck("null out-pointers are fine", trackerServiceData(fmdn, sizeof fmdn, nullptr, nullptr) == DetectionType::GOOGLE_TAG);

    suite("Samsung SmartTag");
    uint8_t st[] = { 0x02, 0x01, 0x06, 0x05, 0x16, 0x5A, 0xFD, 0x10, 0x20 };
    ck("0xFD5A service data is a SmartTag", trackerServiceData(st, sizeof st, &f, &u) == DetectionType::SAMSUNG_TAG && !f);

    return report();
}
