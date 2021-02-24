/* ka10_dd.c: Data Disc 6600 Television Display System, with
   PDP-10 interface and video switch made at Stanford AI lab.

   Copyright (c) 2021, Lars Brinkhoff

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
   LARS BRINKHOFF BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Lars Brinkhoff shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Lars Brinkhoff.

*/

#include "kx10_defs.h"
#ifndef NUM_DEVS_DD
#define NUM_DEVS_DD 0
#endif

#if NUM_DEVS_DD > 0
#include "sim_video.h"

#define DD_DEVNUM         0510
#define VDS_DEVNUM        0340

#define DD_WIDTH     512                    /* Display width. */
#define DD_HEIGHT    480                    /* Display height. */
#define DD_PIXELS    (DD_WIDTH * DD_HEIGHT) /* Total number of pixels. */
#define DD_CHANNELS  32                     /* Data Disc channels. */
#define VDS_OUTPUTS  64                     /* Video switch outputs. */

#define STATUS            u3
#define MA                u4    /* Current memory address. */
#define PIA               u5
#define COLUMN            u6
#define LINE              us9
#define CHANNEL           us10

/* CONI/O Bits */
#define DD_HALT     000000010    /* CONI: Halted. */
#define DD_RESET    000000010    /* CONO: Reset. */
#define DD_INT      000000020    /* CONI: Interrupting. */
#define DD_FORCE    000000020    /* CONO: Force field. */
#define DD_FIELD    000000040    /* CONI: Field. */
#define DD_HALT_ENA 000000100    /* CONI: Halt interrupt enabled. */
#define DD_DDGO     000000100    /* CONO: Go. */
#define DD_LATE     000000200    /* CONI: Late. */
#define DD_SPGO     000000200    /* CONO */
#define DD_LATE_ENA 000000400    /* Late interrupt enabled. */
#define DD_USER     000001000    /* User mode. */
#define DD_NXM      000002000    /* CONI: Accessed non existing memory. */

/* There are 64 displays, fed from the video switch. */
static uint32 dd_surface[VDS_OUTPUTS][DD_PIXELS];
static uint32 dd_palette[VDS_OUTPUTS][2];
static VID_DISPLAY *dd_vptr[VDS_OUTPUTS];

/* There are 32 channels on the Data Disc. */
static uint8 dd_channel[DD_CHANNELS][DD_PIXELS];
static uint8 dd_changed[DD_CHANNELS];
static int dd_windows = 2;

uint32 vds_channel;              /* Currently selected video outputs. */
uint8  vds_changed[VDS_OUTPUTS];
uint32 vds_selection[VDS_OUTPUTS];        /* Data Disc channels. */
uint32 vds_sync_inhibit[VDS_OUTPUTS];
uint32 vds_analog[VDS_OUTPUTS];           /* Analog channel. */

#include "dd-font.h"

t_stat dd_devio(uint32 dev, uint64 *data);
t_stat vds_devio(uint32 dev, uint64 *data);
t_stat dd_svc(UNIT *uptr);
t_stat vds_svc(UNIT *uptr);
t_stat dd_reset(DEVICE *dptr);
t_stat vds_reset(DEVICE *dptr);
t_stat dd_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
t_stat vds_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *dd_description (DEVICE *dptr);
const char *vds_description (DEVICE *dptr);

DIB dd_dib = { DD_DEVNUM, 1, dd_devio, NULL};

UNIT dd_unit = {
    UDATA (&dd_svc, UNIT_IDLE, 0)
};

MTAB dd_mod[] = {
    { 0 }
    };

DEVICE dd_dev = {
    "DD", &dd_unit, NULL, dd_mod,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, dd_reset,
    NULL, NULL, NULL, &dd_dib, DEV_DEBUG | DEV_DISABLE | DEV_DIS | DEV_DISPLAY, 0, dev_debug,
    NULL, NULL, &dd_help, NULL, NULL, &dd_description
    };

