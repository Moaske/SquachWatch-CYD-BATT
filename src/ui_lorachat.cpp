// SquachWatch-CYD — LORA CHATS. See include/ui_lorachat.h.
//
// The message ring lives in the sniffer, behind its lock, 256 deep and both
// networks mixed. This screen keeps its own copy of the newest few of the
// open tab's network and rebuilds it only when the ring has moved, so a quiet
// screen is not copying 70 KB out from under a mutex thirty times a second.
// SQUACH_LORA_FAKE: the PC emulator's one-shot renderer (the `lorachat`
// screen), off sim/lora_sim.cpp, with the T-Deck's compose line. No board
// defines it.
#if SQUACH_LORA || SQUACH_LORA_FAKE
#include "ui_lorachat.h"
#include "lora_sniffer.h"
#include "lora_meshtastic.h"
#include "lora_meshcore.h"
#include "lora_channels.h"
#include "settings.h"
#include "theme.h"
#include "privacy.h"
#if defined(TDECK_PLUS) || SQUACH_LORA_FAKE
#define LORACHAT_COMPOSE 1
#include "ui_lorasettings.h"   // uiLoraMyName(): what MeshCore shows before the colon
#else
#define LORACHAT_COMPOSE 0
#endif
#include <Arduino.h>
#include <string.h>

