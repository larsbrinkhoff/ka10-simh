/* ka10_ten11.c: Rubin 10-11 interface.

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

   This is a device which interfaces with eight Unibuses.  It's
   specific to the MIT AI lab PDP-10.
*/

#include "ka10_defs.h"
#include "sim_tmxr.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>


/* Rubin 10-11 pager. */
static uint64 ten11_pager[256];

/* Physical address of 10-11 control page. */
#define T11CPA          03776000

/* Bits in a 10-11 page table entry. */
#define T11VALID        (0400000000000LL)
#define T11WRITE        (0200000000000LL)
#define T11PDP11        (0003400000000LL)
#define T11ADDR         (0000377776000LL)
#define T11LIMIT        (0000000001777LL)

/* External Unibus interface. */
#define DATO            1
#define DATI            2
#define ACK             3
#define ERR             4
#define TIMEOUT         5

#define UNIT_SHMEM (1u << UNIT_V_UF)
#define TEN11_POLL  100

/* Simulator time units for a Unibus memory cycle. */
#define UNIBUS_MEM_CYCLE 100


static uint16 *ten11_M = NULL;
static int ten11_server;
static int ten11_fd = -1;

static t_stat ten11_svc (UNIT *uptr);
static t_stat ten11_conn_svc (UNIT *uptr);
static t_stat ten11_setmode (UNIT* uptr, int32 val, CONST char* cptr, void* desc);
static t_stat ten11_showmode (FILE* st, UNIT* uptr, int32 val, CONST void* desc);
static t_stat ten11_setpeer (UNIT* uptr, int32 val, CONST char* cptr, void* desc);
static t_stat ten11_showpeer (FILE* st, UNIT* uptr, int32 val, CONST void* desc);
static t_stat ten11_reset (DEVICE *dptr);
static t_stat ten11_attach (UNIT *uptr, CONST char *ptr);
static t_stat ten11_detach (UNIT *uptr);
static t_stat ten11_attach_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr);
static const char *ten11_description (DEVICE *dptr);

static UNIT ten11_unit[2] = {
    { UDATA (&ten11_svc,        UNIT_IDLE|UNIT_ATTABLE, 0), TEN11_POLL },
    { UDATA (&ten11_conn_svc,   UNIT_DIS,               0) }
};

static UNIT *ten11_action_unit = &ten11_unit[0];
static UNIT *ten11_connection_unit = &ten11_unit[1];
#define PEERSIZE 512
static char ten11_peer[PEERSIZE];

static REG ten11_reg[] = {
    { DRDATAD  (POLL, ten11_unit[0].wait,   24,    "poll interval"), PV_LEFT },
    { BRDATA   (PEER, ten11_peer,    8,       8, PEERSIZE),          REG_HRO },
    { NULL }
};

static MTAB ten11_mod[] = {
    { MTAB_XTD|MTAB_VDV,          0, "MODE", "MODE={SHMEM|NETWORK}",
        &ten11_setmode, &ten11_showmode, NULL, "Display access mode" },
    { MTAB_XTD|MTAB_VDV,          0, "PEER", "PEER=address:port",
        &ten11_setpeer, &ten11_showpeer, NULL, "Display destination/source" },
    { 0 }
};

#define DBG_TRC         1
#define DBG_CMD         2

static DEBTAB ten11_debug[] = {
    {"TRACE",   DBG_TRC,    "Routine trace"},
    {"CMD",     DBG_CMD,    "Command Processing"},
    {0},
};

DEVICE ten11_dev = {
    "TEN11", ten11_unit, ten11_reg, ten11_mod,
    1, 8, 16, 2, 8, 16,
    NULL,                                               /* examine */
    NULL,                                               /* deposit */
    &ten11_reset,                                       /* reset */
    NULL,                                               /* boot */
    ten11_attach,                                       /* attach */
    ten11_detach,                                       /* detach */
    NULL,                                               /* context */
    DEV_DISABLE | DEV_DIS | DEV_DEBUG | DEV_MUX,
    DBG_CMD,                                            /* debug control */
    ten11_debug,                                        /* debug flags */
    NULL,                                               /* memory size chage */
    NULL,                                               /* logical name */
    NULL,                                               /* help */
    &ten11_attach_help,                                 /* attach help */
    NULL,                                               /* help context */
    &ten11_description,                                 /* description */
};

