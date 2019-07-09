/* ka10_nlpt.c: PDP-10 "new" line printer

   Copyright (c) 2019, Lars Brinkhoff

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
#include <ctype.h>

#ifndef NUM_DEVS_NLPT
#define NUM_DEVS_NLPT 0
#endif

#if (NUM_DEVS_NLPT > 0)

#define NLPT_DEVNUM 0464
#define STATUS   u3
#define COL      u4
#define POS      u5
#define LINE     u6

#define UNIT_V_CT    (UNIT_V_UF + 0)
#define UNIT_UC      (1 << UNIT_V_CT)
#define UNIT_UTF8    (2 << UNIT_V_CT)
#define UNIT_CT      (3 << UNIT_V_CT)

#define PI_DONE  000007
#define PI_ERROR 000070
#define DONE_FLG 000100
#define BUSY_FLG 000200
#define ERR_FLG  000400
#define CLR_LPT  002000
#define C96      002000
#define C128     004000
#define DEL_FLG  0100000



t_stat          nlpt_devio(uint32 dev, uint64 *data);
t_stat          nlpt_svc (UNIT *uptr);
t_stat          nlpt_reset (DEVICE *dptr);
t_stat          nlpt_attach (UNIT *uptr, CONST char *cptr);
t_stat          nlpt_detach (UNIT *uptr);
t_stat          nlpt_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag,
                         const char *cptr);
const char     *nlpt_description (DEVICE *dptr);

char            nlpt_buffer[134 * 3];
uint8           nlpt_chbuf[5];             /* Read in Character buffers */

/* LPT data structures

   nlpt_dev      LPT device descriptor
   nlpt_unit     LPT unit descriptor
   nlpt_reg      LPT register list
*/

DIB nlpt_dib = { NLPT_DEVNUM, 1, &nlpt_devio, NULL };

UNIT nlpt_unit = {
    UDATA (&nlpt_svc, UNIT_SEQ+UNIT_ATTABLE+UNIT_TEXT, 0), 100
    };

REG nlpt_reg[] = {
    { DRDATA (STATUS, nlpt_unit.STATUS, 18), PV_LEFT | REG_UNIT },
    { DRDATA (TIME, nlpt_unit.wait, 24), PV_LEFT | REG_UNIT },
    { BRDATA(BUFF, nlpt_buffer, 16, 8, sizeof(nlpt_buffer)), REG_HRO},
    { BRDATA(CBUFF, nlpt_chbuf, 16, 8, sizeof(nlpt_chbuf)), REG_HRO},
    { NULL }
};

MTAB nlpt_mod[] = {
    {UNIT_CT, 0, "Lower case", "LC", NULL},
    {UNIT_CT, UNIT_UC, "Upper case", "UC", NULL},
    {UNIT_CT, UNIT_UTF8, "UTF8 ouput", "UTF8", NULL},
    { 0 }
};

DEVICE nlpt_dev = {
    "NLPT", &nlpt_unit, nlpt_reg, nlpt_mod,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, &nlpt_reset,
    NULL, &nlpt_attach, &nlpt_detach,
    &nlpt_dib, DEV_DISABLE | DEV_DEBUG, 0, dev_debug,
    NULL, NULL, &nlpt_help, NULL, NULL, &nlpt_description
};

/* IOT routine */

t_stat nlpt_devio(uint32 dev, uint64 *data) {
    UNIT *uptr = &nlpt_unit;
    int i, c;

    switch(dev & 3) {
    case CONI:
        *data = 0100;
         sim_debug(DEBUG_CONI, &nlpt_dev, "%012llo PC=%06o\n", *data, PC);
         break;

    case CONO:
         sim_debug(DEBUG_CONO, &nlpt_dev, "%012llo PC=%06o\n", *data, PC);
         break;

    case DATAO:
         sim_debug(DEBUG_DATAIO, &nlpt_dev, "DATAO %012llo PC=%06o\n",
                   *data, PC);
         for (i = 0; i < 5; i++) {
             c = (*data >> 29) & 0177;
             sim_debug(DEBUG_DATA, &nlpt_dev, "Character %03o (%c)\n", c, c);
             switch (c) {
             case 015:
                 fputs ("\r\n", stdout);
                 break;
             case 0177:
                 /* Ignore. */
                 break;
             default:
                 fputc (c, stdout);
                 fflush (stdout);
                 break;
             }
             *data <<= 7;
         }
         break;

    case DATAI:
         *data = 0;
         break;
    }
    return SCPE_OK;
}

t_stat nlpt_svc (UNIT *uptr)
{
    return SCPE_OK;
}

t_stat nlpt_reset (DEVICE *dptr)
{
    UNIT *uptr = &nlpt_unit;
    clr_interrupt(NLPT_DEVNUM);
    sim_cancel (&nlpt_unit);
    return SCPE_OK;
}

t_stat nlpt_attach (UNIT *uptr, CONST char *cptr)
{
    t_stat reason;
    reason = attach_unit (uptr, cptr);
    uptr->STATUS &= ~ERR_FLG;
    clr_interrupt(NLPT_DEVNUM);
    return reason;
}

t_stat nlpt_detach (UNIT *uptr)
{
    uptr->STATUS |= ERR_FLG;
    set_interrupt(NLPT_DEVNUM, uptr->STATUS >> 3);
    return detach_unit (uptr);
}

t_stat nlpt_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
fprintf (st, "New Line Printer (NLPT)\n\n");
fprintf (st, "The line printer (NLPT) writes data to a disk file.  The POS register specifies\n");
fprintf (st, "the number of the next data item to be written.  Thus, by changing POS, the\n");
fprintf (st, "user can backspace or advance the printer.\n");
fprint_set_help (st, dptr);
fprint_show_help (st, dptr);
fprint_reg_help (st, dptr);
return SCPE_OK;
}

const char *nlpt_description (DEVICE *dptr)
{
    return "NLPT line printer";
}

#endif
