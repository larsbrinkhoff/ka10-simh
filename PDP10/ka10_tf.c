/* ka10_tf.c: Telefile DC10 disk control

   Copyright (c) 2026, Lars Brinkhoff

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

   This disk controller was used with the BBN TENEX KA10 machines.
*/

#include "kx10_defs.h"

#ifndef NUM_DEVS_TF
#define NUM_DEVS_TF 0
#endif


#if NUM_DEVS_TF > 0

/*

NDVMAX==10              ;MAX NUMBER OF DRIVES
DSK=700                 ;DEVICE NUMBER
DSKCP=62                ;COMMAND POINTER
NPACKS==10              ;NUMBER OF RPO2 DRIVES
NTKUN=^D203             ;NUMBER OF TRACKS PER DRIVE (UNIT)
NWSEC==100      ;SRI-AIG PACKS HAVE 64 WORD SECTORS
NSECS==^D18             ; 18 SECTORS PER ROTATION,
NSURFS==^D20            ; AND 20 SURFACES PER UNIT
NSECPG==1000/NWSEC      ; 8 SECTORS PER PAGE
;THERE ARE NSURFS*NSECS=360 SECTORS/TRACK
;THERE ARE 200*(NWSEC/1000)=45 PAGES + 0 SECTORS PER TRACK

DSKIRQ==1B32            ;PI REQUEST BIT IN CONI WORD
DCBSY==1B20     ;DISK CONTROLLER BUSY (RH OF CONI)
DDBSY==1B22     ;DISK DRIVE BUSY (RH OF CONI)
DTDON==1B27     ;DISK TRANSFER COMPLETE (RH OF CONI)

; DATAO
DSKNOP==0B22            ; .. NOP
DSKCAL==2B22            ; .. RECALIBRATE
DSKPOS==3B22            ; .. POSITION (SEEK)
DSKRDA==4B22            ;OP CODE FOR READ COMMAND
DSKWDA==6B22            ; .. WRITE

        CONSZ DSK,DCBSY ;CONTROLLER BUSY?
        DATAO DSK,DSKNOP        ;TRY TO RESET CONTROLLER
        IORI 1,DSKWDA+DSKCP     ;1 NOW HAS DATAO WORD FOR DISK
        DATAO DSK,1     ;START XFER

;ATTENTION BIT SET FOR DRIVE IN 1,  POSSIBLE REASONS ARE
; 1) SEEK COMPLETE, 2) SEEK INCOMPLETE (ERROR) , 3) POWER
; ON INTERRUPT, 4) RESTORE OPERATION COMPLETE.
*/


#define TF_DEVNUM   0700
#define TF_NAME     "TF"
#define NUM_UNITS   4

/* Disk pack geometry. */
#define SECTOR_SIZE         64
#define SECTORS             18
#define CYLINDERS          203
#define SURFACES            20
#define CYLINDER_SIZE      (SECTOR_SIZE * SECTORS * SURFACES)
#define PACK_SIZE          (CYLINDER_SIZE * CYLINDERS)

/* CONO bits. */
#define CONO_PIA          0000007
#define CONO_UNIT       CONI_UNIT

/* CONI bits. */
#define CONI_ATTN     00000000377 // Unit attention.
#define CONI_DONE     00000000400 // Transfer complete.
#define CONI_UNIT     00000017000 // Unit.
#define CONI_DBUSY    00000020000 // Drive busy.
#define CONI_DERR     00000640000 // (Drive?) "major" errors.
#define CONI_CBUSY    00000100000 // Controller busy.
#define CONI_RO       00004000000 // Write inhibited/protected.
#define CONI_UNSAFE   00010000000 // Unsafe.
#define CONI_SEEK     00020000000 // Seek incomplete.
#define CONI_OFFLINE  00040000000 // Off line.
#define CONI_ENDCYL   04000000000 // End of cylinder.
//                   600000000000 // Parity error.
//                   167430000000 // "Bad" errors.
//                    77700000000 // Controller errors.
#define CONI_INT      (CONI_ATTN | CONI_DONE | CONI_DERR)

/* DATAO bits. */
#define DATAO_UNIT       CONI_UNIT
#define DATAO_COMMAND    0160000
#define DATAO_NOP        0000000 // No-op.
#define DATAO_CAL        0040000 // Recalibrate.
#define DATAO_SEEK       0060000 // Position, seek.
#define DATAO_READ       0100000 // Read.
#define DATAO_WRITE      0140000 // Write.

static t_stat tf_check_interrupt(const char *place);
static t_stat tf_devio(uint32 dev, uint64 *data);
static t_stat tf_svc(UNIT *);
static t_stat tf_reset(DEVICE *);
static t_stat tf_attach(UNIT *, CONST char *);
static t_stat tf_detach(UNIT *);
static t_stat tf_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag,
                       const char *cptr);