static TMLN ten11_ldsc;                                 /* line descriptor */
static TMXR ten11_desc = { 1, 0, 0, &ten11_ldsc };      /* mux descriptor */
#define PEERSIZE 512
static char ten11_peer[PEERSIZE];
SHMEM *pdp11_shmem = NULL;                              /* PDP11 shared memory info */

static t_stat ten11_reset (DEVICE *dptr)
{
sim_debug(DBG_TRC, dptr, "ten11_reset()\n");

ten11_action_unit->flags |= UNIT_ATTABLE;
ten11_action_unit->action = ten11_svc;
ten11_connection_unit->flags |= UNIT_DIS | UNIT_IDLE;
ten11_connection_unit->action = ten11_conn_svc;
ten11_desc.packet = TRUE;
ten11_desc.notelnet = TRUE;
ten11_desc.buffered = 2048;

return SCPE_OK;
}

t_stat ten11_showpeer (FILE* st, UNIT* uptr, int32 val, CONST void* desc)
{
if (ten11_peer[0])
    fprintf(st, "peer=%s", ten11_peer);
else
    fprintf(st, "peer=unspecified");
return SCPE_OK;
}

t_stat ten11_setmode (UNIT* uptr, int32 val, CONST char* cptr, void* desc)
{
char gbuf[CBUFSIZE];

if ((!cptr) || (!*cptr))
    return SCPE_ARG;
if (uptr->flags & UNIT_ATT)
    return SCPE_ALATT;
if (!(uptr->flags & UNIT_ATTABLE))
    return SCPE_NOATT;
cptr = get_glyph (cptr, gbuf, 0);
if (0 == strcmp ("SHMEM", gbuf))
    uptr->flags |= UNIT_SHMEM;
else {
    if (0 == strcmp ("NETWORK", gbuf))
        uptr->flags &= ~UNIT_SHMEM;
    else
        return sim_messagef (SCPE_ARG, "Unknown mode: %s\n", gbuf);
    }
return SCPE_OK;
}

t_stat ten11_showmode (FILE* st, UNIT* uptr, int32 val, CONST void* desc)
{
fprintf(st, "mode=%s", (uptr->flags & UNIT_SHMEM) ? "SHMEM" : "NETWORK");
return SCPE_OK;
}

t_stat ten11_setpeer (UNIT* uptr, int32 val, CONST char* cptr, void* desc)
{
char host[PEERSIZE], port[PEERSIZE];

if ((!cptr) || (!*cptr))
    return SCPE_ARG;
if (!(uptr->flags & UNIT_ATTABLE))
    return SCPE_NOATT;
if (uptr->flags & UNIT_ATT)
    return SCPE_ALATT;
if (uptr->flags & UNIT_SHMEM)
    return sim_messagef (SCPE_ARG, "Peer can't be specified in Shared Memory Mode\n");
if (sim_parse_addr (cptr, host, sizeof(host), NULL, port, sizeof(port), NULL, NULL))
    return sim_messagef (SCPE_ARG, "Invalid Peer Specification: %s\n", cptr);
if (host[0] == '\0')
    return sim_messagef (SCPE_ARG, "Invalid/Missing host in Peer Specification: %s\n", cptr);
strncpy(ten11_peer, cptr, PEERSIZE-1);
return SCPE_OK;
}

