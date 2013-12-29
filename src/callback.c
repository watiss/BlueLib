/*
 *  BlueLib - Abstraction layer for Bluetooth Low Energy softwares
 *
 *  Copyright (C) 2011  Nokia Corporation
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

#include <glib.h>
#include <malloc.h>
#include <unistd.h>
#include <stdint.h>

#include "uuid.h"
#include "gattrib.h"
#include "att.h"
#include "gatt.h"

#include "bluelib.h"
#include "bluelib_gatt.h"
#include "conn_state.h"
#include "callback.h"
#include "gatt_def.h"


static GMainLoop  *event_loop    = NULL;
static GThread    *event_thread  = NULL;

// Avoid ressources deadlock
static GMutex      cb_mutex;

// The callback and the functions are running in two seperate thread, we need
// to use transport variables to return the results.

#define CB_TIMEOUT_S 120 /* For every function that have a callback function.
                          * We will wait 2 minutes before returning */

//#define DEBUG_ON       // Activate the Debug print
#ifdef DEBUG_ON
#define printf_dbg(...) printf("[CB] " __VA_ARGS__)
#else
#define printf_dbg(...)
#endif

/*
 * Global functions
 */
int wait_for_cb(cb_ctx_t *cb_ctx)
{
  int wait_cnt = 0;
  if (!g_mutex_trylock(&cb_ctx->cb_mutex) && is_event_loop_running()) {
    printf_dbg("Waiting for callback\n");
    while (is_event_loop_running() && !g_mutex_trylock(&cb_ctx->cb_mutex)) {
      usleep(100000);

      if (wait_cnt < CB_TIMEOUT_S*10) {
        wait_cnt++;
      } else {
        GError *err = g_error_new(BL_ERROR_DOMAIN, BL_NO_CALLBACK_ERROR,
                                  "Timeout no callback received\n");
        printf_dbg("%s", err->message);
        g_propagate_error(cb_ctx->gerr, err);;
        set_conn_state(STATE_DISCONNECTED);
        return BL_NO_CALLBACK_ERROR;
      }
    }
  }

  if (!is_event_loop_running()) {
    set_conn_state(STATE_DISCONNECTED);
    GError *err = g_error_new(BL_ERROR_DOMAIN, BL_DISCONNECTED_ERROR,
                              "Event loop is not running\n");
    printf_dbg("%s", err->message);
    g_propagate_error(cb_ctx->gerr, err);;
    return BL_DISCONNECTED_ERROR;
  } else {
    if (*cb_ctx->gerr)  {
      return (*cb_ctx->gerr)->code;
    }
  }
    return BL_NO_ERROR;
}


/*
 * Event loop thread
 */
static gpointer _event_thread(gpointer data)
{
  printf_dbg("Event loop START\n");
  g_mutex_lock(&cb_mutex);
  event_loop = g_main_loop_new(NULL, FALSE);
  g_mutex_unlock(&cb_mutex);
  g_main_loop_run(event_loop);
  g_main_loop_unref(event_loop);
  event_loop = NULL;
  printf_dbg("Event loop EXIT\n");
  g_thread_exit(0);
  return 0;
}

int start_event_loop(GError **gerr)
{
  g_mutex_init(&cb_mutex);

  event_thread = g_thread_try_new("event_loop", _event_thread, NULL, gerr);

  for (int cnt = 0;
       (!is_event_loop_running()) && (cnt < 60) && (event_thread != NULL) &&
       (get_conn_state() == STATE_CONNECTING);
       cnt++) {
    sleep(1);
    printf_dbg("wait for event loop\n");
  }

  if (event_thread == NULL) {
    printf_dbg("%s\n", (*gerr)->message);
    return BL_DISCONNECTED_ERROR;
  }
  return BL_NO_ERROR;
}

void stop_event_loop(void)
{
  g_mutex_lock(&cb_mutex);
  if (event_loop)
    g_main_loop_quit(event_loop);
  g_mutex_unlock(&cb_mutex);
}