namespace {

struct Row {
    char     sender[24];
    char     chan[16];
    char     text[201];
    uint32_t ms;
    int16_t  rssi;
};

const uint8_t  KEEP  = 24;       // the newest this many of the open tab
Row            s_rows[KEEP];
uint8_t        s_rowN   = 0;
uint8_t        s_tab    = 0;     // 0 Meshtastic, 1 MeshCore
int            s_scroll = 0;     // rows skipped from the newest
uint32_t       s_sig    = 0xFFFFFFFFu;
uint8_t        s_sigTab = 0xFF;
uint16_t       s_count[2] = { 0, 0 };
// The ring's running total when the chats were last looked at; the unread
// count is what arrived since, for both tabs.
uint32_t       s_seenTotal = 0;

const int TAB_H = 22;
#if LORACHAT_COMPOSE
// The compose line: what is typed, and a short status in its place after Enter.
const int COMPOSE_H = 22;
char     s_compose[161] = { 0 };
uint8_t  s_composeN = 0;
char     s_status[40] = { 0 };
uint32_t s_statusAt = 0;
// The channel chip at the compose line's right end, where it was drawn, for
// uiLoraChatHit; the longest name it shows before it cuts one and ends "..".
int      s_chipX = 0, s_chipY = -100, s_chipW = 0, s_chipH = 0;
const size_t CHIP_CHARS = 14;
#endif

Lora::Proto tabProto(uint8_t t) { return t ? Lora::Proto::MESHCORE : Lora::Proto::MESHTASTIC; }

uint32_t ringTotal() { return (uint32_t)Lora::msgCount() + Lora::msgDropped(); }

void chanName(Lora::Proto p, uint8_t idx, char* out, size_t cap) {
    if (p == Lora::Proto::MESHTASTIC) { Meshtastic::Channel c = Meshtastic::channel(idx); snprintf(out, cap, "%s", c.name[0] ? c.name : "?"); }
    else                              { MeshCore::Channel c = MeshCore::channel(idx);     snprintf(out, cap, "%s", c.name[0] ? c.name : "?"); }
}

// Walk the ring newest first, count both networks, and copy the open tab's
// newest KEEP. Only when something changed.
void refresh() {
    const uint32_t sig = ringTotal();
    if (sig == s_sig && s_sigTab == s_tab) return;
    s_sig = sig; s_sigTab = s_tab;
    s_rowN = 0; s_count[0] = s_count[1] = 0;
    const Lora::Proto want = tabProto(s_tab);
    const uint16_t n = Lora::msgCount();
    Lora::Msgs::Msg m;
    for (uint16_t i = 0; i < n; i++) {
        if (!Lora::msgAt(i, m)) break;
        if (m.proto == Lora::Proto::MESHTASTIC) s_count[0]++;
        else if (m.proto == Lora::Proto::MESHCORE) s_count[1]++;
        else continue;
        if (m.proto != want || s_rowN >= KEEP) continue;
        Row& r = s_rows[s_rowN++];
        snprintf(r.sender, sizeof r.sender, "%s", m.sender[0] ? m.sender : "?");
        chanName(m.proto, m.chan, r.chan, sizeof r.chan);
        snprintf(r.text, sizeof r.text, "%s", m.text);
        r.ms = m.ms;
        r.rssi = m.rssi;
    }
}

void age(uint32_t now, uint32_t ms, char* out, size_t cap) {
    const uint32_t s = (now - ms) / 1000;
    if (s < 60)         snprintf(out, cap, "%lus", (unsigned long)s);
    else if (s < 3600)  snprintf(out, cap, "%lum", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
    else                snprintf(out, cap, "%lud", (unsigned long)(s / 86400));
}

// Word-wrap `text` into `maxW` pixels in the current font. Draws when
// `draw`, and returns the number of lines either way. A word too wide for a
// line on its own is cut where it runs out.
int wrap(TFT_eSPI& t, const char* text, int x, int y, int maxW, int lineH, int yMax, bool draw) {
    char line[64];
    int lines = 0;
    const char* p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        size_t len = 0, lastBreak = 0;
        const char* q = p;
        while (*q && len < sizeof line - 1) {
            line[len] = *q; line[len + 1] = '\0';
            if (t.textWidth(line) > maxW) break;
            if (*q == ' ') lastBreak = len;
            len++; q++;
        }
        if (*q && lastBreak && len < sizeof line - 1) len = lastBreak;
        if (!len) len = 1;
        line[len] = '\0';
        if (draw && y + lines * lineH + lineH <= yMax) {
            t.setCursor(x, y + lines * lineH + Theme::bubbleAscent());
            t.print(line);
        }
        lines++;
        p += len;
    }
    return lines ? lines : 1;
}

const char* listenLine() {
    switch (Settings::loraListen()) {
        case 0:  return "LORA IS OFF (WATCH SETTINGS > LORA)";
        case 1:  return "LISTENING: MESHTASTIC";
        case 2:  return "LISTENING: MESHCORE";
        default: return "LISTENING: BOTH";
    }
}

#if LORACHAT_COMPOSE
// The compose line, above the BACK bar: the typed text with a cursor, the
// tail of it when it is longer than the line; a status for a moment after
// Enter; the way to switch it on when LORA TX is off. At its right end, the
// channel chip: the channel Enter sends on, outlined on the network's
// default, filled in its colour on any other; a tap steps it.
void drawCompose(TFT_eSPI& t, int w, int y, uint32_t now) {
    const uint16_t accent = s_tab ? Theme::PINK : Theme::CYAN;
    const uint16_t bg = Theme::blend(Theme::BG, Theme::WHITE, 18);
    t.fillRoundRect(4, y, w - 8, COMPOSE_H, 4, bg);
    t.drawRoundRect(4, y, w - 8, COMPOSE_H, 4, Theme::blend(Theme::BG, accent, 150));
    t.setTextSize(1);
    const int ty = y + (COMPOSE_H - t.fontHeight()) / 2;

    const Lora::Proto p = tabProto(s_tab);
    char chip[24];
    Lora::Chan::txName(p, Lora::Chan::txPick(p), chip, sizeof chip);
    if (strlen(chip) > CHIP_CHARS) { chip[CHIP_CHARS - 2] = '.'; chip[CHIP_CHARS - 1] = '.'; chip[CHIP_CHARS] = '\0'; }
    s_chipH = COMPOSE_H - 6;
    s_chipW = t.textWidth(chip) + 12;
    if (s_chipW < 36) s_chipW = 36;
    s_chipX = w - 4 - 3 - s_chipW;
    s_chipY = y + 3;
    const bool def = Lora::Chan::txIsDefault(p);
    const uint16_t cbg = def ? Theme::blend(Theme::BG, accent, 40) : accent;
    t.fillRoundRect(s_chipX, s_chipY, s_chipW, s_chipH, 3, cbg);
    t.drawRoundRect(s_chipX, s_chipY, s_chipW, s_chipH, 3, accent);
    t.setTextColor(def ? accent : Theme::BG, cbg);
    t.setCursor(s_chipX + (s_chipW - t.textWidth(chip)) / 2, ty);
    t.print(chip);

    // The text, kept left of the chip.
    const int maxW = s_chipX - 6 - 10;
    char line[168];
    const char* msg = nullptr;
    uint16_t col = Theme::WHITE;
    if (s_status[0] && now - s_statusAt < 2500) { msg = s_status; col = Theme::VAPOR_YELLOW; }
    else if (!Settings::loraTxDbm() && !s_composeN) { msg = "TYPE HERE  (LORA TX IS OFF: SETTINGS)"; col = Theme::blend(Theme::BG, Theme::WHITE, 120); }
    if (msg) {
        snprintf(line, sizeof line, "%s", msg);
        size_t n = strlen(line);
        while (n && t.textWidth(line) > maxW) line[--n] = '\0';
        t.setTextColor(col, bg);
        t.setCursor(10, ty); t.print(line);
        return;
    }
    snprintf(line, sizeof line, "%s_", s_compose);
    const char* q = line;
    while (*q && t.textWidth(q) > maxW) q++;           // the tail that fits
    t.setTextColor(Theme::WHITE, bg);
    t.setCursor(10, ty); t.print(q);
}
#endif

}  // namespace

