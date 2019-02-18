/*
 * Copyright (c) 2019 Lars Brinkhoff
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the names of the authors shall
 * not be used in advertising or otherwise to promote the sale, use or
 * other dealings in this Software without prior written authorization
 * from the authors.
 */

#include <stdio.h>
#include "display.h"                    /* XY plot interface */
#include "clr.h"

static void *clr_dptr;
static int clr_dbit;

#define DEVICE void
extern void _sim_debug_device (unsigned int dbits, DEVICE* dptr, const char* fmt, ...);

#define DEBUGF(...) _sim_debug_device (clr_dbit, clr_dptr, ##  __VA_ARGS__)

int clr_scale = PIX_SCALE;

static int x_deflection = 0;
static int y_deflection = 0;

static unsigned char sync_period = 0;

int
clr_init(void *dev, int debug)
{
  clr_dptr = dev;
  clr_dbit = debug;
  return display_init(DIS_CLR, clr_scale, clr_dptr);
}

void clr_deflection (int x, int y)
{
  x_deflection = x;
  y_deflection = y;
}

#define CLAMP(_X) ((_X) >= 0 ? (_X) : 0)

void clr_intensities (int rgb)
{
  int r = (rgb >> 12) & 037;
  int g = (rgb >>  6) & 037;
  int b = (rgb >>  0) & 037;
  r = CLAMP(r-020)>>1;
  g = CLAMP(g-020)>>1;
  b = CLAMP(b-020)>>1;
  DEBUGF("POINT %d %d\n", x_deflection, y_deflection);
  //display_point(x_deflection, y_deflection, r, 0);
  display_point(x_deflection, y_deflection, 7, 1);
  //display_point(x_deflection, y_deflection, b, 2);
}

int clr_cycle(int us, int slowdown)
{
  static int usec = 0;
  static int msec = 0;
  int new_msec;

  new_msec = (usec += us) / 1000;

  /* if awaiting sync, look for next frame start */
  if (sync_period && (msec / sync_period != new_msec / sync_period))
    sync_period = 0;                /* start next frame */

  msec = new_msec;

  if (sync_period)
    goto age_ret;

 age_ret:
  display_age(us, slowdown);
  return 1;
}
