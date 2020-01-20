/* sel32_scfi.c: SEL-32 SCFI SCSI Disk controller

   Copyright (c) 2018-2019, James C. Bevier
   Portions provided by Richard Cornwell and other SIMH contributers

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
   JAMES C. BEVIER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "sel32_defs.h"

extern t_stat set_dev_addr(UNIT *uptr, int32 val, CONST char *cptr, void *desc);
extern t_stat show_dev_addr(FILE * st, UNIT *uptr, int32 v, CONST void *desc);
extern void chan_end(uint16 chan, uint8 flags);
extern int  chan_read_byte(uint16 chsa, uint8 *data);
extern int  chan_write_byte(uint16 chsa, uint8 *data);
extern void set_devattn(uint16 addr, uint8 flags);
extern t_stat chan_boot(uint16 addr, DEVICE *dptr);
extern int test_write_byte_end(uint16 chsa);

extern uint32   M[];                            /* our memory */
extern uint32   SPAD[];                         /* cpu SPAD memory */

#ifdef NUM_DEVS_SCFI
#define UNIT_V_TYPE        (UNIT_V_UF + 0)
#define UNIT_TYPE          (0xf << UNIT_V_TYPE)

#define GET_TYPE(x)        ((UNIT_TYPE & (x)) >> UNIT_V_TYPE)
#define SET_TYPE(x)        (UNIT_TYPE & ((x) << UNIT_V_TYPE))
#define UNIT_SCFI          UNIT_ATTABLE | UNIT_IDLE

/* INCH command information */
/*
WD 0 - Data address
WD 1 - Flags - 0 -36 byte count

Data - 224 word INCH buffer address
WD 1 Drive 0 Attribute register 
WD 2 Drive 1 Attribute register 
WD 3 Drive 2 Attribute register 
WD 4 Drive 3 Attribute register 
WD 5 Drive 4 Attribute register 
WD 6 Drive 5 Attribute register 
WD 7 Drive 6 Attribute register 
WD 8 Drive 7 Attribute register 

Memory attribute register layout
bits 0-7 - Flags
        bits 0&1 - 00=Reserved, 01=MHD, 10=FHD, 11=MHD with FHD option
        bit  2   - 1=Cartridge module drive
        bit  3   - 0=Reserved
        bit  4   - 1=Drive not present
        bit  5   - 1=Dual Port
        bit  6   - 0=Reserved
        bit  7   - 0=Reserved
bits 8-15 - sector count (sectors per track)(F16=16, F20=20)
bits 16-23 - MHD Head count (number of heads on MHD)
bits 24-31 - FHD head count (number of heads on FHD or number head on FHD option of mini-module)
*/


/* 224 word INCH Buffer layout */
/* 128 word subchannel status storage (SST) */
/*  66 words of program status queue (PSQ) */
/*  26 words of scratchpad */
/*   4 words of label buffer registers */

#define CMD     u3
/* u3 */
/* in u3 is device command code and status */
#define DSK_CMDMSK       0x00ff                 /* Command being run */
#define DSK_STAR         0x0100                 /* STAR value in u4 */
#define DSK_NU2          0x0200                 /*                    */
#define DSK_READDONE     0x0400                 /* Read finished, end channel */
#define DSK_ENDDSK       0x0800                 /* Sensed end of disk */
#define DSK_SEEKING      0x1000                 /* Disk is currently seeking */
#define DSK_READING      0x2000                 /* Disk is reading data */
#define DSK_WRITING      0x4000                 /* Disk is writing data */
#define DSK_BUSY         0x8000                 /* Flag to send a CUE */
/* commands */
#define DSK_INCH           0x00                 /* Initialize channel */
#define DSK_INCH2          0xf0                 /* Initialize channel for processing */
#define DSK_WD             0x01                 /* Write data */
#define DSK_RD             0x02                 /* Read data */
#define DSK_NOP            0x03                 /* No operation */
#define DSK_SNS            0x04                 /* Sense */
#define DSK_SCK            0x07                 /* Seek cylinder, track, sector */
#define DSK_TIC            0x08                 /* Transfer in channel */
#define DSK_FNSK           0x0B                 /* Format for no skip */
#define DSK_LPL            0x13                 /* Lock protected label */
#define DSK_LMR            0x1F                 /* Load mode register */
#define DSK_RES            0x23                 /* Reserve */
#define DSK_WSL            0x31                 /* Write sector label */
#define DSK_RSL            0x32                 /* Read sector label */
#define DSK_REL            0x33                 /* Release */
#define DSK_XEZ            0x37                 /* Rezero */
#define DSK_POR            0x43                 /* Priority Override */
#define DSK_IHA            0x47                 /* Increment head address */
#define DSK_SRM            0x4F                 /* Set reserve track mode */
#define DSK_WTL            0x51                 /* Write track label */
#define DSK_RTL            0x52                 /* Read track label */
#define DSK_XRM            0x5F                 /* Reset reserve track mode */
#define DSK_RAP            0xA2                 /* Read angular positions */
#define DSK_TESS           0xAB                 /* Test STAR (subchannel target address register) */
#define DSK_ICH            0xFF                 /* Initialize Controller */

#define STAR    u4
/* u4 - sector target address register (STAR) */
/* Holds the current cylinder, head(track), sector */
#define DISK_CYL           0xFFFF0000           /* cylinder mask */
#define DISK_TRACK         0x0000FF00           /* track mask */
#define DISK_SECTOR        0x000000ff           /* sector mask */

#define SNS     u5
/* u5 */
/* Sense byte 0  - mode register */
#define SNS_DROFF          0x80000000           /* Drive Carriage will be offset */
#define SNS_TRKOFF         0x40000000           /* Track offset: 0=positive, 1=negative */
#define SNS_RDTMOFF        0x20000000           /* Read timing offset = 1 */
#define SNS_RDSTRBT        0x10000000           /* Read strobe timing: 1=positive, 0=negative */
#define SNS_DIAGMOD        0x08000000           /* Diagnostic Mode ECC Code generation and checking */
#define SNS_RSVTRK         0x04000000           /* Reserve Track mode: 1=OK to write, 0=read only */
#define SNS_FHDOPT         0x02000000           /* FHD or FHD option = 1 */
#define SNS_RESERV         0x01000000           /* Reserved */

/* Sense byte 1 */
#define SNS_CMDREJ         0x800000             /* Command reject */
#define SNS_INTVENT        0x400000             /* Unit intervention required */
#define SNS_SPARE1         0x200000             /* Spare */
#define SNS_EQUCHK         0x100000             /* Equipment check */
#define SNS_DATCHK         0x080000             /* Data Check */
#define SNS_OVRRUN         0x040000             /* Data overrun/underrun */
#define SNS_DSKFERR        0x020000             /* Disk format error */
#define SNS_DEFTRK         0x010000             /* Defective track encountered */

/* Sense byte 2 */
#define SNS_LAST           0x8000               /* Last track flag encountered */
#define SNS_AATT           0x4000               /* At Alternate track */
#define SNS_WPER           0x2000               /* Write protection error */
#define SNS_WRL            0x1000               /* Write lock error */
#define SNS_MOCK           0x0800               /* Mode check */
#define SNS_INAD           0x0400               /* Invalid memory address */
#define SNS_RELF           0x0200               /* Release fault */
#define SNS_CHER           0x0100               /* Chaining error */