static t_stat ten11_attach (UNIT *uptr, CONST char *cptr)
{
t_stat r;
char attach_string[512];

if (!cptr || !*cptr)
    return SCPE_ARG;
if (!(uptr->flags & UNIT_ATTABLE))
    return SCPE_NOATT;
if (uptr->flags & UNIT_SHMEM) {
    void *basead;

    r = sim_shmem_open (cptr, MAXMEMSIZE, &pdp11_shmem, &basead);
    if (r != SCPE_OK)
        return r;
    ten11_M = (uint16 *)basead;
    }
else {
    if (ten11_peer[0] == '\0')
        return sim_messagef (SCPE_ARG, "Must specify peer before attach\n");
    sprintf (attach_string, "%s,Connect=%s", cptr, ten11_peer);
    r = tmxr_attach_ex (&ten11_desc, uptr, attach_string, FALSE);
    if (r != SCPE_OK)                                       /* error? */
        return r;
    sim_activate_after (ten11_connection_unit, 10);    /* start poll */
    }
uptr->flags |= UNIT_ATT;
return SCPE_OK;
}

static t_stat ten11_detach (UNIT *uptr)
{
t_stat r;

if (!(uptr->flags & UNIT_ATT))                      /* attached? */
    return SCPE_OK;
if (uptr->flags & UNIT_SHMEM) {
    sim_shmem_close (pdp11_shmem);
    r = SCPE_OK;
    }
else {
    sim_cancel (uptr);
    sim_cancel (ten11_connection_unit);             /* stop connection poll as well */
    r = tmxr_detach (&ten11_desc, uptr);
    }
uptr->flags &= ~UNIT_ATT;
free (uptr->filename);
uptr->filename = NULL;
return r;
}

static void build (unsigned char *request, unsigned char octet)
{
  request[1]++;
  request[request[1] + 1] = octet;
}

static t_stat ten11_svc (UNIT *uptr)
{
  return SCPE_OK;
}

static t_stat ten11_conn_svc (UNIT *uptr)
{
int32 newconn;

sim_debug(DBG_TRC, &ten11_dev, "ten11_conn_svc()\n");

newconn = tmxr_poll_conn(&ten11_desc);      /* poll for a connection */
if (newconn >= 0) {                         /* got a live one? */
    sim_debug(DBG_CMD, &ten11_dev, "got connection\n");
    ten11_ldsc.rcve = 1;
    sim_activate (ten11_action_unit, ten11_action_unit->wait);  /* Start activity poll */
    }
sim_activate_after (uptr, 10);
return SCPE_OK;
}


static t_stat ten11_attach_help (FILE *st, DEVICE *dptr, UNIT *uptr, int32 flag, const char *cptr)
{
const char helpString[] =
 /* The '*'s in the next line represent the standard text width of a help line */
     /****************************************************************************/
    " The %D device is an implementation of the Rubin 10-11 PDP-10 to PDP-11\n"
    " Memory Access facility.  This allows a PDP 10 system to reach into a\n"
    " PDP-11 simulator and modify or access the contents of the PDP-11 memory.\n"
    "1 Configuration\n"
    " A %D device is configured with various simh SET and ATTACH commands\n"
    "2 $Set commands\n"
    "3 Mode\n"
    " To memory access mode.  Options are SHMEM for Shared Memory access and\n"
    " NETWORK for network access.  This can be configured with the\n"
    " following command:\n"
    "\n"
    "+sim> SET %U MODE=SHMEM\n"
    "+OR\n"
    "+sim> SET %U MODE=NETWORK\n"
    "3 Peer\n"
    " When the memory access mode is specified as NETWORK mode, the peer system's\n"
    " host and port to that data is to be transmitted across is specified by\n"
    " using the following command:\n"
    "\n"
    "+sim> SET %U PEER=host:port\n"
    "2 Attach\n"
    " When in SHMEM shared memory access mode, the device must be attached\n"
    " using an attach command which specifies the shared object name that\n"
    " the peer system will be using:\n"
    "\n"
    "+sim> ATTACH %U SharedObjectName\n"
    "\n"
    " When in NETWORK memory access mode, the device must be attached to a\n"
    " receive port, this is done by using the ATTACH command to specify\n"
    " the receive port number.\n"
    "\n"
    "+sim> ATTACH %U port\n"
    "\n"
    " The Peer host:port value must be specified before the attach command.\n"
    " The default connection uses TCP transport between the local system and\n"
    " the peer.  Alternatively, UDP can be used by specifying UDP on the\n"
    " ATTACH command:\n"
    "\n"
    "+sim> ATTACH %U port,UDP\n"
    "\n"
    "2 Examples\n"
    " To configure two simulators to talk to each other using in Network memory\n"
    " access mode, follow this example:\n"
    " \n"
    " Machine 1\n"
    "+sim> SET %D ENABLE\n"
    "+sim> SET %U PEER=LOCALHOST:2222\n"
    "+sim> ATTACH %U 1111\n"
    " \n"
    " Machine 2\n"
    "+sim> SET %D ENABLE\n"
    "+sim> SET %U PEER=LOCALHOST:1111\n"
    "+sim> ATTACH %U 2222\n"
    "\n"
    " To configure two simulators to talk to each other using SHMEM shared memory\n"
    " access mode, follow this example:\n"
    " \n"
    " Machine 1\n"
    "+sim> SET %D ENABLE\n"
    "+sim> SET %D MODE=SHMEM\n"
    "+sim> ATTACH %U PDP11-1-Core\n"
    " \n"
    " Machine 2\n"
    "+sim> SET %D ENABLE\n"
    "+sim> SET %D MODE=SHMEM\n"
    "+sim> ATTACH %U PDP11-1-Core\n"
    "\n"
    "\n"
    ;

return scp_help (st, dptr, uptr, flag, helpString, cptr);
return SCPE_OK;
}