int is_event_loop_running(void)
{
  g_mutex_lock(&cb_mutex);
  int ret = ((event_thread != NULL) && (event_loop != NULL));
  g_mutex_unlock(&cb_mutex);
  return ret;
}

/*
 * Callback functions
 */
void connect_cb(GIOChannel *io, GError *err, gpointer user_data)
{
  conn_cb_ctx_t *conn_cb_ctx = user_data;
  printf_dbg("Connect callback\n");
  if (err) {
    set_conn_state(STATE_DISCONNECTED);
    g_propagate_error(conn_cb_ctx->cb_ctx.gerr, err);;
  } else {
    conn_cb_ctx->attrib = g_attrib_new(conn_cb_ctx->iochannel);
    set_conn_state(STATE_CONNECTED);
  }
  g_mutex_unlock(&conn_cb_ctx->cb_ctx.cb_mutex);
}

void primary_all_cb(GSList *services, guint8 status,
                    gpointer user_data)
{
  cb_ctx_t *cb_ctx          = user_data;
  GSList   *l               = NULL;
  GSList   *bl_primary_list = NULL;
  GError   *err             = NULL;

  printf_dbg("Primary all callback\n");
  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR, " ");
    goto error;
  }

  if (services == NULL) {
    printf_dbg("Nothing found\n");
    goto exit;
  }

  for (l = services; l; l = l->next) {
    struct gatt_primary *prim = l->data;
    bl_primary_t *bl_primary = bl_primary_new(prim->uuid, prim->changed,
                                              prim->range.start, prim->range.end);

    g_free(prim);

    if (bl_primary == NULL) {
      goto malloc_error;
    }
    if (bl_primary_list == NULL) {
      bl_primary_list = g_slist_alloc();
      if (bl_primary_list == NULL) {
        goto malloc_error;
      }
      bl_primary_list->data = bl_primary;
    } else {
      bl_primary_list = g_slist_append(bl_primary_list, bl_primary);
    }
  }

  cb_ctx->p_ret = bl_primary_list;
  printf_dbg("Success\n");
  goto exit;

malloc_error:
  err = g_error_new(BL_ERROR_DOMAIN, BL_MALLOC_ERROR, "Malloc error\n");
error:
  g_propagate_error(cb_ctx->gerr, err);;
  if (bl_primary_list)
    bl_primary_list_free(bl_primary_list);
  printf_dbg("Error\n");
exit:
  if (l)
    g_slist_free(l);
  g_mutex_unlock(&cb_ctx->cb_mutex);
}

void primary_by_uuid_cb(GSList *ranges, guint8 status,
                        gpointer user_data)
{
  cb_ctx_t *cb_ctx          = user_data;
  GSList   *l               = NULL;
  GSList   *bl_primary_list = NULL;
  GError   *err             = NULL;
  printf_dbg("Primary by UUID callback\n");

  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR,
                      " ");
    goto error;
  }
  if (ranges == NULL) {
    printf_dbg("Nothing found\n");
    goto exit;
  }

  for (l = ranges; l; l = l->next) {
    struct att_range *range = l->data;
    bl_primary_t *bl_primary = bl_primary_new(NULL, 0, range->start,
                                              range->end);
    free(range);

    if (bl_primary == NULL) {
      goto malloc_error;
    }
    if (bl_primary_list == NULL) {
      bl_primary_list = g_slist_alloc();

      if (bl_primary_list == NULL) {
        goto malloc_error;
      }
      bl_primary_list->data = bl_primary;
    } else {
      bl_primary_list = g_slist_append(bl_primary_list, bl_primary);
    }
  }
  cb_ctx->p_ret = bl_primary_list;
  printf_dbg("Success\n");
  goto exit;

malloc_error:
  err = g_error_new(BL_ERROR_DOMAIN, BL_MALLOC_ERROR, "Malloc error\n");
error:
  g_propagate_error(cb_ctx->gerr, err);;
  if (bl_primary_list)
    bl_primary_list_free(bl_primary_list);
  printf_dbg("Error\n");
