/*
 * RV530 EDID: the monitor on the DVI connector's DDC bus.
 *
 * QEMU's generic generator (edid-generate.c behind i2c-ddc) lists most
 * 16:10 sizes only in an Established Timings III descriptor, which Mac OS X
 * 10.6 ignores, times them for 75 Hz with normal blanking (pixel clocks
 * the ATI driver rejects for large modes) and serves a single 128-byte
 * block.  This builder makes an EDID 1.3 base block plus a CEA-861
 * extension in which every mode is a detailed timing with CVT reduced
 * blanking at the `refresh` rate, lowered per size as far as it needs
 * to stay within dual-link DVI, plus the preferred size at 60 Hz:
 *
 *   base:      preferred mode and one more as DTDs, range limits, monitor
 *              name; established 640x480/800x600/1024x768; 8 standard
 *              timings
 *   extension: a video capability block and up to 6 more DTDs (the large
 *              16:10 and 16:9 sizes)
 *
 * Modes larger than xmax x ymax are left out.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "rv530.h"

typedef struct EdidMode {
    uint16_t w, h, hz;
} EdidMode;

/* detailed timings, preferred mode first, then these in order */
static const EdidMode dtd_modes[] = {
    { 2560, 1600, 60 }, { 2560, 1440, 60 }, { 2048, 1152, 60 }, { 1920, 1200, 60 },
    { 1920, 1080, 60 }, { 1680, 1050, 60 }, { 1600, 900, 60 }, { 1440, 900, 60 },
    { 1280, 800, 60 },
};

/* standard timings (60 Hz): EDID 1.3 aspect codes 0 16:10, 1 4:3, 2 5:4,
 * 3 16:9; width at most 2288 */
static const struct { uint16_t w; uint8_t aspect; } std_modes[8] = {
    { 1280, 0 }, { 1440, 0 }, { 1680, 0 }, { 1600, 3 },
    { 1280, 2 }, { 1600, 1 }, { 1280, 3 }, { 1920, 3 },
};
static const uint8_t std_h_num[4] = { 10, 3, 4, 9 }, std_h_den[4] = { 16, 4, 5, 16 };

/*
 * Apple's ATI driver drops detailed timings above dual-link DVI's 330 MHz
 * (measured on 10.6.8: 329.75 MHz kept, 332.25 MHz dropped) and then runs
 * the size at 60 Hz.  The EDID's 16-bit clock field ends at 655.35 MHz.
 */
#define EDID_MAX_CLOCK_KHZ  330000

typedef struct CvtTiming {
    uint32_t clock_khz;
    uint32_t hblank, hfront, hsync;
    uint32_t vblank, vfront, vsync;
} CvtTiming;

/* VESA CVT 1.2 reduced blanking (v1) */
static void cvt_rb(uint32_t w, uint32_t h, uint32_t hz, CvtTiming *t)
{
    const double refresh = hz, min_vbi_us = 460.0;
    uint32_t vsync, vbi, htotal, vtotal;
    double hperiod_us;

    /* the V sync width encodes the aspect ratio */
    if (w * 3 == h * 4) {
        vsync = 4;
    } else if (w * 9 == h * 16) {
        vsync = 5;
    } else if (w * 10 == h * 16) {
        vsync = 6;
    } else if (w * 4 == h * 5 || w * 9 == h * 15) {
        vsync = 7;
    } else {
        vsync = 10;
    }
    hperiod_us = (1000000.0 / refresh - min_vbi_us) / h;
    vbi = (uint32_t)(min_vbi_us / hperiod_us) + 1;
    if (vbi < 3 + vsync + 6) {
        vbi = 3 + vsync + 6;
    }
    htotal = w + 160;
    vtotal = h + vbi;
    /* pixel clock rounded down to a 250 kHz step */
    t->clock_khz = (uint32_t)(refresh * htotal * vtotal / 250000.0) * 250;
    t->hblank = 160;
    t->hfront = 48;
    t->hsync = 32;
    t->vblank = vbi;
    t->vfront = 3;
    t->vsync = vsync;
}

/* the fastest rate up to hz (at least 60) whose timing Apple's driver keeps */
static uint32_t fit_hz(uint32_t w, uint32_t h, uint32_t hz)
{
    while (hz > 60) {
        CvtTiming t;
        cvt_rb(w, h, hz, &t);
        if (t.clock_khz <= EDID_MAX_CLOCK_KHZ) {
            break;
        }
        hz--;
    }
    return hz;
}