/* Sense byte 3 */
#define SNS_REVL           0x80                 /* Revolution lost */
#define SNS_DADE           0x40                 /* Disc addressing or seek error */
#define SNS_BUCK           0x20                 /* Buffer check */
#define SNS_ECCS           0x10                 /* ECC error in sector label */
#define SNS_ECCD           0x08                 /* ECC error iin data */
#define SNS_ECCT           0x04                 /* ECC error in track label */
#define SNS_RTAE           0x02                 /* Reserve track access error */
#define SNS_UESS           0x01                 /* Uncorrectable ECC error */

#define ATTR    u6
/* u6 */
/* u6 holds drive attribute entry */
/* provided by inch command for controller */
/*
bits 0-7 - Flags
        bits 0&1 - 00=Reserved, 01=MHD, 10=FHD, 11=MHD with FHD option
        bit  2   - 1=Cartridge module drive
        bit  3   - 0=Reserved
        bit  4   - 1=Drive not present
        bit  5   - 1=Dual Port
        bit  6   - 0=Reserved
        bit  7   - 0=Reserved
bits 8-15 - sector count (sectors per track)(F16=16, F20=20)
bits 16-23 - MHD Head count (number of heads on MHD)
bits 24-31 - FHD head count (number of heads on FHD or number head on FHD option of mini-module)
*/

#define DDATA   up7
/* Pointer held in up7 */
/* sects/cylinder = sects/track * numhds */
/* allocated during attach command for each unit defined */
struct ddata_t
{
    uint16      cyl;                            /* Cylinder head at */
    uint16      tpos;                           /* Track position */
    uint16      spos;                           /* Sector position */
};

/* disk definition structure */
struct scfi_t
{
    const char  *name;                          /* Device ID Name */
    uint32      taus;                           /* total allocation units */
    uint16      bms;                            /* bit map size */
    uint16      nhds;                           /* Number of heads */
    uint16      ssiz;                           /* sector size in words */
    uint16      spt;                            /* # sectors per track(cylinder) */
    uint8       spau;                           /* # sectors per allocation unit */
    uint8       spb;                            /* # sectors per block (192 WDS)*/
    uint32      cyl;                            /* Number of cylinders */
    uint8       type;                           /* Device type code */
}

scfi_type[] =
{
    /* Class F Disc Devices */
    {"MH040",  20000,  625,   5, 192, 20, 2, 1,  400, 0x40},   /*0 411  40 M */
    {"MH080",  40000, 1250,   5, 192, 20, 2, 1,  800, 0x40},   /*1 823  80 M */
    {"MH160",  80000, 1250,  10, 192, 20, 4, 1, 1600, 0x40},   /*2 823 160 M */
    {"MH300",  76000, 2375,  19, 192, 20, 4, 1,  800, 0x40},   /*3 823 300 M */
    {"MH1GB",  87400, 2732,   1, 192, 20,16, 1,69920, 0x40},   /*4 69920 1000 M */
    {"SG038",  54752, 1711,   1, 192, 20, 8, 1,21900, 0x40},   /*5 21900   38 M */
    {"SG120", 174848, 5464,   1, 192, 20, 8, 1,69939, 0x40},   /*6 69939  120 M */
    {"SG076", 116808, 3491,   1, 192, 20, 8, 1,46723, 0x40},   /*7 46723   76 M */
    {"FH005",   5120,  184,   4, 192, 20, 1, 1,   64, 0x80},   /*8  64    5 M */
    {"CD032",   8000,  250,   1, 192, 20, 2, 1,  800, 0x60},   /*9 823   32 M */
    {"CD032",   8000,  250,   1, 192, 20, 2, 1,  800, 0x60},   /*10 823  32 M */
    {"CD064",   8000,  250,   1, 192, 20, 2, 1,  800, 0x60},   /*11 823  64 M */
    {"CD064",  24000,  750,   3, 192, 20, 2, 1,  800, 0x60},   /*12 823  64 M */
    {"CD096",   8000,  250,   1, 192, 20, 2, 1,  800, 0x60},   /*13 823  96 M */
    {"CD096",  40000, 1250,   5, 192, 20, 2, 1,  800, 0x60},   /*14 823  96 M */
    {"MH600",  80000, 2500,  40, 192, 20, 8, 1,  800, 0x40},   /*15 843 600 M */
    {"FM600",  80000, 2500,  40, 192, 20, 8, 1,  800, 0x40},   /*16 843 600 M */
    {"FM600",   1600,   50,  40, 192, 20, 1, 1,    2, 0x80},   /*17  10 600 M */
    {NULL, 0}
};

#if 0
*****************************************************************
*                  DEVICE ID TABLE                                     
*****************************************************************
         SPACE                                                          
         BOUND     1W                                                   
DID.TBL  EQU       $                                                    
*                                                                       
*DEVICE ID NAME..................................................      
*TOTAL ALLOC. UNITS.....................................        :      
*BIT MAP SIZE      ..............................      :        :       
*NO. OF HEADS      ........................     :      :        :       
*SECTOR SIZE       ...................    :     :      :        :       
*SECTORS/TRACK     ..............    :    :     :      :        :       
*SECTORS/ALOC. UNIT..........   :    :    :     :      :        :       
*SECTORS/BLOCK     .......  :   :    :    :     :      :        :       
*OLD DEVICE ID NAME....  :  :   :    :    :     :      :        :       
*                     :  :  :   :    :    :     :      :        :       
*               ......:..:..:...:....:....:.....:......:........:       
DID      FORM        32, 8, 8,  8,   8,  16,   16,    32,      64       
         SPACE                                                          
*        CLASS 'F' EXTENDED I/O DISC DEVICES
         DID    C'DF01', 3, 3, 26,  64,   2,     ,  1334, C'FL001'
         DID    C'DF02', 1, 2, 20, 192,   5,  625, 20000, C'MH040'
         DID    C'DF03', 1, 2, 20, 192,   5, 1250, 40000, C'MH080'
         DID    C'DF04', 1, 4, 20, 192,  19, 2375, 76000, C'MH300'
         DID    C'DF0E', 1,16, 20, 192,   1, 2732, 87400, C'MH1GB'
         DID    C'DF05', 1, 1, 20, 192,   4,  184,  5120, C'FH005'
         DID    C'DF06', 1, 2, 20, 192,   1,  250,  8000, C'CD032'
         DID    C'DF07', 1, 2, 20, 192,   3,  750, 24000, C'CD064'
         DID    C'DF08', 1, 2, 20, 192,   1,  250,  8000, C'CD096'
         DID    C'DF08', 1, 2, 20, 192,   5, 1250, 40000, C'CD096'
         DID    C'DF09', 1, 8, 20, 192,  40, 2500, 80000, C'MH600'
         DID    C'DF0A', 1, 8, 20, 192,  40, 2500, 80000, C'FM600'
         DID    C'DF0B', 1, 8, 20, 192,   1, 1711,  5472, C'SG038'
         DID    C'DF0C', 1, 8, 20, 192,   1, 5464,174848, C'SG120'
         DID    C'DF0D', 1, 8, 20, 192,   1, 3491,116808, C'SG076'
         DID    C'DF0B', 1, 8, 20, 192,   1, 1711, 54752, C'SG038'
         DID    C'DF0C', 1, 8, 20, 192,   1, 5464,174848, C'SG120'
         DID    C'DF0D', 1, 8, 20, 192,   1, 3491,116808, C'SG076'
*
#endif