exit:
  g_mutex_unlock(&cb_ctx->cb_mutex);
}

void included_cb(GSList *includes, guint8 status, gpointer user_data)
{
  cb_ctx_t *cb_ctx           = user_data;
  GSList   *l                = NULL;
  GSList   *bl_included_list = NULL;
  GError   *err              = NULL;

  printf_dbg("Included callback\n");
  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR,
                      " ");
    goto error;
  }

  if (includes == NULL) {
    printf_dbg("Nothing found\n");
    goto exit;
  }

  for (l = includes; l; l = l->next) {
    struct gatt_included *incl = l->data;
    bl_included_t *bl_included = bl_included_new(incl->uuid, incl->handle,
                                                 incl->range.start, incl->range.end);
    if (bl_included == NULL) {
      goto malloc_error;
    }
    if (bl_included_list == NULL) {
      bl_included_list = g_slist_alloc();
      if (bl_included_list == NULL) {
        goto malloc_error;
      }
      bl_included_list->data = bl_included;
    } else {
      bl_included_list = g_slist_append(bl_included_list, bl_included);
    }
  }

  cb_ctx->p_ret = bl_included_list;
  printf_dbg("Success\n");
  goto exit;

malloc_error:
  err = g_error_new(BL_ERROR_DOMAIN, BL_MALLOC_ERROR, "Malloc error\n");
error:
  g_propagate_error(cb_ctx->gerr, err);;
  if (bl_included_list)
    bl_included_list_free(bl_included_list);
  printf_dbg("Error\n");
exit:
  if (l)
    g_slist_free(l);
  g_mutex_unlock(&cb_ctx->cb_mutex);
}

void char_by_uuid_cb(GSList *characteristics, guint8 status,
                     gpointer user_data)
{
  cb_ctx_t *cb_ctx     = user_data;
  GSList *l            = NULL;
  GSList *bl_char_list = NULL;
  GError *err          = NULL;

  printf_dbg("Characteristic by UUID callback\n");
  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR,
                      " ");
    goto error;
  }

  for (l = characteristics; l; l = l->next) {
    // Extract data
    struct gatt_char *chars = l->data;
    bl_char_t *bl_char = bl_char_new(chars->uuid, chars->handle,
                                     chars->properties, chars->value_handle);

    // Add it to the characteristic
    if (bl_char == NULL)
      goto malloc_error;

    // Append it to the list
    if (bl_char_list == NULL) {
      bl_char_list = g_slist_alloc();

      if (bl_char_list == NULL)
        goto malloc_error;

      bl_char_list->data = bl_char;
    } else {
      bl_char_list = g_slist_append(bl_char_list, bl_char);
    }
  }

  cb_ctx->p_ret = bl_char_list;
  printf_dbg("Success\n");
  goto exit;

malloc_error:
  err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR,
                    "Malloc error\n");
error:
  g_propagate_error(cb_ctx->gerr, err);;
  if (bl_char_list)
    bl_char_list_free(bl_char_list);
  printf_dbg("Error\n");
exit:
  if (l)
    g_slist_free(l);
  g_mutex_unlock(&cb_ctx->cb_mutex);
}