static const char *ten11_description (DEVICE *dptr)
{
return "Rubin 10-11 PDP-10 to PDP-11 Memory Access";
}

static int error (const char *message)
{
  sim_debug (DEBUG_TEN11, &cpu_dev, "%s\r\n", message);
  sim_debug (DEBUG_TEN11, &cpu_dev, "CLOSE\r\n");
  close (ten11_fd);
  ten11_fd = -1;
  return -1;
}

static int transaction (unsigned char *request, unsigned char *response)
{
  const uint8 *ten11_request;
  size_t size;
  t_stat stat;

  sim_debug (DEBUG_TEN11, &cpu_dev, "Sending request: %o %o %o...\n", request[0], request[1], request[2]);

  stat = tmxr_put_packet_ln (&ten11_ldsc, request + 2, (size_t)request[1]);
  if (stat != SCPE_OK)
    return error ("Write error in transaction");

  do {
    tmxr_poll_rx (&ten11_desc);
    stat = tmxr_get_packet_ln (&ten11_ldsc, &ten11_request, &size);
  } while (stat != SCPE_OK);

  if (size > 7)
    return error ("Malformed transaction");

  memcpy (response, ten11_request, size);

  return 0;
}

static int read_word (int addr, int *data)
{
  unsigned char request[8];
  unsigned char response[8];

  sim_interval -= UNIBUS_MEM_CYCLE;

  if ((ten11_action_unit->flags & UNIT_ATT) == 0) {
      *data = 0;
      return 0;
  }

  if (ten11_action_unit->flags & UNIT_SHMEM) {
      *data = ten11_M[addr >> 1];
      return 0;
  }

  memset (request, 0, sizeof request);
  build (request, DATI);
  build (request, addr >> 16);
  build (request, addr >> 8);
  build (request, addr);

  transaction (request, response);

  switch (response[2])
    {
    case ACK:
      *data = response[4];
      *data |= response[3] << 8;
      break;
    case ERR:
      fprintf (stderr, "TEN11: Read error %06o\r\n", addr);
      *data = 0;
      break;
    case TIMEOUT:
      fprintf (stderr, "TEN11: Read timeout %06o\r\n", addr);
      *data = 0;
      break;
    default:
      return error ("Protocol error");
    }

  return 0;
}

