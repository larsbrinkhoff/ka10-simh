/* icl1900_mt.c: ICL1900 2504 mag tape drive simulator.

   Copyright (c) 2018, Richard Cornwell

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

   Magnetic tapes are represented as a series of variable records
   of the form:

        32b byte count
        byte 0
        byte 1
        :
        byte n-2
        byte n-1
        32b byte count

   If the byte count is odd, the record is padded with an extra byte
   of junk.  File marks are represented by a byte count of 0.

*/

#include "icl1900_defs.h"
#include "sim_tape.h"

#if (NUM_DEVS_MT > 0)
#define BUFFSIZE       (64 * 1024)
#define UNIT_MT              UNIT_ATTABLE | UNIT_DISABLE | UNIT_ROABLE

#define CMD          u3             /* Command */
#define STATUS       u4
#define POS          u6             /* Position within buffer */


/*  Command is packed follows:
 *
 *   Lower 3 bits is command.
 *   Next bit is binary/BCD.
 *   Next bit is disconnect flag.
 *   Top 16 bits are count.
 */

#define MT_CMD      077

#define BUF_EMPTY(u)  (u->hwmark == 0xFFFFFFFF)
#define CLR_BUF(u)     u->hwmark =  0xFFFFFFFF

#define MT_NOP       000
#define MT_FSF       001              /* No Qualifier */
#define MT_BSR       002              /* Qualifier */
#define MT_BSF       003              /* Qualifier */
#define MT_REV_READ  011              /* Qualifier */
#define MT_WRITEERG  012              /* Qualifier */
#define MT_WTM       013              /* Qualifier */
#define MT_TEST      014              /* Qualifier */
#define MT_REW       016              /* No Qualifier */
#define MT_READ      031              /* Qualifier */
#define MT_WRITE     032              /* Qualifier */
#define MT_RUN       036              /* No Qualifier */
#define MT_BOOT      037              /* No Qualifier */

#define MT_QUAL      0100             /* Qualifier expected */
#define MT_BUSY      0200             /* Device running command */

#define ST1_OK         00100          /* Unit OK */
#define ST1_WARN       00200          /* Warning, EOT, BOT, TM */
#define ST1_ERR        00400          /* Parity error, blank, no unit */
#define ST1_CORERR     01000          /* Corrected error */
#define ST1_LONG       02000          /* Long Block */
#define ST1_P2       040              /* P2 Status */

#define ST2_ROWS     0030000          /* Number of rows read */
#define ST2_BLNK     0040000          /* Blank Tape */

#define STQ_TERM     001              /* Operation terminated */
#define STQ_WRP      002              /* Write ring present */
#define STQ_ACCP     004              /* Handler accepted order */
#define STQ_S1       010              /* Controller ready to accept */
#define STQ_S2       020              /* Controller ready to accept */
#define STQ_P1       040              /* P1 Status on */


int  mt_busy;    /* Indicates that controller is talking to a drive */
int  mt_drive;   /* Indicates last selected drive */
uint8 mt_buffer[BUFFSIZE];
void mt_cmd (int dev, uint32 cmd, uint32 *resp);
t_stat mt_svc (UNIT *uptr);
t_stat mt_reset (DEVICE *dptr);
t_stat mt_boot (int32 unit_num, DEVICE * dptr);
t_stat mt_attach(UNIT *, CONST char *);
t_stat mt_detach(UNIT *);
t_stat mt_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
CONST char *mt_description (DEVICE *dptr);

DIB mt_dib = {  WORD_DEV|MULT_DEV, &mt_cmd, NULL, NULL };


MTAB                mt_mod[] = {
    {MTUF_WLK, 0, "write enabled", "WRITEENABLED", NULL},
    {MTUF_WLK, MTUF_WLK, "write locked", "LOCKED", NULL},
    {MTAB_XTD | MTAB_VUN, 0, "FORMAT", "FORMAT",
         &sim_tape_set_fmt, &sim_tape_show_fmt, NULL},
    {MTAB_XTD | MTAB_VDV | MTAB_VALR, 0, "DEV", "DEV",
         &set_chan, &get_chan, NULL, "Device Number"},
    {0}
};

