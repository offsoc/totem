/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2009–2010 Philip Withnall <philip@tecnocode.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 * The Totem project hereby grant permission for non-GPL compatible GStreamer
 * plugins to be used and distributed together with GStreamer and Totem. This
 * permission are above and beyond the permissions granted by the GPL license
 * Totem is covered by.
 *
 * See license_change file for details.
 */

#include <config.h>
#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <libpeas/peas-extension-base.h>
#include <libpeas/peas-object-module.h>
#include <libpeas/peas-activatable.h>
#include <gdata/gdata.h>
#include <libsoup/soup.h>

#include "totem-plugin.h"
#include "totem.h"
#include "totem-dirs.h"
#include "totem-video-list.h"
#include "totem-interface.h"
#include "backend/bacon-video-widget.h"
#include "totem-cell-renderer-video.h"

/* Notebook pages */
enum {
	SEARCH_TREE_VIEW = 0,
	RELATED_TREE_VIEW,
	NUM_TREE_VIEWS
};

#define DEVELOPER_KEY	"AI39si5D82T7zgTGS9fmUQAZ7KO5EvKNN_Hf1yoEPf1bpVOTD0At-z7Ovgjupke6o0xdS4drF8SDLfjfmuIXLQQNdE3foPfIdg"
#define CLIENT_ID	"ytapi-GNOME-Totem-444fubtt-1"
#define MAX_RESULTS	10
#define THUMBNAIL_WIDTH	180
#define PULSE_INTERVAL	200

#define TOTEM_TYPE_YOUTUBE_PLUGIN		(totem_youtube_plugin_get_type ())
#define TOTEM_YOUTUBE_PLUGIN(o)			(G_TYPE_CHECK_INSTANCE_CAST ((o), TOTEM_TYPE_YOUTUBE_PLUGIN, TotemYouTubePlugin))
#define TOTEM_YOUTUBE_PLUGIN_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST((k), TOTEM_TYPE_YOUTUBE_PLUGIN, TotemYouTubePluginClass))
#define TOTEM_IS_YOUTUBE_PLUGIN(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), TOTEM_TYPE_YOUTUBE_PLUGIN))
#define TOTEM_IS_YOUTUBE_PLUGIN_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), TOTEM_TYPE_YOUTUBE_PLUGIN))
#define TOTEM_YOUTUBE_PLUGIN_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), TOTEM_TYPE_YOUTUBE_PLUGIN, TotemYouTubePluginClass))

typedef struct {
	Totem *totem;
	GDataYouTubeService *service;
	SoupSession *session;
	BaconVideoWidget *bvw;

	guint current_tree_view;
	GDataQuery *query[NUM_TREE_VIEWS];
	GCancellable *cancellable[NUM_TREE_VIEWS];
	GRegex *regex;
	gboolean button_down;
	GDataYouTubeVideo *playing_video;

	GtkEntry *search_entry;
	GtkButton *search_button;
	GtkProgressBar *progress_bar[NUM_TREE_VIEWS];
	gfloat progress_bar_increment[NUM_TREE_VIEWS];
	GtkNotebook *notebook;
	GtkWidget *vbox;
	GtkAdjustment *vadjust[NUM_TREE_VIEWS];
	GtkListStore *list_store[NUM_TREE_VIEWS];
	GtkTreeView *tree_view[NUM_TREE_VIEWS];
	GtkWidget *cancel_button;
} TotemYouTubePluginPrivate;

TOTEM_PLUGIN_REGISTER (TOTEM_TYPE_YOUTUBE_PLUGIN, TotemYouTubePlugin, totem_youtube_plugin);

/* GtkBuilder callbacks */
void notebook_switch_page_cb (GtkNotebook *notebook, gpointer *page, guint page_num, TotemYouTubePlugin *self);
void search_button_clicked_cb (GtkButton *button, TotemYouTubePlugin *self);
void cancel_button_clicked_cb (GtkButton *button, TotemYouTubePlugin *self);
void search_entry_activate_cb (GtkEntry *entry, TotemYouTubePlugin *self);
gboolean button_press_event_cb (GtkWidget *widget, GdkEventButton *event, TotemYouTubePlugin *self);
gboolean button_release_event_cb (GtkWidget *widget, GdkEventButton *event, TotemYouTubePlugin *self);
void open_in_web_browser_activate_cb (GtkAction *action, TotemYouTubePlugin *self);
void value_changed_cb (GtkAdjustment *adjustment, TotemYouTubePlugin *self);
gboolean starting_video_cb (TotemVideoList *video_list, GtkTreePath *path, TotemYouTubePlugin *self);