UNIT vds_unit = {
    UDATA (&vds_svc, UNIT_IDLE, 0)
};

DIB vds_dib = { VDS_DEVNUM, 1, vds_devio, NULL};

DEVICE vds_dev = {
    "VDS", &vds_unit, NULL, NULL,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, vds_reset,
    NULL, NULL, NULL, &vds_dib, DEV_DEBUG | DEV_DISABLE | DEV_DIS | DEV_DISPLAY, 0, dev_debug,
    NULL, NULL, &vds_help, NULL, NULL, &vds_description
    };

static void unimplemented (const char *text)
{
    fprintf (stderr, "\r\n[UNIMPLEMENTED: %s]\r\n", text);
}

t_stat dd_devio(uint32 dev, uint64 *data) {
     UNIT *uptr = &dd_unit;
     switch(dev & 3) {
     case CONI:
        *data = uptr->PIA | uptr->STATUS;
        sim_debug (DEBUG_CONI, &dd_dev, "%06llo (%6o)\n", *data, PC);
        break;
     case CONO:
        sim_debug (DEBUG_CONO, &dd_dev, "%06llo (%6o)\n", *data, PC);
        clr_interrupt (DD_DEVNUM);
        uptr->PIA = (uint32)(*data & 7);
        if (*data & DD_RESET) {
            sim_debug(DEBUG_DETAIL, &dd_dev, "Reset.\n");
            uptr->PIA = 0;
            uptr->STATUS = 0;
            uptr->COLUMN = 2 * 6;
            uptr->LINE = 0;
            sim_cancel (uptr);
        }
        if (*data & DD_RESET) {
            sim_debug(DEBUG_DETAIL, &dd_dev, "Reset.\n");
        }
        if (*data & DD_FORCE) {
            sim_debug(DEBUG_DETAIL, &dd_dev, "Force field.\n");
        }
        if (*data & 040) {
            sim_debug(DEBUG_DETAIL, &dd_dev, "Halt interrupt enabled.\n");
            uptr->STATUS |= DD_HALT_ENA;
        }
        if (*data & DD_DDGO) {
            sim_debug(DEBUG_DETAIL, &dd_dev, "Go.\n");
        }
        if (*data & DD_SPGO) {
            sim_debug(DEBUG_DETAIL, &dd_dev, "SPGO\n");
        }
        if (*data & DD_LATE_ENA) {
            sim_debug(DEBUG_DETAIL, &dd_dev, "Late interrupt enabled.\n");
            uptr->STATUS |= DD_LATE_ENA;
        }
        if (*data & DD_USER) {
            sim_debug(DEBUG_DETAIL, &dd_dev, "User mode.\n");
            uptr->STATUS |= DD_USER;
        }
        break;
     case DATAI:
        *data = 0;
        sim_debug(DEBUG_DATAIO, &dd_dev, "DATAI (%6o)\n", PC);
        break;
    case DATAO:
        sim_debug(DEBUG_DATAIO, &dd_dev, "DATAO %06llo (%6o)\n", *data, PC);
        uptr->MA = *data & 0777777;
        uptr->STATUS &= ~DD_HALT;
        if (uptr->STATUS & DD_DDGO)
            sim_activate (uptr, 1);
        break;
    }
    return SCPE_OK;
}

void
dd_char (int line, int column, char c)
{
    int i, j;
    uint8 pixels;
    int field = line & 1;

    if (line >= DD_HEIGHT || column >= DD_WIDTH)
        return;

    for (i = 0; i < 12; i += 2, line += 2) {
        pixels = font[c][i + field];
        for (j = 0; j < 5; j++) {
            dd_channel[dd_unit.CHANNEL][DD_WIDTH * line + column + j] = (pixels >> 4) & 1;
            pixels <<= 1;
        }
    }
}