int ten11_read (int addr)
{
  int offset = addr & 01777;
  int data;

  if (addr >= T11CPA) {
    /* Accessing the control page. */
    if (offset >= 0400) {
      sim_debug (DEBUG_TEN11, &cpu_dev,
                 "Control page read NXM: %o @ %o\n",
                 offset, PC);
      return 1;
    }
    MB = ten11_pager[offset];
  } else {
    /* Accessing a memory page. */
    int page = (addr >> 10) & 0377;
    uint64 mapping = ten11_pager[page];
    int unibus, uaddr, limit;

    limit = mapping & T11LIMIT;
    if ((mapping & T11VALID) == 0 || offset > limit) {
      sim_debug (DEBUG_TEN11, &cpu_dev,
                 "(%o) %07o >= 4,,000000 / %llo / %o > %o\n",
                 page, addr, (mapping & T11VALID), offset, limit);
      return 1;
    }

    unibus = (mapping & T11PDP11) >> 26;
    uaddr = ((mapping & T11ADDR) >> 10) + offset;
    uaddr <<= 2;

    read_word (uaddr, &data);
    MB = data;
    MB <<= 20;
    read_word (uaddr + 2, &data);
    MB |= data << 4;
    
    sim_debug (DEBUG_TEN11, &cpu_dev,
               "Read: (%o) %06o -> %012llo\n",
               unibus, uaddr, MB);
  }
  return 0;
}

static int write_word (int addr, int data)
{
  unsigned char request[8];
  unsigned char response[8];

  sim_interval -= UNIBUS_MEM_CYCLE;

  if ((ten11_action_unit->flags & UNIT_ATT) == 0) {
      return 0;
  }

  if (ten11_action_unit->flags & UNIT_SHMEM) {
      ten11_M[addr >> 1] = data;
      return 0;
  }

  memset (request, 0, sizeof request);
  build (request, DATO);
  build (request, addr >> 16);
  build (request, addr >> 8);
  build (request, addr);
  build (request, data >> 8);
  build (request, data);

  transaction (request, response);

  switch (response[2])
    {
    case ACK:
      break;
    case ERR:
      fprintf (stderr, "TEN11: Write error %06o\r\n", addr);
      break;
    case TIMEOUT:
      fprintf (stderr, "TEN11: Write timeout %06o\r\n", addr);
      break;
    default:
      return error ("Protocol error");
    }

  return 0;
}

int ten11_write (int addr)
{
  int offset = addr & 01777;

  if (addr >= T11CPA) {
    /* Accessing the control page. */
    if (offset >= 0400) {
      sim_debug (DEBUG_TEN11, &cpu_dev,
                 "Control page write NXM: %o @ %o\n",
                 offset, PC);
      return 1;
    }
    ten11_pager[offset] = MB;
    sim_debug (DEBUG_TEN11, &cpu_dev,
               "Page %03o: %s %s (%llo) %06llo/%04llo\n",
               offset,
               (MB & T11VALID) ? "V" : "I",
               (MB & T11WRITE) ? "RW" : "R",
               (MB & T11PDP11) >> 26,
               (MB & T11ADDR) >> 10,
               (MB & T11LIMIT));
  } else {
    /* Accessing a memory page. */
    int page = (addr >> 10) & 0377;
    uint64 mapping = ten11_pager[page];
    int unibus, uaddr, limit;
    limit = mapping & T11LIMIT;
    if ((mapping & T11VALID) == 0 || offset > limit) {
      sim_debug (DEBUG_TEN11, &cpu_dev,
                 "(%o) %07o >= 4,,000000 / %llo / %o > %o\n",
                 page, addr, (mapping & T11VALID), offset, limit);
      return 1;
    }
    unibus = (mapping & T11PDP11) >> 26;
    uaddr = ((mapping & T11ADDR) >> 10) + offset;
    uaddr <<= 2;
    sim_debug (DEBUG_TEN11, &cpu_dev,
               "Write: (%o) %06o <- %012llo\n",
               unibus, uaddr, MB);

    if ((MB & 010) == 0)
      write_word (uaddr, MB >> 20);
    if ((MB & 004) == 0)
      write_word (uaddr + 2, MB >> 4);
  }
  return 0;
}