static void
set_up_tree_view (TotemYouTubePlugin *self, GtkBuilder *builder, guint key)
{
	TotemYouTubePluginPrivate *priv = self->priv;
	GtkUIManager *ui_manager;
	GtkActionGroup *action_group;
	GtkAction *action, *menu_item;
	GtkWidget *vscroll, *tree_view;
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	/* Add the cell renderer. This can't be done with GtkBuilder, because it unavoidably sets the expand parameter to FALSE */
	/* TODO: Depends on bug #453692 */
	renderer = GTK_CELL_RENDERER (totem_cell_renderer_video_new (TRUE));
	column = GTK_TREE_VIEW_COLUMN (gtk_builder_get_object (builder, (key == SEARCH_TREE_VIEW) ? "yt_treeview_search_column"
	                                                                                          : "yt_treeview_related_column"));
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_set_attributes (column, renderer, "thumbnail", 0, "title", 1, NULL);

	/* Give the video lists a handle to Totem and connect their scrollbar signals */
	if (key == SEARCH_TREE_VIEW) {
		tree_view = GTK_WIDGET (gtk_builder_get_object (builder, "yt_treeview_search"));
		vscroll = gtk_scrolled_window_get_vscrollbar (GTK_SCROLLED_WINDOW (gtk_builder_get_object (builder, "yt_scrolled_window_search")));
		priv->list_store[key] = GTK_LIST_STORE (gtk_builder_get_object (builder, "yt_list_store_search"));
		priv->tree_view[key] = GTK_TREE_VIEW (tree_view);
		priv->progress_bar[key] = GTK_PROGRESS_BAR (gtk_builder_get_object (builder, "yt_progress_bar_search"));
	} else {
		tree_view = GTK_WIDGET (gtk_builder_get_object (builder, "yt_treeview_related"));
		vscroll = gtk_scrolled_window_get_vscrollbar (GTK_SCROLLED_WINDOW (gtk_builder_get_object (builder, "yt_scrolled_window_related")));
		priv->list_store[key] = GTK_LIST_STORE (gtk_builder_get_object (builder, "yt_list_store_related"));
		priv->tree_view[key] = GTK_TREE_VIEW (tree_view);
		priv->progress_bar[key] = GTK_PROGRESS_BAR (gtk_builder_get_object (builder, "yt_progress_bar_related"));
	}
	g_object_set (tree_view, "totem", priv->totem, NULL);
	g_signal_connect (vscroll, "button-press-event", G_CALLBACK (button_press_event_cb), self);
	g_signal_connect (vscroll, "button-release-event", G_CALLBACK (button_release_event_cb), self);

	/* Add the extra popup menu options. This is done here rather than in the UI file, because it's done for multiple treeviews;
	 * if it were done in the UI file, the same action group would be used multiple times, which GTK+ doesn't like. */
	ui_manager = totem_video_list_get_ui_manager (TOTEM_VIDEO_LIST (tree_view));
	action_group = gtk_action_group_new ("youtube-action-group");
	action = gtk_action_new ("open-in-web-browser", _("_Open in Web Browser"), _("Open the video in your web browser"), "gtk-jump-to");
	gtk_action_group_add_action_with_accel (action_group, action, NULL);

	gtk_ui_manager_insert_action_group (ui_manager, action_group, 1);
	gtk_ui_manager_add_ui (ui_manager, gtk_ui_manager_new_merge_id (ui_manager),
	                       "/ui/totem-video-list-popup/",
	                       "open-in-web-browser",
	                       "open-in-web-browser",
	                       GTK_UI_MANAGER_MENUITEM,
	                       FALSE);

	menu_item = gtk_ui_manager_get_action (ui_manager, "/ui/totem-video-list-popup/open-in-web-browser");
	g_signal_connect (menu_item, "activate", G_CALLBACK (open_in_web_browser_activate_cb), self);

	/* Connect to more scroll events */
	priv->vadjust[key] = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (tree_view));
	g_signal_connect (priv->vadjust[key], "value-changed", G_CALLBACK (value_changed_cb), self);

	priv->cancel_button = GTK_WIDGET (gtk_builder_get_object (builder, "yt_cancel_button"));
}