void uiLoraChatInit(TFT_eSPI& t) {
    s_scroll = 0;
    s_sig = 0xFFFFFFFFu;
    s_seenTotal = ringTotal();
    t.fillRect(0, 0, t.width(), t.height(), Theme::BG);
}

void uiLoraChatSelect(uint8_t tab) {
    if (tab > 1) tab = 1;
    if (tab != s_tab) { s_tab = tab; s_scroll = 0; }
}

void uiLoraChatScroll(int delta) {
    s_scroll += delta;
    if (s_scroll > (int)s_rowN - 1) s_scroll = (int)s_rowN - 1;
    if (s_scroll < 0) s_scroll = 0;
}

#if !SQUACH_LORA_FAKE   // the emulator's own drives the LORA page's badge
uint16_t uiLoraChatUnread() {
    const uint32_t tot = ringTotal();
    // The ring was cleared (LORA SETTINGS > CLEAR CHATS): start counting again.
    if (tot < s_seenTotal) s_seenTotal = tot;
    return (uint16_t)(tot > s_seenTotal ? (tot - s_seenTotal > 999 ? 999 : tot - s_seenTotal) : 0);
}
#endif

void uiLoraChatTick(TFT_eSPI& t, uint32_t now) {
    refresh();
    s_seenTotal = ringTotal();   // looking at it is reading it
    const int w = t.width(), h = t.height();
    int bodyBottom = h - Theme::pinnedBackH(w) - 2;
#if LORACHAT_COMPOSE
    bodyBottom -= COMPOSE_H + 4;
    drawCompose(t, w, bodyBottom + 2, now);
#endif
    t.fillRect(0, 0, w, bodyBottom, Theme::BG);

    // The two tabs, the open one filled.
    const int tabY = Theme::LIST_TOP;
    const char* names[2] = { "MESHTASTIC", "MESHCORE" };
    t.setTextSize(1);
    for (uint8_t i = 0; i < 2; i++) {
        const int x0 = i ? w / 2 + 2 : 4, x1 = i ? w - 4 : w / 2 - 2;
        const bool on = (i == s_tab);
        const uint16_t col = i ? Theme::PINK : Theme::CYAN;
        if (on) t.fillRoundRect(x0, tabY, x1 - x0, TAB_H, 4, col);
        else    t.drawRoundRect(x0, tabY, x1 - x0, TAB_H, 4, col);
        char lab[20];
        snprintf(lab, sizeof lab, "%s %u", names[i], (unsigned)s_count[i]);
        t.setTextColor(on ? Theme::BG : col, on ? col : Theme::BG);
        t.setCursor(x0 + ((x1 - x0) - t.textWidth(lab)) / 2, tabY + (TAB_H - t.fontHeight()) / 2);
        t.print(lab);
    }

    int y = tabY + TAB_H + 4;
    t.setTextColor(Theme::blend(Theme::BG, Theme::WHITE, 140), Theme::BG);
    t.setCursor(6, y);
    t.print(listenLine());
    y += t.fontHeight() + 5;

    if (!s_rowN) {
        Theme::bubbleFontOn(t);
        t.setTextColor(Theme::blend(Theme::BG, Theme::WHITE, 170), Theme::BG);
        const char* msg = s_tab ? "No MeshCore messages yet. Public and hashtag channels show up here."
                                : "No Meshtastic messages yet. The public LongFast channel shows up here.";
        wrap(t, msg, 8, y + 6, w - 16, Theme::bubbleTextH() + 2, bodyBottom, true);
        Theme::bubbleFontOff(t);
        Theme::drawPinnedBack(t, "[ BACK ]");
        return;
    }

    const uint16_t accent = s_tab ? Theme::PINK : Theme::CYAN;
    const int lineH = Theme::bubbleTextH() + 1;
    for (int i = s_scroll; i < s_rowN && y < bodyBottom - 12; i++) {
        const Row& r = s_rows[i];
        // Who, which channel, and how long ago.
        t.setTextSize(1);
        char ago[8];
        age(now, r.ms, ago, sizeof ago);
        t.setTextColor(accent, Theme::BG);
        t.setCursor(6, y);
        {
            char pv[40];
            t.print(Privacy::name(r.sender, pv, sizeof pv));
        }
        t.setTextColor(Theme::blend(Theme::BG, Theme::WHITE, 120), Theme::BG);
        t.print("  ");
        t.print(r.chan);
        t.setCursor(w - 8 - t.textWidth(ago), y);
        t.print(ago);
        y += t.fontHeight() + 2;
        // What they said.
        Theme::bubbleFontOn(t);
        t.setTextColor(Theme::WHITE, Theme::BG);
        const int n = wrap(t, r.text, 10, y, w - 20, lineH, bodyBottom, true);
        Theme::bubbleFontOff(t);
        y += n * lineH + 6;
        if (y < bodyBottom) t.drawFastHLine(6, y - 3, w - 12, Theme::blend(Theme::BG, accent, 60));
    }
    Theme::drawPinnedBack(t, "[ BACK ]");
}

