/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * nemo-progress-ui-handler.c: file operation progress user interface.
 *
 * Copyright (C) 2007, 2011 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street - Suite 500,
 * Boston, MA 02110-1335, USA.
 *
 * Authors: Alexander Larsson <alexl@redhat.com>
 *          Cosimo Cecchi <cosimoc@redhat.com>
 *
 */

#include <config.h>

#include "nemo-progress-ui-handler.h"

#include "nemo-application.h"
#include "nemo-progress-info-widget.h"

#include <glib/gi18n.h>

#include <eel/eel-string.h>

#include <libnemo-private/nemo-progress-info.h>
#include <libnemo-private/nemo-progress-info-manager.h>

#include <libnotify/notify.h>
#include <libxapp/xapp-gtk-window.h>
#include <libxapp/xapp-status-icon.h>

struct _NemoProgressUIHandlerPriv {
	NemoProgressInfoManager *manager;

	GtkWidget *progress_window;
	GtkWidget *window_vbox;

    GtkWidget *list;

	guint active_infos;
    guint active_percent;
	GList *infos;

	XAppStatusIcon *status_icon;
    gboolean should_show_status_icon;
};

G_DEFINE_TYPE (NemoProgressUIHandler, nemo_progress_ui_handler, G_TYPE_OBJECT);

static void
status_icon_activate_cb (XAppStatusIcon        *icon,
                         guint                  button,
                         guint                  _time,
                         NemoProgressUIHandler *self)
{
    self->priv->should_show_status_icon = FALSE;
    xapp_status_icon_set_visible (icon, FALSE);
    gtk_window_present (GTK_WINDOW (self->priv->progress_window));
}

static void
progress_ui_handler_ensure_status_icon (NemoProgressUIHandler *self)
{
	XAppStatusIcon *status_icon;

	if (self->priv->status_icon != NULL) {
		return;
	}

    status_icon = xapp_status_icon_new ();
    xapp_status_icon_set_icon_name (status_icon, "progress-0-symbolic");
    g_signal_connect (status_icon, "activate",
                      (GCallback) status_icon_activate_cb,
                      self);

	xapp_status_icon_set_visible (status_icon, FALSE);

	self->priv->status_icon = status_icon;
}

static gchar *
get_icon_name_from_percent (guint pct)
{
    gchar *icon_name;
    guint rounded = 0;
    gint ones = pct % 10;

    if (ones < 5)
        rounded = pct - ones;
    else
        rounded = pct + (10 - ones);

    icon_name = g_strdup_printf ("progress-%d", rounded);

    return icon_name;
}

static void
progress_ui_handler_update_status_icon (NemoProgressUIHandler *self)
{
	gchar *tooltip;

	progress_ui_handler_ensure_status_icon (self);
    gchar *launchpad_sucks = THOU_TO_STR (self->priv->active_infos);
    tooltip = g_strdup_printf (ngettext ("%1$s file operation active.  %2$d%% complete.",
                               "%1$s file operations active.  %2$d%% complete.",
                               self->priv->active_infos),
                               launchpad_sucks, self->priv->active_percent);
	xapp_status_icon_set_tooltip_text (self->priv->status_icon, tooltip);
    gchar *name = get_icon_name_from_percent (self->priv->active_percent);
    xapp_status_icon_set_icon_name (self->priv->status_icon, name);
    g_free (name);
	g_free (tooltip);

	xapp_status_icon_set_visible (self->priv->status_icon, self->priv->should_show_status_icon);
}

static gboolean
progress_window_delete_event (GtkWidget *widget,
			      GdkEvent *event,
			      NemoProgressUIHandler *self)
{
    gtk_widget_hide (widget);

    self->priv->should_show_status_icon = TRUE;
    progress_ui_handler_update_status_icon (self);

    return TRUE;
}

static void
ensure_first_separator_hidden (NemoProgressUIHandler *self)
{
    GList *l = gtk_container_get_children (GTK_CONTAINER (self->priv->list));

    if (l == NULL)
        return;

    NemoProgressInfoWidgetPriv *priv = NEMO_PROGRESS_INFO_WIDGET (l->data)->priv;

    gtk_widget_hide (GTK_WIDGET (priv->separator));

    g_list_free (l);
}

static void
progress_ui_handler_sort_by_active (NemoProgressUIHandler *self)
{
    gint first_pending = -1;
    gint current_index = 0;
    GList *iter;
    GList *l = gtk_container_get_children (GTK_CONTAINER (self->priv->list));

    if (l == NULL)
        return;

    for (iter = l; iter != NULL; iter = iter->next) {
        NemoProgressInfoWidgetPriv *priv = NEMO_PROGRESS_INFO_WIDGET (iter->data)->priv;

        if (nemo_progress_info_get_is_started (priv->info)) {
            if (first_pending > 0) {
                gtk_box_reorder_child (GTK_BOX (self->priv->list), GTK_WIDGET (iter->data), first_pending);
                break;
            }
        } else {
            if (first_pending == -1) {
                first_pending = current_index;
            }
        }

        current_index++;
    }

    g_list_free (l);
}