static void
impl_activate (PeasActivatable *plugin)
{
	TotemYouTubePlugin *self = TOTEM_YOUTUBE_PLUGIN (plugin);
	TotemYouTubePluginPrivate *priv = self->priv;
	GtkWindow *main_window;
	GtkBuilder *builder;
	guint i;

	priv->totem = g_object_ref (g_object_get_data (G_OBJECT (plugin), "object"));
	priv->bvw = BACON_VIDEO_WIDGET (totem_get_video_widget (priv->totem));

	/* Set up the interface */
	main_window = totem_get_main_window (priv->totem);
	builder = totem_plugin_load_interface ("youtube", "youtube.ui", TRUE, main_window, self);
	g_object_unref (main_window);

	priv->search_entry = GTK_ENTRY (gtk_builder_get_object (builder, "yt_search_entry"));
	priv->search_button = GTK_BUTTON (gtk_builder_get_object (builder, "yt_search_button"));
	priv->notebook = GTK_NOTEBOOK (gtk_builder_get_object (builder, "yt_notebook"));

	/* Set up the tree view pages */
	for (i = 0; i < NUM_TREE_VIEWS; i++)
		set_up_tree_view (self, builder, i);
	priv->current_tree_view = SEARCH_TREE_VIEW;

	priv->vbox = GTK_WIDGET (gtk_builder_get_object (builder, "yt_vbox"));
	gtk_widget_show_all (priv->vbox);

	/* Add the sidebar page */
	totem_add_sidebar_page (priv->totem, "youtube", _("YouTube"), priv->vbox);
	g_object_unref (builder);
}

static void
impl_deactivate (PeasActivatable *plugin)
{
	guint i;
	TotemYouTubePlugin *self = TOTEM_YOUTUBE_PLUGIN (plugin);
	TotemYouTubePluginPrivate *priv = self->priv;

	totem_remove_sidebar_page (priv->totem, "youtube");

	for (i = 0; i < NUM_TREE_VIEWS; i++) {
		/* Cancel any queries which are still underway */
		if (priv->cancellable[i] != NULL)
			g_cancellable_cancel (priv->cancellable[i]);

		if (priv->query[i] != NULL)
			g_object_unref (priv->query[i]);
	}

	if (priv->playing_video != NULL)
		g_object_unref (priv->playing_video);
	if (priv->service != NULL)
		g_object_unref (priv->service);
	g_object_unref (priv->bvw);
	g_object_unref (priv->totem);
	if (priv->regex != NULL)
		g_regex_unref (priv->regex);
}

typedef struct {
	TotemYouTubePlugin *plugin;
	guint tree_view;
} ProgressBarData;

static gboolean
progress_bar_pulse_cb (ProgressBarData *data)
{
	TotemYouTubePlugin *self = data->plugin;

	if (self->priv->progress_bar_increment[data->tree_view] != 0.0) {
		g_slice_free (ProgressBarData, data);
		return FALSE; /* The first entry has been retrieved */
	}

	gtk_progress_bar_pulse (self->priv->progress_bar[data->tree_view]);
	return TRUE;
}

static void
set_progress_bar_text (TotemYouTubePlugin *self, const gchar *text, guint tree_view)
{
	ProgressBarData *data;
	GdkCursor *cursor;

	/* Set the cursor to a watch */
	cursor = gdk_cursor_new (GDK_WATCH);
	gdk_window_set_cursor (gtk_widget_get_window (self->priv->vbox), cursor);
	g_object_unref (cursor);

	/* Call the pulse method */
	data = g_slice_new (ProgressBarData);
	data->plugin = self;
	data->tree_view = tree_view;

	gtk_progress_bar_set_text (self->priv->progress_bar[tree_view], text);
	gtk_progress_bar_set_fraction (self->priv->progress_bar[tree_view], 0.0);
	self->priv->progress_bar_increment[tree_view] = 0.0;
	g_timeout_add (PULSE_INTERVAL, (GSourceFunc) progress_bar_pulse_cb, data);
}

static void
increment_progress_bar_fraction (TotemYouTubePlugin *self, guint tree_view)
{
	TotemYouTubePluginPrivate *priv = self->priv;
	gdouble new_value = MIN (gtk_progress_bar_get_fraction (priv->progress_bar[tree_view]) + priv->progress_bar_increment[tree_view], 1.0);

	g_debug ("Incrementing progress bar by %f (new value: %f)", priv->progress_bar_increment[tree_view], new_value);
	gtk_progress_bar_set_fraction (priv->progress_bar[tree_view], new_value);

	/* Change the text if the operation's been cancelled */
	if (priv->cancellable[tree_view] == NULL || g_cancellable_is_cancelled (priv->cancellable[tree_view]) == TRUE)
		gtk_progress_bar_set_text (priv->progress_bar[tree_view], _("Cancelling query…"));

	/* Update the UI */
	if (gtk_progress_bar_get_fraction (priv->progress_bar[tree_view]) == 1.0) {
		/* The entire search process (including loading thumbnails) is finished, so update the progress bar */
		gdk_window_set_cursor (gtk_widget_get_window (priv->vbox), NULL);
		gtk_progress_bar_set_text (priv->progress_bar[tree_view], " ");
		gtk_progress_bar_set_fraction (priv->progress_bar[tree_view], 0.0);
	}
}

typedef struct {
	TotemYouTubePlugin *plugin;
	GtkTreePath *path;
	guint tree_view;
	GCancellable *cancellable;
} ThumbnailData;

