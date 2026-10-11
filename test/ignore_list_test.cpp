// The ignore list's NVS persistence.
//
// Reported bug: with exactly one device on the list, deleting it worked --
// the row disappeared, count() read zero -- and then the device was back
// after a reboot. The cause is that Preferences::putBytes returns early on
// a zero-length value without touching NVS, so writing the now-empty list
// was a silent no-op and the previous blob, still holding that one device,
// survived. With two or more devices the write is non-empty and everything
// behaves, which is why it went unnoticed.
//
// Nothing in the in-memory state can show this: count() is correct either
// way. The assertion has to be about what is on disk, so these tests reach
// past IgnoreList to the Preferences namespace underneath it and ask how
// many bytes the next boot would find.
#include "ignore_list.h"
#include "test_util.h"
#include <Preferences.h>
#include <cstdlib>
#include <filesystem>

static const char* NS  = "ignore";
static const char* KEY = "dev";

// What begin() would read on the next boot.
static size_t bytesOnDisk() {
    Preferences p;
    p.begin(NS, false);
    return p.getBytesLength(KEY);
}

static const uint8_t A[6] = { 0xB4, 0x1E, 0x52, 0x01, 0x02, 0x03 };
static const uint8_t B[6] = { 0xAC, 0x9F, 0xC3, 0x0A, 0x0B, 0x0C };

int main() {
    // The shim only persists when it has somewhere to persist to.
    const char* dir = "ignore_test_nvs";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
    setenv("SQUACHSIM_NVS", dir, 1);

    suite("Removing one of several leaves the rest on disk");

    ck("adding the first device works", IgnoreList::add(A, DetectionType::FLOCK));
    ck("adding the second works",       IgnoreList::add(B, DetectionType::RING));
    ck("count is 2", IgnoreList::count() == 2);
    ck("two records on disk", bytesOnDisk() == 14);

    ck("removing the first reports success", IgnoreList::remove(A));
    ck("count is 1", IgnoreList::count() == 1);
    ck("the removed device is gone", !IgnoreList::contains(A));
    ck("the other one is still there", IgnoreList::contains(B));
    // The survivor backfills index 0, so this also covers the swap-with-last.
    ck("one record on disk", bytesOnDisk() == 7);
    ck("and it is the right one", IgnoreList::typeAt(0) == DetectionType::RING);

    suite("Removing the last one empties the stored list too");

    ck("removing the last reports success", IgnoreList::remove(B));
    ck("count is 0", IgnoreList::count() == 0);
    ck("nothing in memory", !IgnoreList::contains(B));
    // The bug: this used to be 7. The list looked empty until the reboot
    // read the stale blob back and the device returned.
    ck("nothing on disk either -- it must not come back on reboot",
       bytesOnDisk() == 0);

    suite("clear() empties an already-populated list");

    ck("re-add", IgnoreList::add(A, DetectionType::FLOCK));
    ck("on disk", bytesOnDisk() == 7);
    IgnoreList::clear();
    ck("count is 0 after clear", IgnoreList::count() == 0);
    ck("nothing on disk after clear", bytesOnDisk() == 0);

    suite("An address ignored backwards (before v1.25.0) still matches");
    {
        const uint8_t fwd[6] = { 0x5E, 0xC5, 0xC1, 0xCF, 0x45, 0xC4 };
        const uint8_t old[6] = { 0xC4, 0x45, 0xCF, 0xC1, 0xC5, 0x5E };
        ck("ignored the old way round", IgnoreList::add(old, DetectionType::AIRTAG));
        ck("heard the new way round, still ignored", IgnoreList::contains(fwd));
        ck("not added twice either way", !IgnoreList::add(fwd, DetectionType::AIRTAG));
        ck("UN-IGNORE the new way round takes it off", IgnoreList::remove(fwd) && !IgnoreList::contains(old));
    }

    suite("A rotating tracker's entry follows it (fork addition)");
    {
        IgnoreList::clear();
        const uint8_t t1[6] = { 0xD1, 0x11, 0x22, 0x33, 0x44, 0x55 };
        const uint8_t t2[6] = { 0xD2, 0x66, 0x77, 0x88, 0x99, 0xAA };
        const uint8_t t3[6] = { 0xD3, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
        ck("ignore a Google tag", IgnoreList::add(t1, DetectionType::GOOGLE_TAG));
        ck("rekey to its new address", IgnoreList::rekey(t1, t2));
        ck("the new address is ignored", IgnoreList::contains(t2));
        ck("the old one no longer is", !IgnoreList::contains(t1));
        ck("still one entry", IgnoreList::count() == 1);
        ck("rekey from an address not on the list does nothing", !IgnoreList::rekey(t3, t1));
    }
    // After a restart the tag is back under yet another address, and the
    // stored one is no longer heard: IGNORE reuses that entry.
    {
        const uint8_t t2[6] = { 0xD2, 0x66, 0x77, 0x88, 0x99, 0xAA };
        const uint8_t t3[6] = { 0xD3, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
        const uint8_t t4[6] = { 0xD4, 0x01, 0x02, 0x03, 0x04, 0x05 };
        auto noneLive = [](const uint8_t*) { return false; };
        auto allLive  = [](const uint8_t*) { return true; };
        ck("IGNORE again reuses the stale entry", IgnoreList::addRotating(t3, DetectionType::GOOGLE_TAG, noneLive));
        ck("...so the list does not grow", IgnoreList::count() == 1);
        ck("...and holds the new address", IgnoreList::contains(t3) && !IgnoreList::contains(t2));
        ck("a second tag still heard gets its own entry", IgnoreList::addRotating(t4, DetectionType::GOOGLE_TAG, allLive));
        ck("two entries now", IgnoreList::count() == 2);
        ck("another type's stale entry is never reused",
           IgnoreList::add(A, DetectionType::FLOCK) &&
           IgnoreList::addRotating(B, DetectionType::SAMSUNG_TAG, noneLive) && IgnoreList::contains(A));
        IgnoreList::clear();
    }

    std::filesystem::remove_all(dir);
    return report();
}