void
dd_text (uint64 insn)
{
    char text[6];
    int i;

    text[0] = (insn >> 29) & 0177;
    text[1] = (insn >> 22) & 0177;
    text[2] = (insn >> 15) & 0177;
    text[3] = (insn >>  8) & 0177;
    text[4] = (insn >>  1) & 0177;
    text[5] = 0;
    dd_changed[dd_unit.CHANNEL] = 1;
    sim_debug(DEBUG_CMD, &dd_dev, "TEXT %s to channel %d\n", text, dd_unit.CHANNEL);

    for (i = 0; i < 5; i++) {
        switch (text[i]) {
        case 000:
            break;
        case 011:
            unimplemented ("TAB");
            break;
        case 012:
            dd_unit.LINE += 12;
            dd_unit.LINE &= 0777;
            break;
        case 015:
            dd_unit.COLUMN = 2 * 6;
            break;
        case 0177:
            unimplemented ("RUBOUT");
            break;
        default:
            dd_char (dd_unit.LINE, dd_unit.COLUMN, text[i]);
            dd_unit.COLUMN += 6;
            dd_unit.COLUMN &= 0777;
            break;
        }
    }
}

void
dd_graphics (uint64 insn)
{
    int i, j;
    uint32 pixels = insn >> 4;

    sim_debug (DEBUG_CMD, &dd_dev, "GRAPHICS\n");

    if (dd_unit.LINE >= DD_HEIGHT || dd_unit.COLUMN >= DD_WIDTH)
        return;

    dd_changed[dd_unit.CHANNEL] = 1;

    for (i = 0; i < 12; i += 2) {
        for (j = 0; j < 5; j++) {
            dd_channel[dd_unit.CHANNEL][DD_WIDTH * (dd_unit.LINE + i) + dd_unit.COLUMN + j] = (pixels >> 31) & 1;
            pixels <<= 1;
        }
    }
}

void
dd_function (uint8 data)
{
    int i;

    sim_debug(DEBUG_CMD, &dd_dev, "COMMAND: function code %03o\n", data);
    if (data & 001)
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: graphics mode\n");
    else
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: text mode\n");
    if (data & 002)
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: write enable\n");
    else
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: display direct\n");
    if (data & 004)
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: dark backgroun\n");
    else
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: light backgroun\n");
    switch (data & 011) {
    case 000:
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: double width\n");
      break;
    case 001:
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: single width\n");
      break;
    case 011:
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: erase channel %d\n",
                dd_unit.CHANNEL);
      dd_changed[dd_unit.CHANNEL] = 1;
      for (i = 0; i < DD_PIXELS; i++)
          dd_channel[dd_unit.CHANNEL][i] = 0;
      break;
    }
    if (data & 020)
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: additive\n");
    else
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: replace\n");
    if (data & 040)
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: single height\n");
    else
      sim_debug(DEBUG_DETAIL, &dd_dev, "Function: double height\n");
}

void
dd_command (uint32 command, uint8 data)
{
    switch (command) {
    case 0:
        sim_debug(DEBUG_CMD, &dd_dev, "COMMAND: execute\n");
        unimplemented ("EXECUTE");
        if (dd_unit.LINE < 0740) {
            ;
        }
        break;
    case 1:
        dd_function (data);
        break;
    case 2:
        sim_debug(DEBUG_CMD, &dd_dev, "COMMAND: channel select %d\n", data);
        dd_unit.CHANNEL = data & 077;
        break;
    case 3:
        sim_debug(DEBUG_CMD, &dd_dev, "COMMAND: column select %d\n", data);
        dd_unit.COLUMN = 6 * (data & 0177);
        break;
    case 4:
        dd_unit.LINE = ((data & 037) << 4) | (dd_unit.LINE & 017);
        sim_debug(DEBUG_CMD, &dd_dev, "COMMAND: high order line address -> %d\n",
                  dd_unit.LINE);
        dd_unit.COLUMN = 2 * 6;
        break;
    case 5:
        dd_unit.LINE = (data & 017) | (dd_unit.LINE & 0760);
        sim_debug(DEBUG_CMD, &dd_dev, "COMMAND: low order line address -> %d\n",
                  dd_unit.LINE);
        dd_unit.COLUMN = 2 * 6;
        break;
    case 6:
        sim_debug(DEBUG_CMD, &dd_dev, "COMMAND: write directly\n");
        break;
    case 7:
        sim_debug(DEBUG_CMD, &dd_dev, "COMMAND: line buffer address\n");
        unimplemented ("LINE BUFFER");
        break;
    }
}

