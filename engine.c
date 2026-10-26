/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PHREAKOID - a monophonic acid-bass machine for the Digitone mk1.
 *
 * An integer port of the TB-303 voice in Robin Schmidt's Open303 (MIT, see
 * README.md and LICENSE). One shared voice core: a band-limited saw<->square
 * oscillator into the 303's four-pole ladder filter with its feedback
 * highpass, a resonant exponential filter envelope, accent by velocity (the
 * circuit's own sweep: a 68 ms accent envelope, a direct path and the sweep
 * capacitor, gated above 48 % of ACCNT), glide and a fixed amp envelope. It
 * plays through the Digitone's multimode filter, mixer and effects.
 *
 * Everything is fixed point and runs at 1x (48 kHz). Open303 oversamples its
 * filter and oscillator 4x and runs a 12th-order elliptic anti-alias filter at
 * that rate; at 1x that filter only removes content above 24 kHz, so it is
 * dropped and aliasing is left to the oscillator's PolyBLEP. The filter
 * coefficients are interpolated once per block from filt_coef.h (see
 * tools/build-tables.py), which turns Open303's per-sample transcendentals
 * into table lookups. An amp envelope means DNM_OWN_ENV, so the AMP page does
 * not shape the sound (see README.md).
 *
 * Knobs are the stock SYN parameters (see mod.json / README), so p-locks,
 * LFOs, MIDI CC and presets keep working.
 */
#include "digitone-mk1/machines.h"
#include "filt_coef.h"

#define MY_ID 100               /* must match mod.json: "dnmachine:100" */

/* Output scale: the audio path is Q13 and DNM_PEAK is 1.0, so a raw 1.0
 * (8192) lands on DNM_PEAK at a shift of 27-13 = 14. With the resonance
 * compensation below, the default patch peaks near 0.5 * DNM_PEAK at a shift
 * of 12, leaving headroom for accents. */
#ifndef PH_OUT_SHIFT
#define PH_OUT_SHIFT 12
#endif

/* The output overdrive's soft knee (DRIVE): linear to K, then a quadratic that
 * flattens out to exactly 32767. It is what keeps a hot signal -- the resonant
 * peaks at the top of CUTOF and RESO above all -- off the rail without hard-
 * clipping it, and DRIVE gains into it. The knee sits just under the taps'
 * peaks (their own caps are 78 % of DNM_PEAK), where it shaves about a third
 * of a dB off the peak and nothing off the level.
 *
 * DRIVE's span is 36 dB (ph_drive) and most of it is in the top of the knob,
 * so the stage goes from the rounding it always did -- 20 dB, and the reported
 * complaint was that even that was not a distortion -- to a full square: at
 * the top a hot patch is driven 4x past the knee and the note's own decay is
 * inside it too. The make-up (ph_dgain) turns half the gain back into level
 * until the plateau it leaves is the patch's own peak, and holds there, so the
 * top of the knob buys harmonics rather than level -- the way a distortion
 * pedal's output is set by its clipper and not by its gain.
 *
 * At the bottom of DRIVE the stage's gain and make-up are both exactly unity,
 * so the knob is a bypass there and the knee is all that is left of it -- and
 * what is left is not the overdrive but the output's own limiter. It used to
 * be the same quadratic curve with its onset moved up to 90 % of full scale,
 * which turned out to be no limiter at all: the quadratic reaches the rail and
 * then stops, so anything past 1.1 x full scale -- and the envelope at the top
 * of ENVMD reaches 3.9 x -- came out flat-topped, 16:1, which is the digital
 * clipping this was meant to prevent. It is the overdrive's curve down low and
 * a rational knee up high instead: a = K + d*W/(d + W), whose slope is 1 at K
 * and which approaches the rail without reaching or kinking at it, so a hot
 * peak bends round its corner rather than being squared off. Onset 80 % of
 * full scale, above the tap modes' own caps (78 %), so the 303 voice and every
 * other mode is still bit-identical wherever it did not clamp before. */
#define PH_OD_K      18022                  /* 55 % of 32767 */
#define PH_OD_DMAX   (2 * (32767 - PH_OD_K))/* the knee's width to the rail */
#define PH_OD_Q      (4 * (32767 - PH_OD_K))
#define PH_RAIL_K    26214                  /* 80 %: DRIVE's own off position */
#define PH_RAIL_DMAX (32767 - PH_RAIL_K)    /* the rational knee's width */
/* The 303 path's own output gain, Q8 (256 = 1). Open303's 303 ladder returns
 * `2*g*y4` and every other mode returns `8*(c0*y0 + ... + c4*y4)`; this is the
 * one static factor between the two, measured so the 303 voice peaks where the
 * tap modes do (see ph_mgain). It is *not* a resonance trim: it does not move
 * with CUTOF, RESO, ENVMD or the accent. */
#define PH_VG303     240
/* The voice's master trim, Q8 (60 = 0.234, -12.6 dB). The filter and the amp
 * are calibrated against Open303's own scales, which do not fit this machine's
 * headroom: the reference's output stage is a -12 dB scaler and then whatever
 * the host does, so a full-resonance accented note is *meant* to sit well over
 * 0 dBFS there -- in a DAW it clips, and nobody minds, because it is a 303.
 * Here the -12 dB is already inside the audio unit (AUDIO against DNM_PEAK)
 * and there is nothing after the voice, so the same note ran to 3.3x the rail
 * and the rail's knee was left compressing it -- which is the "digital peak
 * distortion on higher octaves". The measured worst case over all 16 modes and
 * the whole grid (CUTOF x RESO x ENVMD x ACCNT x note; the probe and the table
 * are in docs/evidence.md) is 330 % of the rail, so this trim puts the loudest
 * setting at 77 %, just under the knee's 80 % onset -- and the knee then never
 * engages anywhere: 0 % of samples, 0.00 % THD at the hottest settings.
 * Everything below that is untouched, and because it is one static gain it
 * changes no mode's or resonance's balance. If the mod wants to be hotter
 * overall, this is the one number to raise -- the knee is then what limits,
 * and the harmonics it adds are what the complaint was about. */
#define PH_VGAIN     60

/* ---- fixed-point scales ------------------------------------------------- */
#define AUDIO   8192            /* Q13: the audio path's 1.0 = 8192 */
#define FENV    (1 << 21)       /* filter envelope, Q21 */
#define AENV    (1 << 24)       /* amp envelope, Q24 */
#define PH_SNAP_G 2048          /* a snapped pitch settles over ~0.7 ms */
/* Where the feedback fades out for 1x operation, in cut_l2 (256 per octave):
 * 8 kHz to 12 kHz. See the ladder. */
#define PH_TPR_LO 3319
#define PH_TPR_HI 3469
/* The highest cutoff a note may ask for, in cut_l2: 5.5 kHz. Two things
 * decide it, both measured (out/cap.py, out/ladder.py, docs/evidence.md).
 *
 * 1. The taper below is *continuous* in the cutoff but steep -- 0.59 octave
 *    from full feedback to none -- and the envelope's sweep crosses it in
 *    about 3 ms at the top of CUTOF and ENVMD. A ladder whose k falls from
 *    full to zero in 3 ms is a click, and that is what "plosives on the
 *    accent and envmod" was: on the reported pattern the taper's tearing put
 *    146 windows of the run over six times the note's own 6-12 kHz share (80
 *    of them over twelve); with the sweep held below the taper, none do.
 *    ENVMOD 100 reaches 8 kHz at CUTOF 72 and the table's 20 kHz ceiling at
 *    CUTOF 112, so this was not an extreme corner: it was half the knob.
 * 2. A *pinned* resonant cutoff turns the note into fizz on its own at this
 *    sample rate: the share of a full-envmod note's energy above 6 kHz is
 *    1.6 % with the sweep held at 5.5 kHz, 14 % at 6 kHz and 53 % at 8 kHz,
 *    because the resonant peak then sits inside the band the ear reads as a
 *    whistle rather than a resonance.
 *
 * So the sweep tops out 0.8 octave above the knob's own 3.1 kHz, and the taper
 * stays below as the guard for anything that asks above this. Nothing under
 * CUTOF ~50 changes at all (ENVMOD 100 asks 5.2 kHz there); what the top of
 * the knob loses is the part of the sweep that was unresonant or fizzy. */
#define PH_L2_TOP 3181