uint8   scfi_preio(UNIT *uptr, uint16 chan);
uint8   scfi_startcmd(UNIT *uptr, uint16 chan, uint8 cmd);
uint8   scfi_haltio(uint16 addr);
t_stat  scfi_srv(UNIT *);
t_stat  scfi_boot(int32, DEVICE *);
void    scfi_ini(UNIT *, t_bool);
t_stat  scfi_reset(DEVICE *);
t_stat  scfi_attach(UNIT *, CONST char *);
t_stat  scfi_detach(UNIT *);
t_stat  scfi_set_type(UNIT *uptr, int32 val, CONST char *cptr, void *desc);
t_stat  scfi_get_type(FILE * st, UNIT *uptr, int32 v, CONST void *desc);
t_stat  scfi_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
const char  *scfi_description (DEVICE *dptr);

/* channel program information */
CHANP           sda_chp[NUM_UNITS_SCFI] = {0};

MTAB            scfi_mod[] = {
    {MTAB_XTD | MTAB_VUN | MTAB_VALR, 0, "TYPE", "TYPE",
    &scfi_set_type, &scfi_get_type, NULL, "Type of disk"},
    {MTAB_XTD | MTAB_VUN | MTAB_VALR, 0, "DEV", "DEV", &set_dev_addr,
        &show_dev_addr, NULL, "Device channel address"},
    {0}
};

UNIT            sda_unit[] = {
/* SET_TYPE(6) SG120 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(6), 0), 0, UNIT_ADDR(0x400)},  /* 0 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(6), 0), 0, UNIT_ADDR(0x410)},  /* 1 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(6), 0), 0, UNIT_ADDR(0x420)},  /* 2 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(6), 0), 0, UNIT_ADDR(0x430)},  /* 3 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(6), 0), 0, UNIT_ADDR(0x440)},  /* 4 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(6), 0), 0, UNIT_ADDR(0x450)},  /* 5 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(6), 0), 0, UNIT_ADDR(0x460)},  /* 6 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(6), 0), 0, UNIT_ADDR(0x470)},  /* 7 */
};

//DIB sda_dib = {scfi_preio, scfi_startcmd, NULL, NULL, NULL, scfi_ini, sda_unit, sda_chp, NUM_UNITS_SCFI, 0x0f, 0x0400, 0, 0, 0};

DIB             sda_dib = {
    scfi_preio,     /* uint8 (*pre_io)(UNIT *uptr, uint16 chan)*/   /* Pre Start I/O */
    scfi_startcmd,  /* uint8 (*start_cmd)(UNIT *uptr, uint16 chan, uint8 cmd)*/ /* Start a command */
    NULL,           /* uint8 (*halt_io)(UNIT *uptr) */          /* Stop I/O */
    NULL,           /* uint8 (*test_io)(UNIT *uptr) */          /* Test I/O */
    NULL,           /* uint8 (*post_io)(UNIT *uptr) */          /* Post I/O */
    scfi_ini,       /* void  (*dev_ini)(UNIT *, t_bool) */      /* init function */
    sda_unit,       /* UNIT* units */                           /* Pointer to units structure */
    sda_chp,        /* CHANP* chan_prg */                       /* Pointer to chan_prg structure */
    NUM_UNITS_SCFI, /* uint8 numunits */                        /* number of units defined */
    0xF0,           /* uint8 mask */                            /* 16 devices - device mask */
    0x0400,         /* uint16 chan_addr */                      /* parent channel address */
    0,              /* uint32 chan_fifo_in */                   /* fifo input index */
    0,              /* uint32 chan_fifo_out */                  /* fifo output index */
    {0}             /* uint32 chan_fifo[FIFO_SIZE] */           /* interrupt status fifo for channel */
};

DEVICE          sda_dev = {
    "SDA", sda_unit, NULL, scfi_mod,
    NUM_UNITS_SCFI, 16, 24, 4, 16, 32,
    NULL, NULL, &scfi_reset, &scfi_boot, &scfi_attach, &scfi_detach,
    &sda_dib, DEV_DISABLE|DEV_DEBUG, 0, dev_debug,
    NULL, NULL, &scfi_help, NULL, NULL, &scfi_description
};

#if NUM_DEVS_SCFI > 1
/* channel program information */
CHANP           sdb_chp[NUM_UNITS_SCFI] = {0};

UNIT            sdb_unit[] = {
/* SET_TYPE(4) DM1GB */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(4), 0), 0, UNIT_ADDR(0xC00)},  /* 0 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(4), 0), 0, UNIT_ADDR(0xC10)},  /* 1 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(4), 0), 0, UNIT_ADDR(0xC20)},  /* 2 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(4), 0), 0, UNIT_ADDR(0xC30)},  /* 3 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(4), 0), 0, UNIT_ADDR(0xC40)},  /* 4 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(4), 0), 0, UNIT_ADDR(0xC50)},  /* 5 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(4), 0), 0, UNIT_ADDR(0xC60)},  /* 6 */
    {UDATA(&scfi_srv, UNIT_SCFI|SET_TYPE(4), 0), 0, UNIT_ADDR(0xC70)},  /* 7 */
};

//DIB sdb_dib = {scfi_preio, scfi_startcmd, NULL, NULL, NULL, scfi_ini, sdb_unit, sdb_chp, NUM_UNITS_SCFI, 0x0f, 0x0c00, 0, 0, 0};

DIB             sdb_dib = {
    scfi_preio,     /* uint8 (*pre_io)(UNIT *uptr, uint16 chan)*/   /* Pre Start I/O */
    scfi_startcmd,  /* uint8 (*start_cmd)(UNIT *uptr, uint16 chan, uint8 cmd)*/ /* Start a command */
    NULL,           /* uint8 (*halt_io)(UNIT *uptr) */          /* Stop I/O */
    NULL,           /* uint8 (*test_io)(UNIT *uptr) */          /* Test I/O */
    NULL,           /* uint8 (*post_io)(UNIT *uptr) */          /* Post I/O */
    scfi_ini,       /* void  (*dev_ini)(UNIT *, t_bool) */      /* init function */
    sdb_unit,       /* UNIT* units */                           /* Pointer to units structure */
    sdb_chp,        /* CHANP* chan_prg */                       /* Pointer to chan_prg structure */
    NUM_UNITS_SCFI, /* uint8 numunits */                        /* number of units defined */
    0xF0,           /* uint8 mask */                            /* 16 devices - device mask */
    0x0C00,         /* uint16 chan_addr */                      /* parent channel address */
    0,              /* uint32 chan_fifo_in */                   /* fifo input index */
    0,              /* uint32 chan_fifo_out */                  /* fifo output index */
    0,              /* uint32 chan_fifo[FIFO_SIZE] */           /* interrupt status fifo for channel */
};

DEVICE          sdb_dev = {
    "SDB", sdb_unit, NULL, scfi_mod,
    NUM_UNITS_SCFI, 16, 24, 4, 16, 32,
    NULL, NULL, &scfi_reset, &scfi_boot, &scfi_attach, &scfi_detach,
    &sdb_dib, DEV_DISABLE|DEV_DEBUG, 0, dev_debug,
    NULL, NULL, &scfi_help, NULL, NULL, &scfi_description
};
#endif

