/* ka10_imp.c: IMP, interface message processor.

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

   This emulates the MIT-AI/ML/MC Host/IMP interface.
*/


#include "ka10_defs.h"
#include "sim_ether.h"
#include "sim_imp.h"

#define KAIMP_DEVNUM  0460

/* CONI */
#define IMPID       010 /* Input done. */
#define IMPI32      020 /* Input in 32 bit mode. */
#define IMPIB       040 /* Input busy. */
#define IMPOD      0100 /* Output done. */
#define IMPO32     0200 /* Output in 32-bit mode. */
#define IMPOB      0400 /* Output busy. */
#define IMPERR    01000 /* IMP error. */
#define IMPR      02000 /* IMP ready. */
#define IMPIC     04000 /* IMP interrupt condition. */
#define IMPHER   010000 /* Host error. */
#define IMPHR    020000 /* Host ready. */
#define IMPIHE   040000 /* Inhibit interrupt on host error. */
#define IMPLW   0100000 /* Last IMP word. */

/* CONO */
#define IMPIDC      010 /* Clear input done */
#define IMI32S      020 /* Set 32-bit output */
#define IMI32C      040 /* Clear 32-bit output */
#define IMPODC     0100 /* Clear output done */
#define IMO32S     0200 /* Set 32-bit input */
#define IMO32C     0400 /* Clear 32-bit input */
#define IMPODS    01000 /* Set output done */
#define IMPIR     04000 /* Enable interrupt on IMP ready */
#define IMPHEC   010000 /* Clear host error */
#define IMIIHE   040000 /* Inbihit interrupt on host error */
#define IMPLHW  0200000 /* Set last host word. */

/* CONI timeout.  If no CONI instruction is executed for 3-5 seconds,
   the interface will raise the host error signal. */
#define CONI_TIMEOUT 3000000