/* the image fills the panel's width (wmm) at the mode's aspect ratio */
static void put_dtd(uint8_t *d, const EdidMode *m, uint32_t wmm)
{
    CvtTiming t;
    uint32_t w = m->w, h = m->h, clk, hmm = wmm * h / w;

    cvt_rb(w, h, m->hz, &t);
    clk = t.clock_khz / 10;
    d[0] = clk & 0xff;
    d[1] = clk >> 8;
    d[2] = w & 0xff;
    d[3] = t.hblank & 0xff;
    d[4] = ((w >> 8) << 4) | (t.hblank >> 8);
    d[5] = h & 0xff;
    d[6] = t.vblank & 0xff;
    d[7] = ((h >> 8) << 4) | (t.vblank >> 8);
    d[8] = t.hfront & 0xff;
    d[9] = t.hsync & 0xff;
    d[10] = ((t.vfront & 15) << 4) | (t.vsync & 15);
    d[11] = ((t.hfront >> 8) << 6) | ((t.hsync >> 8) << 4) |
            ((t.vfront >> 4) << 2) | (t.vsync >> 4);
    d[12] = wmm & 0xff;
    d[13] = hmm & 0xff;
    d[14] = ((wmm >> 8) << 4) | (hmm >> 8);
    d[15] = 0;
    d[16] = 0;
    /* digital separate sync, +hsync -vsync (CVT-RB) */
    d[17] = 0x18 | 0x02;
}

static void put_text(uint8_t *d, uint8_t tag, const char *s)
{
    size_t i, n = strlen(s);

    memset(d, 0, 18);
    d[3] = tag;
    memset(d + 5, ' ', 13);
    for (i = 0; i < n && i < 13; i++) {
        d[5 + i] = s[i];
    }
    if (n < 13) {
        d[5 + n] = '\n';
    }
}

static void put_ranges(uint8_t *d, uint32_t max_clock_khz, uint32_t max_hz,
                       uint32_t max_hfreq_khz)
{
    memset(d, 0, 18);
    d[3] = 0xfd;
    d[5] = 50;              /* V 50 Hz up to the fastest mode (at least 75) */
    d[6] = MAX(75, max_hz);
    d[7] = 30;              /* H from 30 kHz */
    d[8] = MIN(255, MAX(160, max_hfreq_khz + 1));
    d[9] = (max_clock_khz + 9999) / 10000;      /* 10 MHz units */
    d[10] = 0;              /* default GTF; the DTDs are what counts */
    d[11] = 0x0a;
    memset(d + 12, ' ', 6);
}

static void checksum(uint8_t *b)
{
    uint8_t sum = 0;
    int i;

    for (i = 0; i < 127; i++) {
        sum += b[i];
    }
    b[127] = -sum;
}

static bool mode_fits(uint32_t w, uint32_t h, uint32_t maxx, uint32_t maxy)
{
    return w <= maxx && h <= maxy;
}