/* start a disk operation */
uint8  scfi_preio(UNIT *uptr, uint16 chan)
{
    DEVICE         *dptr = find_dev_from_unit(uptr);
    int            unit = (uptr - dptr->units);

    sim_debug(DEBUG_CMD, dptr, "scfi_preio CMD %08x unit=%02x\n", uptr->CMD, unit);
    if ((uptr->CMD & 0xff00) != 0) {            /* just return if busy */
        return SNS_BSY;
    }
    sim_debug(DEBUG_CMD, dptr, "scfi_preio unit=%02x\n", unit);
    return 0;                                   /* good to go */
}

uint8  scfi_startcmd(UNIT *uptr, uint16 chan,  uint8 cmd) {
    uint16      addr = GET_UADDR(uptr->CMD);
    DEVICE      *dptr = find_dev_from_unit(uptr);
    int         unit = (uptr - dptr->units);
    uint8       ch;

    sim_debug(DEBUG_CMD, dptr, "scfi_startcmd unit %02x cmd %04x CMD %08x\n", unit, cmd, uptr->CMD);
    if ((uptr->flags & UNIT_ATT) == 0) {        /* unit attached status */
        uptr->SNS |= SNS_INTVENT;                /* unit intervention required */
        if (cmd != DSK_SNS)                     /* we are completed with unit check status */
            return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
    }

    if ((uptr->CMD & DSK_CMDMSK) != 0) {
        uptr->CMD |= DSK_BUSY;                   /* Flag we we are busy */
        return SNS_BSY;
    }
    if ((uptr->CMD & 0xff00) != 0) {             /* if any status info, we are busy */
        return SNS_BSY;
    }
    sim_debug(DEBUG_CMD, dptr, "scfi_startcmd CMD 2 unit=%02x cmd %02x\n", unit, cmd);

    if ((uptr->flags & UNIT_ATT) == 0) {        /* see if unit is attached */
        if (cmd == DSK_SNS) {                   /* not attached, is cmd Sense 0x04 */
            sim_debug(DEBUG_CMD, dptr, "scfi_startcmd CMD sense\n");
            /* bytes 0,1 - Cyl entry from STAR reg in CMD */
            ch = (uptr->CMD >> 24) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "scfi_startcmd sense STAR b0 unit=%02x 1 %02x\n", unit, ch);
            chan_write_byte(addr, &ch) ;
            ch = (uptr->CMD >> 16) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "scfi_startcmd sense STAR b1 unit=%02x 1 %02x\n", unit, ch);
            chan_write_byte(addr, &ch) ;
            /* byte 2 - Track entry from STAR reg in CMD */
            ch = (uptr->CMD >> 8) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "scfi_startcmd sense STAR b2 unit=%02x 1 %02x\n", unit, ch);
            chan_write_byte(addr, &ch) ;
            /* byte 3 - Sector entry from STAR reg in CMD */
            ch = (uptr->CMD) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "scfi_startcmd sense STAR b3 unit=%02x 1 %02x\n", unit, ch);
            chan_write_byte(addr, &ch) ;
            /* bytes 4 - mode reg, byte 0 of SNS */
            ch = (uptr->SNS >> 24) & 0xff;       /* return the sense data for device */
            sim_debug(DEBUG_DETAIL, dptr, "scfi_startcmd sense unit=%02x 1 %02x\n", unit, ch);
            chan_write_byte(addr, &ch) ;
            /* bytes 5-7 - status bytes, bytes 1-3 of SNS */
            ch = (uptr->SNS >> 16) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "scfi_startcmd sense unit=%02x 2 %02x\n", unit, ch);
            chan_write_byte(addr, &ch) ;
            ch = (uptr->SNS >> 8) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "scfi_startcmd sense unit=%02x 3 %02x\n", unit, ch);
            chan_write_byte(addr, &ch) ;
            ch = (uptr->SNS) & 0xff;
            sim_debug(DEBUG_DETAIL, dptr, "scfi_startcmd sense unit=%02x 4 %02x\n", unit, ch);
            chan_write_byte(addr, &ch) ;
            /* bytes 8-11 - drive attribute register (DATR) entries from uptr->ATTR via INCH cmd */
            ch = (uptr->ATTR >> 24) & 0xff;
            chan_write_byte(addr, &ch) ;
            ch = (uptr->ATTR >> 16) & 0xff;
            chan_write_byte(addr, &ch) ;
            ch = (uptr->ATTR >> 8 ) & 0xff;
            chan_write_byte(addr, &ch) ;
            ch = (uptr->ATTR >> 0) & 0xff;
            chan_write_byte(addr, &ch) ;
            /* bytes 12 & 13 contain drive related status */
            ch = 0;     /* zero for now */
            chan_write_byte(addr, &ch) ;
            chan_write_byte(addr, &ch) ;

            uptr->SNS &= 0xff000000;             /* clear status bytes, but leave mode data */
            return SNS_CHNEND|SNS_DEVEND;
        }
        if (cmd == 0x00)                         /* INCH cmd gives unit check */
           return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
            
        uptr->SNS |= (SNS_INTVENT|SNS_CMDREJ);   /* set new error status */
        return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;   /* we done */
    }

    /* Unit is online, so process a command */
    switch (cmd) {

    case DSK_INCH:              /* INCH 0x00 */
    {
        uint32  mema;                           /* memory address */    
        uint32  i;
        UNIT    *up = dptr->units;              /* first unit for this device */
        sim_debug(DEBUG_CMD, dptr,
            "scfi_startcmd starting inch cmd addr %04x CMD %08x\n", addr, uptr->CMD);
        /* CMD has IOCD word 1 contents.  For the disk processor it contains */
        /* a pointer to the INCH buffer followed by 8 drive attribute words that */
        /* contains the flags, sector count, MHD head count, and FHD count */
        /* us9 has the byte count from IOCD wd2 and should be 0x24 (36) */
        /* the INCH buffer address must be returned in CMD and us9 left non-zero */
        /* just return OK and channel software will use up8 as status buffer */
        mema = (uint32)uptr->CMD;                /* get memory address of buffer */
        uptr->CMD = M[mema>>2];                  /* get status buffer address for XIO return status */
        sim_debug(DEBUG_CMD, dptr,
            "scfi_startcmd starting inch cmd addr %04x CMD %08x mema %08x units %02x\n",
            addr, uptr->CMD, mema, dptr->numunits);
        /* the next 8 words have drive data for each unit */
        /* WARNING 8 drives must be defined for this controller */
        /* so we will not have a map fault */
        for (i=0; i<dptr->numunits && i<8; i++) {   /* process all drives */
            up->ATTR = M[(mema>>2)+i+1];          /* save each unit's drive data */
            sim_debug(DEBUG_CMD, dptr,
                "scfi_startcmd ATTR data %08x unit %02x flags %02x sec %02x MHD %02x FHD %02x\n",
                up->ATTR, i, (up->ATTR >> 24)&0xff, (up->ATTR >> 16)&0xff,
                (up->ATTR >> 8)&0xff, (up->ATTR&0xff));
            up++;                               /* next unit for this device */
        }
        sim_debug(DEBUG_CMD, dptr, "scfi_startcmd done INCH cmd addr %04x\n", addr);
//      return SNS_CHNEND|SNS_DEVEND;
//      break;
        uptr->CMD |= DSK_INCH2;                 /* use 0xf0 for inch, just need int */
        sim_activate(uptr, 20);                 /* start things off */
        return (0);
        break;
    }

    case DSK_SCK:                               /* Seek command 0x07 */
    case DSK_XEZ:                               /* Rezero & Read IPL record 0x1f */
        uptr->CMD &= ~(DSK_STAR);                /* show we do not have seek STAR in CMD */
    case DSK_WD:                                /* Write command 0x01 */
    case DSK_RD:                                /* Read command 0x02 */
    case DSK_LMR:                               /* read mode register */

        uptr->CMD |= cmd;                        /* save cmd */
        sim_debug(DEBUG_CMD, dptr,
                "scfi_startcmd done with disk seek r/w cmd %02x addr %04x\n", cmd, addr);
        sim_activate(uptr, 20);                 /* start things off */
        return 0;
        break;

    case DSK_NOP:                   /* NOP 0x03 */
//      return SNS_CHNEND|SNS_DEVEND;           /* return OK */
        uptr->CMD |= cmd;                        /* save cmd */
        sim_activate(uptr, 20);                 /* start things off */
        return 0;
        break;

    case DSK_SNS:                               /* Sense 0x04 */
        uptr->CMD |= cmd;                        /* save cmd */
        sim_activate(uptr, 20);                 /* start things off */
        return 0;
        break;
    }
    sim_debug(DEBUG_CMD, dptr,
            "scfi_startcmd done with scfi_startcmd %02x addr %04x SNS %08x\n",
            cmd, addr, uptr->SNS);
    if (uptr->SNS & 0xff)
        return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
    sim_activate(uptr, 20);                     /* start things off */
    return SNS_CHNEND|SNS_DEVEND;
}