void char_desc_cb(guint8 status, const guint8 *pdu, guint16 plen,
                  gpointer user_data) {
  char_desc_cb_ctx_t   *cd_cb_ctx = user_data;
  struct att_data_list *list   = NULL;
  GError               *err = NULL;
  guint8                format;
  uint16_t              handle = 0xffff;
  int                   i;
  char                  uuid_str[MAX_LEN_UUID_STR];
  uint8_t              *value;

  printf_dbg("[CB] IN char_desc_cb\n");
  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR,
                      " ");
    goto error;
  }

  list = dec_find_info_resp(pdu, plen, &format);
  if (list == NULL) {
    printf("Nothing found\n");
    goto exit;
  }

  for (i = 0; i < list->num; i++) {
    bt_uuid_t uuid;

    value = list->data[i];
    handle = att_get_u16(value);

    if (format == 0x01)
      uuid = att_get_uuid16(&value[2]);
    else
      uuid = att_get_uuid128(&value[2]);

    bt_uuid_to_string(&uuid, uuid_str, MAX_LEN_UUID_STR);
    if (strcmp(uuid_str, GATT_PRIM_SVC_UUID_STR) &&
        strcmp(uuid_str, GATT_SND_SVC_UUID_STR)  &&
        strcmp(uuid_str, GATT_INCLUDE_UUID_STR)  &&
        strcmp(uuid_str, GATT_CHARAC_UUID_STR)) {
      bl_desc_t *bl_desc = bl_desc_new(uuid_str, handle);
      if (bl_desc == NULL) {
        goto malloc_error;
      }
      if (cd_cb_ctx->bl_desc_list == NULL) {
        cd_cb_ctx->bl_desc_list = g_slist_alloc();
        if (cd_cb_ctx->bl_desc_list == NULL) {
          goto malloc_error;
        }
        cd_cb_ctx->bl_desc_list->data = bl_desc;
      } else {
        cd_cb_ctx->bl_desc_list = g_slist_append(cd_cb_ctx->bl_desc_list,
                                                 bl_desc);
      }
    } else {
      printf_dbg("Reach end of descriptor list\n");
      goto exit;
    }
  }
  if ((handle != 0xffff) && (handle < cd_cb_ctx->end_handle)) {
    printf_dbg("[CB] New request\n");
    if (gatt_discover_char_desc(cd_cb_ctx->attrib, handle + 1,
                                cd_cb_ctx->end_handle, char_desc_cb, cd_cb_ctx)) {
      goto proceed;
    }
    err = g_error_new(BL_ERROR_DOMAIN, BL_SEND_REQUEST_ERROR,
                      "Unable to send request\n");
    goto error;
  }

malloc_error:
  err = g_error_new(BL_ERROR_DOMAIN, BL_MALLOC_ERROR, "Malloc error\n");
error:
  g_propagate_error(cd_cb_ctx->cb_ctx.gerr, err);;
exit:
  if (cd_cb_ctx->bl_desc_list) {
    // Return what we got if we add something
    cd_cb_ctx->cb_ctx.p_ret = cd_cb_ctx->bl_desc_list;
  }
  g_mutex_unlock(&cd_cb_ctx->cb_ctx.cb_mutex);
proceed:
  if (list)
    att_data_list_free(list);
  printf_dbg("[CB] OUT char_desc_cb\n");
}

void read_by_hnd_cb(guint8 status, const guint8 *pdu, guint16 plen,
                    gpointer user_data)
{
  cb_ctx_t *cb_ctx = user_data;
  GError   *err    = NULL;
  uint8_t  data[plen];
  ssize_t  vlen;

  printf_dbg("[CB] IN read_by_hnd_cb\n");
  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR, " ");
    goto error;
  }

  if (data == NULL) {
    printf("Nothing found\n");
    goto exit;
  }

  vlen = dec_read_resp(pdu, plen, data, sizeof(data));
  if (vlen < 0) {
    err = g_error_new(BL_ERROR_DOMAIN, BL_PROTOCOL_ERROR,
                      "Protocol error\n");
    goto error;
  }

  cb_ctx->p_ret = bl_value_new(NULL, 0, vlen, data);
  if (cb_ctx->p_ret == NULL)
    goto malloc_error;
  goto exit;

malloc_error:
  err = g_error_new(BL_ERROR_DOMAIN, BL_MALLOC_ERROR, "Malloc error\n");
error:
  g_propagate_error(cb_ctx->gerr, err);;
  if (cb_ctx->p_ret);
  bl_value_free(cb_ctx->p_ret);
exit:
  g_mutex_unlock(&cb_ctx->cb_mutex);
  printf_dbg("[CB] OUT read_by_hnd_cb\n");
}

