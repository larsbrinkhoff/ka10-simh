/* ka10_lds1.c: Evans&Sutherland LDS-1 display.

   Copyright (c) 2020, Lars Brinkhoff

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
#include <time.h>

#ifndef NUM_DEVS_LDS1
#define NUM_DEVS_LDS1 0
#endif

#if (NUM_DEVS_LDS1 > 0)
#include "display/lds1.h"
#include "display/display.h"

/* The LDS-1 device code is the same as for the Type 340. */
#define LDS1_DEVNUM       0130


/*
 * number of (real?) microseconds between svc calls
 * used to age display, poll for WS events
 * and delay "data" interrupt
 * (VB10C could steal cycles)
 */
#define LDS1_CYCLE_US    50

#define STAT_REG        u3
#define XPOS            us9             /* from LP hit */
#define YPOS            us10            /* from LP hit */

/* STAT_REG */
#define STAT_VALID      01000000        /* internal: invisible to PDP-10 */

/* CONO */
#define CONO_PIA          0000007  /* Priority Interrupt Assignment. */
#define CONO_ALL_PIA      0000010  /* Allow Priority Interrupt Assignment. */
#define CONO_STEP         0000040  /* Step. */
#define CONO_CL_HIT       0000100  /* Clear Hit. */
#define CONO_CL_P_STOP    0000200  /* Clear Program Stop. */
#define CONO_CL_IO_STOP   0000400  /* Clear I/O Stop. */
#define CONO_DIS_STOP     0001000  /* Disallow Stop Interrupt. */
#define CONO_ALL_STOP     0002000  /* Allow Stop Interrupt. */
#define CONO_SET_IO_STOP  0004000  /* Set I/O Stop. */
#define CONO_DIS_MAP      0010000  /* Disallow Map/Protect Interrupt. */
#define CONO_ALL_MAP      0020000  /* Allow Map/Protect Interrupt. */
#define CONO_ENA_MEM      0040000  /* Enable Memory Protection and Relocation. */
#define CONO_DIS_MEM      0100000  /* Disallow Memory Alarm Interrupt. */
#define CONO_ALL_MEM      0200000  /* Allow Memory Alarm Interrupt. */
#define CONO_CLEAR        0400000  /* System Clear. */

/* CONI */
#define CONI_PIA          0000007  /* Priority Interrupt Assignment. */
#define CONI_INT          0000010  /* LDS-1 Caused Interupt. */
#define CONI_SCOPE_STOP   0000040  /* Scope Select Violation Stop. */
#define CONI_HIT_STOP     0000100  /* Hit Stop. */
#define CONI_PROGRAM_STOP 0000200  /* Program Stop. */
#define CONI_IO_STOP      0000400  /* I/O Stop. */
#define CONI_MEM_STOP     0001000  /* Memory To Memory Stop. */
#define CONI_STOP_ON      0002000  /* Stop Interrupt On. */
#define CONI_READY        0004000  /* Stopped and Ready. */
#define CONI_MAP_ON       0020000  /* Map/Protect Interrupt On. */
#define CONI_MAP_VIOL     0040000  /* Map/Protect Violation. */
#define CONI_ALARM_ON     0100000  /* Alarm Interrupt On. */
#define CONI_NXM          0200000  /* NXM Alarm. */
#define CONI_PAR          0400000  /* Parity Alarm. */

#define CC_RAR   CC[0]
#define CC_WAR   CC[1]
#define CC_PC    CC[2]
#define CC_SP    CC[3]
#define CC_P1    CC[4]
#define CC_P2    CC[5]
#define CC_DSP   CC[6]
#define CC_RCR   CC[8]
#define CC_WCR   CC[9]
#define CC_DIR   CC[10]
#define CC_RSR   CC[11]
#define CC_SR    CC[12]
#define CC_MAR   CC[13]

#define PROG   001
#define PEEL   002
#define REPT   004
#define EXEC   010

t_stat lds1_devio (uint32 dev, uint64 *data);
t_stat lds1_svc (UNIT *uptr);
t_stat lds1_reset (DEVICE *dptr);
const char *lds1_description (DEVICE *dptr);

static uint64 CC[16];
static unsigned MODE;

DIB lds1_dib[] = {
    { LDS1_DEVNUM, 2, &lds1_devio, NULL }};

UNIT lds1_unit[] = {
    { UDATA (&lds1_svc, UNIT_IDLE, LDS1_CYCLE_US) }
};

#define UPTR(UNIT) (lds1_unit+(UNIT))

DEVICE lds1_dev = {
    "ES", lds1_unit, NULL, NULL,
    NUM_DEVS_LDS1, 0, 0, 0, 0, 0,
    NULL, NULL, lds1_reset,
    NULL, NULL, NULL,
    &lds1_dib, DEV_DISABLE | DEV_DIS | DEV_DEBUG, 0, dev_debug,
    NULL, NULL, NULL, NULL, NULL, &lds1_description
    };

const char *lds1_description (DEVICE *dptr)
{
    return "Evans&Sutherland LDS-1";
}

static void lds1_push (uint64 reg, uint64 mode)
{
    CC_SP--;
    M[CC_SP] = (reg << 23) + CC[reg] + (mode << 18);
}