static void
progress_ui_handler_ensure_window (NemoProgressUIHandler *self)
{
    NemoProgressUIHandlerPriv *priv = NEMO_PROGRESS_UI_HANDLER (self)->priv;

    GtkWidget *main_box, *progress_window;
    GtkWidget *w, *frame;

	if (self->priv->progress_window != NULL) {
		return;
	}
	
	progress_window = xapp_gtk_window_new (GTK_WINDOW_TOPLEVEL);
	self->priv->progress_window = progress_window;

    gtk_window_set_type_hint (GTK_WINDOW (progress_window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_resizable (GTK_WINDOW (progress_window), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (progress_window), 500, -1);

	gtk_window_set_title (GTK_WINDOW (progress_window),
			      _("File Operations"));
	gtk_window_set_wmclass (GTK_WINDOW (progress_window),
				"file_progress", "Nemo");
	gtk_window_set_position (GTK_WINDOW (progress_window),
				 GTK_WIN_POS_CENTER);
	xapp_gtk_window_set_icon_name (XAPP_GTK_WINDOW (progress_window),
                                   "system-run");

	main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
	gtk_container_add (GTK_CONTAINER (progress_window),
                       main_box);
	self->priv->window_vbox = main_box;

    frame = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_NONE);

    w = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    priv->list = w;
    gtk_container_add (GTK_CONTAINER (frame), w);

    g_object_set (priv->list,
                  "margin-left", 5,
                  "margin-right", 5,
                  "margin-top", 5,
                  "margin-bottom", 5,
                  NULL);

    gtk_box_pack_start (GTK_BOX (main_box), frame, FALSE, FALSE, 0);
    gtk_widget_show_all (main_box);

	g_signal_connect (progress_window,
			  "delete-event",
			  (GCallback) progress_window_delete_event, self);
}

static void
progress_ui_handler_add_to_window (NemoProgressUIHandler *self,
				   NemoProgressInfo *info)
{
	GtkWidget *progress;

	progress = nemo_progress_info_widget_new (info);

	progress_ui_handler_ensure_window (self);

    gtk_box_pack_start (GTK_BOX (self->priv->list), progress, FALSE, FALSE, 0);
    gtk_widget_show (progress);

    ensure_first_separator_hidden (self);
    progress_ui_handler_sort_by_active (self);
}

static void
progress_ui_handler_show_complete_notification (NemoProgressUIHandler *self)
{
	NotifyNotification *complete_notification;

	complete_notification = notify_notification_new (_("File Operations"),
							 _("All file operations have been successfully completed"),
							 NULL);
	notify_notification_show (complete_notification, NULL);

	g_object_unref (complete_notification);
}

static void
progress_ui_handler_hide_status (NemoProgressUIHandler *self)
{
	if (self->priv->status_icon != NULL) {
        self->priv->should_show_status_icon = FALSE;
		xapp_status_icon_set_visible (self->priv->status_icon, FALSE);
	}
}

static void
progress_info_finished_cb (NemoProgressInfo *info,
			   NemoProgressUIHandler *self)
{
	self->priv->active_infos--;
	self->priv->infos = g_list_remove (self->priv->infos, info);

	if (self->priv->active_infos > 0) {
		if (!gtk_widget_get_visible (self->priv->progress_window)) {
			progress_ui_handler_update_status_icon (self);
		}

        ensure_first_separator_hidden (self);
	} else {
		if (gtk_widget_get_visible (self->priv->progress_window)) {
			gtk_widget_hide (self->priv->progress_window);
		} else {
			progress_ui_handler_hide_status (self);
			progress_ui_handler_show_complete_notification (self);
		}
	}
}

static void
progress_info_changed_cb (NemoProgressInfo *info,
			   NemoProgressUIHandler *self)
{	
	if (g_list_length(self->priv->infos) > 0) {
        NemoProgressInfo *first_info = (NemoProgressInfo *) g_list_first(self->priv->infos)->data;
        GList *l;
        double progress = 0.0;
        int i = 0;
        for (l = self->priv->infos; l != NULL; l = l->next) {
            progress = (progress + nemo_progress_info_get_progress (l->data)) / (double) ++i;
        }
        if (progress > 0) {
            int iprogress = progress * 100;
            gchar *str = g_strdup_printf (_("%d%% %s"), iprogress, nemo_progress_info_get_status(first_info));
            gtk_window_set_title (GTK_WINDOW (self->priv->progress_window), str);
            xapp_gtk_window_set_progress (XAPP_GTK_WINDOW (self->priv->progress_window), iprogress);
            g_free (str);
            self->priv->active_percent = iprogress;
            if (self->priv->should_show_status_icon) {
                progress_ui_handler_update_status_icon (self);
            }
        }
        else {
            gtk_window_set_title (GTK_WINDOW (self->priv->progress_window), nemo_progress_info_get_status(first_info)); 
            xapp_gtk_window_set_progress (XAPP_GTK_WINDOW (self->priv->progress_window), 0);
        }
    } 
}

static void
progress_info_started_cb (NemoProgressUIHandler *self)
{
    progress_ui_handler_sort_by_active (self);
    ensure_first_separator_hidden (self);
}

static void
handle_new_progress_info (NemoProgressUIHandler *self,
			  NemoProgressInfo *info)
{
	self->priv->infos = g_list_append (self->priv->infos, info);	
	
	g_signal_connect_after (info, "finished",
			  G_CALLBACK (progress_info_finished_cb), self);

    g_signal_connect_swapped (info, "started",
              G_CALLBACK (progress_info_started_cb), self);
			  
	g_signal_connect (info, "progress-changed",
			  G_CALLBACK (progress_info_changed_cb), self);

	self->priv->active_infos++;

	if (self->priv->active_infos == 1) {
		/* this is the only active operation, present the window */
		progress_ui_handler_add_to_window (self, info);
        gtk_window_present (GTK_WINDOW (self->priv->progress_window));
		gtk_window_set_title (GTK_WINDOW (self->priv->progress_window), nemo_progress_info_get_details(info));
        xapp_gtk_window_set_icon_name (XAPP_GTK_WINDOW (self->priv->progress_window), "system-run");
	} else {
		progress_ui_handler_add_to_window (self, info);
        if (self->priv->should_show_status_icon) {
            progress_ui_handler_update_status_icon (self);
        }
        if (gtk_widget_get_visible (GTK_WIDGET (self->priv->progress_window))) {
            gtk_window_present (GTK_WINDOW (self->priv->progress_window));
        }
	}
}

typedef struct {
	NemoProgressInfo *info;
	NemoProgressUIHandler *self;
} TimeoutData;

static void
timeout_data_free (TimeoutData *data)
{
	g_clear_object (&data->self);
	g_clear_object (&data->info);

	g_slice_free (TimeoutData, data);
}

static TimeoutData *
timeout_data_new (NemoProgressUIHandler *self,
		  NemoProgressInfo *info)
{
	TimeoutData *retval;

	retval = g_slice_new0 (TimeoutData);
	retval->self = g_object_ref (self);
	retval->info = g_object_ref (info);

	return retval;
}

static gboolean
new_op_queued_timeout (TimeoutData *data)
{
	NemoProgressInfo *info = data->info;
	NemoProgressUIHandler *self = data->self;

	if (nemo_progress_info_get_is_paused (info)) {
		return TRUE;
	}

	if (!nemo_progress_info_get_is_finished (info)) {
		handle_new_progress_info (self, info);
	}

	timeout_data_free (data);

	return FALSE;
}

static void
release_application (NemoProgressInfo *info,
		     NemoProgressUIHandler *self)
{
	NemoApplication *app;

	/* release the GApplication hold we acquired */
	app = nemo_application_get_singleton ();
	g_application_release (G_APPLICATION (app));
}

static void
progress_info_queued_cb (NemoProgressInfo *info,
			  NemoProgressUIHandler *self)
{
	NemoApplication *app;
	TimeoutData *data;

	/* hold GApplication so we never quit while there's an operation pending */
	app = nemo_application_get_singleton ();
	g_application_hold (G_APPLICATION (app));

	g_signal_connect (info, "finished",
			  G_CALLBACK (release_application), self);

	data = timeout_data_new (self, info);

	/* timeout for the progress window to appear */
	g_timeout_add_seconds (2,
			       (GSourceFunc) new_op_queued_timeout,
			       data);
}

static void
new_progress_info_cb (NemoProgressInfoManager *manager,
		      NemoProgressInfo *info,
		      NemoProgressUIHandler *self)
{
    g_signal_connect (info, "queued",
                      G_CALLBACK (progress_info_queued_cb), self);
}

static void
nemo_progress_ui_handler_dispose (GObject *obj)
{
	NemoProgressUIHandler *self = NEMO_PROGRESS_UI_HANDLER (obj);

	g_clear_object (&self->priv->manager);

	G_OBJECT_CLASS (nemo_progress_ui_handler_parent_class)->dispose (obj);
}

static void
nemo_progress_ui_handler_init (NemoProgressUIHandler *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, NEMO_TYPE_PROGRESS_UI_HANDLER,
						  NemoProgressUIHandlerPriv);

	self->priv->manager = nemo_progress_info_manager_new ();
	g_signal_connect (self->priv->manager, "new-progress-info",
			  G_CALLBACK (new_progress_info_cb), self);
    self->priv->should_show_status_icon = FALSE;
}

static void
nemo_progress_ui_handler_class_init (NemoProgressUIHandlerClass *klass)
{
	GObjectClass *oclass;

	oclass = G_OBJECT_CLASS (klass);
	oclass->dispose = nemo_progress_ui_handler_dispose;
	
	g_type_class_add_private (klass, sizeof (NemoProgressUIHandlerPriv));
}

NemoProgressUIHandler *
nemo_progress_ui_handler_new (void)
{
	return g_object_new (NEMO_TYPE_PROGRESS_UI_HANDLER, NULL);
}