UNIT                mt_unit[] = {
    {UDATA(&mt_svc, UNIT_MT, 0) },       /* 0 */
    {UDATA(&mt_svc, UNIT_MT, 0) },       /* 1 */
    {UDATA(&mt_svc, UNIT_MT, 0) },       /* 2 */
    {UDATA(&mt_svc, UNIT_MT, 0) },       /* 3 */
    {UDATA(&mt_svc, UNIT_MT, 0) },       /* 4 */
    {UDATA(&mt_svc, UNIT_MT, 0) },       /* 5 */
    {UDATA(&mt_svc, UNIT_MT, 0) },       /* 6 */
    {UDATA(&mt_svc, UNIT_MT, 0) },       /* 7 */
};

DEVICE mt_dev = {
    "MT", mt_unit, NULL, mt_mod,
    NUM_DEVS_MT, 8, 22, 1, 8, 22,
    NULL, NULL, &mt_reset, &mt_boot, &mt_attach, &mt_detach,
    &mt_dib, DEV_DISABLE | DEV_DEBUG | UNIT_ADDR(20), 0, dev_debug,
    NULL, NULL, &mt_help, NULL, NULL, &mt_description
    };

void mt_cmd(int dev, uint32 cmd, uint32 *resp) {
    UNIT  *uptr = &mt_unit[mt_drive];
    *resp = 0;
    if (dev & 0400) {
        mt_drive = cmd & 07;
        return;
    }
    if ((uptr->flags & UNIT_ATT) == 0) {
        uptr->CMD = 0;
        return;
    }
    if (uptr->CMD & MT_QUAL) {
        *resp = 5;
        uptr->CMD &= ~MT_QUAL;
    } else {
        switch(cmd & 070) {
        case 000: if (cmd == 0) {
                     *resp = 5;
                     return;
                  }
                  uptr->CMD = cmd;
                  if (cmd != 1)
                       uptr->CMD |= MT_QUAL;
                  break;
        case 010: uptr->CMD = cmd;
                  if (cmd < 016)
                      uptr->CMD |= MT_QUAL;
                  break;
        case 020: if (cmd == SEND_Q) {
                     if (!sim_tape_wrp(uptr))
                         uptr->STATUS |= STQ_WRP;
                     *resp = uptr->STATUS & 037;
                     if (uptr->STATUS & 0777700)
                         *resp |= STQ_P1;
                  } else if (cmd == SEND_P) {
                     *resp = (uptr->STATUS >> 6) & 037;
                     if (uptr->STATUS & 0770000)
                         *resp |= ST1_P2;
                  } else if (cmd == SEND_P2) {
                     *resp = (uptr->STATUS >> 12) & 037;
                  }
                  return;
        case 030: uptr->CMD = cmd;
                  if (cmd < 036)
                       uptr->CMD |= MT_QUAL;
                  break;
        default:
                  return;
        }
    }
        sim_debug(DEBUG_CMD, &mt_dev, "Cmd: unit=%d %02o\n", mt_drive, uptr->CMD);
    if (uptr->flags & MT_BUSY) {
        *resp = 3;
        return;
    }
    if ((uptr->CMD & MT_QUAL) == 0) {
        sim_debug(DEBUG_CMD, &mt_dev, "Cmd: unit=%d start %02o\n", mt_drive, uptr->CMD);
       mt_busy = 1;
       CLR_BUF(uptr);
       uptr->POS = 0;
       uptr->CMD |= MT_BUSY;
       uptr->STATUS = ST1_OK|STQ_ACCP;
       sim_activate(uptr, 100);
    }
    *resp = 5;
}