void rv530_edid_build(uint8_t edid[RV530_EDID_SIZE], uint32_t prefx,
                      uint32_t prefy, uint32_t maxx, uint32_t maxy, uint32_t serial,
                      uint32_t hz)
{
    static const uint8_t header[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    /* sRGB primaries and D65 white, as edid-generate.c */
    static const uint8_t chroma[10] = {
        0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54,
    };
    uint8_t *b = edid, *x = edid + 128;
    EdidMode list[16];
    uint32_t n = 0, i, k, max_clock = 0, max_hz = 60, max_hfreq = 0, wmm = 0, hmm = 0, want;
    uint16_t vendor = (('R' - '@') << 10) | (('V' - '@') << 5) | ('X' - '@');

    memset(edid, 0, RV530_EDID_SIZE);
    if (maxx < prefx) {
        maxx = prefx;
    }
    if (maxy < prefy) {
        maxy = prefy;
    }

    /* the preferred mode (at the requested refresh rate, or the fastest
     * the size allows within the driver's pixel clock limit, and at 60 Hz),
     * then every listed mode that fits */
    hz = MIN(MAX(hz, 50), 240);
    want = hz;
    hz = fit_hz(prefx, prefy, want);
    /*
     * WindowServer keeps a mode per monitor (vendor, product, serial) and
     * reuses it, timing included: a monitor once seen at 60 Hz stays at
     * 60 Hz when its EDID later prefers a faster timing.  A monitor with
     * other timings is another monitor, so the rate goes into the serial
     * (60 Hz monitors keep theirs, and their saved arrangement).
     */
    if (hz != 60) {
        serial ^= hz << 24;
    }
    list[n++] = (EdidMode){ prefx, prefy, hz };
    if (hz != 60) {
        list[n++] = (EdidMode){ prefx, prefy, 60 };
    }
    for (i = 0; i < ARRAY_SIZE(dtd_modes) && n < ARRAY_SIZE(list); i++) {
        const EdidMode *m = &dtd_modes[i];
        if (mode_fits(m->w, m->h, maxx, maxy) &&
            !(m->w == prefx && m->h == prefy)) {
            /* the other sizes at the requested rate too (as far as each
             * fits): the guest may come up in one of them from its saved
             * settings, not in the preferred size */
            list[n++] = (EdidMode){ m->w, m->h, fit_hz(m->w, m->h, want) };
        }
    }
    for (i = 0; i < n; i++) {
        CvtTiming t;
        cvt_rb(list[i].w, list[i].h, list[i].hz, &t);
        max_clock = MAX(max_clock, t.clock_khz);
        max_hz = MAX(max_hz, list[i].hz);
        max_hfreq = MAX(max_hfreq, t.clock_khz / (list[i].w + t.hblank));
        /* a ~100 dpi panel as wide as the widest mode; each image fills
         * its width, so the panel is as tall as the tallest image */
        wmm = MAX(wmm, list[i].w * 254 / 1000);
    }
    for (i = 0; i < n; i++) {
        hmm = MAX(hmm, wmm * list[i].h / list[i].w);
    }
    /* at least single-link DVI: the established and standard timings
     * (DMT, up to 1600x1200 at 162 MHz) are inside the range too */
    max_clock = MAX(max_clock, 165000);

    /* --- base block --- */
    memcpy(b, header, 8);
    b[8] = vendor >> 8;
    b[9] = vendor & 0xff;
    b[10] = 0x30;           /* product 0x0530 */
    b[11] = 0x05;
    stl_le_p(b + 12, serial);   /* a new serial is a new monitor to WindowServer */
    b[16] = 1;              /* week 1 */
    b[17] = 2006 - 1990;
    b[18] = 1;              /* EDID 1.3 */
    b[19] = 3;
    b[20] = 0x80;           /* digital input */
    b[21] = (wmm + 5) / 10;     /* cm */
    b[22] = (hmm + 5) / 10;
    b[23] = 120;            /* gamma 2.2 */
    b[24] = 0x0e;           /* RGB, sRGB, preferred timing is the first DTD */
    memcpy(b + 25, chroma, 10);
    b[35] = 0x21;           /* 640x480@60, 800x600@60 */
    b[36] = 0x08;           /* 1024x768@60 */
    for (i = 0; i < 8; i++) {
        uint32_t w = std_modes[i].w, a = std_modes[i].aspect;
        uint32_t h = w * std_h_num[a] / std_h_den[a];
        if (mode_fits(w, h, maxx, maxy)) {
            b[38 + 2 * i] = w / 8 - 31;
            b[39 + 2 * i] = a << 6;
        } else {
            b[38 + 2 * i] = b[39 + 2 * i] = 0x01;
        }
    }
    /* detailed timings come before the other descriptors */
    put_dtd(b + 54, &list[0], wmm);
    k = 1;
    if (n > k) {
        put_dtd(b + 72, &list[k], wmm);
        k++;
    } else {
        b[72 + 3] = 0x10;   /* dummy descriptor */
    }
    put_ranges(b + 90, max_clock, max_hz, max_hfreq);
    put_text(b + 108, 0xfc, "RV530 Display");
    b[126] = 1;             /* one extension */
    checksum(b);

    /* --- CEA-861 extension: a video capability block, then DTDs --- */
    x[0] = 0x02;
    x[1] = 3;
    x[2] = 7;               /* DTDs start after the 3-byte VCDB */
    x[3] = 0x80;            /* IT formats underscanned (a PC monitor) */
    x[4] = (7 << 5) | 2;    /* extended tag block, 2 bytes */
    x[5] = 0x00;            /* video capability data block */
    x[6] = 0x4a;            /* RGB quantization selectable, IT and CE underscanned */
    for (i = 0; k < n && i < 6; i++, k++) {
        put_dtd(x + 7 + 18 * i, &list[k], wmm);
    }
    checksum(x);
}

/* ------------------------------------------------------------------ */
/* DDC slave (I2C address 0x50) serving both blocks                    */
/* ------------------------------------------------------------------ */

static int rv530_edid_event(I2CSlave *i2c, enum i2c_event event)
{
    RV530EdidState *s = RV530_EDID(i2c);

    if (event == I2C_START_SEND) {
        s->firstbyte = true;
    }
    return 0;
}

static uint8_t rv530_edid_rx(I2CSlave *i2c)
{
    RV530EdidState *s = RV530_EDID(i2c);

    return s->blob[s->reg++ % RV530_EDID_SIZE];
}

static int rv530_edid_tx(I2CSlave *i2c, uint8_t data)
{
    RV530EdidState *s = RV530_EDID(i2c);

    if (s->firstbyte) {
        s->reg = data;
        s->firstbyte = false;
    }
    return 0;
}

static void rv530_edid_reset(DeviceState *dev)
{
    RV530EdidState *s = RV530_EDID(dev);

    s->firstbyte = false;
    s->reg = 0;
}

static const VMStateDescription vmstate_rv530_edid = {
    .name = TYPE_RV530_EDID,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(firstbyte, RV530EdidState),
        VMSTATE_UINT8(reg, RV530EdidState),
        VMSTATE_END_OF_LIST()
    },
};

static void rv530_edid_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, rv530_edid_reset);
    dc->vmsd = &vmstate_rv530_edid;
    isc->event = rv530_edid_event;
    isc->recv = rv530_edid_rx;
    isc->send = rv530_edid_tx;
}

static const TypeInfo rv530_edid_info = {
    .name = TYPE_RV530_EDID,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(RV530EdidState),
    .class_init = rv530_edid_class_init,
};

static void rv530_edid_register_types(void)
{
    type_register_static(&rv530_edid_info);
}

type_init(rv530_edid_register_types)
