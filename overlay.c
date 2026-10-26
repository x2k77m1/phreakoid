/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PHREAKOID's pages: full SYN1/SYN2 content redraws on a PHREAKOID track
 * (digitone-mk1/machines.h, core-dn1 3.1).
 *
 * Stock SYN pages draw FM's pictures (the algorithm diagram, the group
 * headings, the envelope graphs) and format values as FM, which the machines
 * companion cannot replace per track. This paints the whole content area on
 * ev_draw, after the firmware drew the frame: an 8-knob grid a page with our
 * labels, our true value texts and a mini bar under each knob, and a highlight
 * frame on the last-turned encoder's cell. Title bar, left rail and status bar
 * stay stock. The signal path is in engine.c; this file is only the pages.
 *
 * Gating uses firmware state, never pixels: paint only when the active track's
 * sound plays machine 100 (FW_SOUND_SLOT(fw_kit, fw_active_track, 0) >> 8), the
 * showing key is SYN1 or SYN2, the brain shows no screen over the pages, and
 * the shown view is on its first page. Second pages, menus, browsers and any
 * overlay fail the gate. The firmware's own popups and messages are drawn in
 * this same view, where no screen shows over the page, so the key chords that
 * draw them (PTN, BANK, FUNC+SETTINGS, and RECORD/PLAY/STOP with FUNC, a held
 * step key or a held track key while the unit is recording) hold the page off
 * for ~3 s. ev_tick (30/s) re-arms the frame only on change (page, values,
 * highlight), so the overlay is there from the first frame and tracks the kit
 * without redrawing 30 times a second and starving the UI task. Runs on the UI
 * task.
 *
 * Deferred to a later version: p-lock display on a held sequencer step. */
#include <stdint.h>
#include "digitone-mk1/machines.h"

#define MY_ID 100               /* must match engine.c and mod.json */

extern const struct dnm_knob phreakoid_knobs[DNM_KNOBS];
extern uint32_t phreakoid_ticks;        /* engine.c's UI-tick counter */
void phreakoid_reap(void);              /* engine.c's fuse backstop */

/* The showing key: the word right after fw_active_track in firmware RAM.
 * core-dn1 declares fw_active_track at 0x41367ce0 in 1.43 and 0x41368ce0 in
 * 1.44 (its mod.json and fw.s), so the key is 0x41367ce4 / 0x41368ce4 -- the
 * same word VAWT and OB-1 read as KEYVAR_AT. Reading it through the fw_*
 * export rather than a raw address is why this mod needs no port for 1.44:
 * the linker resolves fw_active_track per OS and the key is always 4 bytes on.
 * Values: 1 SYN1, 2 SYN2, 0/3/4/5 the other four page keys (TRIG/FLTR/AMP/LFO),
 * so only 1 and 2 paint. The address goes through an integer so the compiler
 * does not read it as indexing past fw_active_track. */
#define PH_KEY (*(volatile int32_t *)((uint32_t)&fw_active_track + 4u))

/* Brain/screen chain, as core_page_shown uses it: the key dispatcher's brain
 * keeps the screens shown at +0x54 (bottom) and +0x5c (top); the bottom
 * screen's page manager is at +8 and its shown view at +0xc0; a view's place
 * in its list is at +144. */
#define BRAIN_BOTTOM 0x54
#define BRAIN_TOP    0x5c
#define SCREEN_MGR   8
#define MGR_VIEW     0xc0
#define VIEW_CUR     144

/* Content area and grid (panel coords, y = 0 top). The clear rect starts 3 px
 * left of the grid: the stock SYN1 diagram animates with the ALGO value and its
 * squares reach x16-17, while the rail provably ends at x14, so clearing from
 * 15 takes the bleed without touching rail chrome. Cells stay at CX0. */
#define CX0 18
#define CXC 15
#define CX1 127
#define CY0 12
#define CY1 63
#define COLS 4
#define ROWS 2

#define YB(yt) (63 - (yt))      /* panel y -> bitmap y (0 at the bottom) */

/* Knob index -> sound slot, and each slot's stock 8.8 range, read from the
 * 1.43/1.44 parameter records (docs/evidence.md): SYN1 A-H are slots 17-24
 * (ALGO 0-7, RTOC 0-18, RTOA 0-35, RTOB 0-2.8125, HARM 37-89, DTUN/FDBK/MIX
 * 0-127) and SYN2 A-H are slots 27-34 (0-127). Used to draw the mini bars at
 * the true scale; the values themselves are the sound's own. */