static void
thumbnail_loaded_cb (GObject *source_object, GAsyncResult *result, ThumbnailData *data)
{
	GdkPixbuf *thumbnail;
	GError *error = NULL;
	GtkTreeIter iter;
	TotemYouTubePlugin *self = data->plugin;

	/* Finish loading the thumbnail */
	thumbnail = gdk_pixbuf_new_from_stream_finish (result, &error);

	if (thumbnail == NULL) {
		/* Bail out if the operation was cancelled */
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) == TRUE) {
			g_error_free (error);
			goto free_data;
		}

		/* Don't display an error message, since this isn't really meant to happen */
		g_warning ("Error loading video thumbnail: %s", error->message);
		g_error_free (error);
		goto free_data;
	}

	g_debug ("Finished creating thumbnail from stream");

	/* Update the tree view */
	if (gtk_tree_model_get_iter (GTK_TREE_MODEL (self->priv->list_store[data->tree_view]), &iter, data->path) == TRUE) {
		gtk_list_store_set (self->priv->list_store[data->tree_view], &iter, 0, thumbnail, -1);
		g_debug ("Updated list store with new thumbnail");
	}

	g_object_unref (thumbnail);

free_data:
	/* Update the progress bar */
	increment_progress_bar_fraction (self, data->tree_view);

	g_object_unref (data->plugin);
	g_object_unref (data->cancellable);
	gtk_tree_path_free (data->path);
	g_slice_free (ThumbnailData, data);
}

static void
thumbnail_opened_cb (GObject *source_object, GAsyncResult *result, ThumbnailData *data)
{
	GFile *thumbnail_file;
	GFileInputStream *input_stream;
	GError *error = NULL;

	/* Finish opening the thumbnail */
	thumbnail_file = G_FILE (source_object);
	input_stream = g_file_read_finish (thumbnail_file, result, &error);

	if (input_stream == NULL) {
		/* Don't display an error message, since this isn't really meant to happen */
		g_warning ("Error loading video thumbnail: %s", error->message);
		g_error_free (error);
		return;
	}

	/* NOTE: There's no need to reset the cancellable before using it again, as we'll have bailed before now if it was ever cancelled. */

	g_debug ("Creating thumbnail from stream");
	gdk_pixbuf_new_from_stream_at_scale_async (G_INPUT_STREAM (input_stream), THUMBNAIL_WIDTH, -1, TRUE,
	                                                 data->cancellable, (GAsyncReadyCallback) thumbnail_loaded_cb, data);
	g_object_unref (input_stream);
}

typedef struct {
	TotemYouTubePlugin *plugin;
	guint tree_view;
	GCancellable *query_cancellable;
	GCancellable *thumbnail_cancellable;
} QueryData;

static void
query_data_free (QueryData *data)
{
	if (data->thumbnail_cancellable != NULL)
		g_object_unref (data->thumbnail_cancellable);

	g_object_unref (data->query_cancellable);
	g_object_unref (data->plugin);

	g_slice_free (QueryData, data);
}

static void
query_finished_cb (GObject *source_object, GAsyncResult *result, QueryData *data)
{
	GtkWindow *window;
	GDataFeed *feed;
	GError *error = NULL;
	TotemYouTubePlugin *self = data->plugin;

	g_debug ("Search finished!");

	feed = gdata_service_query_finish (GDATA_SERVICE (self->priv->service), result, &error);

	/* Stop the progress bar; a little hacky, but it works */
	self->priv->progress_bar_increment[data->tree_view] = 1.0;
	increment_progress_bar_fraction (self, data->tree_view);

	if (feed != NULL) {
		/* Success! */
		g_object_unref (feed);
		query_data_free (data);

		return;
	}

	/* Bail out if the operation was cancelled */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) == TRUE) {
		/* Cancel the thumbnail thread, if applicable */
		if (data->thumbnail_cancellable != NULL)
			g_cancellable_cancel (data->thumbnail_cancellable);

		goto finish;
	}

	/* Error! */
	window = totem_get_main_window (self->priv->totem);
	if (g_error_matches (error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_PROTOCOL_ERROR) == TRUE) {
		/* Hide the ugly technical message libgdata gives behind a nice one telling them it's out of date (which it likely is
		 * if we're receiving a protocol error). */
		totem_interface_error (_("Error Searching for Videos"),
		                       _("The response from the server could not be understood. "
		                         "Please check you are running the latest version of libgdata."), window);
	} else {
		/* Spew out the error message as provided */
		totem_interface_error (_("Error Searching for Videos"), error->message, window);
	}

	g_object_unref (window);

finish:
	g_error_free (error);
	query_data_free (data);

	return;
}

