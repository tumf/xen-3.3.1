#ifndef _XENFB_H_
#define _XENFB_H_

#include "vl.h"
#include <stdbool.h>
#include <sys/types.h>

struct xenfb;

struct xenfb *xenfb_new(int domid, DisplayState *ds);
void xenfb_shutdown(struct xenfb *xenfb);
extern const unsigned char atkbd_set2_keycode[512];
extern const unsigned char atkbd_unxlate_table[128];

#endif
