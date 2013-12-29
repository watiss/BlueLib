/*
 *  BlueLib - Abstraction layer for Bluetooth Low Energy softwares
 *
 *  Copyright (C) 2013  Netatmo
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef _CALLBACK_H_
#define _CALLBACK_H_

#include <stdint.h>

// Event loop
int  start_event_loop(GError **gerr);
void stop_event_loop(void);
int  is_event_loop_running(void);

// Callback global context
// GError points to Null if no error else it's allocated
typedef struct {
  GMutex   cb_mutex;
  void    *p_ret;
  GError **gerr;
} cb_ctx_t;

// Use this function before using in call user data.
#define CB_CTX_INIT(cb_ctx, pp_gerr)      \
  do {                                    \
    memset(&cb_ctx, 0, sizeof(cb_ctx_t)); \
    /* Be sure to alloc an unlock mutex*/ \
    g_mutex_unlock(&cb_ctx.cb_mutex);     \
    g_mutex_lock(&cb_ctx.cb_mutex);       \
    cb_ctx.gerr = pp_gerr;                \
    if (cb_ctx.gerr && *cb_ctx.gerr) {    \
      g_error_free(*cb_ctx.gerr);         \
      *cb_ctx.gerr = NULL;                \
    }                                     \
  } while (0)

// Callbacks specific contexts
typedef struct {
  cb_ctx_t    cb_ctx;
  GAttrib    *attrib;
  GIOChannel *iochannel;
} conn_cb_ctx_t;

typedef struct {
  cb_ctx_t   cb_ctx;
  uint16_t   end_handle;
  GSList    *bl_desc_list;
  GAttrib   *attrib;
} char_desc_cb_ctx_t;

typedef struct {
  cb_ctx_t cb_ctx;
  uint16_t mtu;
  int      opt_mtu;
  GAttrib *attrib;
} mtu_cb_ctx_t;

// Block the calling thread while waiting for the callback
int wait_for_cb(cb_ctx_t *ctx);

// Callbacks
void connect_cb(GIOChannel *io, GError *err, gpointer user_data);
void primary_all_cb(GSList *services, guint8 status,
    gpointer user_data);
void primary_by_uuid_cb(GSList *ranges, guint8 status,
    gpointer user_data);
void included_cb(GSList *includes, guint8 status, gpointer user_data);
void char_by_uuid_cb(GSList *characteristics, guint8 status,
    gpointer user_data);
void char_desc_cb(guint8 status, const guint8 *pdu, guint16 plen,
    gpointer user_data);
void read_by_hnd_cb(guint8 status, const guint8 *pdu, guint16 plen,
    gpointer user_data);
void read_by_uuid_cb(guint8 status, const guint8 *pdu,
    guint16 plen, gpointer user_data);
void write_req_cb(guint8 status, const guint8 *pdu, guint16 plen,
    gpointer user_data);
void exchange_mtu_cb(guint8 status, const guint8 *pdu, guint16 plen,
    gpointer user_data);
#endif