static const char *tf_description (DEVICE *dptr);


static uint64 tf_coni;
static uint64 tf_cono;
static uint64 tf_drive;
static struct df10 tf_df10;

UNIT tf_unit[] = {
    { UDATA (&tf_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+
                 UNIT_ROABLE, PACK_SIZE) },
    { UDATA (&tf_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+
                 UNIT_ROABLE, PACK_SIZE) },
    { UDATA (&tf_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+
                 UNIT_ROABLE, PACK_SIZE) },
    { UDATA (&tf_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+
                 UNIT_ROABLE, PACK_SIZE) },
    { UDATA (&tf_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+
                 UNIT_ROABLE, PACK_SIZE) },
    { UDATA (&tf_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+
                 UNIT_ROABLE, PACK_SIZE) },
    { UDATA (&tf_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+
                 UNIT_ROABLE, PACK_SIZE) },
    { UDATA (&tf_svc, UNIT_FIX+UNIT_ATTABLE+UNIT_DISABLE+
                 UNIT_ROABLE, PACK_SIZE) }
};

DIB tf_dib = { TF_DEVNUM, 1, &tf_devio, NULL };

MTAB tf_mod[] = {
    {0}
};

DEBTAB tf_debug[] = {
    {"IRQ", DEBUG_IRQ, "Debug IRQ requests"},
    {"CMD", DEBUG_CMD, "Show command execution to devices"},
    {"DATA", DEBUG_DATA, "Show data transfers"},
    {"DETAIL", DEBUG_DETAIL, "Show details about device"},
    {"EXP", DEBUG_EXP, "Show exception information"},
    {"CONI", DEBUG_CONI, "Show CONI instructions"},
    {"CONO", DEBUG_CONO, "Show CONO instructions"},
    {"DATAIO", DEBUG_DATAIO, "Show DATAI and DATAO instructions"},
    {0, 0}
};

REG tf_reg[] = {
    {0}
};

DEVICE tf_dev = {
    TF_NAME, tf_unit, tf_reg, tf_mod,
    NUM_UNITS, 8, 18, 1, 8, 36,
    NULL, NULL, &tf_reset, NULL, &tf_attach, &tf_detach,
    &tf_dib, DEV_DISABLE | DEV_DEBUG | DEV_DIS, 0, tf_debug,
    NULL, NULL, &tf_help, NULL, NULL, &tf_description
};

static t_stat tf_check_interrupt(const char *place)
{
  if ((tf_cono & CONO_PIA) != 0 &&
      (tf_coni & CONI_INT) != 0 &&
      (tf_coni & CONI_CBUSY) == 0) {
      sim_debug(DEBUG_IRQ, &tf_dev, "Interrupt from %s due to %03llo.\n",
                place, tf_coni & CONI_INT);
      set_interrupt(TF_DEVNUM, tf_cono);
  } else {
      sim_debug(DEBUG_IRQ, &tf_dev, "Interrupt off.\n");
      clr_interrupt(TF_DEVNUM);
  }
}

