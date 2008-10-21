/*
 * Copyright 2008 Evenflow, Inc.
 *
 * nautilus-dropbox-tray.h
 * Header file for nautilus-dropbox-tray.c
 *
 * This file is part of nautilus-dropbox.
 *
 * nautilus-dropbox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nautilus-dropbox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nautilus-dropbox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef NAUTILUS_DROPBOX_TRAY_H
#define NAUTILUS_DROPBOX_TRAY_H

#include <glib.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include <libnotify/notify.h>

#include "dropbox-client.h"

G_BEGIN_DECLS

typedef void (*DropboxTrayBubbleActionCB)(gpointer);

typedef enum {UPTODATE, SYNCING, NOT_CONNECTED} DropboxIconState;

typedef struct {  GtkStatusIcon *status_icon;
  GtkMenu *context_menu;
  NotifyNotification *bubble;
  gboolean notify_inited;
  DropboxClient *dc;
  struct {
    gint rs;
    guint timer;
    gboolean was_connected;
  } ca;
} NautilusDropboxTray;

void
nautilus_dropbox_tray_setup(NautilusDropboxTray *ndt, DropboxClient *dc);

void
nautilus_dropbox_tray_start(NautilusDropboxTray *ndt);

gboolean
nautilus_dropbox_tray_bubble(NautilusDropboxTray *ndt,
			     const gchar *caption,
			     const gchar *message,
			     DropboxTrayBubbleActionCB cb,
			     const gchar *cb_desc,
			     gpointer ud,
			     GFreeFunc free_func,
			     GError **gerr);

G_END_DECLS

#endif