t_stat mt_svc (UNIT *uptr)
{
    DEVICE      *dptr = &mt_dev;
    int          unit = (uptr - dptr->units);
    int          dev = GET_UADDR(dptr->flags);
    t_mtrlnt     reclen;
    t_stat       r;
    uint8        ch;
    uint32       word;
    int          i;
    int          stop;
    int          eor;

    /* If not busy, false schedule, just exit */
    if ((uptr->CMD & MT_BUSY) == 0)
        return SCPE_OK;
    switch (uptr->CMD & MT_CMD) {
    case MT_BOOT:
    case MT_READ:
         /* If empty buffer, fill */
         if (BUF_EMPTY(uptr)) {
             sim_debug(DEBUG_DETAIL, dptr, "Read unit=%d ", unit);
             if ((r = sim_tape_rdrecf(uptr, &mt_buffer[0], &reclen,
                              BUFFSIZE)) != MTSE_OK) {
                 sim_debug(DEBUG_DETAIL, dptr, " error %d\n", r);
                 uptr->STATUS = ST1_OK|STQ_TERM;
                 if (r == MTSE_TMK)
                     uptr->STATUS |= ST1_WARN;
                 else if (r == MTSE_WRP)
                     uptr->STATUS |= ST1_ERR;
                 else if (r == MTSE_EOM)
                     uptr->STATUS |= ST1_WARN;
                 else
                     uptr->STATUS |= ST1_ERR;
                 uptr->CMD = 0;
                 mt_busy = 0;
                 chan_set_done(dev);
                 return SCPE_OK;
             }
             uptr->hwmark = reclen;
             sim_debug(DEBUG_DETAIL, dptr, "Block %d chars\n", reclen);
         }
         stop = 0;
         /* Grab three chars off buffer */
         word = 0;
         for(i = 16; i >= 0; i-=8) {
             if (uptr->POS >= uptr->hwmark) {
                /* Add in fill characters */
                if (i == 8) {
                   stop = 2;
                } else if (i == 16) {
                   stop = 1;
                }
                break;
             }
             word |= (uint32)mt_buffer[uptr->POS++] << i;
         }
         sim_debug(DEBUG_DATA, dptr, "unit=%d read %08o\n", unit, word);
         eor = chan_input_word(dev, &word, 0);
         if (eor || uptr->POS >= uptr->hwmark) {
             uptr->STATUS = (stop << 12) | ST1_OK|STQ_TERM;
             if (uptr->POS < uptr->hwmark)
                  uptr->STATUS |= ST1_LONG;
             uptr->CMD = 0;
             mt_busy = 0;
             chan_set_done(dev);
             return SCPE_OK;
         }
         sim_activate(uptr, 100);
         break;

    case MT_WRITEERG: /* Write and Erase */
    case MT_WRITE:
         /* Check if write protected */
         if (sim_tape_wrp(uptr)) {
             uptr->STATUS = ST1_OK|STQ_TERM|ST1_ERR;
             uptr->CMD &= ~MT_BUSY;
             mt_busy = 0;
             chan_set_done(dev);
             return SCPE_OK;
         }

         eor = chan_output_word(dev, &word, 0);
         sim_debug(DEBUG_DATA, dptr, "unit=%d write %08o\n", unit, word);

         /* Put three chars in buffer */
         word = 0;
         for(i = 16; i >= 0; i-=8) {
             mt_buffer[uptr->POS++] = (uint8)((word >> i) & 0xff);
         }
         uptr->hwmark = uptr->POS;
         if (eor) {
             /* Done with transfer */
             reclen = uptr->hwmark;
             sim_debug(DEBUG_DETAIL, dptr, "Write unit=%d Block %d chars\n",
                      unit, reclen);
             r = sim_tape_wrrecf(uptr, &mt_buffer[0], reclen);
             uptr->STATUS = ST1_OK|STQ_TERM;
             if (r != MTSE_OK)
                uptr->STATUS |= ST1_ERR;
             uptr->CMD = 0;
             mt_busy = 0;
             chan_set_done(dev);
             return SCPE_OK;
         }
         sim_activate(uptr, 100);
         break;

    case MT_REV_READ:
         /* If empty buffer, fill */
         if (BUF_EMPTY(uptr)) {
             if (sim_tape_bot(uptr)) {
                 uptr->STATUS = ST1_OK|ST1_WARN|STQ_TERM;
                 uptr->CMD = 0;
                 mt_busy = 0;
                 chan_set_done(dev);
                 return SCPE_OK;
             }
             sim_debug(DEBUG_DETAIL, dptr, "Read rev unit=%d ", unit);
             if ((r = sim_tape_rdrecr(uptr, &mt_buffer[0], &reclen,
                              BUFFSIZE)) != MTSE_OK) {
                 sim_debug(DEBUG_DETAIL, dptr, " error %d\n", r);
                 uptr->STATUS = ST1_OK|STQ_TERM;
                 if (r == MTSE_TMK)
                     uptr->STATUS |= ST1_WARN;
                 else if (r == MTSE_WRP)
                     uptr->STATUS |= ST1_ERR;
                 else if (r == MTSE_EOM)
                     uptr->STATUS |= ST1_WARN;
                 else
                     uptr->STATUS |= ST1_ERR;
                 uptr->CMD = 0;
                 mt_busy = 0;
                 chan_set_done(dev);
                 return SCPE_OK;
             }
             uptr->POS = reclen;
             uptr->hwmark = reclen;
             sim_debug(DEBUG_DETAIL, dptr, "Block %d chars\n", reclen);
         }
             /* Grab three chars off buffer */
             word = 0;
             for(i = 0; i <= 16; i+=8) {
                 word |= (uint32)mt_buffer[--uptr->POS] << i;
                 if (uptr->POS == 0) {
                    stop = 1;
                    break;
                 }
             }
         sim_debug(DEBUG_DATA, dptr, "unit=%d read %08o\n", unit, word);
         eor = chan_input_word(dev, &word, 0);
         if (eor || uptr->POS == 0) {
             uptr->STATUS = (stop << 12) | ST1_OK|STQ_TERM;
             if (uptr->POS != 0)
                  uptr->STATUS |= ST1_LONG;
             uptr->CMD = 0;
             mt_busy = 0;
             chan_set_done(dev);
             return SCPE_OK;
         }
         sim_activate(uptr, 100);
         break;

    case MT_FSF:
         switch(uptr->POS) {
         case 0:
              uptr->POS ++;
              sim_activate(uptr, 500);
              break;
         case 1:
              sim_debug(DEBUG_DETAIL, dptr, "Skip rec unit=%d ", unit);
              r = sim_tape_sprecf(uptr, &reclen);
              if (r == MTSE_TMK) {
                  uptr->POS++;
                  sim_debug(DEBUG_DETAIL, dptr, "MARK\n");
                  sim_activate(uptr, 50);
              } else if (r == MTSE_EOM) {
                  uptr->POS++;
                  uptr->STATUS = ST1_WARN;
                  sim_activate(uptr, 50);
              } else {
                  sim_debug(DEBUG_DETAIL, dptr, "%d\n", reclen);
                  sim_activate(uptr, 10 + (10 * reclen));
              }
              break;
         case 2:
              uptr->STATUS = ST1_OK|(ST1_WARN & uptr->STATUS) | STQ_TERM;
              uptr->CMD = 0;
              mt_busy = 0;
              chan_set_done(dev);
         }
         break;

    case MT_WTM:
         if (uptr->POS == 0) {
             if (sim_tape_wrp(uptr)) {
                 uptr->STATUS = ST1_OK|ST1_ERR|STQ_TERM;
                 uptr->CMD = 0;
                 mt_busy = 0;
                 chan_set_done(dev);
                 return SCPE_OK;
             }
             uptr->POS ++;
             sim_activate(uptr, 500);
         } else {
             sim_debug(DEBUG_DETAIL, dptr, "Write Mark unit=%d\n", unit);
             r = sim_tape_wrtmk(uptr);
             uptr->STATUS = ST1_OK|STQ_TERM;
             if (r != MTSE_OK)
                 uptr->STATUS |= ST1_ERR;
              uptr->CMD = 0;
             mt_busy = 0;
             chan_set_done(dev);
         }
         break;

    case MT_BSR:
         switch (uptr->POS ) {
         case 0:
              if (sim_tape_bot(uptr)) {
                  uptr->STATUS = ST1_OK|ST1_WARN|STQ_TERM;
                  uptr->CMD = 0;
                  mt_busy = 0;
                  chan_set_done(dev);
                  break;
              }
              uptr->POS ++;
              sim_activate(uptr, 500);
              break;
         case 1:
              sim_debug(DEBUG_DETAIL, dptr, "Backspace rec unit=%d ", unit);
              r = sim_tape_sprecr(uptr, &reclen);
              /* We don't set EOF on BSR */
              uptr->STATUS = ST1_OK|STQ_TERM;
              if (r == MTSE_TMK || r == MTSE_BOT) {
                  uptr->STATUS |= ST1_WARN;
              }
              uptr->CMD = 0;
              mt_busy = 0;
              chan_set_done(dev);
         }
         break;

    case MT_BSF:
         switch (uptr->POS ) {
         case 0:
              if (sim_tape_bot(uptr)) {
                  uptr->STATUS = ST1_OK|ST1_WARN|STQ_TERM;
                  uptr->CMD = 0;
                  mt_busy = 0;
                  chan_set_done(dev);
                  break;
              }
              uptr->POS ++;
              sim_activate(uptr, 500);
              break;
         case 1:
              sim_debug(DEBUG_DETAIL, dptr, "Backspace rec unit=%d ", unit);
              r = sim_tape_sprecr(uptr, &reclen);
              /* We don't set EOF on BSR */
              if (r == MTSE_TMK || r == MTSE_BOT) {
                  uptr->POS++;
                  sim_activate(uptr, 50);
              } else {
                  sim_debug(DEBUG_DETAIL, dptr, "%d \n", reclen);
                  sim_activate(uptr, 10 + (10 * reclen));
              }
              break;
         case 2:
              uptr->STATUS = ST1_OK|STQ_TERM;
              uptr->CMD = 0;
              mt_busy = 0;
              chan_set_done(dev);
         }
         break;

    case MT_REW:
         if (uptr->POS == 0) {
             uptr->POS ++;
             sim_activate(uptr, 30000);
             mt_busy = 0;
         } else {
             sim_debug(DEBUG_DETAIL, dptr, "Rewind unit=%d\n", unit);
             r = sim_tape_rewind(uptr);
              uptr->CMD = 0;
             uptr->STATUS = ST1_OK|STQ_TERM;
             chan_set_done(dev);
         }
         break;

    }
    return SCPE_OK;
}