static void
query_progress_cb (GDataEntry *entry, guint entry_key, guint entry_count, QueryData *data)
{
	GList *thumbnails;
	GDataMediaThumbnail *thumbnail = NULL;
	gint delta = G_MININT;
	GtkTreeIter iter;
	const gchar *title, *id, *video_uri;
	GtkProgressBar *progress_bar;
	GDataMediaContent *content;
	TotemYouTubePlugin *self = data->plugin;

	/* Add the entry to the tree view */
	title = gdata_entry_get_title (entry);
	id = gdata_youtube_video_get_video_id (GDATA_YOUTUBE_VIDEO (entry));

	gtk_list_store_append (self->priv->list_store[data->tree_view], &iter);
	gtk_list_store_set (self->priv->list_store[data->tree_view], &iter,
	                    0, NULL, /* the thumbnail will be downloaded asynchronously and added to the tree view later */
	                    1, title,
	                    2, NULL, /* the video URI will be resolved asynchronously and added to the tree view later */
	                    3, entry,
	                    -1);
	g_debug ("Added entry %s to tree view (title: \"%s\")", id, title);

	/* Update the progress bar; we have three steps for each entry in the results: the entry, its thumbnail, and its t parameter.
	 * Since we've dropped t-param resolution in favour of just using the listed content URIs in the video entry itself, the t-param step is
	 * no longer asynchronous. However, for simplicity it's been kept in the progress bar's update lifecycle. */
	g_assert (entry_count > 0);
	progress_bar = self->priv->progress_bar[data->tree_view];
	self->priv->progress_bar_increment[data->tree_view] = 1.0 / (entry_count * 3.0);
	g_debug ("Setting progress_bar_increment to 1.0 / (%u * 3.0) = %f", entry_count, self->priv->progress_bar_increment[data->tree_view]);
	gtk_progress_bar_set_fraction (progress_bar,
	                               gtk_progress_bar_get_fraction (progress_bar) + self->priv->progress_bar_increment[data->tree_view]);

	/* Look up a playback URI for the video. We can only support video/3gpp first.
	 * See: http://code.google.com/apis/youtube/2.0/reference.html#formatsp. */
	content = GDATA_MEDIA_CONTENT (gdata_youtube_video_look_up_content (GDATA_YOUTUBE_VIDEO (entry), "video/3gpp"));

	if (content != NULL) {
		video_uri = gdata_media_content_get_uri (content);
		g_debug ("Using video URI %s (content type: %s)", video_uri, gdata_media_content_get_content_type (content));
	} else {
		/* Cop out */
		g_warning ("Couldn't find a playback URI for entry %s.", id);
		video_uri = NULL;
	}

	/* Update the tree view with the new MRL */
	gtk_list_store_set (self->priv->list_store[data->tree_view], &iter, 2, video_uri, -1);
	g_debug ("Updated list store with new video URI (\"%s\") for entry %s", video_uri, id);

	/* Update the progress bar */
	increment_progress_bar_fraction (self, data->tree_view);

	/* Download the entry's thumbnail, ready for adding it to the tree view.
	 * Find the thumbnail size which is closest to the wanted size (THUMBNAIL_WIDTH), so that we:
	 * a) avoid fuzzy images due to scaling up, and
	 * b) avoid downloading too much just to scale down by a factor of 10. */
	thumbnails = gdata_youtube_video_get_thumbnails (GDATA_YOUTUBE_VIDEO (entry));
	for (; thumbnails != NULL; thumbnails = thumbnails->next) {
		gint new_delta;
		GDataMediaThumbnail *current_thumb = (GDataMediaThumbnail*) thumbnails->data;

		g_debug ("%u pixel wide thumbnail available for entry %s", gdata_media_thumbnail_get_width (current_thumb), id);

		new_delta = gdata_media_thumbnail_get_width (current_thumb) - THUMBNAIL_WIDTH;
		if (delta == 0) {
			break;
		} else if ((delta == G_MININT) ||
		           (delta < 0 && new_delta > delta) ||
		           (delta > 0 && new_delta > 0 && new_delta < delta)) {
			delta = new_delta;
			thumbnail = current_thumb;
			g_debug ("Choosing a %u pixel wide thumbnail (delta: %i) for entry %s",
			         gdata_media_thumbnail_get_width (current_thumb), new_delta, id);
		}
	}

	if (thumbnail != NULL) {
		GFile *thumbnail_file;
		ThumbnailData *t_data;

		t_data = g_slice_new (ThumbnailData);
		t_data->plugin = g_object_ref (self);
		t_data->path = gtk_tree_model_get_path (GTK_TREE_MODEL (self->priv->list_store[data->tree_view]), &iter);
		t_data->tree_view = data->tree_view;

		/* We can use the same cancellable for reading the file and making a pixbuf out of it, as they're consecutive operations */
		/* This will be cancelled if the main query is cancelled, in query_finished_cb() */
		data->thumbnail_cancellable = g_cancellable_new ();
		t_data->cancellable = g_object_ref (data->thumbnail_cancellable);

		g_debug ("Starting thumbnail download for entry %s", id);
		thumbnail_file = g_file_new_for_uri (gdata_media_thumbnail_get_uri (thumbnail));
		g_file_read_async (thumbnail_file, G_PRIORITY_DEFAULT, data->thumbnail_cancellable,
		                   (GAsyncReadyCallback) thumbnail_opened_cb, t_data);
		g_object_unref (thumbnail_file);
	}
}