static t_stat tf_devio(uint32 dev, uint64 *data) {
    UNIT *uptr = &tf_unit[tf_drive];
    uint64 word;
    uint32 addr;
    switch (dev & 7) {
    case CONI:
        *data = tf_coni;
        *data |= (tf_drive + 1) << 9;
        if (sim_is_active(uptr))
            *data |= CONI_DBUSY;
        if (uptr->flags & UNIT_RO) {
            sim_debug(DEBUG_CONI, &tf_dev, "Drive %lld is read-only\n",
                      tf_drive);
            *data |= CONI_RO;
        }
        if ((uptr->flags & UNIT_ATT) == 0) {
            sim_debug(DEBUG_CONI, &tf_dev, "Drive %lld is offline\n",
                      tf_drive);
            *data |= CONI_OFFLINE;
        }
        sim_debug(DEBUG_CONI, &tf_dev, "%012llo drive %lld PC=%06o\n",
                  *data, tf_drive, PC);
        break;

    case CONO:
        sim_debug(DEBUG_CONO, &tf_dev, "%012llo PC=%06o\n", *data, PC);
        tf_coni &= ~CONI_DONE;
        tf_cono = *data;
        if (*data & CONO_UNIT) {
            tf_drive = ((*data & CONO_UNIT) >> 9) - 1;
            sim_debug(DEBUG_CMD, &tf_dev, "Drive %llo\n", tf_drive);
        }
        tf_check_interrupt("CONO");
        break;

    case DATAI:
        *data = 0;
        sim_debug(DEBUG_DATAIO, &tf_dev, "DATAI, %012llo PC=%06o\n",
                  *data, PC);
        break;

    case DATAO:
        sim_debug(DEBUG_DATAIO, &tf_dev, "DATAO, %012llo PC=%06o\n",
                  *data, PC);
        if (*data & CONO_UNIT) {
            tf_drive = ((*data & CONO_UNIT) >> 9) - 1;
            sim_debug(DEBUG_CMD, &tf_dev, "Drive %llo\n", tf_drive);
        }
        tf_coni &= ~CONI_DONE;
        tf_coni &= ~(1ULL << tf_drive);
        uptr->u3 = *data & DATAO_COMMAND;
        switch (uptr->u3) {
        case DATAO_CAL:
            sim_debug(DEBUG_CMD, &tf_dev, "Recalibrate.\n");
            tf_coni |= CONI_CBUSY | CONI_SEEK;
            sim_activate(&tf_unit[tf_drive], 100);
            break;
        case DATAO_SEEK:
            sim_debug(DEBUG_CMD, &tf_dev, "Seek %012llo\n",
                      *data & ~(DATAO_UNIT | DATAO_COMMAND));
            tf_coni |= CONI_CBUSY | CONI_SEEK;
            sim_activate(&tf_unit[tf_drive], 100);
            break;
        case DATAO_READ:
            tf_coni |= CONI_CBUSY;
            addr = (uint32)(*data & 0777);
            sim_debug(DEBUG_CMD, &tf_dev, "Read, address %06o\n", addr);
            addr = M[addr];
            word = M[addr];
            sim_debug(DEBUG_CMD, &tf_dev, "DF10 address %06o, "
                      "control %06llo,,%06llo\n",
                      addr, word >> 18, word & 0777777);
            //df10_setup(&tf_df10, addr);
            //tf_coni &= ~CONI_CBUSY;
            sim_activate(&tf_unit[tf_drive], 100);
            break;
        case DATAO_WRITE:
            tf_coni |= CONI_CBUSY;
            addr = (uint32)(*data & 0777);
            sim_debug(DEBUG_CMD, &tf_dev, "Write, address %06o\n", addr);
            addr = M[addr];
            word = M[addr];
            sim_debug(DEBUG_CMD, &tf_dev, "DF10 address %06o, "
                      "control %06llo,,%06llo\n",
                      addr, word >> 18, word & 0777777);
            //df10_setup(&tf_df10, addr);
            //tf_coni &= ~CONI_CBUSY;
            sim_activate(&tf_unit[tf_drive], 100);
            break;
        case DATAO_NOP:
            sim_debug(DEBUG_CMD, &tf_dev, "No-op.\n");
            break;
        default:
            sim_debug(DEBUG_CMD, &tf_dev, "Error, unknown command %06o.\n",
                      uptr->u3);
            break;
        }
        tf_check_interrupt("DATAO");
        break;
    }
    return SCPE_OK;
}

t_stat tf_svc (UNIT *uptr)
{
    tf_coni &= ~CONI_CBUSY;

    switch (uptr->u3) {
    case DATAO_CAL:
    case DATAO_SEEK:
        tf_coni |= 1ULL << (uptr - tf_unit);
        tf_coni &= ~CONI_SEEK;
        sim_debug(DEBUG_IRQ, &tf_dev, "Drive %ld calibrate/seek done.\n",
                  uptr - tf_unit);
        break;
    case DATAO_READ:
    case DATAO_WRITE:
        tf_coni |= CONI_DONE;
        sim_debug(DEBUG_IRQ, &tf_dev, "Drive %ld read/write done.\n",
                  uptr - tf_unit);
        break;
    }

    tf_check_interrupt("tf_svc");
    return SCPE_OK;
}

t_stat
tf_reset(DEVICE *dptr)
{
    df10_init(&tf_df10, TF_DEVNUM, 0);
    return SCPE_OK;
}

/* Device attach */
t_stat tf_attach (UNIT *uptr, CONST char *cptr)
{
    t_stat r;
    DEVICE *rptr;
    DIB *dib;

    r = attach_unit (uptr, cptr);
    if (r != SCPE_OK || (sim_switches & SIM_SW_REST) != 0)
        return r;
    return SCPE_OK;
}

/* Device detach */
t_stat tf_detach (UNIT *uptr)
{
    if (!(uptr->flags & UNIT_ATT))
        return SCPE_OK;
    if (sim_is_active (uptr))
        sim_cancel (uptr);
    return detach_unit (uptr);
}

t_stat tf_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
    fprintf (st, "%s\n\n", tf_description (&tf_dev));
    fprint_set_help (st, dptr);
    fprint_show_help (st, dptr);
    fprint_reg_help (st, dptr);
    return SCPE_OK;
}

const char *tf_description (DEVICE *dptr)
{
    return "Telefile DC10 disk controller";
}

#endif
