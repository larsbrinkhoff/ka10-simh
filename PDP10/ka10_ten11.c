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

/* Simulator time units for a Unibus memory cycle. */
#define UNIBUS_MEM_CYCLE 100


static int ten11_server;
static int ten11_fd = -1;

static int read_all (int fd, unsigned char *data, int n);
static int write_all (int fd, unsigned char *data, int n);

static void build (unsigned char *request, unsigned char octet)
{
  request[1]++;
  request[request[1] + 1] = octet;
}

static int error (const char *message)
{
  sim_debug (DEBUG_TEN11, &cpu_dev, "%s\r\n", message);
  sim_debug (DEBUG_TEN11, &cpu_dev, "CLOSE\r\n");
  close (ten11_fd);
  ten11_fd = -1;
  return -1;
}

int ten11_check (void)
{
  struct sockaddr_in address;
  int n, s;

  if (ten11_fd != -1)
    return 0;

  n = sizeof address;
  ten11_fd = accept (ten11_server, (struct sockaddr *)&address, &n);
  if (ten11_fd == -1)
    return errno == EWOULDBLOCK ? 0 : -1;

  fprintf (stderr, "Accepted connection\r\n");

#if 0
  n = fcntl (ten11_fd, F_GETFL, 0);
  if (n == -1)
    {
      close (ten11_fd);
      return -1;
    }
  //if (fcntl (ten11_fd, F_SETFL, n & ~O_NONBLOCK) == -1)
  if (fcntl (ten11_fd, F_SETFL, n | O_NONBLOCK) == -1)
    {
      close (ten11_fd);
      return -1;
    }
#endif

  sim_debug (DEBUG_TEN11, &cpu_dev, "Unibus attached\r\n");

  return 0;
}

int ten11_init (void)
{
  struct sockaddr_in address;
  struct in_addr addr;
  int n;

  ten11_server = socket (PF_INET, SOCK_STREAM, 0);
  if (ten11_server == -1)
    return -1;
  
  n = fcntl (ten11_server, F_GETFL, 0);
  if (n == -1)
    {
      close (ten11_server);
      return -1;
    }
  if (fcntl (ten11_server, F_SETFL, n | O_NONBLOCK) == -1)
    {
      close (ten11_server);
      return -1;
    }

  n = 1;
  setsockopt (ten11_server, SOL_SOCKET, SO_REUSEADDR, (void *)&n, sizeof n);

  addr.s_addr = INADDR_ANY;

  memset (&address, '\0', sizeof address);
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  address.sin_len = sizeof address;
#endif
  address.sin_family = PF_INET;
  address.sin_port = htons (1234);
  address.sin_addr = addr;
  
  if (bind (ten11_server, (struct sockaddr *)&address, sizeof (address)) == -1)
    {
      close (ten11_server);
      return -1;
    }

  if (listen (ten11_server, 1) == -1)
    {
      close (ten11_server);
      return -1;
    } 

  return 0;
}

static void flush_socket (int fd)
{
  int flag = 1;
  setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof flag);
  flag = 0;
  setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof flag);
}

static int write_all (int fd, unsigned char *data, int n)
{
  int m;

#if 0
  fprintf (stderr, "[write_all:");
  for (m = 0; m < n; m++)
    fprintf (stderr, " %o", data[m]);
  fprintf (stderr, "]\r\n");
#endif

  while (n > 0) {
    m = write (fd, data, n);
    if (m == -1)
      return -1;
    if (m == 0)
      error ("Write EOF");
    data += m;
    n -= m;
  }

  return 0;
}

static int read_all (int fd, unsigned char *data, int n)
{
  int m;

  //fprintf (stderr, "[RA:%d]", n); fflush (stderr);
  while (n > 0) {
    //fprintf (stderr, "[got:"); fflush (stderr);
    m = read (fd, data, n);
    //fprintf (stderr, "%d]", m); fflush (stderr);
    if (m == -1)
      {
        if (errno == EWOULDBLOCK)
          continue;
        return -1;
      }
    if (m == 0)
      return -1;
    data += m;
    n -= m;
  }

  return 0;
}

static int transaction (unsigned char *request, unsigned char *response)
{
  sim_debug (DEBUG_TEN11, &cpu_dev, "Sending request: %o %o %o...\n", request[0], request[1], request[2]);
  if (write_all (ten11_fd, request, request[1] + 2) == -1)
    return error ("Write error in transaction");
  flush_socket (ten11_fd);

  sim_debug (DEBUG_TEN11, &cpu_dev, "Read packet length\n");
  if (read_all (ten11_fd, response, 2) == -1)
    return error ("Read error reading packet");

  sim_debug (DEBUG_TEN11, &cpu_dev, "Packet length %o %o\n", response[0], response[1]);
  if (response[0] != 0 || response[1] > 7) {
    char c;
    while (read (ten11_fd, &c, 1) == 1)
      fprintf (stderr, "[%o]\r\n", c);
    return error ("Malformed transaction");
  }
  sim_debug (DEBUG_TEN11, &cpu_dev, "Read data\n");
  if (read_all (ten11_fd, response + 2, response[1]) == -1)
    return error ("Read error in transaction");

  return 0;
}

static int read_word (int addr, int *data)
{
  unsigned char request[8];
  unsigned char response[8];

  sim_interval -= UNIBUS_MEM_CYCLE;

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