/* Reset */

t_stat mt_reset (DEVICE *dptr)
{
    UNIT *uptr = dptr->units;
    int unit;

    for (unit = 0; unit < dptr->numunits; unit++, uptr++) {
       uptr->CMD = 0;
       uptr->STATUS = 0;
       if ((uptr->flags & UNIT_ATT) != 0)
           uptr->STATUS = ST1_OK;
       mt_busy = 0;
    }
    chan_clr_done(GET_UADDR(dptr->flags));
    return SCPE_OK;
}

/* Boot from given device */
t_stat
mt_boot(int32 unit_num, DEVICE * dptr)
{
    UNIT    *uptr = &dptr->units[unit_num];
    int     chan = GET_UADDR(dptr->flags);

    if ((uptr->flags & UNIT_ATT) == 0)
        return SCPE_UNATT;      /* attached? */

    M[64 + chan] = 0;
    M[256 + 4 * chan] = B2;
    M[257 + 4 * chan] = 020;
    loading = 1;
    mt_busy = 1;
    CLR_BUF(uptr);
    uptr->CMD = MT_BUSY|MT_BOOT;
    uptr->STATUS = ST1_OK|STQ_ACCP;
    sim_activate (uptr, 100);
    return SCPE_OK;
}

t_stat
mt_attach(UNIT * uptr, CONST char *file)
{
    t_stat              r;
    DEVICE             *dptr = &mt_dev;
    int                 unit = (uptr - dptr->units);

    if ((r = sim_tape_attach_ex(uptr, file, 0, 0)) != SCPE_OK)
       return r;
    uptr->STATUS = ST1_OK;
    return SCPE_OK;
}

t_stat
mt_detach(UNIT * uptr)
{
    uptr->STATUS = 0;
    return sim_tape_detach(uptr);
}


t_stat mt_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cmt)
{
fprintf (st, "The Paper Tape Reader can be set to one of twp modes: 7P, or 7B.\n\n");
fprintf (st, "  mode \n");
fprintf (st, "  7P    Process even parity input tapes. \n");
fprintf (st, "  7B    Ignore parity of input data.\n");
fprintf (st, "The default mode is 7B.\n");
return SCPE_OK;
}

CONST char *mt_description (DEVICE *dptr)
{
    return "MT";

}
#endif