/* Handle processing of disk requests. */
t_stat scfi_srv(UNIT *uptr)
{
    uint16          chsa = GET_UADDR(uptr->CMD);
    DEVICE          *dptr = find_dev_from_unit(uptr);
    DIB             *dibp = (DIB *)dptr->ctxt;  /* get pointer to Dev Info Blk for this device */
    CHANP           *chp = (CHANP *)dibp->chan_prg; /* get pointer to channel program */
    struct ddata_t  *data = (struct ddata_t *)(uptr->up7);
    int             cmd = uptr->CMD & DSK_CMDMSK;
    int             type = GET_TYPE(uptr->flags);
    int32           trk, cyl;
    int             unit = (uptr - dptr->units);
    int             len;
    int             dlen = 0;                   /* total bytes processed */
    int             i;
    uint8           ch;
//  int             tsize = scfi_type[type].spt * scfi_type[type].ssiz*4; /* get track size in bytes */
    uint16          ssize = scfi_type[type].ssiz*4;    /* Size of one sector in bytes */
    int32           tstart = 0;                 /* Location of start of cyl/track/sect in data */
    uint8           buf2[1024];
    uint8           buf[1024];

    sim_debug(DEBUG_DETAIL, &sda_dev, "scfi_srv entry unit %02x cmd %02x chsa %04x chan %04x count %04x\n",
        unit, cmd, chsa, chsa>>8, chp->ccw_count);

//    if ((uptr->flags & UNIT_ATT) == 0) {      /* unit attached status */
//        return SCPE_OK;
//    }
    if ((uptr->flags & UNIT_ATT) == 0) {        /* unit attached status */
        uptr->SNS |= SNS_INTVENT;                /* unit intervention required */
        if (cmd != DSK_SNS)                     /* we are completed with unit check status */
            return SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK;
    }

    sim_debug(DEBUG_CMD, dptr, "scfi_srv cmd=%02x chsa %04x count %04x\n", cmd, chsa, chp->ccw_count);
    switch (cmd) {
    case 0:                                     /* No command, stop disk */
         break;

    case DSK_INCH2:                             /* use 0xff for inch, just need int */
        uptr->CMD &= ~(0xffff);                 /* remove old cmd */
        sim_debug(DEBUG_CMD, dptr,
                "scfi_srv cmd INCH chsa %04x count %04x completed\n", chsa, chp->ccw_count);
        chan_end(chsa, SNS_CHNEND);             /* return just channel end */
        break;

    case DSK_NOP:                               /* NOP 0x03 */
        uptr->CMD &= ~(0xffff);                 /* remove old cmd */
        sim_debug(DEBUG_CMD, dptr,
                "scfi_srv cmd NOP chsa %04x count %04x completed\n", chsa, chp->ccw_count);
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);  /* return OK */
        break;

    case DSK_SNS: /* 0x4 */
        ch = uptr->SNS & 0xff;
        sim_debug(DEBUG_DETAIL, dptr, "scfi_srv sense unit=%02x 1 %02x\n", unit, ch);
        chan_write_byte(chsa, &ch) ;
        ch = (uptr->SNS >> 8) & 0xff;
        sim_debug(DEBUG_DETAIL, dptr, "scfi_srv sense unit=%02x 2 %02x\n", unit, ch);
        chan_write_byte(chsa, &ch) ;
        ch = 0;
        sim_debug(DEBUG_DETAIL, dptr, "scfi_srv sense unit=%02x 3 %02x\n", unit, ch);
        chan_write_byte(chsa, &ch) ;
        ch = unit;
        sim_debug(DEBUG_DETAIL, dptr, "scfi_srv sense unit=%02x 4 %02x\n", unit, ch);
        chan_write_byte(chsa, &ch) ;
        ch = 4;
        sim_debug(DEBUG_CMD, dptr, "DISK SENSE %02x chars complete %08x, unit %02x\n",
                ch, uptr->SNS, unit);
        uptr->CMD &= ~(0xff00);
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
        break;

    case DSK_SCK:            /* Seek cylinder, track, sector 0x07 */

        /* If we are waiting on seek to finish, check if there yet. */
        if (uptr->CMD & DSK_SEEKING) {
            /* see if on cylinder yet */
            if ((uptr->CMD >> 16) == data->cyl) {
                /* we are on cylinder, seek is done */
                sim_debug(DEBUG_CMD, dptr, "scfi_srv seek on cylinder unit=%02x %04x %04x\n",
                    unit, uptr->CMD >> 16, data->cyl); 
                uptr->CMD &= ~(0xffff);         /* remove old status bits & cmd */
                set_devattn(chsa, SNS_DEVEND);  /* start the operation */
                sim_debug(DEBUG_DETAIL, dptr, "scfi_srv seek end unit=%02x %04x %04x\n",
                    unit, uptr->CMD >> 16, data->cyl);
                sim_activate(uptr, 20);
                break;
            } else {
                /* Compute delay based of difference. */
                /* Set next state = index */
                i = (uptr->CMD >> 16) - data->cyl;
                sim_debug(DEBUG_CMD, dptr, "scfi_srv seek unit=%02x %04x %04x\n", unit, uptr->CMD >> 16, i); 
                if (i > 0 ) {
                    if (i > 50) {
                        data->cyl += 50;        /* seek 50 cyl */
                        sim_activate(uptr, 800);
                    } else
                    if (i > 20) {
                        data->cyl += 20;        /* seek 20 cyl */
                        sim_activate(uptr, 400);
                    } else {
                        data->cyl++;            /* Seek 1 cyl */
                        sim_activate(uptr, 200);
                    }
                    if (data->cyl >= scfi_type[type].cyl)   /* test for over max */
                        data->cyl = scfi_type[type].cyl-1;  /* make max */
                } else {
                    if (i < -50) {
                        data->cyl -= 50;        /* seek 50 cyl */
                        sim_activate(uptr, 800);
                    } else
                    if (i < -20) {
                        data->cyl -= 20;        /* seek 20 cyl */
                        sim_activate(uptr, 400);
                    } else {
                        data->cyl--;            /* seek 1 cyl */
                        sim_activate(uptr, 200);
                    }
                    if ((int32)data->cyl < 0)   /* test for less than zero */
                        data->cyl = 0;          /* make zero */
                }
                sim_debug(DEBUG_DETAIL, dptr, "scfi_srv seek next unit=%02x %04x %04x\n",
                        unit, uptr->CMD >> 16, data->cyl);
                sim_activate(uptr, 2);
                break;
            }
        }

        /* not seeking, so start a new seek */
        /* Read in 4 character seek code */
        for (i = 0; i < 4; i++) {
            if (chan_read_byte(chsa, &buf[i])) {
                /* we have error, bail out */
                uptr->CMD &= ~(0xffff);         /* remove old status bits & cmd */
                uptr->SNS |= SNS_CMDREJ|SNS_EQUCHK;
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                break;
            }
        }
rezero:
        sim_debug(DEBUG_DETAIL, dptr, "scfi_srv seek unit=%02x star %02x %02x %02x %02x\n",
            unit, buf[0], buf[1], buf[2], buf[3]);
        /* save STAR (target sector) data in CMD */
        uptr->CMD = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3]);
        cyl = uptr->CMD >> 16;                  /* get the cylinder */
        trk = buf[2];                           /* get the track */
        sim_debug(DEBUG_DETAIL, dptr, "scfi_srv SEEK %08x cyl %04x trk %02x sec %02x unit=%02x\n",
            uptr->CMD, cyl&0xffff, trk, buf[3], unit);
        sim_debug(DEBUG_DETAIL, dptr, "scfi_srv Disk %s cyl %04x hds %02x sec/trk %02x unit=%02x\n",
            scfi_type[type].name, scfi_type[type].cyl, scfi_type[type].nhds, scfi_type[type].spt, unit);

        uptr->CMD |= DSK_STAR;                  /* show we have seek STAR in CMD */
        /* calc the sector address of data */
        /* calculate file position in bytes of requested sector */
        /* file offset in bytes */
        tstart =  uptr->CMD * (scfi_type[type].spb * scfi_type[type].ssiz * 4);
        data->tpos = trk;                       /* save the track/head number */
        data->spos = buf[3];                    /* save the sector number */
        sim_debug(DEBUG_DETAIL, dptr, "scfi_srv seek start %08x trk %04x sec %02x\n", tstart, trk, buf[3]);
        if ((sim_fseek(uptr->fileref, tstart, SEEK_SET)) != 0) {  /* seek home */
            sim_debug(DEBUG_DETAIL, dptr, "scfi_srv Error on seek to %08x\n", tstart);
        }

        /* Check if already on correct cylinder */
        if (cyl != data->cyl) {
            /* No, Do seek */
            uptr->CMD |= DSK_SEEKING;            /* show we are seeking */
            sim_debug(DEBUG_DETAIL, dptr, "scfi_srv seek unit=%02x cyl %04x trk %04x sec %04x\n",
                unit, cyl, trk, buf[3]);
            sim_activate(uptr, 20);
            chan_end(chsa, SNS_CHNEND);
        } else {
            sim_debug(DEBUG_DETAIL, dptr, "scfi_srv calc sect addr seek start %08x cyl %04x trk %04x sec %02x\n",
                tstart, cyl, trk, buf[3]);
            uptr->CMD &= ~(0xffff);              /* remove old status bits & cmd */
            sim_activate(uptr, 20);
            chan_end(chsa, SNS_DEVEND|SNS_CHNEND);
        }
        return SCPE_OK;

    case DSK_XEZ:          /* Rezero & Read IPL record */

        sim_debug(DEBUG_CMD, dptr, "RD REZERO IPL unit=%02x seek 0\n", unit);
        /* Do a seek to 0 */
        uptr->CMD = 0;                           /* set STAR to 0, 0, 0 */
        uptr->CMD &= ~(0xffff);                  /* remove old cmd */
        uptr->CMD |= DSK_SCK;                    /* show as seek command */
        tstart = 0;                             /* byte offset is 0 */
        dlen = 0;                               /* no data written yet */
        /* Read in 1 dummy character for length to inhibit SLI posting */
        if (chan_read_byte(chsa, &buf[0])) {
            /* we have error, bail out */
            uptr->CMD &= ~(0xffff);              /* remove old status bits & cmd */
            uptr->SNS |= SNS_CMDREJ|SNS_EQUCHK;
            chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
            break;
        }
        /* zero stuff */
        buf[0] = buf[1] = buf[2] = buf[3] = 0;
        goto rezero;                            /* murge with seek code */
        break;

    case DSK_LMR:
        sim_debug(DEBUG_CMD, dptr, "Load Mode Reg unit=%02x\n", unit);
        /* Read in 1 character of mode data */
        if (chan_read_byte(chsa, &buf[0])) {
            /* we have error, bail out */
            uptr->CMD &= ~(0xffff);              /* remove old status bits & cmd */
            uptr->SNS |= SNS_CMDREJ|SNS_EQUCHK;
            chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
            break;
        }
        uptr->CMD &= ~(0xffff);                  /* remove old cmd */
        uptr->SNS &= 0x00ffffff;                 /* clear old mode data */
        uptr->SNS |= (buf[0] << 24);             /* save mode value */
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
        break;

    case DSK_RD:            /* Read Data */
        /* tstart has start of sector address in bytes */
        if ((uptr->CMD & DSK_READING) == 0) {    /* see if we are reading data */
            uptr->CMD |= DSK_READING;            /* read from disk starting */
            dlen = 0;                           /* no data read yet */
            sim_debug(DEBUG_CMD, dptr, "DISK READ starting unit=%02x CMD %08x count %04x\n",
                unit, uptr->CMD, chp->ccw_count);
        }

        if (uptr->CMD & DSK_READING) {           /* see if we are reading data */
            /* read in a sector of data from disk */
            if ((len=sim_fread(buf, 1, ssize, uptr->fileref)) != ssize) {
                sim_debug(DEBUG_CMD, dptr,
                    "Error %08x on read %04x of diskfile cyl %04x hds %02x sec %02x\n",
                    len, ssize, data->cyl, data->tpos, data->spos);
                uptr->CMD &= ~(0xffff);          /* remove old status bits & cmd */
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                break;
            }

            sim_debug(DEBUG_CMD, dptr, "scfi_srv  after READ chsa %04x count %04x\n", chsa, chp->ccw_count);
            /* process the next sector of data */
            for (i=0; i<len; i++) {
                ch = buf[i];                    /* get a char from buffer */
                if (chan_write_byte(chsa, &ch)) {   /* put a byte to memory */
                    sim_debug(DEBUG_DATA, dptr,
                        "DISK Read %04x bytes from diskfile cyl %04x hds %02x sec %02x tstart %08x\n",
                        dlen+i, data->cyl, data->tpos, data->spos, tstart);
                    uptr->CMD &= ~(0xffff);      /* remove old status bits & cmd */
                    chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
                    goto rddone;
                }
            }
            dlen += len;                        /* add byte read to total count */

            sim_debug(DEBUG_CMD, dptr,
                "DISK READ from sec end %04x bytes end %04x from diskfile cyl %04x hds %02x sec %02x tstart %08x\n",
                dlen, ssize, data->cyl, data->tpos, data->spos, tstart);
            data->spos++;
            /* see if we are done reading data */
            if (test_write_byte_end(chsa)) {
                sim_debug(DEBUG_DATA, dptr,
                    "DISK Read complete Read %04x bytes from diskfile cyl %04x hds %02x sec %02x tstart %08x\n",
                    dlen, data->cyl, data->tpos, data->spos, tstart);
                uptr->CMD &= ~(0xffff);          /* remove old status bits & cmd */
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
            }
rddone:
            sim_activate(uptr, 10);             /* wait to read next sector */
            break;
        }
        break;

    case DSK_WD:            /* Write Data */
        /* tstart has start of sector address in bytes */
        if ((uptr->CMD & DSK_WRITING) == 0) {    /* see if we are writing data */
            uptr->CMD |= DSK_WRITING;            /* write to disk starting */
            dlen = 0;                           /* no data written yet */
            sim_debug(DEBUG_CMD, dptr, "DISK WRITE starting unit=%02x CMD %08x bytes %04x\n",
                unit, uptr->CMD, dlen);
        }
        if (uptr->CMD & DSK_WRITING) {           /* see if we are writing data */
            /* process the next sector of data */
            len = 0;                            /* used here as a flag for short read */
            for (i=0; i<ssize; i++) {
                if (chan_read_byte(chsa, &ch)) {    /* get a byte from memory */
                    /* if error on reading 1st byte, we are done writing */
                    if (i == 0) {
                        uptr->CMD &= ~(0xffff);  /* remove old status bits & cmd */
                        sim_debug(DEBUG_DATA, dptr,
                            "DISK Wrote %04x bytes to diskfile cyl %04x hds %02x sec %02x tstart %08x\n",
                            ssize, data->cyl, data->tpos, data->spos, tstart);
                        chan_end(chsa, SNS_CHNEND|SNS_DEVEND);
                        goto wrdone;
                    }
                    ch = 0;                     /* finish out the sector with zero */
                    len++;                      /* show we have no more data to write */
                }
                buf2[i] = ch;                   /* save the char */
            }
            dlen += ssize;                      /* add 1 sector of bytes */
            /* write the sector to disk */
            if ((i=sim_fwrite(buf2, 1, ssize, uptr->fileref)) != ssize) {
                sim_debug(DEBUG_CMD, dptr,
                    "Error %08x on write %04x to diskfile cyl %04x hds %02x sec %02x\n",
                    i, ssize, data->cyl, data->tpos, data->spos);
                uptr->CMD &= ~(0xffff);          /* remove old status bits & cmd */
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
                break;
            }
            if (len != 0) {                     /* see if done with write command */
                sim_debug(DEBUG_DATA, dptr,
                    "DISK WroteB %04x bytes to diskfile cyl %04x hds %02x sec %02x tstart %08x\n",
                    ssize, data->cyl, data->tpos, data->spos, tstart);
                uptr->CMD &= ~(0xffff);          /* remove old status bits & cmd */
                chan_end(chsa, SNS_CHNEND|SNS_DEVEND);  /* we done */
                break;
            }
            sim_debug(DEBUG_CMD, dptr,
                "DISK WR to sec end %04x bytes end %04x to diskfile cyl %04x hds %02x sec %02x tstart %08x\n",
                dlen, ssize, data->cyl, data->tpos, data->spos, tstart);
            data->spos++;
wrdone:
            sim_activate(uptr, 10);
            break;
         }
         break;

    default:
        sim_debug(DEBUG_DETAIL, dptr, "invalid command %02x unit %02x\n", cmd, unit);
        uptr->SNS |= SNS_CMDREJ;
        uptr->CMD &= ~(0xffff);                  /* remove old status bits & cmd */
        chan_end(chsa, SNS_CHNEND|SNS_DEVEND|SNS_UNITCHK);
        break;
    }
    sim_debug(DEBUG_CMD, dptr,
        "scfi_srv done cmd %02x chsa %04x count %04x\n", cmd, chsa, chp->ccw_count);
    return SCPE_OK;
}