void
dd_decode (uint64 insn)
{
    switch (insn & 077) {
    case 001: case 003: case 005: case 007:
    case 011: case 013: case 015: case 017:
    case 021: case 023: case 025: case 027:
    case 031: case 033: case 035: case 037:
    case 041: case 043: case 045: case 047:
    case 051: case 053: case 055: case 057:
    case 061: case 063: case 065: case 067:
    case 071: case 073: case 075: case 077:
      dd_text (insn);
      break;
    case 002: case 022: case 042: case 062:
      dd_graphics (insn);
      break;
    case 000: case 040: case 060:
      sim_debug(DEBUG_CMD, &dd_dev, "HALT\n");
      dd_unit.STATUS |= DD_HALT;
      break;
    case 020:
      dd_unit.MA = insn >> 18;
      sim_debug(DEBUG_CMD, &dd_dev, "JUMP %06o\n", dd_unit.MA);
      break;
    case 006: case 016: case 026: case 036:
    case 046: case 056: case 066: case 076:
    case 012: case 032: case 052: case 072:
      sim_debug(DEBUG_CMD, &dd_dev, "NOP\n");
      break;
    case 010: case 030: case 050: case 070:
      sim_debug(DEBUG_CMD, &dd_dev, "(weird command)\n");
    case 004: case 014: case 024: case 034:
    case 044: case 054: case 064: case 074:
      dd_command ((insn >> 9) & 7, (insn >> 28) & 0377);
      dd_command ((insn >> 6) & 7, (insn >> 20) & 0377);
      dd_command ((insn >> 3) & 7, (insn >> 12) & 0377);
      break;
    default:
      sim_debug(DEBUG_CMD, &dd_dev, "(UNDOCUMENTED %012llo)\n", insn);
      break;
    }
}

t_stat
dd_svc (UNIT *uptr)
{
    if (uptr->MA >= MEMSIZE) {
        uptr->STATUS |= DD_HALT | DD_NXM;
     } else {
        dd_decode (M[uptr->MA]);
        uptr->MA++;
    }
 
    if (uptr->STATUS & DD_HALT) {
        uptr->STATUS |= DD_INT;
        if (uptr->STATUS & DD_HALT_ENA)
            set_interrupt (DD_DEVNUM, uptr->PIA);
    } else
        sim_activate_after (uptr, 100);

    return SCPE_OK;
}

static void
dd_display (int n)
{
    uint32 selection = vds_selection[n];
    int i, j;

    if (selection == 0)
        return;

#if 1
    if (!(selection & (selection - 1))) {
        for (i = 0; (selection & 020000000000) == 0; i++)
            selection <<= 1;
        if (!dd_changed[i] && !vds_changed[n])
            return;
        sim_debug(DEBUG_CMD, &vds_dev, "Output %d from channel %d\n", n, i);
        for (j = 0; j < DD_PIXELS; j++)
            dd_surface[n][j] = dd_palette[n][dd_channel[i][j]];
    } else {
#endif

#if 0
    for (j = 0; j < DD_PIXELS; j++) {
        uint8 pixel = 0;
        for (i = 0; i < DD_CHANNELS; i++, selection <<= 1) {
            if (selection & 020000000000)
                pixel |= dd_channel[i][j];
            dd_surface[n][j] = dd_palette[n][pixel];
        }
    }
#else
    }
#endif

    vid_draw_window (dd_vptr[n], 0, 0, DD_WIDTH, DD_HEIGHT, dd_surface[n]);
    vid_refresh_window (dd_vptr[n]);
}

