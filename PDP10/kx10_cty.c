/* kx10_cty.c: PDP6, KA-10 and KI-10 front end (console terminal) simulator

   Copyright (c) 2013-2020, Richard Cornwell

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
   RICHARD CORNWELL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Richard Cornwell shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Richard Cornwell

*/

#include "kx10_defs.h"
#if PDP6 | KA | KI
#define UNIT_DUMMY      (1 << UNIT_V_UF)

extern int32 tmxr_poll;
t_stat ctyi_svc (UNIT *uptr);
t_stat ctyo_svc (UNIT *uptr);
t_stat cty_reset (DEVICE *dptr);
t_stat cty_stop_os (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat tty_set_mode (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat cty_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char *cty_description (DEVICE *dptr);

/* CTY data structures

   cty_dev       CTY device descriptor
   cty_unit      CTY unit descriptor
   cty_reg       CTY register list
*/
#define TEL_RDY           0010
#define TEL_BSY           0020
#define KEY_RDY           0040
#define KEY_BSY           0100
#define KEY_TST          04000
#define CTY_DEVNUM        0120
/* MIT AI lab extensions. */
#define PORT_NUMBER    0070000
#define ALL_INTERRUPT  0100000
#define PORT_SELECT    0400000
#define MAX_PORTS      8

#define PIA               u5
#define PORT              (cty_unit[0].u6)
#define DATA(unit)        (((unsigned char *)((unit)->up7))[PORT])
#define STATUS            (cty_status[PORT])
#define STATUS0           (cty_status[0])

t_stat cty_devio(uint32 dev, uint64 *data);

static unsigned char ctyi_data[MAX_PORTS], ctyo_data[MAX_PORTS];
static uint32 cty_status[MAX_PORTS];

DIB cty_dib = { CTY_DEVNUM, 1, cty_devio, NULL};

UNIT cty_unit[] = {
    { UDATA (&ctyo_svc, TT_MODE_7B, 0), 10000 },
    { UDATA (&ctyi_svc, TT_MODE_7B|UNIT_IDLE, 0), 0 },
    };


MTAB cty_mod[] = {
    { UNIT_DUMMY, 0, NULL, "STOP", &cty_stop_os },
    { TT_MODE, TT_MODE_UC, "UC", "UC", &tty_set_mode },
    { TT_MODE, TT_MODE_7B, "7b", "7B", &tty_set_mode },
    { TT_MODE, TT_MODE_8B, "8b", "8B", &tty_set_mode },
    { TT_MODE, TT_MODE_7P, "7p", "7P", &tty_set_mode },
    { 0 }
    };

REG cty_reg[] = {
    { HRDATAD (WRU, sim_int_char, 8, "interrupt character") },
    { 0 }
    };

DEVICE cty_dev = {
    "CTY", cty_unit, cty_reg, cty_mod,
    2, 10, 31, 1, 8, 8,
    NULL, NULL, &cty_reset,
    NULL, NULL, NULL, &cty_dib, DEV_DEBUG, 0, dev_debug,
    NULL, NULL, &cty_help, NULL, NULL, &cty_description
    };

t_stat cty_devio(uint32 dev, uint64 *data) {
     uint64     res;
     switch(dev & 3) {
     case CONI:
        res = cty_unit[0].PIA | (STATUS & (TEL_RDY | TEL_BSY));
        res |= STATUS & (KEY_RDY | KEY_BSY);
        res |= STATUS & KEY_TST;
        *data = res;
        sim_debug(DEBUG_CONI, &cty_dev, "CTY %03o CONI %06o\n", dev, (uint32)*data);
        break;
     case CONO:
         res = *data;
         if (res & PORT_SELECT)
             PORT = (res & PORT_NUMBER) >> 12;
         STATUS0 &= ~ALL_INTERRUPT;
         STATUS0 |= res & ALL_INTERRUPT;
         cty_unit[0].PIA = res & 07;
         cty_unit[1].PIA = res & 07;
         cty_unit[0].PIA &= ~(KEY_TST);
         STATUS &= ~((res >> 4) & (TEL_RDY | TEL_BSY));
         STATUS |= (res & (TEL_RDY | TEL_BSY | KEY_TST));
         STATUS &= ~((res >> 4) & (KEY_RDY | KEY_BSY));
         STATUS |= (res & (KEY_RDY | KEY_BSY));
         if (STATUS & (TEL_RDY | KEY_RDY))
             set_interrupt(dev, cty_unit[0].PIA);
         else
             clr_interrupt(dev);
         sim_debug(DEBUG_CONO, &cty_dev, "CTY %03o CONO %06o\n", dev, (uint32)*data);
         break;
     case DATAI:
         res = DATA(&cty_unit[1]) & 0xff;
         STATUS &= ~KEY_RDY;
         if ((STATUS & TEL_RDY) == 0)
             clr_interrupt(dev);
         *data = res;
         sim_debug(DEBUG_DATAIO, &cty_dev, "CTY %03o DATAI %06o\n", dev, (uint32)*data);
         break;
    case DATAO:
         DATA(&cty_unit[0]) = *data & 0x7f;
         STATUS &= ~TEL_RDY;
         STATUS |= TEL_BSY;
         if ((STATUS & KEY_RDY) == 0)
             clr_interrupt(dev);
         sim_activate(&cty_unit[0], cty_unit[0].wait);
         sim_debug(DEBUG_DATAIO, &cty_dev, "CTY %03o DATAO %06o\n", dev, (uint32)*data);
         break;
    }
    return SCPE_OK;
}



t_stat ctyo_svc (UNIT *uptr)
{
    unsigned char *data = (unsigned char *)uptr->up7;
    t_stat  r;
    int32   ch;

    if (data[0] != 0) {
        ch = sim_tt_outcvt ( data[0], TT_GET_MODE (uptr->flags)) ;
        if ((r = sim_putchar_s (ch)) != SCPE_OK) {   /* output; error? */
            sim_activate (uptr, uptr->wait);               /* try again */
            return ((r == SCPE_STALL)? SCPE_OK: r);        /* !stall? report */
        }
    }
    STATUS &= ~TEL_BSY;
    STATUS |= TEL_RDY;
    if ((STATUS0 && ALL_INTERRUPT) || PORT == 0)
        set_interrupt(CTY_DEVNUM, uptr->PIA);
    return SCPE_OK;
}

t_stat ctyi_svc (UNIT *uptr)
{
    unsigned char *data = (unsigned char *)uptr->up7;
    int32 ch;

    sim_clock_coschedule (uptr, tmxr_poll);
                                                       /* continue poll */
    if (STATUS & KEY_RDY)
        return SCPE_OK;
    if ((ch = sim_poll_kbd ()) < SCPE_KFLAG)           /* no char or error? */
        return ch;
    if (ch & SCPE_BREAK)                               /* ignore break */
        return SCPE_OK;
    data[0] = 0177 & sim_tt_inpcvt(ch, TT_GET_MODE (uptr->flags));
    data[0] = ch & 0177;
    STATUS |= KEY_RDY;
    if ((STATUS0 && ALL_INTERRUPT) || PORT == 0)
        set_interrupt(CTY_DEVNUM, uptr->PIA);
    return SCPE_OK;
}

/* Reset */

t_stat cty_reset (DEVICE *dptr)
{
    int i;
    PORT = 0;
    cty_unit[0].up7 = ctyi_data;
    cty_unit[1].up7 = ctyo_data;
    for (i = 0; i < MAX_PORTS; i++)
        cty_status[i] &= ~(TEL_RDY | TEL_BSY | KEY_RDY | KEY_BSY);
    clr_interrupt(CTY_DEVNUM);
    sim_clock_coschedule (&cty_unit[1], tmxr_poll);

    return SCPE_OK;
}

/* Stop operating system */

t_stat cty_stop_os (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
#if ITS
    if (cpu_unit[0].flags & UNIT_ITSPAGE)
        M[037] = FMASK;
    else
#endif
    M[CTY_SWITCH] = 1;                                 /* tell OS to stop */
    return SCPE_OK;
}

t_stat tty_set_mode (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    cty_unit[0].flags = (cty_unit[0].flags & ~TT_MODE) | val;
    cty_unit[1].flags = (cty_unit[1].flags & ~TT_MODE) | val;
    return SCPE_OK;
}

t_stat cty_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
fprintf (st, "To stop the cpu use the command:\n\n");
fprintf (st, "    sim> SET CTY STOP\n\n");
#if ITS
fprintf (st, "If the CPU is in standard mode, this will write 1 to location\n\n");
fprintf (st, "%03o, causing TOPS10 to stop.  If the CPU is in ITS mode, this\n\n", CTY_SWITCH);
fprintf (st, "will write -1 to location 037, causing ITS to stop.\n\n");
#else
fprintf (st, "This will write a 1 to location %03o, causing TOPS10 to stop\n\n", CTY_SWITCH);
#endif
fprintf (st, "The additional terminals can be set to one of four modes: UC, 7P, 7B, or 8B.\n\n");
fprintf (st, "  mode  input characters        output characters\n\n");
fprintf (st, "  UC    lower case converted    lower case converted to upper case,\n");
fprintf (st, "        to upper case,          high-order bit cleared,\n");
fprintf (st, "        high-order bit cleared  non-printing characters suppressed\n");
fprintf (st, "  7P    high-order bit cleared  high-order bit cleared,\n");
fprintf (st, "                                non-printing characters suppressed\n");
fprintf (st, "  7B    high-order bit cleared  high-order bit cleared\n");
fprintf (st, "  8B    no changes              no changes\n\n");
fprintf (st, "The default mode is 7P.  In addition, each line can be configured to\n");
fprintf (st, "behave as though it was attached to a dataset, or hardwired to a terminal:\n\n");
fprint_reg_help (st, &cty_dev);
return SCPE_OK;
}

const char *cty_description (DEVICE *dptr)
{
    return "Console TTY Line";
}
#endif
