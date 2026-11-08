/*
 * The monitor's EDID on DDC (r5xx_edid.c): EDID 1.3 base block plus a
 * CEA-861 extension, served by an I2C slave at 0x50.  Shared by the rv530
 * and the evergreen (radeon-hd5770) device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DISPLAY_R5XX_EDID_H
#define HW_DISPLAY_R5XX_EDID_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

/* the monitor's EDID on DDC (r5xx_edid.c): base block + CEA extension */
#define RV530_EDID_SIZE 256
#define TYPE_RV530_EDID "rv530-edid"
typedef struct RV530EdidState {
    I2CSlave i2c;
    bool firstbyte;
    uint8_t reg;
    uint8_t blob[RV530_EDID_SIZE];
} RV530EdidState;
OBJECT_DECLARE_SIMPLE_TYPE(RV530EdidState, RV530_EDID)

void rv530_edid_build(uint8_t edid[RV530_EDID_SIZE], uint32_t prefx,
                      uint32_t prefy, uint32_t maxx, uint32_t maxy, uint32_t serial,
                      uint32_t hz);

#endif