/* Called when self->priv->cancellable[tree_view] is destroyed (for either tree view) */
static void
cancellable_notify_cb (TotemYouTubePlugin *self, GCancellable *old_cancellable)
{
	guint i;

	/* Disable the "Cancel" button, if it applies to the current tree view */
	if (self->priv->cancellable[self->priv->current_tree_view] == old_cancellable)
		gtk_widget_set_sensitive (self->priv->cancel_button, FALSE);

	/* NULLify the cancellable */
	for (i = 0; i < NUM_TREE_VIEWS; i++) {
		if (self->priv->cancellable[i] == old_cancellable)
			self->priv->cancellable[i] = NULL;
	}
}

static void
set_current_operation (TotemYouTubePlugin *self, guint tree_view, GCancellable *cancellable)
{
	/* Cancel previous searches on this tree view */
	if (self->priv->cancellable[tree_view] != NULL)
		g_cancellable_cancel (self->priv->cancellable[tree_view]);

	/* Make this the current cancellable action for the given tab */
	g_object_weak_ref (G_OBJECT (cancellable), (GWeakNotify) cancellable_notify_cb, self);
	self->priv->cancellable[tree_view] = cancellable;

	/* Enable the "Cancel" button if it applies to the current tree view */
	if (self->priv->current_tree_view == tree_view)
		gtk_widget_set_sensitive (self->priv->cancel_button, TRUE);
}

static void
execute_query (TotemYouTubePlugin *self, guint tree_view, gboolean clear_tree_view)
{
	QueryData *data;

	/* Set up the query */
	data = g_slice_new (QueryData);
	data->plugin = g_object_ref (self);
	data->tree_view = tree_view;
	data->query_cancellable = g_cancellable_new ();
	data->thumbnail_cancellable = NULL;

	/* Make this the current cancellable action for the given tab */
	set_current_operation (self, tree_view, data->query_cancellable);

	/* Clear the tree views */
	if (clear_tree_view == TRUE)
		gtk_list_store_clear (self->priv->list_store[tree_view]);

#ifdef HAVE_LIBGDATA_0_9
	if (tree_view == SEARCH_TREE_VIEW) {
		gdata_youtube_service_query_videos_async (self->priv->service, self->priv->query[tree_view], data->query_cancellable,
		                                          (GDataQueryProgressCallback) query_progress_cb, data, NULL,
		                                          (GAsyncReadyCallback) query_finished_cb, data);
	} else {
		gdata_youtube_service_query_related_async (self->priv->service, self->priv->playing_video, self->priv->query[tree_view],
		                                           data->query_cancellable, (GDataQueryProgressCallback) query_progress_cb, data, NULL,
		                                           (GAsyncReadyCallback) query_finished_cb, data);
	}
#else
	if (tree_view == SEARCH_TREE_VIEW) {
		gdata_youtube_service_query_videos_async (self->priv->service, self->priv->query[tree_view], data->query_cancellable,
		                                          (GDataQueryProgressCallback) query_progress_cb, data,
		                                          (GAsyncReadyCallback) query_finished_cb, data);
	} else {
		gdata_youtube_service_query_related_async (self->priv->service, self->priv->playing_video, self->priv->query[tree_view],
		                                           data->query_cancellable, (GDataQueryProgressCallback) query_progress_cb, data,
		                                           (GAsyncReadyCallback) query_finished_cb, data);
	}
#endif /* !HAVE_LIBGDATA_0_9 */
}