static const uint8_t ph_slot[DNM_KNOBS] = {
    17, 18, 19, 20, 21, 22, 23, 24, 27, 28, 29, 30, 31, 32, 33, 34,
};
static const int32_t ph_rmin[DNM_KNOBS] = {
    0, 0, 0, 0, 37 * 256, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int32_t ph_rmax[DNM_KNOBS] = {
    7 * 256, 18 * 256, 35 * 256, 720, 89 * 256, 127 * 256, 127 * 256,
    127 * 256, 127 * 256, 127 * 256, 127 * 256, 127 * 256, 127 * 256,
    127 * 256, 127 * 256, 127 * 256,
};

static uint32_t ph_enc_at;
static int32_t ph_enc = -1;             /* the last-turned encoder's cell 0-7 */
static int32_t ph_enc_page = -1;        /* and the page it was turned on */
static void *ph_brain;

/* The firmware draws its messages and popups - "COPY PATTERN", "SELECT BANK",
 * the SETTINGS menu and their like - in this same view: no brain screen appears
 * above it, so page_shown() cannot see them and the page would paint its grid
 * over the message. The chords that draw them are watched here and the page
 * yields for a moment:
 *
 *   - PTN, BANK, and SETTINGS with FUNC, in any mode: they open a menu over
 *     the page.
 *   - RECORD, PLAY or STOP with FUNC, a held step key or a held track key,
 *     while the unit is recording: in grid recording that press starts the
 *     transport or acts on the track, and the firmware says so in a message.
 *     Out of record mode the same chords are plain transport (FUNC+PLAY is
 *     just PLAY) and draw nothing, so the page stays.
 *   - bare RECORD+PLAY and RECORD+STOP are record mode's own entry (RECORD
 *     toggles grid recording, and is its modifier for live and step) and show
 *     nothing either.
 *
 * Record mode is tracked from those chords, because the popups above exist
 * only inside it and firmware RAM does not offer it: RECORD toggles grid
 * recording, RECORD+PLAY enters live and RECORD+STOP step, and PLAY or STOP
 * leaves live and step (the model VAWT's overlay and the template's lock
 * display use). FUNC comes in the key event's flags; every other key has to be
 * tracked. */
static uint32_t ph_ptn_down;
static uint32_t ph_bank_down;
static uint32_t ph_global_down;
static uint32_t ph_page_down;
static uint32_t ph_rec_down;
static uint32_t ph_trigs;               /* held step keys, one bit each */
static uint32_t ph_trks;                /* held track keys, one bit each */
static uint32_t ph_rec;                 /* 0 off, 1 grid, 2 live, 3 step */
static uint32_t ph_msg_until;           /* page off until this tick */
#define PH_MSG_HOLD  90u                /* ~3 s at the 30/s tick */
#define PH_EVK_PTN    4
#define PH_EVK_BANK   5
#define PH_EVK_GLOBAL 7                 /* the SETTINGS key */
#define PH_EVK_REC   10
#define PH_EVK_PLAY  11
#define PH_EVK_STOP  12
#define PH_EVK_PAGE  19
#define PH_EVK_TRIG0 26                 /* 26-41: the 16 step keys */
#define PH_EVK_TRIG1 41
#define PH_EVK_TRK0  42                 /* 42-45: the four track keys */
#define PH_EVK_TRK1  45
#define PH_EVF_FUNC    0x02
#define PH_EVF_RELEASE 0x10

static int32_t msg_up(void)
{
    return (int32_t)(phreakoid_ticks - ph_msg_until) < 0;
}

/* Knobs that step through a fixed list and have nothing to be a fraction of:
 * OCT, PRIO and WAVE (whose value text names the step). Their cells keep the
 * label and the value and skip the bar entirely -- frame included, or the empty
 * row would read as a bar at zero. */
static const uint8_t ph_nobar[DNM_KNOBS] = {
    1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
};

/* One cell's mini bar, from the knob's own stock range: left to right
 * everywhere, from the middle for TUNE (HARM is centred on 63 = 0). */
static void ph_bar(void *bmp, int32_t k, int32_t x, int32_t ytop, int32_t w,
                   int32_t value)
{
    int32_t yb = YB(ytop + 19), span = ph_rmax[k] - ph_rmin[k];
    int32_t v = value - ph_rmin[k];
    int32_t fill, f0, f1;
    if (v < 0)
        v = 0;
    if (v > span)
        v = span;
    fw_framerect(bmp, x, yb - 4, x + w, yb, 1);
    if (k != 4) {
        fill = (w * v) / span;
        if (fill > 0)
            fw_fillrect(bmp, x + 1, yb - 3, x + fill, yb - 1, 1);
        return;
    }
    fill = (w * v) / span;
    f0 = x + 1 + w / 2;
    f1 = f0 + fill - w / 2;
    if (f1 < f0) {
        int32_t t = f0;
        f0 = f1;
        f1 = t;
    }
    if (f1 > f0)
        fw_fillrect(bmp, f0, yb - 3, f1, yb - 1, 1);
    fw_fillrect(bmp, x + w / 2, yb - 3, x + 1 + w / 2, yb - 1, 1);
}

/* One cell: the label, the true value text under it, the mini bar, and the
 * highlight frame on the last-turned encoder's cell. */
static void ph_cell(void *bmp, int32_t k, int32_t x, int32_t ytop, int32_t w,
                    int32_t value, int32_t sel)
{
    char buf[16];
    const char *name = phreakoid_knobs[k].name;
    if (!name || !name[0])
        return;
    fw_textf(bmp, fw_font5, x + 1, YB(ytop) - 7, -1, "%s", name);
    if (phreakoid_knobs[k].fmt) {
        phreakoid_knobs[k].fmt(value, buf);
        fw_textf(bmp, fw_font5, x + 1, YB(ytop) - 14, -1, "%s", buf);
    }
    if (!ph_nobar[k])
        ph_bar(bmp, k, x + 1, ytop, w - 2, value);
    /* Last, and short of the mini-bar zone: the outline covers label + value
     * only (panel ytop..ytop+17; bars start at +19). */
    if (sel)
        fw_framerect(bmp, x, YB(ytop + 17), x + w, YB(ytop), 1);
}

/* A screen over the pages (a menu, a browser, a modal): the brain's top and
 * bottom screens differ. Unknown counts as over - without a brain there is no
 * paint either way, and the tick's key sweep must not trust the bits. Those
 * screens can also eat a key's release, which is why the sweep exists. */
static int32_t screen_over(void)
{
    void *bottom, *top;
    if (!ph_brain)
        return 1;
    bottom = *(void *volatile *)((uint8_t *)ph_brain + BRAIN_BOTTOM);
    top = *(void *volatile *)((uint8_t *)ph_brain + BRAIN_TOP);
    return !bottom || bottom != top;
}

/* The gate the paint uses: the active track's sound plays PHREAKOID, the
 * showing key is SYN1/SYN2, no screen sits over the pages, and the shown view
 * is on its first page. */
static int32_t page_shown(void)
{
    void *bottom, *mgr, *view;
    uint8_t *kit;
    int32_t track, key;
    if (screen_over())
        return 0;
    bottom = *(void *volatile *)((uint8_t *)ph_brain + BRAIN_BOTTOM);
    track = fw_active_track;
    if (track < 0 || track > 3)
        return 0;
    kit = fw_kit;
    if (!kit || (FW_SOUND_SLOT(kit, track, 0) >> 8) != MY_ID)
        return 0;
    key = PH_KEY;
    if (key != 1 && key != 2)
        return 0;
    mgr = *(void *volatile *)((uint8_t *)bottom + SCREEN_MGR);
    if (!mgr)
        return 0;
    view = *(void *volatile *)((uint8_t *)mgr + MGR_VIEW);
    if (!view)
        return 0;
    return *(volatile int32_t *)((uint8_t *)view + VIEW_CUR) == 0;
}

/* The page's eight shown values: the active track's sound, by slot. */
static void shown_values(uint8_t *kit, int32_t track, int32_t page,
                         int32_t *vals)
{
    int32_t base = (page - 1) * 8, i;
    for (i = 0; i < 8; i++)
        vals[i] = FW_SOUND_SLOT(kit, track, ph_slot[base + i]);
}

void phreakoid_draw(void *bmp, void *ctrl)
{
    int32_t track = fw_active_track, key, page, colw, rowh, r, c, vals[8];
    uint8_t *kit;
    (void)ctrl;
    /* A firmware message (copy/paste/clear) keeps the frame while it is up. */
    if (msg_up() || !page_shown())
        return;
    kit = fw_kit;
    key = PH_KEY;
    page = key;
    fw_fillrect(bmp, CXC, YB(CY1), CX1, YB(CY0), 0);
    shown_values(kit, track, page, vals);
    colw = (CX1 - CX0 + 1) / COLS;
    rowh = (CY1 - CY0 + 1) / ROWS;
    for (r = 0; r < ROWS; r++) {
        for (c = 0; c < COLS; c++) {
            int32_t i = r * COLS + c;
            int32_t k = (page - 1) * 8 + i;
            int32_t x = CX0 + c * colw;
            int32_t yt = CY0 + r * rowh;
            int32_t sel = i == ph_enc && ph_enc_page == page &&
                (uint32_t)(phreakoid_ticks - ph_enc_at) < 45u;
            ph_cell(bmp, k, x, yt, colw - 2, vals[i], sel);
        }
    }
}

int phreakoid_key_handler(void *brain, void *ev)
{
    /* Remember the key dispatcher's brain for the draw-time screen check, track
     * the keys record mode and the popup chords need, and yield the page when
     * the firmware is about to draw over it (see the state's comment above).
     * Never takes the event. */
    int32_t id, flags, press, func;
    if (brain)
        ph_brain = brain;
    if (!ev)
        return 0;
    id = *(volatile int32_t *)((const uint8_t *)ev + 12);
    flags = *(volatile int32_t *)((const uint8_t *)ev + 16);
    press = !(flags & PH_EVF_RELEASE);
    func = flags & PH_EVF_FUNC;
    if (id == PH_EVK_PTN) {
        ph_ptn_down = (uint32_t)press;
    } else if (id == PH_EVK_BANK) {
        ph_bank_down = (uint32_t)press;
    } else if (id == PH_EVK_GLOBAL) {
        ph_global_down = (uint32_t)press;
    } else if (id == PH_EVK_PAGE) {
        ph_page_down = (uint32_t)press;
    } else if (id >= PH_EVK_TRIG0 && id <= PH_EVK_TRIG1) {
        if (press)
            ph_trigs |= 1u << (uint32_t)(id - PH_EVK_TRIG0);
        else
            ph_trigs &= ~(1u << (uint32_t)(id - PH_EVK_TRIG0));
    } else if (id >= 20 && id <= 25) {
        ph_trigs = 0;                   /* a page key drops any held step key */
    } else if (id >= PH_EVK_TRK0 && id <= PH_EVK_TRK1) {
        /* A track press can open the Mod Menu (hold) or switch track, and that
         * screen eats the other keys' releases: hold nothing across it, then
         * keep this key's own state (it is a modifier too). */
        ph_ptn_down = ph_bank_down = ph_global_down = ph_page_down = 0;
        ph_trigs = ph_rec_down = 0;
        if (press)
            ph_trks |= 1u << (uint32_t)(id - PH_EVK_TRK0);
        else
            ph_trks &= ~(1u << (uint32_t)(id - PH_EVK_TRK0));
    }
    if (id == PH_EVK_REC)
        ph_rec_down = (uint32_t)press;

    /* Record mode. A chorded RECORD is a pattern action, not the toggle, so it
     * leaves the mode alone; so does a chorded PLAY or STOP. */
    if (id == PH_EVK_REC && press) {
        if (!(func || ph_ptn_down || ph_bank_down || ph_global_down ||
              ph_page_down || ph_trigs || ph_trks))
            ph_rec = ph_rec ? 0 : 1;
    } else if ((id == PH_EVK_PLAY || id == PH_EVK_STOP) && press) {
        if (ph_rec_down)
            ph_rec = (id == PH_EVK_PLAY) ? 2 : 3;   /* RECORD held: live, step */
        else if (ph_rec > 1 && !(func || ph_trigs || ph_trks))
            ph_rec = 0;                             /* PLAY/STOP end live/step */
    }

    /* The chords the firmware answers with a popup or a message (the state's
     * comment above lists them). */
    if (press && (id == PH_EVK_PTN || id == PH_EVK_BANK ||
                  (id == PH_EVK_GLOBAL && func) ||
                  ((id == PH_EVK_REC || id == PH_EVK_PLAY || id == PH_EVK_STOP) &&
                   ph_rec && (func || ph_trigs || ph_trks))))
        ph_msg_until = phreakoid_ticks + PH_MSG_HOLD;
    return 0;
}

int phreakoid_enc_handler(void *brain, void *ev)
{
    int32_t enc, key;
    if (brain)
        ph_brain = brain;
    if (!ev)
        return 0;
    enc = *(volatile int32_t *)((const uint8_t *)ev + 12);
    if (enc < 1 || enc > 8)
        return 0;
    /* Page from the showing key itself, not from the last painted frame: a
     * turn can land while another screen is up, and the cached page would then
     * be stale. */
    key = PH_KEY;
    if (key != 1 && key != 2)
        return 0;
    ph_enc = enc - 1;
    ph_enc_page = key;
    ph_enc_at = phreakoid_ticks;
    return 0;
}

/* Change signature for the tick: re-arm the frame only when something the
 * overlay shows actually changed. Forcing a recompose every tick keeps the UI
 * task redrawing 30 times a second, which starves key/encoder handling on
 * hardware (turns feel buffered, values land after release). */
static int32_t tick_valid;
static int32_t msg_dirtied;             /* the frame was re-composed once for
                                           the message now up */
static int32_t tick_track = -1;
static int32_t tick_key = -1;
static int32_t tick_vals[8];
static int32_t tick_enc = -1;
static int32_t tick_enc_page = -1;
static int32_t tick_hl;

void phreakoid_tick(void *ctrl)
{
    int32_t track, mid, key, page, i, hl, dirty = 0, vals[8];
    uint8_t *kit;
    phreakoid_ticks++;              /* the highlight clock runs always */
    phreakoid_reap();               /* and the fuse ages out */
    if (!ctrl)
        return;
    /* A screen over the pages can eat a key's release, which would stick a
     * modifier or step bit; drop them there. Cheap, and it must run even when
     * the page is not up. */
    if (screen_over())
        ph_ptn_down = ph_bank_down = ph_global_down = ph_page_down =
            ph_rec_down = ph_trigs = ph_trks = 0;   /* ph_rec is the mode, and
                                                       survives a screen */
    track = fw_active_track;
    if (track < 0 || track > 3)
        return;
    kit = fw_kit;
    if (!kit)
        return;
    mid = FW_SOUND_SLOT(kit, track, 0) >> 8;
    if (mid != MY_ID) {
        tick_valid = 0;
        return;
    }
    key = PH_KEY;
    if (key != 1 && key != 2) {
        tick_valid = 0;
        return;
    }
    /* While a message or popup is up the page stays off: do not re-arm, so the
     * firmware keeps the frame. Handing it back once (the same dirty flag
     * re-arms the page) gives it the chance to draw the page under the message
     * -- measured on the emulator it still composes only the message's own box,
     * so the grid drawn a moment before the chord can stay around the message
     * until the page next repaints. The expiry dirties again (tick_valid == 0)
     * and the page comes back on the next change. */
    if (msg_up()) {
        if (!msg_dirtied) {
            msg_dirtied = 1;
            *((volatile uint8_t *)ctrl + 0x20) = 1;
        }
        tick_valid = 0;
        return;
    }
    msg_dirtied = 0;
    page = key;
    shown_values(kit, track, page, vals);
    hl = ph_enc >= 0 && ph_enc_page == page &&
        (uint32_t)(phreakoid_ticks - ph_enc_at) < 45u;
    if (!tick_valid || track != tick_track || key != tick_key ||
        ph_enc != tick_enc || ph_enc_page != tick_enc_page || hl != tick_hl)
        dirty = 1;
    else {
        for (i = 0; i < 8; i++) {
            if (vals[i] != tick_vals[i]) {
                dirty = 1;
                break;
            }
        }
    }
    if (!dirty)
        return;
    /* Something changed (or the first sighting): recompose once so the overlay
     * paints it. Without this the firmware only redraws on its own dirty flag,
     * so the overlay appears only after another SYN press. */
    tick_valid = 1;
    tick_track = track;
    tick_key = key;
    for (i = 0; i < 8; i++)
        tick_vals[i] = vals[i];
    tick_enc = ph_enc;
    tick_enc_page = ph_enc_page;
    tick_hl = hl;
    *((volatile uint8_t *)ctrl + 0x20) = 1;
}
