/* ka10_tab.c: Sylvania Data Tablet, DT-1.

   Copyright (c) 2018, Lars Brinkhoff

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

   This is a device which interfaces with a Sylvania Data Tablet.
*/

#include <time.h>
#include "sim_video.h"
#include "display/display.h"
#include "ka10_defs.h"

#ifdef USE_DISPLAY
#if NUM_DEVS_TAB > 0
#define RBTCON_DEVNUM   0514

/* CONI/O bits. */
#define TAB_PIA         0000007
#define TAB_DONE        0000010

static t_stat      tab_svc (UNIT *uptr);
static t_stat      tab_devio(uint32 dev, uint64 *data);
static t_stat      tab_reset (DEVICE *dptr);
static const char  *tab_description (DEVICE *dptr);

static uint64 status = 0;
static int key_code = 0;

UNIT                tab_unit[] = {
    {UDATA(tab_svc, UNIT_DISABLE, 0)},  /* 0 */
};
DIB tab_dib = {RBTCON_DEVNUM, 1, &tab_devio, NULL};

MTAB tab_mod[] = {
    { 0 }
    };

DEVICE              tab_dev = {
    "TAB", tab_unit, NULL, tab_mod,
    1, 8, 0, 1, 8, 36,
    NULL, NULL, &tab_reset, NULL, NULL, NULL,
    &tab_dib, DEV_DISABLE | DEV_DIS | DEV_DEBUG, 0, NULL,
    NULL, NULL, NULL, NULL, NULL, &tab_description
};

static t_stat tab_svc (UNIT *uptr)
{
  int c = SCPE_OK;

#ifdef USE_DISPLAY
  if (display_last_char) {
    c = display_last_char | SCPE_KFLAG;
    display_last_char = 0;
  }
#endif

  sim_activate (uptr, 100000);

  return SCPE_OK;
}

t_stat tab_devio(uint32 dev, uint64 *data)
{
    DEVICE *dptr = &tab_dev;

    switch(dev & 07) {
    case CONO:
        status &= ~TAB_PIA;
        status |= *data & TAB_PIA;
        if (status & TAB_PIA)
          sim_activate (tab_unit, 1);
        else
          sim_cancel (tab_unit);
        break;
    case CONI:
        *data = status;
        break;
    case DATAO:
        break;
    case DATAI:
        status &= ~TAB_DONE;
        clr_interrupt(RBTCON_DEVNUM);
        *data = key_code;
        break;
    }

    return SCPE_OK;
}

static t_stat tab_reset (DEVICE *dptr)
{
    return SCPE_OK;
}

const char *tab_description (DEVICE *dptr)
{
    return "Sylvania Data Tablet";
}
#endif
#endif