void
search_button_clicked_cb (GtkButton *button, TotemYouTubePlugin *self)
{
	TotemYouTubePluginPrivate *priv = self->priv;
	const gchar *search_terms;

	search_terms = gtk_entry_get_text (priv->search_entry);
	g_debug ("Searching for \"%s\"", search_terms);

	/* Focus the "Search" page */
	gtk_notebook_set_current_page (priv->notebook, SEARCH_TREE_VIEW);

	/* Update the UI */
	set_progress_bar_text (self, _("Fetching search results…"), SEARCH_TREE_VIEW);

	/* Clear details pertaining to related videos, since we're doing a new search */
	gtk_list_store_clear (priv->list_store[RELATED_TREE_VIEW]);
	if (priv->playing_video != NULL)
		g_object_unref (priv->playing_video);
	priv->playing_video = NULL;

	/* If this is the first query, set up some stuff which we didn't do before to save memory */
	if (priv->query[SEARCH_TREE_VIEW] == NULL) {
		/* If this is the first query, compile the regex used to resolve the t param. Doing this here rather than when
		 * activating the plugin means we don't waste cycles if the plugin's never used. It also means we don't waste
		 * cycles repeatedly creating new regexes for each video whose t param we resolve. */
		/* We're looking for a line of the form:
		 * var swfHTML = (isIE) ? "<object...econds=194&t=vjVQa1PpcFP36LLlIaDqZIG1w6e30b-7WVBgsQLLA3s%3D&rv.6.id=OzLjC6Pm... */
		priv->regex = g_regex_new ("swfHTML = .*&fmt_url_map=([^&]+)&", G_REGEX_OPTIMIZE, 0, NULL);
		g_assert (priv->regex != NULL);

		/* Set up the GData service (needed for the tree views' queries) */
#ifdef HAVE_LIBGDATA_0_9
		priv->service = gdata_youtube_service_new (DEVELOPER_KEY, NULL);
#else
		priv->service = gdata_youtube_service_new (DEVELOPER_KEY, CLIENT_ID);
#endif /* !HAVE_LIBGDATA_0_9 */

		/* Set up network timeouts, if they're supported by our version of libgdata.
		 * This will return from queries with %GDATA_SERVICE_ERROR_NETWORK_ERROR if network operations take longer than 30 seconds. */
#ifdef HAVE_LIBGDATA_0_7
		gdata_service_set_timeout (GDATA_SERVICE (priv->service), 30);
#endif /* HAVE_LIBGDATA_0_7 */

		/* Set up the queries */
		priv->query[SEARCH_TREE_VIEW] = gdata_query_new_with_limits (NULL, 0, MAX_RESULTS);
		priv->query[RELATED_TREE_VIEW] = gdata_query_new_with_limits (NULL, 0, MAX_RESULTS);
	}

	/* Do the query */
	gdata_query_set_q (priv->query[SEARCH_TREE_VIEW], search_terms);
	execute_query (self, SEARCH_TREE_VIEW, TRUE);
}

void
cancel_button_clicked_cb (GtkButton *button, TotemYouTubePlugin *self)
{
	/* It's possible for the operation to finish (and consequently the cancellable to disappear) while the GtkButton is deciding whether the
	 * user is actually pressing it (in its timeout). */
	if (self->priv->cancellable[self->priv->current_tree_view] == NULL)
		return;

	g_debug ("Cancelling search");
	g_cancellable_cancel (self->priv->cancellable[self->priv->current_tree_view]);
}

void
search_entry_activate_cb (GtkEntry *entry, TotemYouTubePlugin *self)
{
	search_button_clicked_cb (self->priv->search_button, self);
}

static void
load_related_videos (TotemYouTubePlugin *self)
{
	g_assert (self->priv->playing_video != NULL);
	g_debug ("Loading related videos for %s", gdata_youtube_video_get_video_id (self->priv->playing_video));

	/* Update the UI */
	set_progress_bar_text (self, _("Fetching related videos…"), RELATED_TREE_VIEW);

	/* Clear the existing results and do the query */
	gtk_list_store_clear (self->priv->list_store[RELATED_TREE_VIEW]);
	execute_query (self, RELATED_TREE_VIEW, FALSE);
}

void
notebook_switch_page_cb (GtkNotebook *notebook, gpointer *page, guint page_num, TotemYouTubePlugin *self)
{
	/* Change the tree view */
	self->priv->current_tree_view = page_num;

	/* Sort out the "Cancel" button's sensitivity */
	gtk_widget_set_sensitive (self->priv->cancel_button, (self->priv->cancellable[page_num] != NULL) ? TRUE : FALSE);

	/* If we're changing to the "Related Videos" tree view and have played a video, load
	 * the related videos for that video; but only if the related tree view's empty first */
	if (page_num == RELATED_TREE_VIEW && self->priv->playing_video != NULL &&
	    gtk_tree_model_iter_n_children (GTK_TREE_MODEL (self->priv->list_store[RELATED_TREE_VIEW]), NULL) == 0) {
		load_related_videos (self);
	}
}