#if LORACHAT_COMPOSE
void uiLoraChatStepChannel(uint32_t now) {
    const Lora::Proto p = tabProto(s_tab);
    const uint8_t was = Lora::Chan::txPick(p);
    if (Lora::Chan::txStep(p) == was) {
        snprintf(s_status, sizeof s_status, "NO OTHER CHANNEL: LORA SETTINGS");
        s_statusAt = now;
    }
}

void uiLoraChatKey(char c, uint32_t now) {
    if (c == '\b') { if (s_composeN) s_compose[--s_composeN] = '\0'; return; }
    if (c == '\n') {
        if (!s_composeN) return;
        if (!Settings::loraTxDbm()) { snprintf(s_status, sizeof s_status, "LORA TX IS OFF: SETTINGS"); s_statusAt = now; return; }
        snprintf(s_status, sizeof s_status, "SENDING...");
        // LORA SETTINGS' NAME, else the squad nickname (include/lora_myname.h),
        // on the channel the chip shows.
        const Lora::Proto p = tabProto(s_tab);
        const bool ok = Lora::sendText(p, uiLoraMyName(), s_compose, (int8_t)Settings::loraTxDbm(), (int16_t)Lora::Chan::txPick(p));
        snprintf(s_status, sizeof s_status, ok ? "SENT" : "SEND FAILED");
        s_statusAt = now;
        if (ok) { s_composeN = 0; s_compose[0] = '\0'; s_sig = 0xFFFFFFFFu; }
        return;
    }
    if (c < 32 || c > 126) return;
    const uint8_t cap = s_tab ? 120 : 160;      // MeshCore's text is shorter
    if (s_composeN < cap) { s_compose[s_composeN++] = c; s_compose[s_composeN] = '\0'; }
}
#else
void uiLoraChatKey(char, uint32_t) {}
void uiLoraChatStepChannel(uint32_t) {}
#endif

LoraChatHit uiLoraChatHit(TFT_eSPI& t, int x, int y) {
#if LORACHAT_COMPOSE
    // The chip, with a few pixels of slack round it.
    if (x >= s_chipX - 4 && x < s_chipX + s_chipW + 4 && y >= s_chipY - 4 && y < s_chipY + s_chipH + 4)
        return LoraChatHit::CHANNEL;
#endif
    const int tabY = Theme::LIST_TOP;
    if (y < tabY || y > tabY + TAB_H + 4) return LoraChatHit::NONE;
    return x < t.width() / 2 ? LoraChatHit::TAB_MESHTASTIC : LoraChatHit::TAB_MESHCORE;
}

#endif
