/* ka10_clr.c: Color scope display, and Spacewar consoles.

   Copyright (c) 2018, Philip L. Budne
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
   PHILIP BUDNE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   Except as contained in this notice, the name of Philip Budne shall not be
   used in advertising or otherwise to promote the sale, use or other dealings
   in this Software without prior written authorization from Richard Cornwell.

*/

/*
 * CONO
 *      400000 Enable robot console.
 *      373737 Color scole output.
 *          40 Enable Spacewar consoles.
 *          20 Enable color scope.
 * DATAO
 *      777000 X deflection.
 *      000777 Y deflection.
 * DATAI
 *      Reads Spacewar or robot console.
 */

#include "ka10_defs.h"
#include <time.h>

#include "display/display.h"
#include "display/clr.h"

/*
 * MIT Spacewar console switches
 * WCNSLS is the mnemonic defined/used in the SPCWAR sources
 */
#if NUM_DEVS_WCNSLS > 0
#define WCNSLS_DEVNUM 0420

#define DPY_CYCLE_US    50

t_stat wcnsls_devio(uint32 dev, uint64 *data);
const char *wcnsls_description (DEVICE *dptr);
t_stat clr_svc (UNIT *uptr);
t_stat clr_reset (DEVICE *dptr);

DIB wcnsls_dib[] = {
    { WCNSLS_DEVNUM, 1, &wcnsls_devio, NULL }};

UNIT wcnsls_unit[] = {
    { UDATA (&clr_svc, UNIT_IDLE, 0) }};

DEVICE wcnsls_dev = {
    "WCNSLS", wcnsls_unit, NULL, NULL,
    NUM_DEVS_WCNSLS, 0, 0, 0, 0, 0,
    NULL, NULL, clr_reset,
    NULL, NULL, NULL,
    &wcnsls_dib, DEV_DISABLE | DEV_DIS | DEV_DEBUG, 0, NULL,
    NULL, NULL, NULL, NULL, NULL, &wcnsls_description
    };

const char *wcnsls_description (DEVICE *dptr)
{
    return "MIT Spacewar Consoles";
}

t_stat wcnsls_devio(uint32 dev, uint64 *data) {
    static int inited = 0;
    uint64 switches;

    switch (dev & 3) {
    case CONI:
        *data = 0;

    case CONO:
        /* CONO WCNSLS,40       ;enable spacewar consoles */
        if (*data & 020) {
            if (!inited) {
              inited = 1;
              clr_init (&wcnsls_dev, 0);
              sim_activate_after (wcnsls_unit, DPY_CYCLE_US);
            }
            clr_intensities (*data & 0373737);
        }
        break;

    case DATAI:
        switches = 0777777777777LL;     /* 1 is off */

/*
 * map 32-bit "spacewar_switches" value to what PDP-6/10 game expects
 * (four 9-bit bytes)
 */
/* bits inside the bytes */
#define CCW     0400                    /* counter clockwise (L) */
#define CW      0200                    /* clockwise (R) */
#define THRUST  0100
#define HYPER   040
#define FIRE    020

/* shift values for the players' bytes */
#define UR      0               /* upper right: enterprise "top plug" */
#define LR      9               /* lower right: klingon "second plug" */
#define LL      18              /* lower left: thin ship "third plug" */
#define UL      27              /* upper left: fat ship "bottom plug" */

#if 1
#define DEBUGSW(X) (void)0
#else
#define DEBUGSW(X) printf X
#endif

#define SWSW(UC, LC, BIT, POS36, FUNC36) \
        if (spacewar_switches & BIT) {                  \
            switches &= ~(((uint64)FUNC36)<<POS36);     \
            DEBUGSW(("mapping %#o %s %s to %03o<<%d\r\n", \
                    (uint32)BIT, #POS36, #FUNC36, FUNC36, POS36)); \
        }
        SPACEWAR_SWITCHES;
#undef SWSW

        if (spacewar_switches)
            DEBUGSW(("in %#lo out %#llo\r\n", spacewar_switches, switches));

        *data = switches;
        sim_debug(DEBUG_DATAIO, &wcnsls_dev, "WCNSLS %03o DATI %012llo PC=%06o\n",
                  dev, switches, PC);
        break;

    case DATAO:
        clr_deflection ((*data >> 9) & 0777, *data & 0777);
        break;
    }
    return SCPE_OK;
}
#endif

t_stat clr_svc (UNIT *uptr)
{
    display_age(DPY_CYCLE_US, 0);
    sim_activate_after (uptr, DPY_CYCLE_US);
    return SCPE_OK;
}

t_stat clr_reset (DEVICE *dptr)
{
    display_reset();
    sim_cancel(wcnsls_unit);
    return SCPE_OK;
}