/* ---- value text --------------------------------------------------------- */
static void put_int(char *buf, int32_t v)
{
    char t[8];
    int32_t n = 0;
    if (v < 0) {
        *buf++ = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    while (n)
        *buf++ = t[--n];
    *buf = 0;
}

static void put_pct(char *buf, int32_t p)
{
    if (p < 0)
        p = 0;
    if (p > 100)
        p = 100;
    put_int(buf, p);
    while (*buf)
        buf++;
    *buf++ = '%';
    *buf = 0;
}

/* "470ms", "1.3s" */
static void put_time(char *buf, int32_t ms)
{
    if (ms >= 1000) {
        put_int(buf, ms / 1000);
        while (*buf)
            buf++;
        *buf++ = '.';
        *buf++ = (char)('0' + (ms % 1000) / 100);
        *buf++ = 's';
        *buf = 0;
    } else {
        put_int(buf, ms);
        while (*buf)
            buf++;
        *buf++ = 'm';
        *buf++ = 's';
        *buf = 0;
    }
}

/* "450", "1.3k" */
static void put_hz(char *buf, int32_t hz)
{
    if (hz >= 1000) {
        put_int(buf, hz / 1000);
        while (*buf)
            buf++;
        *buf++ = '.';
        *buf++ = (char)('0' + (hz % 1000) / 100);
        *buf++ = 'k';
        *buf = 0;
    } else {
        put_int(buf, hz);
    }
}

static void t_oct(int32_t value, char *buf)
{
    int32_t o = (value >> 8) - 3;
    if (o > 0)
        *buf++ = '+';
    put_int(buf, o);
}

static void t_mode(int32_t value, char *buf)
{
    static const char *names[16] = {
        "FLAT", "LP6", "LP12", "LP18", "LP24", "HP6", "HP12", "HP18",
        "HP24", "BP1", "BP2", "BP3", "BP4", "BP5", "BP6", "303",
    };
    int32_t m = value >> 8;
    const char *s;
    int32_t i;
    if (m < 0)
        m = 0;
    if (m > 15)
        m = 15;
    s = names[m];
    for (i = 0; s[i]; i++)
        buf[i] = s[i];
    buf[i] = 0;
}

static void t_pw(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    r = r < 0 ? 0 : r > 35 ? 35 : r;
    put_pct(buf, 5 + r * 90 / 35);
}

static void t_prio(int32_t value, char *buf)
{
    static const char *names[3] = { "LOW", "LAST", "HIGH" };
    int32_t p = value >> 8;
    const char *s;
    int32_t i;
    if (p < 0)
        p = 0;
    if (p > 2)
        p = 2;
    s = names[p];
    for (i = 0; s[i]; i++)
        buf[i] = s[i];
    buf[i] = 0;
}

static void t_tune(int32_t value, char *buf)
{
    int32_t v = (value >> 8) - 63;      /* HARM's stock slot is 37..89, 63 = 0 */
    if (v > 0)
        *buf++ = '+';
    put_int(buf, v);
}

static void t_cut(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    r = r < 0 ? 0 : r > 127 ? 127 : r;
    put_hz(buf, ph_cut_hz[r]);
}

static void t_pct127(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    r = r < 0 ? 0 : r > 127 ? 127 : r;
    put_pct(buf, r * 100 / 127);
}

static void t_envmod(int32_t value, char *buf)
{
    int32_t r = value >> 8;                 /* MIX (SYN1 H) is 0..127 in the OS */
    r = r < 0 ? 0 : r > 127 ? 127 : r;
    put_pct(buf, r * 100 / 127);
}

static void t_wave(int32_t value, char *buf)
{
    /* a switch, not a fade: DETUNE's lower half is the saw, the upper the
     * pulse, so the text has to step at the same place the audio does */
    if ((value >> 8) < 64) {
        buf[0] = 'S'; buf[1] = 'A'; buf[2] = 'W'; buf[3] = 0;
    } else {
        buf[0] = 'S'; buf[1] = 'Q'; buf[2] = 'R'; buf[3] = 0;
    }
}

static void t_decay(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    r = r < 0 ? 0 : r > 127 ? 127 : r;
    put_time(buf, ph_decay_ms[r]);
}

static void t_slide(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    r = r < 0 ? 0 : r > 127 ? 127 : r;
    put_time(buf, r * 200 / 127);
}

static void t_attack(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    int32_t t;
    r = r < 0 ? 0 : r > 127 ? 127 : r;
    t = ph_attack_t[r];         /* tenths of a ms */
    put_int(buf, t / 10);
    while (*buf)
        buf++;
    *buf++ = '.';
    *buf++ = (char)('0' + t % 10);
    *buf++ = 'm';
    *buf++ = 's';
    *buf = 0;
}

static void t_drive(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    r = r < 0 ? 0 : r > 127 ? 127 : r;
    if (!r) {                       /* the bottom is a bypass, not 0 dB of it */
        buf[0] = 'O'; buf[1] = 'F'; buf[2] = 'F'; buf[3] = 0;
        return;
    }
    /* dB = 20*u + 16*u^2, the curve ph_drive carries (u = r/127) */
    put_int(buf, (20 * r * 127 + 16 * r * r) / (127 * 127));
    while (*buf)
        buf++;
    *buf++ = 'd';
    *buf++ = 'B';
    *buf = 0;
}

static void t_srr(int32_t value, char *buf)
{
    int32_t r = value >> 8;
    r = r < 0 ? 0 : r > 127 ? 127 : r;
    if (!ph_srr_hz[r]) {             /* the top is a bypass: no hold at all */
        buf[0] = 'O'; buf[1] = 'F'; buf[2] = 'F'; buf[3] = 0;
        return;
    }
    put_hz(buf, ph_srr_hz[r]);
}

/* ---- knobs -------------------------------------------------------------- */
/* SYN1 A-H then SYN2 A-H. Every value is a stock parameter's, in its stock
 * range, read from the 1.43/1.44 parameter records (see docs/evidence.md):
 * 107 SYN1 A ALGO 0-7, 108 B RTOC 0-18, 109 C RTOA 0-35, 110 D RTOB
 * 0-2.8125, 111 E HARM 37-89 (63 is 0), 112 F DTUN 0-127, 113 G FDBK 0-127,
 * 114 H MIX 0-127 (the SDK comment's -63..63 is stale); 119-126 SYN2 A-H are
 * 0-127. Labels are cut to five characters. */
const struct dnm_knob phreakoid_knobs[DNM_KNOBS] = {
    [0]  = { "OCT",   t_oct,    0 },
    [1]  = { "MODE",  t_mode,   0 },
    [2]  = { "PW",    t_pw,     0 },
    [3]  = { "PRIO",  t_prio,   0 },
    [4]  = { "TUNE",  t_tune,   0 },
    [5]  = { "CUTOF", t_cut,    0 },
    [6]  = { "RESO",  t_pct127, 0 },
    [7]  = { "ENVMD", t_envmod, 0 },
    [8]  = { "WAVE",  t_wave,   0 },
    [9]  = { "DECAY", t_decay,  0 },
    [10] = { "ACCNT", t_pct127, 0 },
    [11] = { "SLIDE", t_slide,  0 },
    [12] = { "ATACK", t_attack, 0 },
    [13] = { "ACDEC", t_decay,  0 },
    [14] = { "DRIVE", t_drive,  0 },
    [15] = { "SRR",   t_srr,    0 },
};

/* A stock 303 patch, 8.8 in each knob's stock range (see the knob map). */
static const int16_t my_defaults[DNM_KNOBS] = {
    768, 3840, 4608, 256, 16128, 22528, 16384, 8192,
    0, 19968, 24576, 9728, 16384, 13312, 0, 32512,
};

/* ---- monophony fuse ----------------------------------------------------- */
/* A machine is loaded per track, so there is one core per track (PH_TRACKS):
 * a second PHREAKOID on another track is a second voice of its own, with its
 * own notes and its own monophony, and neither can touch the other. Within a
 * track one core sounds and at most one of its voices renders, chosen by PRIO
 * (newest by default). A release frees its slot at once, so a sequencer's
 * trigs hand over without waiting; a freed slot resumes a held note, which
 * glides back. The UI tick backstops voices the companion stops sending. */
#define PH_TICK_REAP  120u
#define PH_TRACKS     4                 /* T1-T4: struct dnm_voice's track is 0-3 */
static uint8_t  ph_held[8];     /* the voice's gate is down */
static uint8_t  ph_pending[8];  /* a note held over one block: bit 0 set, and
                                 * bit 1 if it lands on a held note (slides) */
static uint32_t ph_birth[8];
static uint32_t ph_seen[8];
static int32_t  ph_note[8];
static uint8_t  ph_vtr[8];      /* the track each voice last rendered for */
static uint32_t ph_stale;       /* the tick aged these voices out, one bit
                                 * each; the render applies it (see
                                 * ph_stale_apply) */
static uint32_t ph_clock;
uint32_t phreakoid_ticks;

/* ---- the voice core, one instance per track ----------------------------- */
/* A track's machine is one voice: one core per track, addressed here through
 * `st`. The field names are aliased to it so the DSP body below reads as it
 * did when the core was global; what changed with the tracks is where the
 * state lives, not how it is computed. */
struct ph_st {
    uint32_t phase;                 /* oscillator, Q32 */
    uint32_t inc;                   /* slewed phase increment, Q32 */
    int32_t  y1, y2, y3, y4;        /* ladder, Q13 */
    int32_t  fbx, fby;              /* feedback highpass */
    int32_t  prex, prey;            /* pre highpass */
    int32_t  postx, posty;          /* post highpass */
    int32_t  dcy;                   /* amp de-clicker */
    int32_t  fenv;                  /* filter envelope, Q21 */
    int32_t  fenv_d;                /* its decay, num(20) */
    int32_t  rc1;                   /* filter-envelope attack RC, Q15 */
    int32_t  wenv, wcap;            /* accent sweep: its envelope (Q16), the cap (Q15) */
    int32_t  rc1c;                  /* attack RC coefficient, c(20) */
    int32_t  aenv;                  /* amp envelope, Q24 */
    int32_t  on;                    /* a note is held */
    int32_t  core;                  /* the core is sounding */
    int32_t  acc;                   /* accent strength this note, Q8 (0..256) */
    uint32_t srr_acc;               /* SRR hold phase, Q32 */
    int32_t  srr_hold;              /* the held output sample */
    /* ---- the handover ---- */
    /* The firmware's DSP runs every voice through its own multimode filter
     * (docs/ADAPTING.md, on the Digitone mk1: the main CPU's audio render
     * "runs each [voice] through its multimode filter, then mixes them"), so a
     * voice that stops carrying the core has its filter see a step to silence,
     * and the voice that takes the core up has its filter see a step to the
     * core's level. That step is the click a held-over note - a slide - made,
     * at any SLIDE and any filter setting, and the fuse alone cannot avoid it:
     * it hands the core over between blocks, and a block boundary is exactly
     * where a step is heard. Instead of cutting over, the two voices write the
     * same core samples for one block, one ramping out and the other in, so
     * each filter sees a ramp. The lower of the two - the one the companion
     * calls first - renders the block and stores it here; the other replays
     * it. */
    int32_t  owner;                 /* the voice carrying the core, -1: none */
    int32_t  powner;                /* the voice that carried it last block */
    uint32_t stamp, lastv;          /* the block: the companion renders a track's
                                     * voices in ascending order, so a call at or
                                     * below the last one opens a new block */
    uint32_t xfstamp;               /* the stamp the stored block belongs to */
    int32_t  blk[32];
};
static struct ph_st ph[PH_TRACKS];
static struct ph_st *st;            /* this call's track core */

#define ph_phase  (st->phase)
#define ph_inc    (st->inc)
#define ph_y1     (st->y1)
#define ph_y2     (st->y2)
#define ph_y3     (st->y3)
#define ph_y4     (st->y4)
#define ph_fbx    (st->fbx)
#define ph_fby    (st->fby)
#define ph_prex   (st->prex)
#define ph_prey   (st->prey)
#define ph_postx  (st->postx)
#define ph_posty  (st->posty)
#define ph_dcy    (st->dcy)
#define ph_fenv   (st->fenv)
#define ph_fenv_d (st->fenv_d)
#define ph_rc1    (st->rc1)
#define ph_rc1c   (st->rc1c)
#define ph_wenv   (st->wenv)
#define ph_wcap   (st->wcap)
#define ph_aenv   (st->aenv)
#define ph_on     (st->on)
#define ph_core   (st->core)
#define ph_acc    (st->acc)
#define ph_srr_acc  (st->srr_acc)
#define ph_srr_hold (st->srr_hold)
#define ph_owner  (st->owner)
#define ph_blk    (st->blk)

/* The fuse's backstop: a voice the companion stops sending is aged out of the
 * track it was rendering for, and a core it was carrying is released with it.
 * (The UI tick lives in overlay.c: it ages the fuse out and re-arms the
 * overlay's frame on change.)
 *
 * The tick runs on the UI task, the voice state belongs to the render's
 * interrupt, and the tick can be preempted in the middle of a handover. So the
 * tick only raises a flag here and the render applies it, in interrupt
 * context, at the top of its next call for the track (ph_stale_apply). The
 * flags are one word and both sides are read-modify-write, so a set racing a
 * clear can lose one: the voice is then not aged out now, and the tick raises
 * it again on its next pass ~33 ms later. */
void phreakoid_reap(void)
{
    int32_t w;
    for (w = 0; w < 8; w++) {
        if ((ph_held[w] || ph_pending[w]) &&
            (int32_t)(phreakoid_ticks - ph_seen[w]) > (int32_t)PH_TICK_REAP)
            ph_stale |= 1u << (uint32_t)w;
    }
}

/* Apply the tick's ageing, from the render (interrupt context). All eight
 * flags are applied on every call, not just this voice's: the voice that was
 * dropped is often one the companion never calls again, and the flag would
 * otherwise wait for a call that never comes. */
static void ph_stale_apply(void)
{
    int32_t w;
    if (!ph_stale)
        return;                         /* the common case: three cycles */
    for (w = 0; w < 8; w++) {
        struct ph_st *so;
        if (!(ph_stale & (1u << (uint32_t)w)))
            continue;
        ph_stale &= ~(1u << (uint32_t)w);
        ph_held[w] = 0;
        ph_pending[w] = 0;
        ph_birth[w] = 0;
        so = &ph[ph_vtr[w] & (PH_TRACKS - 1)];
        if (so->owner == w) {
            so->owner = -1;
            so->core = 0;
        }
    }
}

static inline __attribute__((always_inline)) int32_t clamp32(int32_t x,
                                                             int32_t lo,
                                                             int32_t hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

/* One ramp of a handover, Q15: d > 0 ramps in (1/32 .. 1), d < 0 out
 * (31/32 .. 0). The two ramps of a handover sum to 1, and the voice that
 * replays the block takes its share exactly (see the write-out). */
static inline int32_t ph_xg(int32_t d, int32_t i)
{
    return (d > 0) ? ((i + 1) << 10) : ((31 - i) << 10);
}

/* PolyBLEP residual for a falling edge at phase 0 (VAWT's, Q16 phase). */
static inline __attribute__((always_inline)) int32_t ph_blep(uint32_t t,
                                                             uint32_t dt)
{
    if (t < dt) {
        uint32_t u = (t << 16) / dt;
        return ((int32_t)(u << 1) - (int32_t)((u * u) >> 16) - 65536) >> 1;
    }
    if (t > (uint32_t)(0x10000u - dt)) {
        uint32_t q = ((0x10000u - t) << 16) / dt;
        return ((int32_t)((q * q) >> 16) - (int32_t)(q << 1) + 65536) >> 1;
    }
    return 0;
}

/* one-pole highpass: y = b0*(x - x1) + a1*y1 (Q15 coefficients, Q13 state) */
static inline __attribute__((always_inline)) int32_t hp13(int32_t in,
                                                           int32_t b0,
                                                           int32_t a1,
                                                           int32_t *x1,
                                                           int32_t *y1)
{
    int32_t y = ((b0 * (in - *x1)) >> 15) + ((a1 * *y1) >> 15);
    *x1 = in;
    *y1 = y;
    return y;
}

/* one-pole lowpass step: y += c*(x - y), c Q20, at least one unit of motion.
 * The product is formed on magnitudes: the bottom of ATTAK has c = 70347 and
 * |d| reaches 32768 (full-scale fenv15), so a signed 32-bit product overflows
 * and used to drive ph_rc1 negative, pinning the cutoff at 200 Hz under
 * ~0.33 ms. Every call site keeps |d|*c under 2^32: |d| <= 2^15 wherever c is
 * large, and ph_dcy's |d| reaches 2^16 only against c = 27097. */
static inline __attribute__((always_inline)) int32_t lp_step(int32_t y,
                                                              int32_t x,
                                                              int32_t c)
{
    int32_t d = (x - y);
    uint32_t m = (uint32_t)(d < 0 ? -d : d) * (uint32_t)c;
    int32_t s = (int32_t)(m >> 20);
    if (d < 0)
        s = -s;
    if (s == 0 && d)
        s = d > 0 ? 1 : -1;
    return y + s;
}

/* The same step, four samples' worth at once: `>> 18` against the same `c`
 * moves the state four times as far, so an envelope that used to be stepped
 * every sample keeps its rate when stepped every fourth. Run at 1/4 rate the
 * Euler error of the exponentials is (1-c)^4 vs 1-4c, i.e. under 0.3 % at the
 * fastest coefficient either envelope has (PH_AMP_REL_N's 84/4096); the
 * at-least-one-unit rule still holds, once per four samples. */
static inline __attribute__((always_inline)) int32_t lp_step4(int32_t y,
                                                               int32_t x,
                                                               int32_t c)
{
    int32_t d = (x - y);
    uint32_t m = (uint32_t)(d < 0 ? -d : d) * (uint32_t)c;
    int32_t s = (int32_t)(m >> 18);
    if (d < 0)
        s = -s;
    if (s == 0 && d)
        s = d > 0 ? 1 : -1;
    return y + s;
}

/* The pitch slew, exact to the phase unit: d*g/65536 built in two pieces, so
 * the low 16 bits of the error are not dropped before the multiply. The old
 * form stepped by (d >> 16) * g, which rounds a rising error down and leaves
 * a snapped or sliding note up to 65535 phase units flat for good (23 cents
 * at 55 Hz, 6 at 220); a falling one converged only because >> rounds toward
 * minus infinity. Both operands are Q16 increments, so |d| <= 65536 and
 * |d >> 16| * g <= 32768 * 65536 -- the two pieces fit int32. */
static inline __attribute__((always_inline)) int32_t ph_slew(int32_t d,
                                                             int32_t g)
{
    int32_t s = (d >> 16) * g +
                (int32_t)((((uint32_t)d & 0xFFFFu) * (uint32_t)g) >> 16);
    if (s == 0)
        s = (d > 0) ? 1 : -1;
    return s;
}

/* the ladder output-tap weights for the non-303 modes (TeeBeeFilter's c0..c4) */
static const int8_t ph_taps[15][5] = {
    { 1, 0, 0, 0, 0 }, { 0, 1, 0, 0, 0 }, { 0, 0, 1, 0, 0 }, { 0, 0, 0, 1, 0 },
    { 0, 0, 0, 0, 1 }, { 1, -1, 0, 0, 0 }, { 1, -2, 1, 0, 0 }, { 1, -3, 3, -1, 0 },
    { 1, -4, 6, -4, 1 }, { 0, 0, 1, -2, 1 }, { 0, 0, 0, 1, -1 }, { 0, 1, -3, 3, -1 },
    { 0, 0, 1, -1, 0 }, { 0, 1, -2, 1, 0 }, { 0, 1, -1, 0, 0 },
};

/* Static per-mode output gain, one entry per row above, applied as
 * `filt = (s * ph_mgain[mode]) >> 9` (512 = unity; the shift keeps the factors
 * inside int16). The taps are the same ladder read at different nodes, so
 * their raw levels differ by up to ~26 dB and each needs its own factor,
 * measured at the default patch against the 303. They are not equally peaky,
 * though: FLAT and the highpasses hand the saw's attack transient straight
 * through while the resonant 303 tap smooths it, so an exact RMS match would
 * send HP6/HP18's peaks far past the ladder's -- each entry carries the gain
 * its crest factor allows, which leaves the taps up to ~4.5 dB below the
 * ladder in RMS (index 15 is the TB_303 path and needs no entry).
 *
 * This table does not move with any knob, and it is the *only* per-mode level
 * term: RESO, ENVMD and the accent change a mode's loudness through the filter
 * itself, which is what the filter is for. The voice as a whole is then
 * trimmed by PH_VGAIN so that the loudest point in the box stays under the
 * output knee -- see there, and docs/evidence.md for the sweep. The spread
 * that remains between the modes at the top of RESO is each mode's own
 * peakiness, not a level error. `tools/phreakoid_host_test.py`'s
 * `modes-level` measures it at the default patch -- re-run it before touching
 * this table. */
static const int16_t ph_mgain[15] = {
    3350, 5189, 6290, 7069, 7551, 4405, 6321, 9140,
    13048, 25560, 21877, 25551, 15830, 16650, 10047,
};

/* ---- the system LFOs ------------------------------------------------------ */
/* The firmware's LFO engine writes each voice's two LFOs into fw_lfo_state --
 * the destination sound slot and a signed 8.8 modulation -- and then applies
 * them itself to a working copy of the sound, for FM. This machine bypasses
 * FM, and `dnm_knob()` reads the base value (p-locks included, LFO not), so the
 * modulation has to be added here instead. That is how the VAWT mod does it.
 * FILTER and AMP destinations are deliberately *not* applied: those act
 * downstream in the Digitone's own filter and mixer, and doing both would
 * double them. A knob's stock range (the clamp the firmware applies) comes from
 * the companion's `dnm_knob_range`, looked up once; only the slots are spelled
 * out, and they are the two SYN pages in knob order (docs/evidence.md). PRIO is
 * the one knob an LFO misses: it decides which voice takes a note and is read
 * before this, not a sound parameter. */
static const uint8_t ph_kslot[DNM_KNOBS] = {
    17, 18, 19, 20, 21, 22, 23, 24,   /* SYN1 A-H: ALGO RTOC RTOA RTOB HARM DTUN FDBK MIX */
    27, 28, 29, 30, 31, 32, 33, 34,   /* SYN2 A-H */
};
static int32_t ph_kmin[DNM_KNOBS], ph_kmax[DNM_KNOBS];
static int32_t ph_lfo_ready;

static void ph_lfo_prepare(void)
{
    int32_t k;
    if (ph_lfo_ready)
        return;
    for (k = 0; k < DNM_KNOBS; k++)
        dnm_knob_range(k, &ph_kmin[k], &ph_kmax[k]);
    ph_lfo_ready = 1;
}

/* the knob's value with both LFOs added and clamped, as the firmware would have
 * applied them; a destination of 0 is "none", which no knob's slot is */
static int32_t ph_lfo_add(int32_t k, int32_t val,
                          int32_t d0, int32_t m0, int32_t d1, int32_t m1)
{
    if (d0 == ph_kslot[k]) {
        val += m0;
        if (val < ph_kmin[k])
            val = ph_kmin[k];
        else if (val > ph_kmax[k])
            val = ph_kmax[k];
    }
    if (d1 == ph_kslot[k]) {
        val += m1;
        if (val < ph_kmin[k])
            val = ph_kmin[k];
        else if (val > ph_kmax[k])
            val = ph_kmax[k];
    }
    return val;
}

/* Open303's triggerNote resets the oscillator and clears the filter buffers
 * "only if amplitude is near zero (to avoid clicks)". ph_core is exactly that
 * condition - the amp envelope has run out - so a note that lands on a release
 * tail, or on a note still held, re-arms the envelopes but leaves the
 * oscillator, the ladders, the highpasses, the de-clicker and the envelope RCs
 * running. Zeroing them into a sounding core is a step in the output, and what
 * the de-clicker was zeroed against is a hard cut. The pitch belongs to the
 * pitch block below, which snaps or glides the increment. */
static void ph_trigger(void)
{
    if (!ph_core) {
        ph_phase = 0;
        ph_y1 = ph_y2 = ph_y3 = ph_y4 = 0;
        ph_fbx = ph_fby = 0;
        ph_prex = ph_prey = 0;
        ph_postx = ph_posty = 0;
        ph_dcy = 0;
        ph_rc1 = 0;
        ph_wenv = ph_wcap = 0;
        ph_srr_acc = 0;
        ph_srr_hold = 0;
    }
    ph_fenv = FENV;
    ph_aenv = AENV;
    ph_on = 1;
    ph_core = 1;
}

/* `block` is restrict: the companion hands in its own 32-sample window, which
 * is never the core's state. Without the qualifier a store to block[i]
 * (int32_t, the same type as the state's fields) may have overwritten
 * ph_y1..ph_y4 and the envelopes as far as the compiler knows, so it is free
 * to reload the voice from memory after every sample it writes. */
static void phreakoid_render(int32_t voice, int32_t *restrict block, const struct dnm_voice *v)
{
    int32_t kn[DNM_KNOBS];
    int32_t i, w, rank, prio, any_held, other_held, was_held, fresh, deferred, was_core;
    int32_t tr, yield, xfade, replay;
    uint32_t t, dt, pwth, srr_inc;
    int32_t oct, tune, cut_raw, reso, envmod, wave, mode, pw_raw;
    int32_t dg, wq, envsc, envof, cut_l2, idx, frac;
    int32_t odk, odw, odq;                  /* the knee in the path: DRIVE's, or the rail's */
    int32_t b0, kq, kgq, g8, c15, k8, rs8, ogain8, dgain8, stlim, mg8;
    int32_t ogd8, gf8;                      /* gains folded per block */
    int32_t ak8, rel;                       /* the accent lift, the release */
    int32_t aenv15 = 0;                     /* held between control ticks */
    int32_t accsweep;                       /* the accent's cutoff offset, 1/256 oct */
    int32_t drv;                            /* DRIVE's raw knob */
    int32_t triggered = 0;
    int32_t gtarget = 0, gslide = 0;

    voice &= 7;
    tr = v->track & (PH_TRACKS - 1);
    ph_stale_apply();
    st = &ph[tr];

    /* the block opens on the first call for this track: the companion renders
     * the voices in ascending order, so a call at or below the last one is a
     * new block. powner is the voice that carried the core through the last
     * block; it stays fixed for the whole block, so a voice that renders later
     * can still tell whether it is the one handing the core over. */
    if (voice <= (int32_t)st->lastv) {
        st->stamp++;
        st->powner = st->owner;
    }
    st->lastv = (uint32_t)voice;

    ph_seen[voice] = phreakoid_ticks;
    ph_note[voice] = v->pitch;
    was_held = (ph_vtr[voice] == (uint8_t)tr) ? ph_held[voice] : 0;
    was_core = ph_core;
    if (ph_vtr[voice] != (uint8_t)tr) {
        /* the voice left another track: it can no longer finish that track's
         * core, so the core is released rather than left to resume stale */
        struct ph_st *so = &ph[ph_vtr[voice] & (PH_TRACKS - 1)];
        if (so->owner == voice) {
            so->owner = -1;
            so->core = 0;
        }
        ph_pending[voice] = 0;
    }
    ph_vtr[voice] = (uint8_t)tr;

    /* register the note: held, and the newest */
    if (v->fresh) {
        ph_held[voice] = 1;
        ph_birth[voice] = ++ph_clock;
    } else if (!v->gate) {
        ph_held[voice] = 0;
    } else {
        ph_held[voice] = 1;
    }

    /* A voice the core cannot use this block - released, no fresh note, and
     * neither the voice carrying the core nor the one that carried it last
     * block - writes silence. That is the same result the yield path below
     * reaches, without the fuse bookkeeping, the knob pass or the pitch block:
     * after a few notes every allocated voice of a machine track holds the
     * machine's sound, so the companion calls this render for all eight every
     * block, and most of them have nothing to say. */
    if (!v->fresh && !ph_held[voice] && !ph_pending[voice] &&
        voice != st->owner && voice != st->powner) {
        for (i = 0; i < 32; i++)
            block[i] = 0;
        return;
    }

    /* who wins the one voice: PRIO low / last / high among the held voices
     * (ties to the newer). A voice that is not held only renders as the owner
     * finishing its release tail. */
    prio = clamp32(dnm_knob(voice, 3) >> 8, 0, 2);
    rank = 0;
    any_held = 0;
    other_held = 0;
    for (w = 0; w < 8; w++) {
        if (!ph_held[w] || ph_vtr[w] != (uint8_t)tr)
            continue;
        any_held = 1;
        if (w == voice)
            continue;
        other_held = 1;
        if (prio == 0)
            rank += (ph_note[w] < ph_note[voice] ||
                     (ph_note[w] == ph_note[voice] && (int32_t)(ph_birth[w] - ph_birth[voice]) > 0));
        else if (prio == 2)
            rank += (ph_note[w] > ph_note[voice] ||
                     (ph_note[w] == ph_note[voice] && (int32_t)(ph_birth[w] - ph_birth[voice]) > 0));
        else
            rank += (int32_t)(ph_birth[w] - ph_birth[voice]) > 0;
    }

    /* A note that lands while the core is still sounding, and below it a voice
     * that can render this block (held, or the owner finishing its tail), must
     * write nothing: the companion renders the voices in ascending order, so
     * that voice has rendered already - it cannot see this note - and two
     * voices rendering the one core is two consecutive 32-sample stretches
     * summed, which is the click. The note starts on the next block instead,
     * which is also where Open303's slideToNote gets its block. A note whose
     * own voice is the first that would render takes the block now: the later
     * voices see it held and newer, and yield. */
    deferred = !v->fresh && (ph_pending[voice] & 1);
    if (deferred && !v->gate) {             /* the note ended before its block */
        ph_pending[voice] = 0;
        deferred = 0;
    }
    if (v->fresh) {
        int32_t lower_live = 0;
        int32_t leg = was_held || other_held;
        ph_pending[voice] = 0;
        for (w = 0; w < voice; w++) {
            if (ph_vtr[w] == (uint8_t)tr && (ph_held[w] || w == ph_owner))
                lower_live = 1;
        }
        if (ph_core && lower_live) {
            ph_pending[voice] = (uint8_t)(1 | (leg ? 2 : 0));
            for (i = 0; i < 32; i++)
                block[i] = 0;
            return;
        }
    } else if (deferred) {
        ph_held[voice] = 1;                 /* the held-over note joins now */
    }
    fresh = v->fresh || deferred;

    /* who carries the core this block */
    yield = rank > 0 || (!ph_held[voice] && (any_held || voice != ph_owner));

    /* A handover: the core passes from powner to this voice, or away from this
     * voice (struct ph_st). Neither participant cuts; the lower of the two is
     * the one the companion calls first, so it renders the block, and the
     * other replays its samples, each ramping its own way. */
    xfade = 0;
    replay = 0;
    if (ph_core && st->powner >= 0) {
        if (yield) {
            if (voice == st->powner) {              /* handing the core over */
                if (st->owner != st->powner) {      /* a voice below took it */
                    if (st->xfstamp == st->stamp) {
                        xfade = -1;
                        replay = 1;
                    }
                } else {
                    xfade = -1;                     /* the taker is above: render it */
                }
            }
        } else if (st->powner != voice) {           /* taking the core over */
            xfade = 1;
            if (st->powner < voice && st->xfstamp == st->stamp)
                replay = 1;                         /* the voice below rendered it */
        }
    }

    if (yield && xfade == 0) {                      /* the core is not this voice's */
        ph_pending[voice] = 0;                      /* a newer note took the voice */
        for (i = 0; i < 32; i++)
            block[i] = 0;
        return;
    }
    if (yield) {
        ph_pending[voice] = 0;
        if (replay) {                               /* the taker below rendered it */
            for (i = 0; i < 32; i++) {
                int32_t gr = 32768 - ph_xg(xfade, i);
                int32_t o = ph_blk[i] - ((ph_blk[i] * gr) >> 15);
                block[i] = clamp32(o << PH_OUT_SHIFT, -(1 << 27), 1 << 27);
            }
            return;
        }
        /* render the core one last time, ramping out (the loop below) */
    } else {
        ph_owner = voice;                           /* this voice carries it on */
    }

    /* A core that has run out (its amp envelope reached zero) with no fresh
     * note has nothing left to render: the knob/LFO pass, the pitch block and
     * the coefficients would all be thrown away by the `!ph_core` return
     * further down. One voice per silent track lands here every block - the
     * one that carried the core last - so this keeps a silent machine track
     * near free. ph_inc is not refreshed here, which is safe: nothing reads it
     * until the next trigger, and a trigger into a silent core snaps it
     * (!was_core) anyway. */
    if (!ph_core && !fresh) {
        for (i = 0; i < 32; i++)
            block[i] = 0;
        return;
    }

    /* knobs, once per block: the base value (p-locks included) plus both
     * system LFOs where their destination is this knob's slot */
    ph_lfo_prepare();
    {
        int32_t d0 = FW_LFO(voice, 0)[FW_LFO_DEST];
        int32_t m0 = FW_LFO(voice, 0)[FW_LFO_MOD];
        int32_t d1 = FW_LFO(voice, 1)[FW_LFO_DEST];
        int32_t m1 = FW_LFO(voice, 1)[FW_LFO_MOD];
        for (i = 0; i < DNM_KNOBS; i++)
            kn[i] = ph_lfo_add(i, dnm_knob(voice, i), d0, m0, d1, m1);
    }

    oct = (kn[0] >> 8) - 3;
    mode = clamp32(kn[1] >> 8, 0, 15);
    pw_raw = clamp32(kn[2] >> 8, 0, 35);
    tune = (kn[4] >> 8) - 63;               /* HARM: 63 is 0, 37..89 raw */
    cut_raw = clamp32(kn[5] >> 8, 0, 127);
    reso = clamp32(kn[6] >> 8, 0, 127);
    envmod = clamp32(kn[7] >> 8, 0, 127);   /* MIX: 0..127 raw */
    wave = clamp32(kn[8] >> 8, 0, 127);
    srr_inc = (uint32_t)ph_srr[clamp32(kn[15] >> 8, 0, 127)];
    drv = clamp32(kn[14] >> 8, 0, 127);
    /* Where the master trim goes. Both tables of DRIVE were calibrated against
     * the voice's untrimmed level, so the overdrive has to keep seeing it: its
     * gain goes in whole and the trim is taken out of its make-up instead. With
     * DRIVE off there is no stage to protect, and the trim goes in as the gain
     * -- ahead of the rail's knee, so that knee stays out of the path at every
     * setting. Put the trim in front of the overdrive and its knee would stop
     * reaching its own threshold; put it after the rail's knee and the peak
     * would still be bent before it was turned down. */
    odq = PH_OD_Q;                          /* only the overdrive's knee uses it */
    if (drv > 0) {
        dg = ph_drive[drv];
        dgain8 = (ph_dgain[drv] * PH_VGAIN) >> 8;   /* DRIVE make-up, Q8 */
        odk = PH_OD_K;
        odw = PH_OD_DMAX;
    } else {
        dg = PH_VGAIN;
        dgain8 = 256;                       /* unity */
        odk = PH_RAIL_K;                    /* the output's own limiter */
        odw = PH_RAIL_DMAX;                 /* (rational: W, not a width) */
    }

    if (fresh) {
        int32_t accent_i = clamp32(kn[10] >> 8, 0, 127);
        int32_t legato = deferred ? ((ph_pending[voice] >> 1) & 1)
                                  : (was_held || other_held);
        if (deferred)
            ph_pending[voice] = 0;
        /* Open303 triggerNote vs slideToNote: a fresh note whose predecessor
         * was released retriggers; one that lands on a still-held note slides
         * (so the amp envelope does not restart into silence). */
        if (!legato || !ph_core) {
            ph_trigger();
            triggered = 1;
        }
        ph_on = 1;
        ph_acc = (v->velocity >= 100) ? accent_i * 256 / 127 : 0;
        /* the sweep amp's gate, at note time (as the circuit's, which the
         * ACCENT pot biases): above 48 % of the knob the sweep is scaled from
         * 0 there to full at 127. An unaccented note leaves the envelope and
         * the capacitor draining, so a line of accents keeps its tail. */
        if (ph_acc > 0)
            ph_wenv = (ph_acc > PH_ACC_THR)
                      ? (((ph_acc - PH_ACC_THR) * 256) / (256 - PH_ACC_THR)) << 8 : 0;
        ph_fenv_d = ph_decay[(ph_acc > 0) ? clamp32(kn[13] >> 8, 0, 127)
                                          : clamp32(kn[9] >> 8, 0, 127)];
        ph_rc1c = ph_attack[clamp32(kn[12] >> 8, 0, 127)];
    }

    /* pitch: a trigger snaps (Open303 triggerNote), a slide glides
     * (slideToNote) - snap only the block that actually triggered. The glide
     * itself runs per sample in the loop, so it is not 32x too slow.
     *
     * A snap settles over ~0.7 ms rather than in one sample. The PolyBLEP
     * residual is a function of the phase increment, so a step in the
     * increment steps the output while the phase is inside the residual's
     * window - and a note that lands on a sounding core snaps with the amp
     * already up, which is a tick. Open303's wavetable oscillator has no
     * residual, and ~0.7 ms is not heard as a glide. */
    {
        int32_t target = v->pitch + ((oct * 12 + tune) << 16);
        int32_t tinc = (int32_t)dnm_phase_inc(target);
        int32_t slide_i = clamp32(kn[11] >> 8, 0, 127);
        gtarget = tinc;
        if (!was_core) {
            ph_inc = (uint32_t)tinc;        /* silent: nothing to step */
        } else if ((fresh && triggered) || slide_i == 0) {
            gslide = PH_SNAP_G;
        } else {
            gslide = ph_slide[slide_i];
        }
    }

    if (!ph_core) {
        for (i = 0; i < 32; i++)
            block[i] = 0;
        return;
    }
    if (!fresh)
        ph_on = v->gate ? 1 : 0;

    /* The taker replays: the voice below rendered this block, the state above
     * is the taker's, and the block is the other voice's own core samples,
     * ramped in. */
    if (replay) {
        for (i = 0; i < 32; i++) {
            int32_t gr = 32768 - ph_xg(xfade, i);
            int32_t o = ph_blk[i] - ((ph_blk[i] * gr) >> 15);
            block[i] = clamp32(o << PH_OUT_SHIFT, -(1 << 27), 1 << 27);
        }
        return;
    }
    if (xfade) {                            /* this voice renders: store the block */
        st->xfstamp = st->stamp;
    }

    /* coefficients, once per block */
    envsc = 190 + envmod * 7;                   /* Q8.8, 0.74 .. 4.21 (Open303) */
    envof = 79;                                 /* Q8.8, ~0.31 (Open303) */
    /* WAVE is a switch, not a blend: DETUNE's travel picks the saw or the
     * pulse (whose width is PW). Half the knob is each; an LFO or p-lock on it
     * flips between the two waveforms. */
    wq = (wave >= 64) ? 127 : 0;
    pwth = (uint32_t)((5 + pw_raw * 90 / 35) * 42949673u);   /* cycles, Q32 */

    /* --- the accent sweep: the 303's own envelope, and its capacitor ------
     * Open303's accent lifts the cutoff by accentGain times an RC that
     * follows the *filter envelope*, so the accent's brightness lasts as long
     * as DECAY does. The circuit's sweep amp output does not: it has its own
     * envelope (tau 68 ms, BLACKPYRAMIDS_303's fit on the recordings) and it
     * feeds a direct path plus the sweep capacitor, which only charges while
     * the envelope is up -- so accents in a row stack, the 303's "wow". The
     * two paths split by the RES pot, which the resonance amount r stands for
     * here. The sweep amp also conducts only above 48 % of ACCNT -- below it
     * an accent is loudness only, as on the instrument.
     *
     * One state word, one decay and two multiplies a block; the cap drains to
     * zero (the +1) so a rested voice cannot hold a cutoff offset. */
    rs8 = ph_reso_skew[reso];                   /* the RES pot's position, Q8 */
    ph_wenv -= ((ph_wenv * PH_WDEC) >> 16) + 1;
    if (ph_wenv < 0)
        ph_wenv = 0;
    {
        int32_t vin = ph_wenv >> 1;             /* Q16 -> Q15 */
        int32_t ch = ph_wch[reso], dis = ph_wdis[reso], d;
        if (vin > ph_wcap)                      /* the diode: it only charges */
            ph_wcap += ((vin - ph_wcap) * ch) >> 16;
        d = (ph_wcap * dis) >> 16;
        if (d == 0 && ph_wcap > 0)
            d = 1;
        ph_wcap -= d;
        accsweep = ((((vin * PH_WOCT_D) >> 15) * (256 - rs8)) +
                    (((ph_wcap * PH_WOCT_C) >> 15) * rs8)) >> 8;
    }

    cut_l2 = PH_L2_LO + cut_raw * PH_CSTEP;
    cut_l2 += (envsc * (ph_rc1 - (envof << 7))) >> 15;
    cut_l2 += accsweep;
    cut_l2 = clamp32(cut_l2, PH_L2_LO, PH_L2_TOP);
    {
        int32_t g = ((cut_l2 - PH_L2_LO) * (PH_N_FC - 1) * 256) / (PH_L2_HI - PH_L2_LO);
        g = clamp32(g, 0, (PH_N_FC - 1) * 256);
        idx = g >> 8;
        frac = g & 255;
        if (idx >= PH_N_FC - 1) {
            idx = PH_N_FC - 1;
            frac = 0;
        }
        {
            /* the two coefficient sets are different functions of the cutoff,
             * and the mode picks one (see tools/build-tables.py) */
            const int16_t *tb = (mode >= 15) ? ph_b0_303 : ph_b0;
            const int16_t *tk = (mode >= 15) ? ph_k_303 : ph_k_gen;
            const int16_t *tg = (mode >= 15) ? ph_kg_303 : ph_k_gen;
            b0 = tb[idx] + (((tb[idx + 1] - tb[idx]) * frac) >> 8);
            kq = tk[idx] + (((tk[idx + 1] - tk[idx]) * frac) >> 8);
            kgq = tg[idx] + (((tg[idx + 1] - tg[idx]) * frac) >> 8);
        }
    }
    rs8 = ph_reso_skew[reso];
    c15 = 2 * b0;                               /* 2*b0, Q15 */
    /* What the ladder's state clamps may be: (c15 * s) >> 15 has to stay inside
     * int32, so |s| <= 2e9/c15 -- 1.7 million at CUTOF's bottom, 46 000 at the
     * top. That is one divide per block, and it is what the arithmetic allows
     * rather than what the sound wants: it is a wrap guard, not a saturation,
     * and the ladder is linear up to it (see the 303 block). */
    stlim = 2000000000 / c15;
    k8 = (kq * rs8) >> 8;                       /* k = K*r, Q8 */
    k8 += (ph_acc * kq) >> 12;                  /* accent tightens resonance a little */
    /* but never past the self-oscillation threshold. K is *measured* as the
     * feedback at which the closed loop stops decaying (tools/build-tables.py
     * bisects it), times RESO_MAX, so k = K sits just under the onset at every
     * cutoff; the accent above could push k past it at the top of RESO, and a
     * ladder with loop gain > 1 does not resonate, it runs away into its own
     * state clamps -- which is what "overload, and it ducks as you turn
     * resonance up" was. */
    if (k8 > kq)
        k8 = kq;
    /* And one sixteenth of the travel back off it. The table's K is the
     * *linear* onset, so at the top of RESO the ladder's poles sit on the unit
     * circle: the resonance does not decay, and in integer arithmetic what it
     * does instead is a limit cycle. A sweep that excites it (the envelope's
     * cutoff crossing a held note, which at these settings is most of a note)
     * then swells and, at the top, breaks up into a click -- the reported
     * "sharp pops on ENVMD and ACCNT", and why ACDEC 30 ms and more ACCNT made
     * them constant: both sweep the cutoff through the note faster, and the
     * accent also lifts k. Measured on a slide from G3 to B7 at CUTOF/RESO/
     * ENVMD 127 (out/ping.py): at k = K that click carries 90 % of the burst's
     * energy under 500 Hz and the burst peaks at 0.43 FS; a sixteenth of k
     * lower it carries 0 %, the burst peaks at 0.24, and the note's steady
     * level moves 0.3 dB. Losing 1/16 of the pot's top costs about 1 dB of
     * resonant gain up there -- the 303's own resonance pot never quite
     * reaches self-oscillation either.
     *
     * Only where k is actually on the circle, which is the top of the knob and
     * the upper half of the cutoff: *below* RESO ~85 % the poles are well
     * inside it and nothing there changes; and below 1.4 kHz the table's K is
     * already *over* this loop's real threshold (it was fitted without the
     * feedback highpass, see `ph_k_303`), so the bottom of the knob keeps its
     * full +20 dB squelch -- reso-squelches guards it. The two ramps multiply:
     * the pot from rs8 212 (RESO 66) to 256, the cutoff from cut_l2 2300
     * (1.4 kHz) to 2556 (1.7 kHz). */
    {
        int32_t r8 = ((rs8 - 212) * 1680) >> 8;
        int32_t c8 = cut_l2 - 2300;
        if (r8 > 0 && c8 > 0) {
            if (r8 > 256)
                r8 = 256;
            if (c8 > 256)
                c8 = 256;
            k8 -= ((k8 >> 4) * ((r8 * c8) >> 8)) >> 8;
        }
    }
    /* The one place this port *has* to leave Open303's structure alone: the
     * reference runs its ladder at 4x the host rate, so a 20 kHz cutoff is a
     * 0.65 rad/sample filter with its resonance well inside the band it is
     * sampled at. At 1x (48 kHz) a cutoff at 20 kHz puts a loop gain of ~1
     * within a quarter of Nyquist, and that ring is what read as "digital
     * glitching": at CUTOF/ENVMD 127 the energy above 6 kHz went to two thirds
     * of the output. Open303 itself never runs the *knob* past 2.4 kHz
     * (Open303VST.cpp:227), so the musical range is far below this.
     *
     * So the feedback -- and the make-up that goes with it, so the level does
     * not jump -- fades out between 8 kHz and 12 kHz (0.17 and 0.25 fs). That
     * is above anything the CUTOF knob can ask for on its own (3.1 kHz) and
     * above the envelope's reach at CUTOF up to ~72 with ENVMD 100 (the
     * classic plink keeps its whole sweep); above that only the envelope's
     * attack transient can reach it, where the alternative is a near-Nyquist
     * ring. An earlier window started at 3.5 kHz, i.e. inside that sweep, and
     * that is what made the filter go odd above half CUTOF. Linear in
     * log-cutoff, i.e. in cut_l2. */
    if (cut_l2 > PH_TPR_LO) {
        int32_t t8 = 256 - ((256 * (cut_l2 - PH_TPR_LO)) / (PH_TPR_HI - PH_TPR_LO));
        if (t8 < 0)
            t8 = 0;
        k8 = (k8 * t8) >> 8;
        rs8 = (rs8 * t8) >> 8;
    }
    /* g = ((K/17 - 1)*r + 1) * (1 + r), Q8, Open303's output gain. It is a
     * level calibration and was written against *their* K polynomial, not the
     * measured feedback K above, so it keeps using theirs: the resonance is the
     * only thing the measured K is allowed to change. */
    g8 = ((((((kgq - 4352) / 17) * rs8) >> 8) + 256) * (256 + rs8)) >> 8;
    /* Level compensation (a deliberate deviation, see docs). At full resonance
     * the ladder's own g has already brought the level back up; what is left is
     * the low-k end, where the un-resonated passband is up to 13 dB hotter than
     * the rest of the sweep (and hotter still when CUTOF is open). Keying the
     * make-up off k - clamped at k = 10, beyond which the level is flat at any
     * cutoff - holds one mix level across the knob. */
    ogain8 = 270 + ((clamp32(k8, 0, 2560) * 27) >> 8);
    /* The two output gains are adjacent (only the overdrive's knee sits
     * between them), so they fold into one multiply per sample. The fold is
     * Q8, so it can land one unit off the pair -- a static 0.1 dB, no more. */
    ogd8 = (ogain8 * dg) >> 8;
    /* And the accent's lift on the filter envelope: (fenv*115)>>8 plus
     * (acc*fenv)>>6 is one multiply by 115 + 4*acc, Q8. */
    ak8 = 115 + 4 * ph_acc;
    rel = (ph_acc > 0) ? PH_AMP_REL_A : PH_AMP_REL_N;
    /* the tap modes' static output gain (see ph_mgain) */
    mg8 = ph_mgain[mode];
    /* the 303 path's (2*g)*PH_VG303, folded (see the ladder below) */
    gf8 = (2 * g8 * PH_VG303) >> 8;

    t = ph_phase;

    for (i = 0; i < 32; i++) {
        int32_t osc, s, filt, amp, out, fenv15;
        uint32_t tt;

        /* --- oscillator: saw <-> square, PolyBLEP --- */
        if (gslide) {
            int32_t d = gtarget - (int32_t)ph_inc;
            if (d == 0)
                gslide = 0;         /* settled: no slew for the rest of the block */
            else
                ph_inc = (uint32_t)((int32_t)ph_inc + ph_slew(d, gslide));
        }
        dt = ph_inc >> 16;
        if (dt < 1)
            dt = 1;
        t += ph_inc;
        tt = t >> 16;
        {
            int32_t bl = ph_blep(tt, dt);
            int32_t saw = (int32_t)tt - 32768 - bl;
            if (wq <= 0) {
                osc = saw;
            } else {
                int32_t sq;
                uint32_t u = (uint32_t)(t - pwth) >> 16;
                sq = (t < pwth) ? 32767 : -32767;
                sq += bl - ph_blep(u, dt);
                /* a gentle knee rounds the 303 square's corners */
                if (sq > 24576)
                    sq = 24576 + ((sq - 24576) >> 2);
                else if (sq < -24576)
                    sq = -24576 + ((sq + 24576) >> 2);
                osc = saw + (((sq - saw) * wq) >> 7);
            }
        }

        /* --- filter --- */
        {
            int32_t in = osc >> 2;                  /* Q15 -> Q13 */
            int32_t fbi, fby, y0;
            in = hp13(in, PH_PRE_B0, PH_PRE_A1, &ph_prex, &ph_prey);
            fbi = (k8 * ph_y4) >> 8;
            fby = hp13(fbi, PH_FB_B0, PH_FB_A1, &ph_fbx, &ph_fby);
            if (mode >= 15) {
                /* 303 ladder: Open303's TB_303 recursion, linear, exactly as
                 * rosic_TeeBeeFilter.h runs it:
                 *
                 *   y1 += 2*b0*(y0 - y1 + y2)
                 *   y2 +=   b0*(y1 - 2*y2 + y3)
                 *   y3 +=   b0*(y2 - 2*y3 + y4)
                 *   y4 +=   b0*(y3 - 2*y4)
                 *   out = 2*g*y4
                 *
                 * There is no saturation in the loop -- Open303's `shape()` is
                 * only in commented-out code -- and none is added here. The
                 * states are clamped at the arithmetic's own limit (stlim, one
                 * divide per block) so a runaway cannot wrap; that clamp sits
                 * far above every signal the ladder produces (the ring peaks
                 * near 3.5 audio units against 46 000 at the top of CUTOF). */
                y0 = in - fby;
                s = clamp32(y0 - ph_y1 + ph_y2, -stlim, stlim);
                ph_y1 = clamp32(ph_y1 + ((c15 * s) >> 15), -stlim, stlim);
                s = clamp32(ph_y1 - 2 * ph_y2 + ph_y3, -stlim, stlim);
                ph_y2 = clamp32(ph_y2 + ((b0 * s) >> 15), -stlim, stlim);
                s = clamp32(ph_y2 - 2 * ph_y3 + ph_y4, -stlim, stlim);
                ph_y3 = clamp32(ph_y3 + ((b0 * s) >> 15), -stlim, stlim);
                s = clamp32(ph_y3 - 2 * ph_y4, -stlim, stlim);
                ph_y4 = clamp32(ph_y4 + ((b0 * s) >> 15), -stlim, stlim);
                filt = (gf8 * ph_y4) >> 8;
            } else {
                /* the other TeeBeeFilter modes: the same ladder, one tap mix,
                 * and Open303's `8*(c0*y0 + ... + c4*y4)` behind the static
                 * per-mode factor. */
                const int8_t *cw = ph_taps[mode];
                y0 = (in >> 3) - fby;
                ph_y1 = clamp32(ph_y1 + ((b0 * (y0 - ph_y1)) >> 15), -40000, 40000);
                ph_y2 = clamp32(ph_y2 + ((b0 * (ph_y1 - ph_y2)) >> 15), -40000, 40000);
                ph_y3 = clamp32(ph_y3 + ((b0 * (ph_y2 - ph_y3)) >> 15), -40000, 40000);
                ph_y4 = clamp32(ph_y4 + ((b0 * (ph_y3 - ph_y4)) >> 15), -40000, 40000);
                s = cw[0] * y0 + cw[1] * ph_y1 + cw[2] * ph_y2 +
                    cw[3] * ph_y3 + cw[4] * ph_y4;
                /* 512 = unity; ph_mgain carries Open303's 8 and the mode's
                 * peak normalisation. */
                filt = (s * mg8) >> 9;
            }
            filt = clamp32(filt, -30000, 30000);
        }

        /* --- post highpass --- */
        /* Open303's sub-audio allpass (14 Hz) and band-reject notch (7.5 Hz)
         * are dropped here: they sit below the post highpass and, in integer
         * fixed point, the notch's DC numerator and denominator are both a few
         * LSBs (a huge, quantisation-driven DC gain). The pre and post high-
         * passes already remove the sub-audio band. */
        filt = hp13(filt, PH_POST_B0, PH_POST_A1, &ph_postx, &ph_posty);

        /* --- filter envelope, its RCs, and the amp envelope --- */
        /* The three envelopes run at 1/4 rate (see lp_step4): none of them is
         * read between samples, and the de-clicker below puts the 4-sample
         * steps of the last one through its own 200 Hz pole. */
        if (!(i & 3)) {
            ph_fenv -= ((ph_fenv * ph_fenv_d) >> 18) + 4;
            if (ph_fenv < 0)
                ph_fenv = 0;
            fenv15 = ph_fenv >> 6;              /* Q21 -> Q15 */
            ph_rc1 = lp_step4(ph_rc1, fenv15, ph_rc1c);
            if (ph_aenv > 0) {
                if (ph_on)
                    ph_aenv -= ((ph_aenv * PH_AMP_DECAY) >> 20) + 4;
                else
                    ph_aenv -= ((ph_aenv * rel) >> 10) + 4;
                if (ph_aenv < 0)
                    ph_aenv = 0;
            }
            if (ph_aenv == 0)
                ph_core = 0;
            aenv15 = ph_aenv >> 9;              /* Q24 -> Q15 */
            if (ph_on)
                aenv15 += (fenv15 * ak8) >> 8;
            aenv15 = clamp32(aenv15, 0, 57344);
        }
        /* Open303 adds 0.45*fenv + accentGain*4.0*fenv on top of the amp
         * envelope and clamps nothing; its output stage then applies -12 dB
         * (ampScaler), so the two cancel and a full accent lifts the VCA by
         * about +12 dB. This voice has the -12 dB in its audio unit already
         * (8192 against a 32767 rail) and no separate scaler, so the lift is
         * capped here. The cap cannot go below 1.45 without erasing the accent
         * altogether: the unaccented path already reaches 1 + 0.45 at a full
         * envelope, and an earlier attempt at 1.4 measured an accented note
         * 0.7 % *quieter* than an unaccented one. At 1.75 the cap leaves the
         * accent a 1.2x lift on the attack on top of its sweep (1.5 octaves
         * plus the 2.7 of the capacitor, see the block above) and +4 % of
         * feedback, and no accented note reaches the output clamp. */
        ph_dcy = lp_step(ph_dcy, aenv15, PH_DC_B0);
        amp = ph_dcy;

        out = (filt * amp) >> 15;
        out = (out * ogd8) >> 8;                /* the trim, and the resonance flatten */
        /* DRIVE: the output stage's overdrive. The signal is gained into a soft
         * knee and part of the gain is taken back afterwards -- at the top of
         * the knob the make-up stops falling (ph_dgain's floor) -- so the knob
         * adds harmonics instead of level, and everything that runs hot, the
         * resonant peaks at the top of CUTOF and RESO above all, flattens here
         * instead of slamming a rail. A linear stage with a hard clamp is what
         * "digital overload, and it ducks" was.
         *
         * At the bottom of the knob the gain and the make-up are both exactly
         * unity, which leaves the limiter alone in the path, so the constants
         * above swap in the rail's own there -- and everything under its
         * onset, which is nearly every note, comes out bit for bit. The gain
         * itself is already in ogd8 above: the knee wants the driven level. */
        {
            int32_t sg = (out < 0) ? -1 : 1;
            int32_t a = (out < 0) ? -out : out, d;
            if (a > odk) {
                d = a - odk;
                if (dg > 256) {
                    /* the overdrive: quadratic, flat at the rail */
                    if (d > odw)
                        d = odw;
                    a = odk + d - ((d * d) / odq);
                } else {
                    /* the output's limiter: rational, to the rail but never
                     * flat there (see the constants above) */
                    a = odk + ((d * odw) / (d + odw));
                }
            }
            out = (sg < 0) ? -a : a;
        }
        out = (out * dgain8) >> 8;              /* its make-up */
        out = clamp32(out, -32767, 32767);
        /* SRR: the output's sample-and-hold. A Q32 phase accumulator wraps
         * once per held sample, so the step is FS/rate: one step down from the
         * top a new sample lands every 2.5 at 48 kHz. The top step's step is 0
         * -- the knob's bypass -- and the hold is skipped there instead of
         * being fed an unwrappable increment, so the reduction leaves the path
         * entirely. The hold is one assignment, the cheapest place to put it. */
        if (srr_inc) {
            ph_srr_acc += srr_inc;
            if (ph_srr_acc < srr_inc)
                ph_srr_hold = out;
            out = ph_srr_hold;
        }
        if (xfade) {
            /* a handover: the core's sample unfaded, and this voice's ramp over
             * it. The two voices' ramps are (i+1)/32 and (31-i)/32, so they sum
             * to the core exactly. */
            ph_blk[i] = out;
            out = (out * ph_xg(xfade, i)) >> 15;
        }
        out <<= PH_OUT_SHIFT;                   /* Q13 -> peaking near DNM_PEAK */
        out = clamp32(out, -(1 << 27), 1 << 27);
        block[i] = out;
    }
    ph_phase = t;
}

/* ---- the descriptor ----------------------------------------------------- */
/* The icon: the acid smiley, 16 rows of 16, bit 15 the left, the top row first.
 * Traced pixel for pixel from the 16x16 source drawing, whose ink is 12 columns
 * wide, centred: two spare columns on each side. `out/icon.py` reads that source
 * back and checks these rows against the tile the firmware draws. */
static const uint16_t phreakoid_icon[16] = {
    0x07e0, 0x0810, 0x1008, 0x2044,
    0x2244, 0x2204, 0x2024, 0x2444,
    0x1388, 0x0808, 0x0a10, 0x0a20,
    0x0540, 0x0140, 0x0140, 0x0080,
};

const struct dnm_machine phreakoid_machine = {
    MY_ID, "PHREAKOID", phreakoid_icon, phreakoid_knobs, phreakoid_render,
    DNM_OWN_ENV, my_defaults,
};