/* initialize the disk */
void scfi_ini(UNIT *uptr, t_bool f)
{
    DEVICE  *dptr = find_dev_from_unit(uptr);
    int     i = GET_TYPE(uptr->flags);

    uptr->CMD &= ~0xffff;                        /* clear out the flags but leave ch/sa */
    /* capacity is total allocation units times sectors per allocation unit */
    /* total sectors on disk */
    uptr->capac  = scfi_type[i].taus * scfi_type[i].spau;

    sim_debug(DEBUG_EXP, &sda_dev, "SDA init device %s on unit SDA%.1x cap %x\n",
                    dptr->name, GET_UADDR(uptr->CMD), uptr->CMD);
}

t_stat scfi_reset(DEVICE * dptr)
{
    /* add reset code here */
    return SCPE_OK;
}

/* create the disk file for the specified device */
int scfi_format(UNIT *uptr) {
//  struct ddata_t  *data = (struct ddata_t *)uptr->up7;
    uint16          addr = GET_UADDR(uptr->CMD);
    int             type = GET_TYPE(uptr->flags);
    DEVICE          *dptr = find_dev_from_unit(uptr);
    uint16          tsize = scfi_type[type].spt * scfi_type[type].ssiz*4; /* get track size in bytes */
    int32           ssize = scfi_type[type].ssiz * 4;  /* disk sector size in bytes */
    uint32          cyl;
    uint16          spc = scfi_type[type].nhds * scfi_type[type].spt;   /* sectors/cyl */
    uint32          cap = scfi_type[type].taus * scfi_type[type].spau;  /* disk capacity in sectors */
    uint32          cylv = cap / spc;           /* calc number of cylinders */
    uint8           *buff;

    /* see if user wants to initialize the disk */
    if (!get_yn("Initialize disk? [Y] ", TRUE)) {
        return 1;
    }

    /* seek to sector 0 */
    if ((sim_fseek(uptr->fileref, 0, SEEK_SET)) != 0) { /* seek home */
        fprintf (stderr, "Error on seek to 0\r\n");
    }

    /* get buffer for track data */
    if ((buff = (uint8 *)calloc(tsize, sizeof(uint8))) == 0) {
        detach_unit(uptr);
        return SCPE_ARG;
    }
    /* put dummy data in first word of disk */
    buff[0] = 'Z';
    buff[1] = 'E';
    buff[2] = 'R';
    buff[3] = 'O';
    sim_debug(DEBUG_CMD, dptr, "Creating disk file of trk size %04x capacity %d\n", tsize, cap*ssize);
    /* write zeros to each track of the disk */
    for (cyl = 0; cyl < cylv; cyl++) {
        if ((sim_fwrite(buff, 1, tsize, uptr->fileref)) != tsize) {
            sim_debug(DEBUG_CMD, dptr, "Error on write to diskfile cyl %04x\n", cyl);
        }
        if (cyl == 0) {
            buff[0] = 0;
            buff[1] = 0;
            buff[2] = 0;
            buff[3] = 0;
        }
    }
    if ((cyl % 400) == 0)
        fputc('.', stderr);
    fputc('\r', stderr);
    fputc('\n', stderr);
    /* seek home again */
    sim_fseek(uptr->fileref, 0, SEEK_SET);      /* seek home */
    free(buff);                                 /* free cylinder buffer */
    set_devattn(addr, SNS_DEVEND);              /* start us up */
    return 0;
}

