/* ki10_ddc.c: SUMEX RES-10 DDC.

   Copyright (c) 2022, Lars Brinkhoff

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

   This is a device which interfaces with the DDC "drum" or disk
   swapping device used on SUMEX-AIM.
*/

#include "kx10_defs.h"

#if NUM_DEVS_DDC > 0

#define DDC_DEVNUM 0440

/* CONO bits. */
/* TBD */

static t_stat ddc_devio(uint32 dev, uint64 *data);
static t_stat ddc_svc(UNIT *uptr);
static t_stat ddc_reset (DEVICE *dptr);
static const char *ddc_description (DEVICE *dptr);

DIB ddc_dib = { DDC_DEVNUM, 1, &ddc_devio, NULL };

UNIT ddc_unit = {
    UDATA (&ddc_svc, UNIT_IDLE, 0)
};

REG ddc_reg[] = {
    {0}
};

DEVICE ddc_dev = {
    "DDC", &ddc_unit, ddc_reg, NULL,
    1, 8, 0, 1, 8, 36,
    NULL, NULL, &ddc_reset, NULL, NULL, NULL,
    &ddc_dib, DEV_DISABLE | DEV_DIS | DEV_DEBUG, 0, dev_debug,
    NULL, NULL, NULL, NULL, NULL, &ddc_description
};

static t_stat ddc_svc(UNIT *uptr)
{
    return SCPE_OK;
}

t_stat ddc_devio(uint32 dev, uint64 *data)
{
    uint64 bits;

    switch(dev & 07) {
    case CONO:
        bits = *data;
        sim_debug(DEBUG_CONO, &ddc_dev, "%012llo\n", bits);
        break;
    case CONI:
        sim_debug(DEBUG_CONI, &ddc_dev, "%012llo\n", 0LL);
        break;
    case DATAI:
        sim_debug(DEBUG_DATAIO, &ddc_dev, "DATAI %012llo\n", 0LL);
        break;
    case DATAO:
        sim_debug(DEBUG_DATAIO, &ddc_dev, "DATAO %012llo\n", *data);
        break;
    }

    return SCPE_OK;
}

static t_stat ddc_reset (DEVICE *dptr)
{
    return SCPE_OK;
}

const char *ddc_description (DEVICE *dptr)
{
    return "SUMEX RES-10 DDC";
}
#endif