t_stat
vds_svc (UNIT *uptr)
{
    int i;
    for (i = 6; i < dd_windows + 6; i++)
        dd_display (i);
    for (i = 0; i < DD_CHANNELS; i++)
        dd_changed[i] = 0;
    for (i = 0; i < VDS_OUTPUTS; i++)
        vds_changed[i] = 0;

    sim_activate_after (uptr, 33333);
}

uint32 dd_keyboard_line (void *p)
{
    int i;
    VID_DISPLAY *vptr = (VID_DISPLAY *)p;
    for (i = 0; i < VDS_OUTPUTS; i++) {
        if (vptr == dd_vptr[i])
            return i;
    }
    return ~0U;
}

t_stat dd_reset (DEVICE *dptr)
{
    if (dptr->flags & DEV_DIS || sim_switches & SWMASK('P')) {
        sim_cancel (&dd_unit);
        memset (dd_channel, 0, sizeof dd_channel);
        memset (dd_changed, 0, sizeof dd_changed);
        return SCPE_OK;
    }

    return SCPE_OK;
}

t_stat dd_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
    return SCPE_OK;
}

const char *dd_description (DEVICE *dptr)
{
    return "Data Disc Television Display System";
}

t_stat vds_reset (DEVICE *dptr)
{
    t_stat r;
    int i;
    if (dptr->flags & DEV_DIS || sim_switches & SWMASK('P')) {
        for (i = 0; i < VDS_OUTPUTS; i++) {
            if (dd_vptr[i] != NULL)
                vid_close_window (dd_vptr[i]);
        }
        vds_channel = 0;
        memset (dd_vptr, 0, sizeof dd_vptr);
        memset (dd_palette, 0, sizeof dd_palette);
        memset (vds_selection, 0, sizeof vds_selection);
        memset (vds_sync_inhibit, 0, sizeof vds_sync_inhibit);
        memset (vds_analog, 0, sizeof vds_analog);
        sim_cancel (&vds_unit);
        return SCPE_OK;
    }

    for (i = 6; i < dd_windows + 6; i++) {
        if (dd_vptr[i] == NULL) {
            char title[40];
            snprintf (title, sizeof title, "Data Disc display %d", i);
            r = vid_open_window (&dd_vptr[i], &dd_dev, title, DD_WIDTH, DD_HEIGHT, 0);
            if (r != SCPE_OK)
                return r;
            dd_palette[i][0] = vid_map_rgb_window (dd_vptr[i], 0x00, 0x00, 0x00);
            dd_palette[i][1] = vid_map_rgb_window (dd_vptr[i], 0x00, 0xFF, 0x30);
        }
    }

    sim_activate (&vds_unit, 1);
    return SCPE_OK;
}

t_stat vds_devio(uint32 dev, uint64 *data)
{
    switch(dev & 3) {
    case CONO:
        sim_debug(DEBUG_CONO, &vds_dev, "%012llo (%6o)\n", *data, PC);
        vds_channel = *data & 077;
        break;
    case DATAO:
        sim_debug(DEBUG_DATAIO, &vds_dev, "%012llo (%6o)\n", *data, PC);
        vds_changed[vds_channel] = 1;
        vds_selection[vds_channel] = *data >> 4;
        vds_sync_inhibit[vds_channel] = (*data >> 3) & 1;
        vds_analog[vds_channel] = *data & 7;
        sim_debug(DEBUG_DETAIL, &vds_dev, "Output %d selection %011o\n",
                  vds_channel, vds_selection[vds_channel]);
#if 0
        sim_debug(DEBUG_DETAIL, &vds_dev, "Sync inhibit %d\n",
                  vds_sync_inhibit[vds_channel]);
        sim_debug(DEBUG_DETAIL, &vds_dev, "Analog %d\n",
                  vds_analog[vds_channel]);
#endif
        break;
    }
    return SCPE_OK;
}

t_stat vds_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
    return SCPE_OK;
}

const char *vds_description (DEVICE *dptr)
{
    return "Video Switch";
}
#endif