static void lds1_group_0 (uint64 word)
{
    int reg = word >> 23;

    CC_P2 = CC[reg];
    if (((word >> 22) & 1) == 0) {
        CC[reg] = word & RMASK;
    }
    if ((word >> 32) & 1) {
        uint64 mode = MODE;
        if (((word >> 31) & 1) == 0)
            mode = 0;
        lds1_push (reg, mode);
    }
    if ((word >> 31) & 1) {
        switch ((word >> 18) & 017) {
        case 1:
          MODE |= PROG;
          MODE &= ~(EXEC | REPT);
          break;
        case 2:
          MODE |= PEEL;
          MODE &= ~(EXEC | REPT);
          break;
        case 3:
          MODE |= PEEL;
          MODE &= ~(EXEC | REPT);
          break;
        case 4:
        case 5:
        case 6:
        case 7:
          MODE |= REPT;
          MODE &= ~EXEC;
          break;
        case 010:
        case 011:
        case 012:
        case 013:
          MODE |= EXEC;
          MODE &= ~REPT;
          break;
        case 014:
        case 015:
        case 016:
        case 017:
          MODE |= EXEC;
          MODE |= REPT;
          break;
        }
    }
}

static void lds1_group_1 (uint64 word)
{
}

static void lds1_group_2 (uint64 word)
{
}

static void lds1_group_3 (uint64 word)
{
  int f1 = (word >> 31) & 3;
  int dev = (word >> 27) & 017;
  int reg = (word >> 23) & 017;
  int i = (word >> 22) & 1;
  int count = (word >> 18) & 017;
  int address = word & LMASK;

  if (i == 0) {
    if (f1 & 2)
      CC_DSP = address;
    else
      CC_RAR = address;
  }
}

static void lds1_group_456 (uint64 word)
{
}

static void lds1_group_7 (uint64 word)
{
}

static void lds1_insn (uint64 word)
{
  sim_debug (DEBUG_DETAIL, &lds1_dev, "Instruction: %012llo\n", word);
  switch ((word >> 33) & 7) {
  case 0: lds1_group_0 (word); break;
  case 1: lds1_group_1 (word); break;
  case 2: lds1_group_2 (word); break;
  case 3: lds1_group_3 (word); break;
  case 4:
  case 5:
  case 6: lds1_group_456 (word); break;
  case 7: lds1_group_7 (word); break;
  }
}

t_stat lds1_devio(uint32 dev, uint64 *data) {
    int         unit = (dev - LDS1_DEVNUM) >> 2;
    UNIT        *uptr;

    if (unit < 0 || unit >= NUM_DEVS_LDS1)
        return SCPE_OK;
    uptr = UPTR(unit);

    switch (dev & 3) {
    case CONI:
        *data = (uint64)0;
        sim_debug (DEBUG_CONI, &lds1_dev, "%012llo PC=%06o\n",
                   *data, PC);
        break;

    case CONO:
        clr_interrupt(dev);
        sim_debug(DEBUG_CONO, &lds1_dev, "%012llo PC=%06o\n",
                  *data, PC);
        if (*data & CONO_ALL_PIA)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Allow PIA.\n");
        if (*data & CONO_STEP)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Step.\n");
        if (*data & CONO_CL_HIT)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Clear hit.\n");
        if (*data & CONO_CL_P_STOP)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Clear program stop.\n");
        if (*data & CONO_CL_IO_STOP)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Clear IO stop.\n");
        if (*data & CONO_DIS_STOP)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Disallow stop interrupt.\n");
        if (*data & CONO_ALL_STOP)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Allow stop interrupt.\n");
        if (*data & CONO_SET_IO_STOP)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Set IO stop.\n");
        if (*data & CONO_DIS_MAP)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Disallow map interrupt.\n");
        if (*data & CONO_ALL_MAP)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Allow map interrupt.\n");
        if (*data & CONO_ENA_MEM)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Enable memory protection.\n");
        if (*data & CONO_DIS_MEM)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Disable memory protection.\n");
        if (*data & CONO_ALL_MEM)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "Allow protection interrupt.\n");
        if (*data & CONO_CLEAR)
            sim_debug (DEBUG_DETAIL, &lds1_dev, "System clear.\n");
        break;

    case DATAO:
        sim_debug(DEBUG_DATAIO, &lds1_dev, "DATAO %012llo PC=%06o\n",
                  *data, PC);
        
        lds1_insn (*data);
        break;

    case DATAI:
        *data = 0;
        sim_debug(DEBUG_DATAIO, &lds1_dev, "DATAI %012llo PC=%06o\n",
                  *data, PC);
        break;
    }
    return SCPE_OK;
}

/* Timer service - */
t_stat lds1_svc (UNIT *uptr)
{
    display_age(LDS1_CYCLE_US, 0);       /* age the display */

    lds1_insn (CC_PC);
    CC_PC++;

    return SCPE_OK;
}

/* Reset routine */

t_stat lds1_reset (DEVICE *dptr)
{
    if (!(dptr->flags & DEV_DIS)) {
        display_reset();
        lds1_clear(dptr);
    }
    sim_cancel (&lds1_unit[0]);             /* deactivate unit */
    return SCPE_OK;
}

#endif /* NUM_DEVS_LDS1 */