t_stat         kaimp_devio(uint32 dev, uint64 *data);
t_stat         kaimp_srv(UNIT *);
t_stat         kaimp_reset (DEVICE *dptr);
const char     *kaimp_description (DEVICE *dptr);
t_stat         kaimp_showmac (FILE* st, UNIT* uptr, int32 val, CONST void* desc);
t_stat         kaimp_setmac  (UNIT* uptr, int32 val, CONST char* cptr, void* desc);
t_stat         imp_set_mpx (UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat         imp_show_mpx (FILE *st, UNIT *uptr, int32 val, CONST void *desc);

static IMP      imp;
static uint16   pia = 0;
static uint32   status = 0;
static uint64   ibuf = 0;
static uint64   obuf = 0;
static int32    last_coni;
static int      ibits = 0;
static int      obits = 0;
static int      imp_mpx_lvl = 0;
static ETH_MAC  kaimp_mac;


UNIT kaimp_unit[] = {
    {UDATA(kaimp_srv, UNIT_DISABLE, 0)},  /* 0 */
};
DIB kaimp_dib = {KAIMP_DEVNUM, 1, &kaimp_devio, NULL};

MTAB kaimp_mod[] = {
    { MTAB_XTD|MTAB_VDV|MTAB_VALR|MTAB_NC, 0, "MAC", "MAC=xx:xx:xx:xx:xx:xx",
      &kaimp_setmac, &kaimp_showmac, NULL, "MAC address" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALR, 0, "MPX", "MPX",
      &imp_set_mpx, &imp_show_mpx, NULL},
    { 0 }
    // ipaddr
    // gwaddr
    // if
    // mac
    };

DEVICE imp_dev = {
    "KAIMP", kaimp_unit, NULL, kaimp_mod,
    1, 8, 0, 1, 8, 36,
    NULL, NULL, kaimp_reset, NULL, NULL, NULL,
    &kaimp_dib, DEV_DISABLE | DEV_DIS | DEV_DEBUG, 0, NULL,
    NULL, NULL, NULL, NULL, NULL, &kaimp_description
};

static void check_interrupts (void)
{
    clr_interrupt (KAIMP_DEVNUM);

    if (status & (IMPERR | IMPIC) == IMPERR)
        set_interrupt(KAIMP_DEVNUM, pia);
    if (status & (IMPR | IMPIC) == (IMPR | IMPIC))
        set_interrupt(KAIMP_DEVNUM, pia);
    if (status & (IMPHER | IMPIHE) == IMPHER)
        set_interrupt(KAIMP_DEVNUM, pia);
    if (status & IMPID) {
        if (status & IMPLW)
            set_interrupt(KAIMP_DEVNUM, pia);
        else
            set_interrupt_mpx(KAIMP_DEVNUM, pia, imp_mpx_lvl);
    }
    if (status & IMPOD)
        set_interrupt_mpx(KAIMP_DEVNUM, pia, imp_mpx_lvl + 1);
}

t_stat kaimp_devio(uint32 dev, uint64 *data)
{
    DEVICE *dptr = &imp_dev;
    UNIT *uptr = kaimp_unit;

    switch(dev & 07) {
    case CONO:
        pia = *data & 7;
        if (*data & IMPIDC) //Clear input done.
            status &= ~IMPID;
        if (*data & IMI32S) //Set 32-bit input.
            status |= IMPI32;
        if (*data & IMI32C) //Clear 32-bit input
            status &= ~IMPI32;
        if (*data & IMPODC) //Clear output done.
            status &= ~IMPOD;
        if (*data & IMO32C) //Clear 32-bit output.
            status &= ~IMPO32;
        if (*data & IMO32S) //Set 32-bit output.
            status |= IMPO32;
        if (*data & IMPODS) //Set output done.
            status |= IMPOD;
        if (*data & IMPIR) { //Enable interrup on IMP ready.
            status |= IMPIC;
            status &= ~IMPERR;
        }
        if (*data & IMPHEC) { //Clear host error.
            /* Only if there has been a CONI lately. */
            if (last_coni - sim_interval < CONI_TIMEOUT)
                status &= ~IMPHER;
        }
        if (*data & IMIIHE) //Inhibit interrupt on host error.
            status |= IMPIHE;
        if (*data & IMPLHW) //Last host word.
            status |= IMPLHW;
        break;
    case CONI:
        last_coni = sim_interval;
        *data = (status | pia);
        break;
    case DATAO:
        obuf = *data;
        obits = (status & IMPO32) ? 32 : 36;
        status |= IMPOB;
        status &= ~IMPOD;
        sim_activate(uptr, 2000);
        break;
    case DATAI:
        *data = ibuf;
        ibuf = 0;
        ibits = 0;
        status |= IMPIB;
        status &= ~(IMPID | IMPLW);
        sim_activate(uptr, 2000);
        break;
    }

    check_interrupts ();
    return SCPE_OK;
}

t_stat kaimp_srv(UNIT * uptr)
{
    DEVICE *dptr = find_dev_from_unit(uptr);

    if (status & IMPOB) {
        int last = (obits == 1) && (status & IMPLHW);
        imp_send_bits (&imp, (obuf >> 35) & 1, 1, last);
        obuf <<= 1;
        obits--;
        if (obits == 0) {
            status |= IMPOD;
            status &= ~(IMPOB | IMPLHW);
        }
    }

    /*
    if (! IMP Ready) {
        status |= IMPERR;
    }
    */

    if (last_coni - sim_interval > CONI_TIMEOUT) {
        /* If there has been no CONI for a while, raise host error. */
        status |= IMPHER;
    }

    check_interrupts ();

    sim_activate(uptr, 1000);
    return SCPE_OK;
}

const char *kaimp_description (DEVICE *dptr)
{
    return "KA Host/IMP interface";
}

t_stat imp_set_mpx (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    int32 mpx;
    t_stat r;

    if (cptr == NULL)
        return SCPE_ARG;
    mpx = (int32) get_uint (cptr, 8, 8, &r);
    if (r != SCPE_OK)
        return r;
    imp_mpx_lvl = mpx;
    return SCPE_OK;
}

t_stat imp_show_mpx (FILE *st, UNIT *uptr, int32 val, CONST void *desc)
{
   if (uptr == NULL)
      return SCPE_IERR;

   fprintf (st, "MPX=%o", imp_mpx_lvl);
   return SCPE_OK;
}

t_stat kaimp_showmac (FILE* st, UNIT* uptr, int32 val, CONST void* desc)
{
    char buffer[20];
    eth_mac_fmt(&kaimp_mac, buffer);
    fprintf(st, "MAC=%s", buffer);
    return SCPE_OK;
}

t_stat kaimp_setmac (UNIT* uptr, int32 val, CONST char* cptr, void* desc)
{
    t_stat status;

    if (!cptr) return SCPE_IERR;
    if (uptr->flags & UNIT_ATT) return SCPE_ALATT;

    status = eth_mac_scan_ex(&kaimp_mac, cptr, uptr);
    if (status != SCPE_OK)
      return status;

    return SCPE_OK;
}

static int kaimp_receive_bit (int bit, int last)
{
    if ((status & IMPIB) == 0)
        /* Busy now, not ready to receive a bit. */
        return SCPE_ARG;

    ibuf |= (uint64)(bit&1) << (35-ibits);
    ibits++;
    if (last)
        status |= IMPLW;
    if (ibits == (status & IMPI32 ? 32 : 36) || (status & IMPLW)) {
        /* Received a full word, or IMP said this was the last bit. */
        ibuf &= ~(0777777777777LL >> ibits);
        status &= ~IMPIB;
        status |= IMPID;
        check_interrupts ();
    }

    return SCPE_OK;
}

t_stat kaimp_reset (DEVICE *dptr)
{
    imp_reset (&imp);
    imp.receive_bit = kaimp_receive_bit;
    last_coni = sim_interval;
    return SCPE_OK;
}
