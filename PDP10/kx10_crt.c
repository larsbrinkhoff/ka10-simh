/* kx10_crt.c: Interface to remote vector displays.

   Copyright (c) 2021, Lars Brinkhoff

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
#include "sim_tmxr.h"
#include "sim_video.h"

#define CRT_LINES 6

static t_stat crt_rx_svc (UNIT *uptr);
static t_stat crt_tx_svc (UNIT *uptr);
static t_stat crt_reset (DEVICE *dptr);
static t_stat crt_attach (UNIT *uptr, CONST char *cptr);
static t_stat crt_detach (UNIT *uptr);

static TMLN crt_ldsc[CRT_LINES];
static TMXR crt_desc = { CRT_LINES, 0, 0, crt_ldsc };

#define SIZE 10240
static uint8 buffer[CRT_LINES][SIZE];
static int head[CRT_LINES];
static int tail[CRT_LINES];
static uint8 input[CRT_LINES][3];
static int input_idx[CRT_LINES];
static void (*send_info) (int);

static UNIT crt_unit[CRT_LINES + 1] = {
  { UDATA (&crt_rx_svc, UNIT_IDLE+UNIT_ATTABLE, 0) },
  { UDATA (&crt_tx_svc, UNIT_IDLE, 0) },
  { UDATA (&crt_tx_svc, UNIT_IDLE, 0) },
  { UDATA (&crt_tx_svc, UNIT_IDLE, 0) },
  { UDATA (&crt_tx_svc, UNIT_IDLE, 0) },
  { UDATA (&crt_tx_svc, UNIT_IDLE, 0) },
  { UDATA (&crt_tx_svc, UNIT_IDLE, 0) }
};

static UNIT *const crt_rx_unit = &crt_unit[0];
static UNIT *const crt_tx_unit = &crt_unit[1];

DEVICE crt_dev = {
  "CRT", crt_unit, NULL, NULL,
  CRT_LINES + 1, 8, 16, 1, 8, 16,
  NULL, NULL, crt_reset,
  NULL, crt_attach, crt_detach,
  NULL, DEV_DISABLE | DEV_DIS | DEV_DEBUG | DEV_DISPLAY, 0, dev_debug,
  NULL, NULL, NULL, NULL, NULL, NULL
};

static uint32 convert[] = {
  0,
  0,
  0,
  0,
  SIM_KEY_A,
  SIM_KEY_B,
  SIM_KEY_C,
  SIM_KEY_D,
  SIM_KEY_E,
  SIM_KEY_F,
  SIM_KEY_G,
  SIM_KEY_H,
  SIM_KEY_I,
  SIM_KEY_J,
  SIM_KEY_K,
  SIM_KEY_L,
  SIM_KEY_M, //10
  SIM_KEY_N,
  SIM_KEY_O,
  SIM_KEY_P,
  SIM_KEY_Q,
  SIM_KEY_R,
  SIM_KEY_S,
  SIM_KEY_T,
  SIM_KEY_U,
  SIM_KEY_V,
  SIM_KEY_W,
  SIM_KEY_X,
  SIM_KEY_Y,
  SIM_KEY_Z,
  SIM_KEY_1,
  SIM_KEY_2,
  SIM_KEY_3, //20
  SIM_KEY_4,
  SIM_KEY_5,
  SIM_KEY_6,
  SIM_KEY_7,
  SIM_KEY_8,
  SIM_KEY_9,
  SIM_KEY_0,
  SIM_KEY_ENTER,
  SIM_KEY_ESC,
  SIM_KEY_BACKSPACE,
  SIM_KEY_TAB,
  SIM_KEY_SPACE,
  SIM_KEY_MINUS,
  SIM_KEY_EQUALS,
  SIM_KEY_LEFT_BRACKET,
  SIM_KEY_RIGHT_BRACKET, //30
  SIM_KEY_BACKSLASH,
  SIM_KEY_UNKNOWN,  // hashtilde ???
  SIM_KEY_SEMICOLON,
  SIM_KEY_SINGLE_QUOTE,
  SIM_KEY_BACKQUOTE,
  SIM_KEY_COMMA,
  SIM_KEY_PERIOD,
  SIM_KEY_SLASH,
  SIM_KEY_CAPS_LOCK,
  SIM_KEY_F1,
  SIM_KEY_F2,
  SIM_KEY_F3,
  SIM_KEY_F4,
  SIM_KEY_F5,
  SIM_KEY_F6,
  SIM_KEY_F7, //40
  SIM_KEY_F8,
  SIM_KEY_F9,
  SIM_KEY_F10,
  SIM_KEY_F11,
  SIM_KEY_F12,
  SIM_KEY_PRINT,
  SIM_KEY_SCRL_LOCK,
  SIM_KEY_PAUSE,
  SIM_KEY_INSERT,
  SIM_KEY_HOME,
  SIM_KEY_PAGE_UP,
  SIM_KEY_DELETE,
  SIM_KEY_END,
  SIM_KEY_PAGE_DOWN,
  SIM_KEY_RIGHT,
  SIM_KEY_LEFT, //50
  SIM_KEY_DOWN,
  SIM_KEY_UP,
  SIM_KEY_NUM_LOCK,
  SIM_KEY_KP_DIVIDE, //54
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //5F
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //6F
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //7F
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //8F
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //9F
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //AF
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //BF
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //CF
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //DF
  SIM_KEY_CTRL_L,
  SIM_KEY_SHIFT_L,
  SIM_KEY_ALT_L,
  SIM_KEY_WIN_L,
  SIM_KEY_CTRL_R,
  SIM_KEY_SHIFT_R,
  SIM_KEY_ALT_R,
  SIM_KEY_WIN_R,
  0, 0, 0, 0, 0, 0, 0, 0, //EF
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 //FF
};

static void
handle_input (int line)
{
  SIM_KEY_EVENT ev;

  sim_debug (DEBUG_DATA, &crt_dev, "Display %d received character %02x\n",
             line, input[line][2]);

  switch (input[line][1]) {
  case 1:
    ev.state = SIM_KEYPRESS_DOWN;
    break;
  case 2:
    ev.state = SIM_KEYPRESS_UP;
    break;
  default:
    sim_debug (DEBUG_EXP, &crt_dev, "Bad input.\n");
    return;
  }
  ev.key = convert[input[line][2]];
  ev.dev = &iii_dev;
  ev.vptr = NULL;

  extern int dkb_keyboard_line (SIM_KEY_EVENT *, int);
  dkb_keyboard_line (&ev, line);
}

static t_stat
crt_rx_svc (UNIT *uptr)
{
  int32 ch, ln;

  ln = tmxr_poll_conn (&crt_desc);
  if (ln >= 0) {
    crt_ldsc[ln].rcve = 1;
    crt_ldsc[ln].xmte = 1;
    sim_debug (DEBUG_CMD, &crt_dev, "Connect display %d\n", ln);
    if (send_info)
      send_info (ln);
  }

  tmxr_poll_rx (&crt_desc);
  for (ln = 0; ln < CRT_LINES; ln++) {
    if (crt_ldsc[ln].conn) {
      ch = tmxr_getc_ln (&crt_ldsc[ln]);
      if (ch & TMXR_VALID) {
        input[ln][input_idx[ln]++] = ch & 0xFF;
        if (input_idx[ln] == 3) {
          handle_input (ln);
          input_idx[ln] = 0;
        }
      }
    } else if (crt_ldsc[ln].rcve) {
      sim_debug (DEBUG_CMD, &crt_dev, "Disconnect display %d\n", ln);
      crt_ldsc[ln].rcve = 0;
      crt_ldsc[ln].xmte = 0;
    }
  }

  sim_activate_after (uptr, 10000);
  return SCPE_OK;
}

static t_stat
crt_tx_svc (UNIT *uptr)
{
  int32 ch, ln;

  tmxr_poll_tx (&crt_desc);

  ln = uptr - crt_tx_unit;
  if (!tmxr_txdone_ln (&crt_ldsc[ln]))
    return sim_activate_after (uptr, 100);

  while (tail[ln] != head[ln]) {
    ch = buffer[ln][tail[ln]];
    if (tmxr_putc_ln (&crt_ldsc[ln], ch) == SCPE_STALL) {
      return sim_activate_after (uptr, 100);
    } else {
      tmxr_poll_tx (&crt_desc);
      tail[ln]++;
      if (tail[ln] == SIZE)
        tail[ln] = 0;
    }
  }

  return SCPE_OK;
}

static t_stat crt_reset (DEVICE *dptr)
{
  if (sim_switches & SWMASK('P')) {
    memset (head, 0, sizeof head);
    memset (tail, 0, sizeof tail);
    memset (input_idx, 0, sizeof input_idx);
  } else if (dptr->flags & DEV_DIS) {
    sim_cancel (crt_rx_unit);
  } else {
    sim_activate (crt_rx_unit, 1);
  }
  return SCPE_OK;
}

static t_stat
crt_attach (UNIT *uptr, CONST char *cptr)
{
  t_stat r;

  r = tmxr_attach (&crt_desc, uptr, cptr);
  if (r != SCPE_OK)
    return r;

  return SCPE_OK;
}

static t_stat
crt_detach (UNIT *uptr)
{
  if (!(uptr->flags & UNIT_ATT))
    return SCPE_OK;
  if (sim_is_active (uptr))
    sim_cancel (uptr);
  return tmxr_detach (&crt_desc, uptr);
}

static void transmit (int line, int x)
{
  x &= 0xFF;
  if (head[line] != tail[line]
      || tmxr_putc_ln (&crt_ldsc[line], x) == SCPE_STALL) {
    buffer[line][head[line]++] = x;
    if (head[line] == SIZE)
      head[line] = 0;
    if (!sim_is_active (&crt_tx_unit[line]))
      sim_activate (&crt_tx_unit[line], 1);
  }
}

static int current_x = -1;
static int current_y = -1;

void crt_point (int line, int x, int y)
{
  int diff, dx, dy;
  sim_debug (DEBUG_DETAIL, &crt_dev, "Display %d point %d,%d\n", line, x, y);
  if (line >= CRT_LINES)
    return;
  if (!crt_ldsc[line].conn) {
    head[line] = tail[line] = 0;
    return;
  }
  diff = head[line] - tail[line];
  if (diff < 0)
    diff += SIZE;
  if (diff > SIZE - 100) {
    sim_debug (DEBUG_EXP, &crt_dev, "Display %d buffer full.\n", line);
    return;
  }
  if (x < 0 || x > 1023 || y < 0 || y > 1023) {
    sim_debug (DEBUG_EXP, &crt_dev, "Display %d point outside CRT.\n", line);
    return;
  }
  dx = x - current_x;
  dy = y - current_y;
  if (dx < 128 && dx >= -128 && dy < 128 && dy >= -128) {
    transmit (line, 5);
    transmit (line, dx);
    transmit (line, dy);
  } else {
    transmit (line, 2);
    transmit (line, x >> 8);
    transmit (line, x);
    transmit (line, y >> 8);
    transmit (line, y);
  }
  current_x = x;
  current_y = y;
}

void crt_line (int line, int x1, int y1, int x2, int y2)
{
  int diff, dx, dy;
  sim_debug (DEBUG_DETAIL, &crt_dev, "Display %d line %d,%d-%d,%d\n",
             line, x1, y2, x2, y2);
  if (line >= CRT_LINES)
    return;
  if (!crt_ldsc[line].conn) {
    head[line] = tail[line] = 0;
    return;
  }
  diff = head[line] - tail[line];
  if (diff < 0)
    diff += SIZE;
  if (diff > SIZE - 100) {
    sim_debug (DEBUG_EXP, &crt_dev, "Display %d buffer full.\n", line);
    return;
  }
  if (x1 < 0 || x1 > 1023 || y1 < 0 || y1 > 1023
      || x2 < 0 || x2 > 1023 || y2 < 0 || y2 > 1023) {
    sim_debug (DEBUG_EXP, &crt_dev, "Display %d line outside CRT.\n", line);
    return;
  }
  dx = x2 - x1;
  dy = y2 - y1;
  if (x1 == current_x && y1 == current_y
      && dx < 128 && dx >= -128 && dy < 128 && dy >= -128) {
    transmit (line, 4);
    transmit (line, dx);
    transmit (line, dy);
  } else {
    transmit (line, 3);
    transmit (line, x1 >> 8);
    transmit (line, x1);
    transmit (line, y1 >> 8);
    transmit (line, y1);
    transmit (line, x2 >> 8);
    transmit (line, x2);
    transmit (line, y2 >> 8);
    transmit (line, y2);
  }
  current_x = x2;
  current_y = y2;
}

void crt_info (void (*callback) (int))
{
  send_info = callback;
}