void
open_in_web_browser_activate_cb (GtkAction *action, TotemYouTubePlugin *self)
{
	GtkTreeSelection *selection;
	GtkTreeModel *model;
	GList *paths, *path;

	selection = gtk_tree_view_get_selection (self->priv->tree_view[self->priv->current_tree_view]);
	paths = gtk_tree_selection_get_selected_rows (selection, &model);

	for (path = paths; path != NULL; path = path->next) {
		GtkTreeIter iter;
		GDataYouTubeVideo *video;
		GDataLink *page_link;
		GError *error = NULL;

		if (gtk_tree_model_get_iter (model, &iter, (GtkTreePath*) (path->data)) == FALSE)
			continue;

		/* Get the HTML page for the video; its <link rel="alternate" ... /> */
		gtk_tree_model_get (model, &iter, 3, &video, -1);
		page_link = gdata_entry_look_up_link (GDATA_ENTRY (video), GDATA_LINK_ALTERNATE);
		g_object_unref (video);

		/* Display the page */
		if (gtk_show_uri (gtk_widget_get_screen (GTK_WIDGET (self->priv->bvw)), gdata_link_get_uri (page_link),
		                  GDK_CURRENT_TIME, &error) == FALSE) {
			GtkWindow *window = totem_get_main_window (self->priv->totem);
			totem_interface_error (_("Error Opening Video in Web Browser"), error->message, window);
			g_object_unref (window);
			g_error_free (error);
		}
	}

	g_list_foreach (paths, (GFunc) gtk_tree_path_free, NULL);
	g_list_free (paths);
}

void
value_changed_cb (GtkAdjustment *adjustment, TotemYouTubePlugin *self)
{
	if (self->priv->button_down == FALSE &&
	    gtk_tree_model_iter_n_children (GTK_TREE_MODEL (self->priv->list_store[self->priv->current_tree_view]), NULL) >= MAX_RESULTS &&
	    (gtk_adjustment_get_value (adjustment) + gtk_adjustment_get_page_size (adjustment)) / gtk_adjustment_get_upper (adjustment) > 0.8) {
		/* Only load more results if we're not already querying */
		if (self->priv->cancellable[self->priv->current_tree_view] != NULL)
			return;

		set_progress_bar_text (self, _("Fetching more videos…"), self->priv->current_tree_view);
		gdata_query_next_page (self->priv->query[self->priv->current_tree_view]);
		execute_query (self, self->priv->current_tree_view, FALSE);
	}
}

gboolean
button_press_event_cb (GtkWidget *widget, GdkEventButton *event, TotemYouTubePlugin *self)
{
	self->priv->button_down = TRUE;
	return FALSE;
}

gboolean
button_release_event_cb (GtkWidget *widget, GdkEventButton *event, TotemYouTubePlugin *self)
{
	self->priv->button_down = FALSE;
	value_changed_cb (self->priv->vadjust[self->priv->current_tree_view], self);
	return FALSE;
}

gboolean
starting_video_cb (TotemVideoList *video_list, GtkTreePath *path, TotemYouTubePlugin *self)
{
	GtkTreeIter iter;
	GDataYouTubeVideo *video_entry;
	gchar *video_uri;

	/* Store the current entry */
	if (gtk_tree_model_get_iter (GTK_TREE_MODEL (self->priv->list_store[self->priv->current_tree_view]), &iter, path) == FALSE)
		return FALSE;
	gtk_tree_model_get (GTK_TREE_MODEL (self->priv->list_store[self->priv->current_tree_view]), &iter,
	                    2, &video_uri,
	                    3, &video_entry,
	                    -1);

	/* If there's no video URI set, display an error message and suggest that the user watch the video in their browser. This typically happens
	 * because the video isn't offered in any format we support. */
	if (video_uri == NULL) {
		GtkDialog *dialog;
		GtkWindow *main_window;

		main_window = totem_get_main_window (self->priv->totem);
		dialog = GTK_DIALOG (gtk_message_dialog_new (main_window,
		                                             GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
		                                             _("Video Format Not Supported")));
		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
		                                          _("This video is not available in any formats which Totem supports. Would you like to "
		                                            "open it in your web browser instead?"));
		gtk_dialog_add_buttons (dialog,
		                        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		                        _("_Open in Web Browser"), GTK_RESPONSE_OK,
		                        NULL);
		g_object_unref (main_window);

		if (gtk_dialog_run (dialog) == GTK_RESPONSE_OK) {
			/* Open the video in the user's web browser */
			open_in_web_browser_activate_cb (NULL, self);
		}

		gtk_widget_destroy (GTK_WIDGET (dialog));
	}

	g_free (video_uri);

	if (self->priv->playing_video != NULL)
		g_object_unref (self->priv->playing_video);
	self->priv->playing_video = g_object_ref (video_entry);

	/* If we're currently viewing the related videos page, load the new related videos */
	if (self->priv->current_tree_view == RELATED_TREE_VIEW)
		load_related_videos (self);

	return TRUE;
}