void read_by_uuid_cb(guint8 status, const guint8 *pdu, guint16 plen,
                     gpointer user_data)
{
  cb_ctx_t             *cb_ctx = user_data;
  struct att_data_list *list;
  GSList               *bl_value_list = NULL;
  GError               *err = NULL;

  printf_dbg("[CB] IN read_by_uuid_cb\n");
  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR, " ");
    goto error;
  }

  list = dec_read_by_type_resp(pdu, plen);
  if (list == NULL) {
    printf("Nothing found\n");
    goto exit;
  }

  for (int i = 0; i < list->num; i++) {
    bl_value_t *bl_value = bl_value_new(NULL, att_get_u16(list->data[i]),
                                        list->len - 2, list->data[i] + 2);
    if (bl_value == NULL)
      goto malloc_error;

    // Add it to the value list
    if (bl_value_list == NULL) {
      bl_value_list = g_slist_alloc();
      if (bl_value_list == NULL)
        goto malloc_error;

      bl_value_list->data = bl_value;
    } else {
      bl_value_list = g_slist_append(bl_value_list, bl_value);
    }
  }
  att_data_list_free(list);
  cb_ctx->p_ret = bl_value_list;
  goto exit;

malloc_error:
  err = g_error_new(BL_ERROR_DOMAIN, BL_MALLOC_ERROR, "Malloc error\n");
  att_data_list_free(list);
error:
  g_propagate_error(cb_ctx->gerr, err);;
  if (bl_value_list)
    bl_value_list_free(bl_value_list);
exit:
  g_mutex_unlock(&cb_ctx->cb_mutex);
}

void write_req_cb(guint8 status, const guint8 *pdu, guint16 plen,
                  gpointer user_data)
{
  cb_ctx_t *cb_ctx = user_data;
  GError   *err    = NULL;
  printf_dbg("[CB] IN write_req_cb\n");
  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR, " ");
    g_propagate_error(cb_ctx->gerr, err);;
    goto end;
  }

  if (!dec_write_resp(pdu, plen) && !dec_exec_write_resp(pdu, plen)) {
    err = g_error_new(BL_ERROR_DOMAIN, BL_PROTOCOL_ERROR,
                      "Protocol error\n");
    goto end;
  }

end:
  g_mutex_unlock(&cb_ctx->cb_mutex);
  printf_dbg("[CB] OUT write_req_cb\n");
}

void exchange_mtu_cb(guint8 status, const guint8 *pdu, guint16 plen,
                     gpointer user_data)
{
  mtu_cb_ctx_t *mtu_cb_ctx = user_data;
  GError   *err    = NULL;
  printf_dbg("[CB] IN exchange_mtu_cb\n");

  if (status) {
    printf("%s\n",  att_ecode2str(status));
    err = g_error_new(BL_ERROR_DOMAIN, BL_REQUEST_FAIL_ERROR, " ");
    goto error;
  }

  if (!dec_mtu_resp(pdu, plen, &mtu_cb_ctx->mtu)) {
    err = g_error_new(BL_ERROR_DOMAIN, BL_PROTOCOL_ERROR,
                      "Protocol error\n");
    goto error;
  }

  mtu_cb_ctx->mtu = MIN(mtu_cb_ctx->mtu, mtu_cb_ctx->opt_mtu);
  /* Set new value for MTU in client */
  if (!g_attrib_set_mtu(mtu_cb_ctx->attrib, mtu_cb_ctx->mtu)) {
    err = g_error_new(BL_ERROR_DOMAIN, BL_PROTOCOL_ERROR,
                      "Unable to set new MTU value\n");
  } else {
    printf_dbg(cb_ret_msg, "Success\n");
  }

  goto exit;
error:
  g_propagate_error(mtu_cb_ctx->cb_ctx.gerr, err);;
exit:
  g_mutex_unlock(&mtu_cb_ctx->cb_ctx.cb_mutex);
  printf_dbg("[CB] OUT exchange_mtu_cb\n");
}