/* attach the selected file to the disk */
t_stat scfi_attach(UNIT *uptr, CONST char *file) {
    uint16          addr = GET_UADDR(uptr->CMD);
    int             type = GET_TYPE(uptr->flags);
    DEVICE          *dptr = find_dev_from_unit(uptr);
    t_stat          r;
//  uint16          tsize;                      /* track size in bytes */
    uint16          ssize;                      /* sector size in bytes */
    struct ddata_t  *data;
    uint8           buff[1024];

    /* have simulator attach the file to the unit */
    if ((r = attach_unit(uptr, file)) != SCPE_OK)
        return r;

    if (scfi_type[type].name == 0) {            /* does the assigned disk have a name */
        detach_unit(uptr);                      /* no, reject */
        return SCPE_FMT;                        /* error */
    }

    /* get a buffer to hold scfi_t structure */
    /* extended data structure per unit */
    if ((data = (struct ddata_t *)calloc(1, sizeof(struct ddata_t))) == 0) {
        detach_unit(uptr);
        return SCPE_FMT;
    }

    uptr->up7 = (void *)data;                   /* save pointer to structure in up7 */
    /* track size in bytes is sectors/track times words/sector time 4 bytse/word */
//  tsize = scfi_type[type].spt * scfi_type[type].ssiz * 4; /* get track size in bytes */
    uptr->capac = scfi_type[type].taus * scfi_type[type].spau;  /* disk capacity in sectors */
    ssize = scfi_type[type].ssiz * 4;           /* disk sector size in bytes */
    uptr->capac *= ssize;                       /* disk capacity in bytes */

    sim_debug(DEBUG_CMD, dptr, "Disk taus %d spau %d ssiz %d cap %d\n",
        scfi_type[type].taus, scfi_type[type].spau, scfi_type[type].ssiz * 4,
        uptr->capac);                           /* disk capacity */

    if ((sim_fseek(uptr->fileref, 0, SEEK_SET)) != 0) { /* seek home */
        detach_unit(uptr);                      /* if no space, error */
        return SCPE_FMT;                        /* error */
    }

    /* read in the 1st sector of the 'disk' */
    if ((r = sim_fread(&buff[0], sizeof(uint8), ssize, uptr->fileref) != ssize)) {
        sim_debug(DEBUG_CMD, &sda_dev, "Disk format fread ret = %04x\n", r);
        goto fmt;
    }

    if ((buff[0] | buff[1] | buff[2] | buff[3]) == 0) {
        sim_debug(DEBUG_CMD, &sda_dev,
        "Disk format buf0 %02x buf1 %02x buf2 %02x buf3 %02x\n",
        buff[0], buff[1], buff[2], buff[3]);
fmt:
        /* format the drive */
        if (scfi_format(uptr)) {
            detach_unit(uptr);                  /* if no space, error */
            return SCPE_FMT;                    /* error */
        }
    }

    if ((sim_fseek(uptr->fileref, 0, SEEK_SET)) != 0) { /* seek home */
        detach_unit(uptr);                      /* if no space, error */
        return SCPE_FMT;                        /* error */
    }

    data->cyl = 0;                              /* current cylinder position */
    data->tpos = 0;                             /* current track position */
    data->spos = 0;                             /* current sector position */

    sim_debug(DEBUG_CMD, &sda_dev, "Attach %8s hds %d spt %d spc %d cyl %d capacity %d\n",
        scfi_type[type].name, scfi_type[type].nhds, scfi_type[type].spt, 
        scfi_type[type].nhds * scfi_type[type].spt, 
        scfi_type[type].cyl, uptr->capac);
    sim_debug(DEBUG_CMD, &sda_dev, "File %s attached to %s\r\n", file, scfi_type[type].name);

    set_devattn(addr, SNS_DEVEND);
    return SCPE_OK;
}

/* detach a disk device */
t_stat scfi_detach(UNIT *uptr) {
    struct ddata_t       *data = (struct ddata_t *)uptr->up7;

    if (data != 0) {
        free(data);                             /* free disk data structure */
    }
    uptr->up7 = 0;                              /* no pointer to disk data */
    uptr->CMD &= ~0xffff;                        /* no cmd and flags */
    return detach_unit(uptr);                   /* tell simh we are done with disk */
}

/* boot from the specified disk unit */
t_stat scfi_boot(int32 unit_num, DEVICE *dptr) {
    UNIT    *uptr = &dptr->units[unit_num];     /* find disk unit number */

    sim_debug(DEBUG_CMD, &sda_dev, "SCFI Disk Boot dev/unit %04x\n", GET_UADDR(uptr->CMD));
    SPAD[0xf4] = GET_UADDR(uptr->CMD);           /* put boot device chan/sa into spad */
    SPAD[0xf8] = 0xF000;                        /* show as F class device */
    if ((uptr->flags & UNIT_ATT) == 0)
        return SCPE_UNATT;                      /* attached? */
    return chan_boot(GET_UADDR(uptr->CMD), dptr);    /* boot the ch/sa */
}

/* Disk option setting commands */
t_stat scfi_set_type(UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    int     i;

    if (cptr == NULL)
        return SCPE_ARG;
    if (uptr == NULL)
        return SCPE_IERR;
    if (uptr->flags & UNIT_ATT)
        return SCPE_ALATT;
    for (i = 0; scfi_type[i].name != 0; i++) {
        if (strcmp(scfi_type[i].name, cptr) == 0) {
            uptr->flags &= ~UNIT_TYPE;
            uptr->flags |= SET_TYPE(i);
            uptr->capac  = scfi_type[i].taus * scfi_type[i].spau;
            return SCPE_OK;
        }
    }
    return SCPE_ARG;
}

t_stat scfi_get_type(FILE * st, UNIT *uptr, int32 v, CONST void *desc)
{
    if (uptr == NULL)
        return SCPE_IERR;
    fputs("TYPE=", st);
    fputs(scfi_type[GET_TYPE(uptr->flags)].name, st);
    return SCPE_OK;
}

/* help information for disk */
t_stat scfi_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag,
    const char *cptr)
{
    int i;
    fprintf (st, "SEL-32 SCFI Disk Processor\r\n");
    fprintf (st, "Use:\r\n");
    fprintf (st, "    sim> SET %sn TYPE=type\r\n", dptr->name);
    fprintf (st, "Type can be: ");
    for (i = 0; scfi_type[i].name != 0; i++) {
        fprintf(st, "%s", scfi_type[i].name);
        if (scfi_type[i+1].name != 0)
        fprintf(st, ", ");
    }
    fprintf (st, ".\nEach drive has the following storage capacity:\r\n");
    for (i = 0; scfi_type[i].name != 0; i++) {
        /* disk capacity in sectors */
        int32 capac = scfi_type[i].taus * scfi_type[i].spau;
        int32 ssize = scfi_type[i].ssiz * 4;    /* disk sector size in bytes */
        int32 size = capac * ssize;             /* disk capacity in bytes */
        size /= 1024;                           /* make KB */
        size = (10 * size) / 1024;              /* size in MB * 10 */
        fprintf(st, "      %-8s %4d.%1d MB\r\n", scfi_type[i].name, size/10, size%10);
    }
    fprint_set_help(st, dptr);
    fprint_show_help(st, dptr);
    return SCPE_OK;
}

const char *scfi_description (DEVICE *dptr)
{
    return "SEL-32 SCFI Disk Processor";
}

#endif