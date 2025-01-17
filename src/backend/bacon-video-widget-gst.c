/* 
 * Copyright (C) 2003-2004 the Gstreamer project
 * 	Julien Moutte <julien@moutte.net>
 *      Ronald Bultje <rbultje@ronald.bitfreak.net>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * $Id$
 *
 *
 * The Totem project hereby grant permission for non-gpl compatible GStreamer
 * plugins to be used and distributed together with GStreamer and Totem. This
 * permission are above and beyond the permissions granted by the GPL license
 * Totem is covered by.
 *
 * Monday 7th February 2005: Christian Schaller: Add excemption clause.
 * See license_change file for details.
 *
 */

#include <config.h>

#ifdef HAVE_NVTV
#include <nvtv_simple.h>
#endif 

/* GStreamer Interfaces */
#include <gst/interfaces/xoverlay.h>
#include <gst/interfaces/navigation.h>
#include <gst/interfaces/mixer.h>
#include <gst/interfaces/colorbalance.h>

/* system */
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

/* gtk+/gnome */
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <gconf/gconf-client.h>

#include "bacon-video-widget.h"
#include "baconvideowidget-marshal.h"
#include "video-utils.h"
#include "gstscreenshot.h"

#define DEFAULT_HEIGHT 420
#define DEFAULT_WIDTH 315
#define CONFIG_FILE ".gnome2"G_DIR_SEPARATOR_S"totem_config"
#define DEFAULT_TITLE _("Totem Video Window")

/* Signals */
enum
{
  SIGNAL_ERROR,
  SIGNAL_EOS,
  SIGNAL_REDIRECT,
  SIGNAL_TITLE_CHANGE,
  SIGNAL_CHANNELS_CHANGE,
  SIGNAL_TICK,
  SIGNAL_GOT_METADATA,
  SIGNAL_BUFFERING,
  SIGNAL_SPEED_WARNING,
  LAST_SIGNAL
};

/* Enum for none-signal stuff that needs to go through the AsyncQueue */
enum
{
  RATIO = LAST_SIGNAL
};

/* Arguments */
enum
{
  PROP_0,
  PROP_LOGO_MODE,
  PROP_POSITION,
  PROP_CURRENT_TIME,
  PROP_STREAM_LENGTH,
  PROP_PLAYING,
  PROP_SEEKABLE,
  PROP_SHOWCURSOR,
  PROP_MEDIADEV,
  PROP_SHOW_VISUALS
};

static char *video_props_str[4] = {
  GCONF_PREFIX"/brightness",
  GCONF_PREFIX"/contrast",
  GCONF_PREFIX"/saturation",
  GCONF_PREFIX"/hue"
};

struct BaconVideoWidgetPrivate
{
  double display_ratio, movie_ratio;
  BaconVideoWidgetAspectRatio ratio_type;

  GstElement *play;
  guint update_id;
  GstXOverlay *xoverlay;
  GstColorBalance *balance;

  GstElement *audiosink, *audiocapsfilter, *audioconvert;

  GdkPixbuf *logo_pixbuf;

  gboolean media_has_video, media_has_audio;
  gint64 stream_length;
  gint64 current_time_nanos;
  gint64 current_time;
  float current_position;

  GstTagList *tagcache, *audiotags, *videotags;

  gboolean cache_errors;
  GError *last_error;
  gboolean got_redirect;

  GdkWindow *video_window;
  GtkAllocation video_window_allocation;

  /* Visual effects */
  GList *vis_plugins_list;
  gboolean show_vfx;
  VisualsQuality visq;
  GstElement *vis_element;
  GstElement *vis_capsfilter;

  /* Other stuff */
  int xpos, ypos;
  gboolean logo_mode;
  gboolean cursor_shown;
  gboolean fullscreen_mode;
  gboolean auto_resize;
  
  gint video_width, video_width_pixels;
  gint video_height, video_height_pixels;
  gdouble video_fps;

  guint init_width;
  guint init_height;
  
  char *mrl;
  char *media_device;

  BaconVideoWidgetAudioOutType speakersetup;
  TvOutType tv_out_type;
  gint connection_speed;

  GConfClient *gc;
};

static void bacon_video_widget_set_property (GObject * object,
					     guint property_id,
					     const GValue * value,
					     GParamSpec * pspec);
static void bacon_video_widget_get_property (GObject * object,
					     guint property_id,
					     GValue * value,
					     GParamSpec * pspec);

static void bacon_video_widget_finalize (GObject * object);

static GtkWidgetClass *parent_class = NULL;

static int bvw_table_signals[LAST_SIGNAL] = { 0 };

static void
get_media_size (BaconVideoWidget *bvw, gint *width, gint *height)
{
  if (GST_STATE (bvw->priv->play) >= GST_STATE_PAUSED &&
      (bvw->priv->media_has_video ||
       (bvw->priv->show_vfx && bvw->priv->vis_element))) {
    *width = bvw->priv->video_width;
    *height = bvw->priv->video_height;
  } else {
    if (bvw->priv->logo_pixbuf != NULL) {
      *width = gdk_pixbuf_get_width (bvw->priv->logo_pixbuf);
      *height = gdk_pixbuf_get_height (bvw->priv->logo_pixbuf);
    } else {
      *width = bvw->priv->init_width;
      *height = bvw->priv->init_height;
    }
  }
}

static void
bacon_video_widget_realize (GtkWidget * widget)
{
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);
  GdkWindowAttr attributes;
  gint attributes_mask, w, h;

  /* Creating our widget's window */
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.x = widget->allocation.x;
  attributes.y = widget->allocation.y;
  attributes.width = widget->allocation.width;
  attributes.height = widget->allocation.height;
  attributes.wclass = GDK_INPUT_OUTPUT;
  attributes.visual = gtk_widget_get_visual (widget);
  attributes.colormap = gtk_widget_get_colormap (widget);
  attributes.event_mask = gtk_widget_get_events (widget);
  attributes.event_mask |= GDK_EXPOSURE_MASK |
			   GDK_POINTER_MOTION_MASK |
			   GDK_BUTTON_PRESS_MASK |
			   GDK_KEY_PRESS_MASK;
  attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP;

  widget->window = gdk_window_new (gtk_widget_get_parent_window (widget),
				   &attributes, attributes_mask);
  gdk_window_set_user_data (widget->window, widget);
  //gdk_window_show (widget->window);

  /* Creating our video window */
  attributes.window_type = GDK_WINDOW_CHILD;
  attributes.x = 0;
  attributes.y = 0;
  attributes.width = widget->allocation.width;
  attributes.height = widget->allocation.height;
  attributes.wclass = GDK_INPUT_OUTPUT;
  attributes.event_mask = gtk_widget_get_events (widget);
  attributes.event_mask |= GDK_EXPOSURE_MASK |
			  GDK_POINTER_MOTION_MASK |
			  GDK_BUTTON_PRESS_MASK |
			  GDK_KEY_PRESS_MASK;
  attributes_mask = GDK_WA_X | GDK_WA_Y;

  bvw->priv->video_window = gdk_window_new (widget->window,
				            &attributes, attributes_mask);
  gdk_window_set_user_data (bvw->priv->video_window, widget);
  //gdk_window_show (bvw->priv->video_window);

  widget->style = gtk_style_attach (widget->style, widget->window);

  gtk_style_set_background (widget->style, widget->window, GTK_STATE_NORMAL);

  GTK_WIDGET_SET_FLAGS (widget, GTK_REALIZED);

  /* nice hack to show the logo fullsize, while still being resizable */
  get_media_size (BACON_VIDEO_WIDGET (widget), &w, &h);
  totem_widget_set_preferred_size (widget, w, h);

#ifdef HAVE_NVTV
  if (!(nvtv_simple_init() && nvtv_enable_autoresize(TRUE))) {
    nvtv_simple_enable(FALSE);
  } 
#endif

}

static void
bacon_video_widget_unrealize (GtkWidget *widget)
{
#ifdef HAVE_NVTV
  /* Kill the TV out */
  nvtv_simple_exit();
#endif

  if (GTK_WIDGET_CLASS (parent_class)->unrealize)
    GTK_WIDGET_CLASS (parent_class)->unrealize (widget);
}

static void
bacon_video_widget_show (GtkWidget *widget)
{
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);

  gdk_window_show (widget->window);
  gdk_window_show (bvw->priv->video_window);

  if (GTK_WIDGET_CLASS (parent_class)->show)
    GTK_WIDGET_CLASS (parent_class)->show (widget);
}

static void
bacon_video_widget_hide (GtkWidget *widget)
{
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);

  gdk_window_hide (widget->window);
  gdk_window_hide (bvw->priv->video_window);

  if (GTK_WIDGET_CLASS (parent_class)->hide)
    GTK_WIDGET_CLASS (parent_class)->hide (widget);
}

static gboolean
bacon_video_widget_expose_event (GtkWidget *widget, GdkEventExpose *event)
{
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);
  XID window;

  if (event && event->count > 0)
    return TRUE;

  g_return_val_if_fail (bvw->priv->xoverlay != NULL &&
			GST_IS_X_OVERLAY (bvw->priv->xoverlay),
			FALSE);

  window = GDK_WINDOW_XWINDOW (bvw->priv->video_window);

  gdk_draw_rectangle (widget->window, widget->style->black_gc, TRUE, 0, 0,
		      widget->allocation.width, widget->allocation.height);
  gst_x_overlay_set_xwindow_id (bvw->priv->xoverlay, window);

  if (GST_STATE (bvw->priv->play) >= GST_STATE_PAUSED &&
      (bvw->priv->media_has_video ||
       (bvw->priv->vis_element && bvw->priv->show_vfx))) {
    gst_x_overlay_expose (bvw->priv->xoverlay);
  } else if (bvw->priv->logo_pixbuf != NULL) {
    /* draw logo here */
    GdkPixbuf *logo;
    gfloat width = gdk_pixbuf_get_width (bvw->priv->logo_pixbuf),
           height = gdk_pixbuf_get_height (bvw->priv->logo_pixbuf);
    gfloat ratio;

    if ((gfloat) widget->allocation.width /
        gdk_pixbuf_get_width (bvw->priv->logo_pixbuf) >
        (gfloat) widget->allocation.height /
        gdk_pixbuf_get_height (bvw->priv->logo_pixbuf)) {
      ratio = (gfloat) widget->allocation.height /
	      gdk_pixbuf_get_height (bvw->priv->logo_pixbuf);
    } else {
      ratio = (gfloat) widget->allocation.width /
	      gdk_pixbuf_get_width (bvw->priv->logo_pixbuf);
    }
    width *= ratio;
    height *= ratio;

    logo = gdk_pixbuf_scale_simple (bvw->priv->logo_pixbuf,
				    width, height,
				    GDK_INTERP_BILINEAR);

    gdk_draw_pixbuf (GDK_DRAWABLE (bvw->priv->video_window),
		     widget->style->fg_gc[0], logo,
		     0, 0, 0, 0, width, height,
		     GDK_RGB_DITHER_NONE, 0, 0);

    gdk_pixbuf_unref (logo);
  }
  else {
    gdk_draw_rectangle (bvw->priv->video_window, widget->style->black_gc, TRUE,
		      0, 0,
		      bvw->priv->video_window_allocation.width,
		      bvw->priv->video_window_allocation.height);
  }
  
  return TRUE;
}

static gboolean
bacon_video_widget_motion_notify (GtkWidget *widget,
				  GdkEventMotion *event)
{
  gboolean res = FALSE;
#if 0
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);

  g_return_val_if_fail (bvw->priv->play != NULL, FALSE);

  if (bvw->priv->media_has_video) {
    GstElement *videosink = NULL;

    g_object_get (G_OBJECT (bvw->priv->play), "video-sink", &videosink, NULL);
    if (GST_IS_BIN (videosink)) {
      videosink = gst_bin_get_by_interface (GST_BIN (videosink),
          GST_TYPE_NAVIGATION);
    }

    if (videosink && GST_IS_NAVIGATION (videosink) &&
        GST_STATE (videosink) >= GST_STATE_PAUSED) {
      GstNavigation *nav = GST_NAVIGATION (videosink);

      gst_navigation_send_mouse_event (nav,
          "mouse-move", 0, event->x, event->y);

      /* we need a return value... */
      res = TRUE;
    }
  }
#endif
  if (GTK_WIDGET_CLASS (parent_class)->motion_notify_event)
    res |= GTK_WIDGET_CLASS (parent_class)->motion_notify_event (widget, event);

  return res;
}

static gboolean
bacon_video_widget_button_press (GtkWidget *widget,
				 GdkEventButton *event)
{
  gboolean res = FALSE;
#if 0
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);

  g_return_val_if_fail (bvw->priv->play != NULL, FALSE);

  if (bvw->priv->media_has_video) {
    GstElement *videosink = NULL;

    g_object_get (G_OBJECT (bvw->priv->play), "video-sink", &videosink, NULL);
    if (GST_IS_BIN (videosink)) {
      videosink = gst_bin_get_by_interface (GST_BIN (videosink),
         GST_TYPE_NAVIGATION);
    }

    if (videosink && GST_IS_NAVIGATION (videosink) &&
        GST_STATE (videosink) >= GST_STATE_PAUSED) {
      GstNavigation *nav = GST_NAVIGATION (videosink);

      gst_navigation_send_mouse_event (nav,
          "mouse-button-press", event->button, event->x, event->y);

      /* we need a return value... */
      res = TRUE;
    }
  }
#endif
  if (GTK_WIDGET_CLASS (parent_class)->button_press_event)
    res |= GTK_WIDGET_CLASS (parent_class)->button_press_event (widget, event);

  return res;
}

static gboolean
bacon_video_widget_button_release (GtkWidget *widget,
				   GdkEventButton *event)
{
  gboolean res = FALSE;
#if 0
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);

  g_return_val_if_fail (bvw->priv->play != NULL, FALSE);

  if (bvw->priv->media_has_video) {
    GstElement *videosink = NULL;

    g_object_get (G_OBJECT (bvw->priv->play), "video-sink", &videosink, NULL);
    if (GST_IS_BIN (videosink)) {
      videosink = gst_bin_get_by_interface (GST_BIN (videosink),
          GST_TYPE_NAVIGATION);
    }

    if (videosink && GST_IS_NAVIGATION (videosink) &&
        GST_STATE (videosink) >= GST_STATE_PAUSED) {
      GstNavigation *nav = GST_NAVIGATION (videosink);

      gst_navigation_send_mouse_event (nav,
          "mouse-button-release", event->button, event->x, event->y);

      /* we need a return value... */
      res = TRUE;
    }
  }
#endif
  if (GTK_WIDGET_CLASS (parent_class)->button_release_event)
    res |= GTK_WIDGET_CLASS (parent_class)->button_release_event (widget, event);

  return res;
}

static void
bacon_video_widget_size_request (GtkWidget * widget,
				 GtkRequisition * requisition)
{
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);

  requisition->width = bvw->priv->init_width;
  requisition->height = bvw->priv->init_height;
}

static void
bacon_video_widget_size_allocate (GtkWidget * widget,
				  GtkAllocation * allocation)
{
  BaconVideoWidget *bvw = BACON_VIDEO_WIDGET (widget);

  g_return_if_fail (widget != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (widget));

  widget->allocation = *allocation;

  if (GTK_WIDGET_REALIZED (widget)) {
    gfloat width, height, ratio;
    guint w, h;

    gdk_window_move_resize (widget->window,
                            allocation->x, allocation->y,
                            allocation->width, allocation->height);

    /* resize video_window */
    get_media_size (bvw, &w, &h);
    width = w;
    height = h;

    if ((gfloat) allocation->width / width >
        (gfloat) allocation->height / height) {
      ratio = (gfloat) allocation->height / height;
    } else {
      ratio = (gfloat) allocation->width / width;
    }

    width *= ratio;
    height *= ratio;

    bvw->priv->video_window_allocation.width = width;
    bvw->priv->video_window_allocation.height = height;
    bvw->priv->video_window_allocation.x = (allocation->width - width) / 2;
    bvw->priv->video_window_allocation.y = (allocation->height - height) / 2;
    gdk_window_move_resize (bvw->priv->video_window,
                            (allocation->width - width) / 2,
			    (allocation->height - height) / 2,
                            width, height);
  }
}

static void
bacon_video_widget_class_init (BaconVideoWidgetClass * klass)
{
  GObjectClass *object_class;
  GtkWidgetClass *widget_class;

  object_class = (GObjectClass *) klass;
  widget_class = (GtkWidgetClass *) klass;

  parent_class = gtk_type_class (gtk_box_get_type ());

  /* GtkWidget */
  widget_class->size_request = bacon_video_widget_size_request;
  widget_class->size_allocate = bacon_video_widget_size_allocate;
  widget_class->realize = bacon_video_widget_realize;
  widget_class->unrealize = bacon_video_widget_unrealize;
  widget_class->show = bacon_video_widget_show;
  widget_class->hide = bacon_video_widget_hide;
  widget_class->expose_event = bacon_video_widget_expose_event;
  widget_class->motion_notify_event = bacon_video_widget_motion_notify;
  widget_class->button_press_event = bacon_video_widget_button_press;
  widget_class->button_release_event = bacon_video_widget_button_release;

  /* FIXME:
   * - once I've re-added DVD supports, I want to add event handlers
   *    ( keys, mouse buttons, mouse motion) here, too.
   */
  
  /* GObject */
  object_class->set_property = bacon_video_widget_set_property;
  object_class->get_property = bacon_video_widget_get_property;
  object_class->finalize = bacon_video_widget_finalize;

  /* Properties */
  g_object_class_install_property (object_class, PROP_LOGO_MODE,
				   g_param_spec_boolean ("logo_mode", NULL,
							 NULL, FALSE,
							 G_PARAM_READWRITE));
  g_object_class_install_property (object_class, PROP_POSITION,
				   g_param_spec_int ("position", NULL, NULL,
						     0, G_MAXINT, 0,
						     G_PARAM_READABLE));
  g_object_class_install_property (object_class, PROP_STREAM_LENGTH,
				   g_param_spec_int64 ("stream_length", NULL,
						     NULL, 0, G_MAXINT64, 0,
						     G_PARAM_READABLE));
  g_object_class_install_property (object_class, PROP_PLAYING,
				   g_param_spec_boolean ("playing", NULL,
							 NULL, FALSE,
							 G_PARAM_READABLE));
  g_object_class_install_property (object_class, PROP_SEEKABLE,
				   g_param_spec_boolean ("seekable", NULL,
							 NULL, FALSE,
							 G_PARAM_READABLE));
  g_object_class_install_property (object_class, PROP_SHOWCURSOR,
				   g_param_spec_boolean ("showcursor", NULL,
							 NULL, FALSE,
							 G_PARAM_READWRITE));
  g_object_class_install_property (object_class, PROP_MEDIADEV,
				   g_param_spec_string ("mediadev", NULL,
							NULL, FALSE,
							G_PARAM_READWRITE));
  g_object_class_install_property (object_class, PROP_SHOW_VISUALS,
				   g_param_spec_boolean ("showvisuals", NULL,
							 NULL, FALSE,
							 G_PARAM_WRITABLE));

  /* Signals */
  bvw_table_signals[SIGNAL_ERROR] =
    g_signal_new ("error",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, error),
		  NULL, NULL,
		  baconvideowidget_marshal_VOID__STRING_BOOLEAN_BOOLEAN,
		  G_TYPE_NONE, 3, G_TYPE_STRING,
		  G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);

  bvw_table_signals[SIGNAL_EOS] =
    g_signal_new ("eos",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, eos),
		  NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  bvw_table_signals[SIGNAL_GOT_METADATA] =
    g_signal_new ("got-metadata",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, got_metadata),
		  NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  bvw_table_signals[SIGNAL_REDIRECT] =
    g_signal_new ("got-redirect",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, got_redirect),
		  NULL, NULL, g_cclosure_marshal_VOID__STRING,
		  G_TYPE_NONE, 1, G_TYPE_STRING);

  bvw_table_signals[SIGNAL_TITLE_CHANGE] =
    g_signal_new ("title-change",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, title_change),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__STRING,
		  G_TYPE_NONE, 1, G_TYPE_STRING);

  bvw_table_signals[SIGNAL_CHANNELS_CHANGE] =
    g_signal_new ("channels-change",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, channels_change),
		  NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  bvw_table_signals[SIGNAL_TICK] =
    g_signal_new ("tick",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, tick),
		  NULL, NULL,
		  baconvideowidget_marshal_VOID__INT64_INT64_FLOAT_BOOLEAN,
		  G_TYPE_NONE, 4, G_TYPE_INT64, G_TYPE_INT64, G_TYPE_FLOAT,
                  G_TYPE_BOOLEAN);

  bvw_table_signals[SIGNAL_BUFFERING] =
    g_signal_new ("buffering",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, buffering),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);

  bvw_table_signals[SIGNAL_SPEED_WARNING] =
    g_signal_new ("speed-warning",
		  G_TYPE_FROM_CLASS (object_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (BaconVideoWidgetClass, speed_warning),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
}

static void
bacon_video_widget_init (BaconVideoWidget * bvw)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  GTK_WIDGET_SET_FLAGS (GTK_WIDGET (bvw), GTK_CAN_FOCUS);
  GTK_WIDGET_SET_FLAGS (GTK_WIDGET (bvw), GTK_NO_WINDOW);
  GTK_WIDGET_UNSET_FLAGS (GTK_WIDGET (bvw), GTK_DOUBLE_BUFFERED);

  bvw->priv = g_new0 (BaconVideoWidgetPrivate, 1);
  
  bvw->priv->update_id = 0;
  bvw->priv->tagcache = NULL;
  bvw->priv->audiotags = bvw->priv->videotags = NULL;
}

static void
shrink_toplevel (BaconVideoWidget * bvw)
{
  GtkWidget *toplevel, *widget;
  widget = GTK_WIDGET (bvw);
  toplevel = gtk_widget_get_toplevel (widget);
  if (toplevel != widget && GTK_IS_WINDOW (toplevel) != FALSE)
    gtk_window_resize (GTK_WINDOW (toplevel), 1, 1);
}

static void
got_stream_length (GstElement * play, gint64 length_nanos,
                   BaconVideoWidget * bvw)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  bvw->priv->stream_length = (gint64) length_nanos / GST_MSECOND;
  
  //g_signal_emit (G_OBJECT (bvw), bvw_table_signals[SIGNAL_GOT_METADATA], 0, NULL);
}

static void
got_time_tick (GstElement * play, gint64 time_nanos, BaconVideoWidget * bvw)
{
  gboolean seekable;

  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  if (bvw->priv->logo_mode != FALSE)
    return;

  bvw->priv->current_time_nanos = time_nanos;

  bvw->priv->current_time = (gint64) time_nanos / GST_MSECOND;

  if (bvw->priv->stream_length == 0)
    bvw->priv->current_position = 0;
  else {
    bvw->priv->current_position =
        (float) bvw->priv->current_time / bvw->priv->stream_length;
  }
  
  seekable = bacon_video_widget_is_seekable (bvw);

  g_signal_emit (G_OBJECT (bvw),
                 bvw_table_signals[SIGNAL_TICK], 0,
                 bvw->priv->current_time, bvw->priv->stream_length,
                 bvw->priv->current_position,
                 seekable);
}

static gboolean
cb_iterate (BaconVideoWidget *bvw)
{
  GstFormat fmt = GST_FORMAT_TIME;
  gint64 pos = -1, len = -1;

  if (gst_element_query_position (bvw->priv->play, &fmt, &pos, &len)) {
    if (len != -1 && len / GST_MSECOND != bvw->priv->stream_length) {
      got_stream_length (bvw->priv->play, len, bvw);
    }
    if (pos != -1) {
      got_time_tick (bvw->priv->play, pos, bvw);
    }
  }

  return TRUE;
}

static void parse_stream_info (BaconVideoWidget *bvw);

static gboolean
bacon_video_widget_bus_callback (GstBus * bus, GstMessage * message,
				 BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR: {
      char *debug;
      GError *error;

      gst_message_parse_error (message, &error, &debug);
      if (bvw->priv->cache_errors) {
	if (!bvw->priv->last_error)
          bvw->priv->last_error = error;
	else
	  g_error_free (error);
      } else {
        g_signal_emit (G_OBJECT (bvw), bvw_table_signals[SIGNAL_ERROR], 0,
		       error->message, TRUE, FALSE);
	g_error_free (error);
      }

      /* Cleaning the error infos */
      g_free (debug);
      break;
    }
    case GST_MESSAGE_WARNING: {
      GError *error;
      char *debug;

      gst_message_parse_warning (message, &error, &debug);
      g_signal_emit (G_OBJECT (bvw), bvw_table_signals[SIGNAL_ERROR], 0,
		     error->message, FALSE, FALSE);

      /* clean-up */
      g_error_free (error);
      g_free (debug);
      break;
    }
    case GST_MESSAGE_TAG: {
      GstTagList *tag_list, *result;
      GstElementFactory *f;
      GstObject *src;

      gst_message_parse_tag (message, &tag_list);
      src = GST_MESSAGE_SRC (message);

      /* all tags */
      result = gst_tag_list_merge (bvw->priv->tagcache, tag_list,
				   GST_TAG_MERGE_APPEND);
      if (bvw->priv->tagcache)
        gst_tag_list_free (bvw->priv->tagcache);
      bvw->priv->tagcache = result;

      /* media-type-specific tags */
      if (GST_IS_ELEMENT (src) &&
	  (f = gst_element_get_factory (GST_ELEMENT (src)))) {
        const gchar *klass = gst_element_factory_get_klass (f);
        GstTagList **cache = NULL;

        if (g_strrstr (klass, "Video")) {
          cache = &bvw->priv->videotags;
        } else if (g_strrstr (klass, "Audio")) {
          cache = &bvw->priv->audiotags;
        }

        if (cache) {
          result = gst_tag_list_merge (*cache, tag_list, GST_TAG_MERGE_APPEND);
          if (*cache)
            gst_tag_list_free (*cache);
          *cache = result;
        }
      }

      /* clean up */
      gst_tag_list_free (tag_list);

      g_signal_emit (G_OBJECT (bvw),
		     bvw_table_signals[SIGNAL_GOT_METADATA], 0);
      break;
    }
#if 0
      case ASYNC_NOTIFY_STREAMINFO:
        {
          g_signal_emit (G_OBJECT (bvw),
			 bvw_table_signals[SIGNAL_GOT_METADATA], 0, NULL);
          g_signal_emit (bvw, bvw_table_signals[SIGNAL_CHANNELS_CHANGE], 0);
          break;
        }
#endif
    case GST_MESSAGE_EOS:
      g_signal_emit (G_OBJECT (bvw), bvw_table_signals[SIGNAL_EOS], 0, NULL);
      break;
    case GST_MESSAGE_BUFFERING: {
      const GstStructure *str;
      gint percent = 0;

      str = gst_message_get_structure (message);
      gst_structure_get_int (str, "buffer-percent", &percent);
      g_signal_emit (G_OBJECT (bvw), bvw_table_signals[SIGNAL_BUFFERING], 0,
		     percent);
      break;
    }
    case GST_MESSAGE_APPLICATION: {
      const GstStructure *str;
      const char *title;

      str = gst_message_get_structure (message);
      title = gst_structure_get_name (str);

      g_message ("Unhandled application message %s", title);
      break;
    }
#if 0
      case ASYNC_REDIRECT:
	{
	  g_signal_emit (G_OBJECT (bvw), bvw_table_signals[SIGNAL_REDIRECT],
			 0, signal->signal_data.redirect.new_location);
	  g_free (signal->signal_data.redirect.new_location);
	  break;
	}
#endif
    case GST_MESSAGE_STATE_CHANGED: {
      GstElementState old_state, new_state;

      /* we only care about playbin (pipeline) state changes */
      if (GST_MESSAGE_SRC (message) != GST_OBJECT (bvw->priv->play))
	break;

      gst_message_parse_state_changed (message, &old_state, &new_state);

      /* now do stuff */
      if (old_state == GST_STATE_PLAYING) {
        if (bvw->priv->update_id != 0) {
          g_source_remove (bvw->priv->update_id);
          bvw->priv->update_id = 0;
        }
      } else if (new_state == GST_STATE_PLAYING) {
        if (bvw->priv->update_id != 0)
          g_source_remove (bvw->priv->update_id);
        bvw->priv->update_id =
	    g_timeout_add (200, (GSourceFunc) cb_iterate, bvw);
      }

      if (old_state <= GST_STATE_READY &&
          new_state >= GST_STATE_PAUSED) {
        parse_stream_info (bvw);
      } else if (new_state <= GST_STATE_READY &&
                 old_state >= GST_STATE_PAUSED) {
        bvw->priv->media_has_video = FALSE;
        bvw->priv->media_has_audio = FALSE;

        /* clean metadata cache */
        if (bvw->priv->tagcache) {
          gst_tag_list_free (bvw->priv->tagcache);
          bvw->priv->tagcache = NULL;
        }
        if (bvw->priv->audiotags) {
          gst_tag_list_free (bvw->priv->audiotags);
          bvw->priv->audiotags = NULL;
        }
        if (bvw->priv->videotags) {
          gst_tag_list_free (bvw->priv->videotags);
          bvw->priv->videotags = NULL;
        }

        bvw->priv->video_width = 0;
        bvw->priv->video_height = 0;
      }
      break;
    }
    default:
      g_message ("Unhandled message type 0x%x", GST_MESSAGE_TYPE (message));
      break;
  }

  /* yes, remove the message from the queue */
  return TRUE;
}

#if 0
static void
group_switch (GstElement *play, BaconVideoWidget *bvw)
{
  BVWSignal *signal;

  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  if (bvw->priv->tagcache) {
    gst_tag_list_free (bvw->priv->tagcache);
    bvw->priv->tagcache = NULL;
  }
  if (bvw->priv->audiotags) {
    gst_tag_list_free (bvw->priv->audiotags);
    bvw->priv->audiotags = NULL;
  }
  if (bvw->priv->videotags) {
    gst_tag_list_free (bvw->priv->videotags);
    bvw->priv->videotags = NULL;
  }

  signal = g_new0 (BVWSignal, 1);
  signal->signal_id = ASYNC_NOTIFY_STREAMINFO;

  g_async_queue_push (bvw->priv->queue, signal);

  g_idle_add ((GSourceFunc) bacon_video_widget_signal_idler, bvw);
}
#endif

static void
got_video_size (BaconVideoWidget * bvw)
{
  if (bvw->priv->auto_resize && !bvw->priv->fullscreen_mode) {
    gint w, h;

    shrink_toplevel (bvw);
    get_media_size (bvw, &w, &h);
    totem_widget_set_preferred_size (GTK_WIDGET (bvw), w, h);
  } else {
    bacon_video_widget_size_allocate (GTK_WIDGET (bvw),
				      &GTK_WIDGET (bvw)->allocation);

    /* Uhm, so this ugly hack here makes media loading work for
     * weird laptops with NVIDIA graphics cards... Dunno what the
     * bug is really, but hey, it works. :). */
    gdk_window_hide (GTK_WIDGET (bvw)->window);
    gdk_window_show (GTK_WIDGET (bvw)->window);

    bacon_video_widget_expose_event (GTK_WIDGET (bvw), NULL);
  }
}

static void
got_source (GObject    *play,
	    GParamSpec *pspec,
	    BaconVideoWidget *bvw)
{
  GObject *source = NULL;
  GObjectClass *klass;

  if (bvw->priv->tagcache) {
    gst_tag_list_free (bvw->priv->tagcache);
    bvw->priv->tagcache = NULL;
  }
  if (bvw->priv->audiotags) {
    gst_tag_list_free (bvw->priv->audiotags);
    bvw->priv->audiotags = NULL;
  }
  if (bvw->priv->videotags) {
    gst_tag_list_free (bvw->priv->videotags);
    bvw->priv->videotags = NULL;
  }

  if (!bvw->priv->media_device)
    return;

  g_object_get (play, "source", &source, NULL);
  if (!source)
    return;

  klass = G_OBJECT_GET_CLASS (source);
  if (g_object_class_find_property (klass, "device")) {
    g_object_set (source, "device", bvw->priv->media_device, NULL);
  }
  g_object_unref (G_OBJECT (source));
}

static gboolean
idle_resize (BaconVideoWidget *bvw)
{
  bacon_video_widget_set_aspect_ratio (bvw, bvw->priv->ratio_type);

  /* once */
  return FALSE;
}

static void
parse_video_caps (GObject *obj, GParamSpec *pspec,
		  BaconVideoWidget * bvw)
{
  GstPad *pad = GST_PAD (obj);
  GstCaps *caps;
  GstStructure *s;

  if (!(caps = gst_pad_get_negotiated_caps (pad)))
    return;

  s = gst_caps_get_structure (caps, 0);
  if (s) {
    const GValue *par;

    if (!(gst_structure_get_double (s, "framerate", &bvw->priv->video_fps) &&
          gst_structure_get_int (s, "width", &bvw->priv->video_width) &&
          gst_structure_get_int (s, "height", &bvw->priv->video_height))) {
      gst_caps_unref (caps);
      return;
    }
    bvw->priv->video_width_pixels = bvw->priv->video_width;
    bvw->priv->video_height_pixels = bvw->priv->video_height;
    if ((par = gst_structure_get_value (s,
                   "pixel-aspect-ratio"))) {
      gint num = gst_value_get_fraction_numerator (par),
          den = gst_value_get_fraction_denominator (par);

      if (num > den)
        bvw->priv->video_width *= (gfloat) num / den;
      else
        bvw->priv->video_height *= (gfloat) den / num;
    }
    bvw->priv->movie_ratio = (gfloat) bvw->priv->video_width /
        bvw->priv->video_height;

    /* now set for real */
    g_idle_add ((GSourceFunc) idle_resize, bvw);
  }

  gst_caps_unref (caps);
}

static void get_visualization_size (BaconVideoWidget * bvw,
				    int *w, int *h, int *fps);

static void
parse_stream_info (BaconVideoWidget *bvw)
{
  GList *streaminfo = NULL;
  GstPad *videopad = NULL;

  g_object_get (G_OBJECT (bvw->priv->play), "stream-info",
		&streaminfo, NULL);
  streaminfo = g_list_copy (streaminfo);
  g_list_foreach (streaminfo, (GFunc) g_object_ref, NULL);
  for ( ; streaminfo != NULL; streaminfo = streaminfo->next) {
    GObject *info = streaminfo->data;
    gint type;
    GParamSpec *pspec;
    GEnumValue *val;

    if (!info)
      continue;
    g_object_get (info, "type", &type, NULL);
    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (info), "type");
    val = g_enum_get_value (G_PARAM_SPEC_ENUM (pspec)->enum_class, type);

    if (strstr (val->value_name, "AUDIO")) {
      if (!bvw->priv->media_has_audio) {
        bvw->priv->media_has_audio = TRUE;
      }
    } else if (strstr (val->value_name, "VIDEO")) {
      bvw->priv->media_has_video = TRUE;
      if (!videopad) {
        g_object_get (info, "object", &videopad, NULL);
      }
    }
  }

  if (videopad) {
    GstCaps *caps;

    /* handle explicit caps as well - they're set later */
    g_signal_handlers_disconnect_by_func (videopad, parse_video_caps, bvw);
    if ((caps = gst_pad_get_negotiated_caps (videopad))) {
      parse_video_caps (G_OBJECT (videopad), NULL, bvw);
      gst_caps_unref (caps);
    } else {
      g_signal_connect (videopad, "notify::caps",
			G_CALLBACK (parse_video_caps), bvw);
    }
  } else if (bvw->priv->show_vfx && bvw->priv->vis_element) {
    int dummy;
    get_visualization_size (bvw, &bvw->priv->video_width,
			    &bvw->priv->video_height, &dummy);
  }

  g_list_foreach (streaminfo, (GFunc) g_object_unref, NULL);
  g_list_free (streaminfo);
}

static void
stream_info_set (GObject * obj, GParamSpec * pspec, BaconVideoWidget * bvw)
{
#if 0
  BVWSignal *signal;
#endif
  parse_stream_info (bvw);
#if 0
  signal = g_new0 (BVWSignal, 1);
  signal->signal_id = ASYNC_NOTIFY_STREAMINFO;

  g_async_queue_push (bvw->priv->queue, signal);

  g_idle_add ((GSourceFunc) bacon_video_widget_signal_idler, bvw);
#endif
}

static void
bacon_video_widget_finalize (GObject * object)
{
  BaconVideoWidget *bvw = (BaconVideoWidget *) object;

  if (bvw->priv->media_device) {
    g_free (bvw->priv->media_device);
    bvw->priv->media_device = NULL;
  }
    
  if (bvw->priv->mrl) {
    g_free (bvw->priv->mrl);
    bvw->priv->mrl = NULL;
  }
    
  if (bvw->priv->vis_plugins_list) {
    g_list_foreach (bvw->priv->vis_plugins_list, (GFunc) g_free, NULL);
    g_list_free (bvw->priv->vis_plugins_list);
    bvw->priv->vis_plugins_list = NULL;
  }

  if (bvw->priv->play != NULL && GST_IS_ELEMENT (bvw->priv->play)) {
    gst_element_set_state (GST_ELEMENT (bvw->priv->play), GST_STATE_READY);
    gst_object_unref (GST_OBJECT (bvw->priv->play));
    bvw->priv->play = NULL;
  }

  if (bvw->priv->update_id) {
    g_source_remove (bvw->priv->update_id);
    bvw->priv->update_id = 0;
  }

  if (bvw->priv->tagcache) {
    gst_tag_list_free (bvw->priv->tagcache);
    bvw->priv->tagcache = NULL;
  }
  if (bvw->priv->audiotags) {
    gst_tag_list_free (bvw->priv->audiotags);
    bvw->priv->audiotags = NULL;
  }
  if (bvw->priv->videotags) {
    gst_tag_list_free (bvw->priv->videotags);
    bvw->priv->videotags = NULL;
  }

  g_free (bvw->priv);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
bacon_video_widget_set_property (GObject * object, guint property_id,
                                 const GValue * value, GParamSpec * pspec)
{
  BaconVideoWidget *bvw;

  bvw = BACON_VIDEO_WIDGET (object);

  switch (property_id)
    {
    case PROP_LOGO_MODE:
      bacon_video_widget_set_logo_mode (bvw,
	  g_value_get_boolean (value));
      break;
    case PROP_SHOWCURSOR:
      bacon_video_widget_set_show_cursor (bvw,
	  g_value_get_boolean (value));
      break;
    case PROP_MEDIADEV:
      bacon_video_widget_set_media_device (bvw,
	  g_value_get_string (value));
      break;
    case PROP_SHOW_VISUALS:
      bacon_video_widget_set_show_visuals (bvw,
	  g_value_get_boolean (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
bacon_video_widget_get_property (GObject * object, guint property_id,
                                 GValue * value, GParamSpec * pspec)
{
  BaconVideoWidget *bvw;

  bvw = BACON_VIDEO_WIDGET (object);

  switch (property_id)
    {
      case PROP_LOGO_MODE:
	g_value_set_boolean (value,
	    bacon_video_widget_get_logo_mode (bvw));
	break;
      case PROP_POSITION:
	g_value_set_int64 (value, bacon_video_widget_get_position (bvw));
	break;
      case PROP_STREAM_LENGTH:
	g_value_set_int64 (value,
	    bacon_video_widget_get_stream_length (bvw));
	break;
      case PROP_PLAYING:
	g_value_set_boolean (value,
	    bacon_video_widget_is_playing (bvw));
	break;
      case PROP_SEEKABLE:
	g_value_set_boolean (value,
	    bacon_video_widget_is_seekable (bvw));
	break;
      case PROP_SHOWCURSOR:
	g_value_set_boolean (value,
	    bacon_video_widget_get_show_cursor (bvw));
	break;
      case PROP_MEDIADEV:
	g_value_take_string (value, bvw->priv->media_device);
	break;
      default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

/* ============================================================= */
/*                                                               */
/*                       Public Methods                          */
/*                                                               */
/* ============================================================= */

char *
bacon_video_widget_get_backend_name (BaconVideoWidget * bvw)
{
  guint major, minor, micro;

  gst_version (&major, &minor, &micro);

  return g_strdup_printf ("GStreamer version %d.%d.%d", major, minor, micro);
}

static gboolean
has_subp (BaconVideoWidget * bvw)
{
  GList *streaminfo = NULL;
  gboolean res = FALSE;

  if (bvw->priv->play == NULL || bvw->priv->mrl == NULL)
    return FALSE;

  g_object_get (G_OBJECT (bvw->priv->play), "stream-info", &streaminfo, NULL);
  streaminfo = g_list_copy (streaminfo);
  g_list_foreach (streaminfo, (GFunc) g_object_ref, NULL);
  for ( ; streaminfo != NULL; streaminfo = streaminfo->next) {
    GObject *info = streaminfo->data;
    gint type;
    GParamSpec *pspec;
    GEnumValue *val;

    if (!info)
      continue;
    g_object_get (info, "type", &type, NULL);
    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (info), "type");
    val = g_enum_get_value (G_PARAM_SPEC_ENUM (pspec)->enum_class, type);

    if (strstr (val->value_name, "SUBPICTURE")) {
      res = TRUE;
      break;
    }
  }
  g_list_foreach (streaminfo, (GFunc) g_object_unref, NULL);
  g_list_free (streaminfo);

  return res;
}

int
bacon_video_widget_get_subtitle (BaconVideoWidget * bvw)
{
  int subtitle = -1;

  g_return_val_if_fail (bvw != NULL, -2);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), -2);
  g_return_val_if_fail (bvw->priv->play != NULL, -2);

  if (has_subp (bvw))
    g_object_get (G_OBJECT (bvw->priv->play), "current-subpicture", &subtitle, NULL);
  else
    g_object_get (G_OBJECT (bvw->priv->play), "current-text", &subtitle, NULL);

  if (subtitle == -1)
    subtitle = -2;

  return subtitle;
}

void
bacon_video_widget_set_subtitle (BaconVideoWidget * bvw, int subtitle)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (bvw->priv->play != NULL);

  if (subtitle == -1)
    subtitle = 0;
  else if (subtitle == -2)
    subtitle = -1;

  if (has_subp (bvw))
    g_object_set (G_OBJECT (bvw->priv->play), "current-subpicture", subtitle, NULL);
  else
    g_object_set (G_OBJECT (bvw->priv->play), "current-text", subtitle, NULL);
}

static GList *
get_list_of_type (BaconVideoWidget * bvw, const gchar * type_name)
{
  GList *streaminfo = NULL, *ret = NULL;
  gint num = 0;

  if (bvw->priv->play == NULL || bvw->priv->mrl == NULL)
    return NULL;

  g_object_get (G_OBJECT (bvw->priv->play), "stream-info", &streaminfo, NULL);
  streaminfo = g_list_copy (streaminfo);
  g_list_foreach (streaminfo, (GFunc) g_object_ref, NULL);
  for ( ; streaminfo != NULL; streaminfo = streaminfo->next) {
    GObject *info = streaminfo->data;
    gint type;
    GParamSpec *pspec;
    GEnumValue *val;
    gchar *lc = NULL, *cd = NULL;

    if (!info)
      continue;
    g_object_get (info, "type", &type, NULL);
    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (info), "type");
    val = g_enum_get_value (G_PARAM_SPEC_ENUM (pspec)->enum_class, type);

    if (strstr (val->value_name, type_name)) {
      g_object_get (info, "codec", &cd, "language-code", &lc, NULL);

      if (lc) {
        ret = g_list_prepend (ret, lc);
	g_free (cd);
      } else if (cd) {
        ret = g_list_prepend (ret, cd);
      } else {
        ret = g_list_prepend (ret, g_strdup_printf ("%s %d", type_name, num++));
      }
    }
  }
  g_list_foreach (streaminfo, (GFunc) g_object_unref, NULL);
  g_list_free (streaminfo);

  return g_list_reverse (ret);
}

GList * bacon_video_widget_get_subtitles (BaconVideoWidget * bvw)
{
  GList *list;

  g_return_val_if_fail (bvw != NULL, NULL);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), NULL);
  g_return_val_if_fail (bvw->priv->play != NULL, NULL);

  if (!(list =  get_list_of_type (bvw, "SUBPICTURE")))
    list = get_list_of_type (bvw, "TEXT");

  return list;
}

GList * bacon_video_widget_get_languages (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, NULL);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), NULL);
  g_return_val_if_fail (bvw->priv->play != NULL, NULL);

  return get_list_of_type (bvw, "AUDIO");
}

int
bacon_video_widget_get_language (BaconVideoWidget * bvw)
{
  int language = -1;

  g_return_val_if_fail (bvw != NULL, -2);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), -2);
  g_return_val_if_fail (bvw->priv->play != NULL, -2);

  g_object_get (G_OBJECT (bvw->priv->play), "current-audio", &language, NULL);

  if (language == -1)
    language = -2;

  return language;
}

void
bacon_video_widget_set_language (BaconVideoWidget * bvw, int language)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (bvw->priv->play != NULL);

  if (language == -1)
    language = 0;
  else if (language == -2)
    language = -1;

  g_object_set (G_OBJECT (bvw->priv->play), "current-audio", language, NULL);
}

int
bacon_video_widget_get_connection_speed (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, 0);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), 0);

  return bvw->priv->connection_speed;
}

void
bacon_video_widget_set_connection_speed (BaconVideoWidget * bvw, int speed)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  bvw->priv->connection_speed = speed;
  gconf_client_set_int (bvw->priv->gc,
       GCONF_PREFIX"/connection_speed", speed, NULL);
}

void
bacon_video_widget_set_deinterlacing (BaconVideoWidget * bvw,
				      gboolean deinterlace)
{
}

gboolean
bacon_video_widget_get_deinterlacing (BaconVideoWidget * bvw)
{
  return FALSE;
}

gboolean
bacon_video_widget_set_tv_out (BaconVideoWidget * bvw, TvOutType tvout)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);

  bvw->priv->tv_out_type = tvout;
  gconf_client_set_int (bvw->priv->gc,
      GCONF_PREFIX"/tv_out_type", tvout, NULL);

#ifdef HAVE_NVTV
  if (tvout == TV_OUT_NVTV_PAL) {
    nvtv_simple_set_tvsystem(NVTV_SIMPLE_TVSYSTEM_PAL);
  } else if (tvout == TV_OUT_NVTV_NTSC) {
    nvtv_simple_set_tvsystem(NVTV_SIMPLE_TVSYSTEM_NTSC);
  }
#endif

  return FALSE;
}

TvOutType
bacon_video_widget_get_tv_out (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, 0);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), 0);

  return bvw->priv->tv_out_type;
}

static void
set_audio_filter (BaconVideoWidget *bvw)
{
  gint channels = -1, n, count;
  GstCaps *caps, *res = gst_caps_new_empty ();
  GstPad *pad;
  const GstStructure *s;
  GstStructure *copy;
  const GValue *v;

  /* reset old */
  g_object_set (G_OBJECT (bvw->priv->audiocapsfilter),
      "filter-caps", gst_caps_new_any (), NULL);

  /* get possible caps */
  pad = gst_element_get_pad (bvw->priv->audiocapsfilter, "src");
  caps = gst_pad_get_allowed_caps (pad);
  gst_object_unref (GST_OBJECT (pad));

  /* find number of channels */
  switch (bvw->priv->speakersetup) {
    case BVW_AUDIO_SOUND_STEREO:
      channels = 2;
      break;
    case BVW_AUDIO_SOUND_4CHANNEL:
      channels = 4;
      break;
    case BVW_AUDIO_SOUND_5CHANNEL:
      channels = 5;
      break;
    case BVW_AUDIO_SOUND_41CHANNEL:
      /* so alsa has this as 5.1, but empty center speaker. We don't really
       * do that yet. ;-). So we'll take the placebo approach. At some point,
       * we should allow to set custom matrices on audioconvert. */
    case BVW_AUDIO_SOUND_51CHANNEL:
      channels = 6;
      break;
    case BVW_AUDIO_SOUND_AC3PASSTHRU:
    default:
      g_return_if_reached ();
  }

  /* find caps matching this channelcount */
  count = gst_caps_get_size (caps);
  for (n = 0; n < count; n++) {
    s = gst_caps_get_structure (caps, n);
    v = gst_structure_get_value (s, "channels");
    if (!v)
      continue;

    /* get channel count (or list of ~) */
    if (G_VALUE_TYPE (v) == G_TYPE_INT) {
      gint c = g_value_get_int (v);

      if (channels == c) {
	copy = gst_structure_copy (s);
        gst_caps_append_structure (res, copy);
      }
    } else if (G_VALUE_TYPE (v) == GST_TYPE_INT_RANGE) {
      gint c1 = gst_value_get_int_range_min (v),
           c2 = gst_value_get_int_range_max (v);

      if (c1 <= channels  && c2 >= channels) {
        copy = gst_structure_copy (s);
	gst_structure_set (copy, "channels", G_TYPE_INT, channels, NULL);
        gst_caps_append_structure (res, copy);
      }
    } else if (G_VALUE_TYPE (v) == GST_TYPE_LIST) {
      const GValue *kid;
      gint nkid, kidcount = gst_value_list_get_size (v), kidc;

      for (nkid = 0; nkid < kidcount; nkid++) {
        kid = gst_value_list_get_value (v, nkid);
        if (G_VALUE_TYPE (kid) != G_TYPE_INT)
          continue;
        kidc = g_value_get_int (kid);
        if (kidc == channels) {
          copy = gst_structure_copy (s);
	  gst_structure_set (copy, "channels", G_TYPE_INT, channels, NULL);
	  gst_caps_append_structure (res, copy);
        }
      }
    }
  }

  /* set */
  if (gst_caps_is_empty (res)) {
    gst_caps_unref (res);
    res = gst_caps_new_any ();
  }
  g_object_set (G_OBJECT (bvw->priv->audiocapsfilter),
      "filter-caps", res, NULL);

  /* reset */
  pad = gst_element_get_pad (bvw->priv->audioconvert, "src");
  gst_pad_set_caps (pad, NULL);
  gst_object_unref (GST_OBJECT (pad));
}

BaconVideoWidgetAudioOutType
bacon_video_widget_get_audio_out_type (BaconVideoWidget *bvw)
{
  g_return_val_if_fail (bvw != NULL, -1);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), -1);

  return bvw->priv->speakersetup;
}

gboolean
bacon_video_widget_set_audio_out_type (BaconVideoWidget *bvw,
                                       BaconVideoWidgetAudioOutType type)
{
  GstPad *pad;

  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);

  if (type == bvw->priv->speakersetup)
    return FALSE;
  else if (type == BVW_AUDIO_SOUND_AC3PASSTHRU)
    return FALSE;

  bvw->priv->speakersetup = type;
  gconf_client_set_int (bvw->priv->gc,
      GCONF_PREFIX"/audio_output_type", type, NULL);

  pad = gst_element_get_pad (bvw->priv->audioconvert, "sink");
  GST_STREAM_LOCK (pad);
  set_audio_filter (bvw);
  GST_STREAM_UNLOCK (pad);
  gst_object_unref (GST_OBJECT (pad));

  return FALSE;
}

/* =========================================== */
/*                                             */
/*               Play/Pause, Stop              */
/*                                             */
/* =========================================== */

gboolean
bacon_video_widget_open_with_subtitle (BaconVideoWidget * bvw,
		const gchar * mrl, const gchar *subtitle_uri, GError ** error)
{
  gboolean ret;
  gchar buf[256];

  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (mrl != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (bvw->priv->play != NULL, FALSE);
  g_return_val_if_fail (bvw->priv->mrl == NULL, FALSE);

  /* hmm... */
  if (bvw->priv->mrl && !strcmp (bvw->priv->mrl, mrl))
    return TRUE;

  /* this allows non-URI type of files in the thumbnailer and so on */
  g_free (bvw->priv->mrl);
  if (mrl[0] == '/') {
    bvw->priv->mrl = g_strdup_printf ("file://%s", mrl);
  } else {
    if (strchr (mrl, ':')) {
      bvw->priv->mrl = g_strdup (mrl);
    } else {
      if (!getcwd (buf, 255)) {
	g_set_error (error, BVW_ERROR, BVW_ERROR_GENERIC,
	     _("Failed to retrieve working directory"));
	return FALSE;
      }
      bvw->priv->mrl = g_strdup_printf ("file://%s/%s", buf, mrl);
    }
  }

  gst_element_set_state (GST_ELEMENT (bvw->priv->play), GST_STATE_READY);

  /* Resetting last_error to NULL */
  if (bvw->priv->last_error) {
    g_error_free (bvw->priv->last_error);
    bvw->priv->last_error = NULL;
  }
  bvw->priv->got_redirect = FALSE;

  bvw->priv->media_has_video = FALSE;
  bvw->priv->stream_length = 0;

  if (g_strrstr (bvw->priv->mrl, "#subtitle:")) {
    gchar **uris;

    uris = g_strsplit (bvw->priv->mrl, "#subtitle:", 2);
    g_object_set (G_OBJECT (bvw->priv->play), "uri",
                  uris[0], "suburi", uris[1], NULL);
    g_strfreev (uris);
  } else {
    g_object_set (G_OBJECT (bvw->priv->play), "uri",
		  bvw->priv->mrl, "suburi", subtitle_uri, NULL);
  }

  bvw->priv->cache_errors = TRUE;
  ret = (gst_element_set_state (bvw->priv->play, GST_STATE_PAUSED) ==
      GST_STATE_SUCCESS);
  if (!ret) {
    while (!bvw->priv->last_error && g_main_iteration (FALSE)) ;
  }
  bvw->priv->cache_errors = FALSE;
  if (!ret && !bvw->priv->got_redirect) {
    if (error) {
      if (bvw->priv->last_error) {
	GError *e = bvw->priv->last_error;

#define is_error(e, d, c) \
  (e->domain == GST_##d##_ERROR && \
   e->code == GST_##d##_ERROR_##c)
	if (is_error (e, RESOURCE, NOT_FOUND) ||
	    is_error (e, RESOURCE, OPEN_READ)) {
	  if (strchr (mrl, ':') &&
	      (g_str_has_prefix (mrl, "dvd") ||
	       g_str_has_prefix (mrl, "cd") ||
	       g_str_has_prefix (mrl, "vcd"))) {
	    *error = g_error_new_literal (BVW_ERROR, BVW_ERROR_INVALID_DEVICE,
					  e->message);
	  } else {
	    if (e->code == GST_RESOURCE_ERROR_NOT_FOUND) {
	      g_set_error (error, BVW_ERROR, BVW_ERROR_FILE_NOT_FOUND,
		  _("Location not found."));
	    } else {
	      g_set_error (error, BVW_ERROR, BVW_ERROR_FILE_PERMISSION,
		  _("You don't have permission to open that location."));
	    }
	  }
	} else if (e->domain == GST_RESOURCE_ERROR) {
	  *error = g_error_new_literal (BVW_ERROR, BVW_ERROR_FILE_GENERIC,
					e->message);
	} else if (is_error (e, STREAM, WRONG_TYPE) ||
		   is_error (e, STREAM, CODEC_NOT_FOUND) ||
		   is_error (e, STREAM, NOT_IMPLEMENTED)) {
	  *error = g_error_new_literal (BVW_ERROR, BVW_ERROR_CODEC_NOT_HANDLED,
	      				e->message);
	} else {
	  /* generic error, no code; take message */
	  *error = g_error_new_literal (BVW_ERROR, BVW_ERROR_GENERIC,
					e->message);
	}
      }
      if (!*error) {
	/* well, I Really can't make anything out of the error... */
	g_set_error (error, BVW_ERROR, BVW_ERROR_FILE_GENERIC,
	    _("Failed to open media file; unknown error"));
      }
    }

    g_free (bvw->priv->mrl);
    bvw->priv->mrl = NULL;
  }

  if (ret)
    g_signal_emit (bvw, bvw_table_signals[SIGNAL_CHANNELS_CHANGE], 0);

  return ret || bvw->priv->got_redirect;
}

gboolean
bacon_video_widget_play (BaconVideoWidget * bvw, GError ** error)
{
  gboolean ret;

  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  /* Resetting last_error to NULL */
  if (bvw->priv->last_error) {
    g_error_free (bvw->priv->last_error);
    bvw->priv->last_error = NULL;
  }

  bvw->priv->cache_errors = TRUE;
  ret = (gst_element_set_state (GST_ELEMENT (bvw->priv->play),
      GST_STATE_PLAYING) == GST_STATE_SUCCESS);
  if (!ret) {
    while (!bvw->priv->last_error && g_main_iteration (FALSE)) ;
  }
  bvw->priv->cache_errors = FALSE;
  if (!ret) {
    g_set_error (error, BVW_ERROR, BVW_ERROR_GENERIC, _("Failed to play: %s"),
	 (bvw->priv->last_error != NULL) ?
	 bvw->priv->last_error->message : _("unknown error"));
  }

  return ret;
}

gboolean
bacon_video_widget_seek_time (BaconVideoWidget *bvw, gint64 time, GError **gerror)
{
  gboolean was_playing;

  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  /* Resetting last_error to NULL */
  if (bvw->priv->last_error) {
    g_error_free (bvw->priv->last_error);
    bvw->priv->last_error = NULL;
  }

  /* FIXME: hold lock */
  was_playing = (GST_STATE (bvw->priv->play) == GST_STATE_PLAYING);

  if (was_playing)
    gst_element_set_state (bvw->priv->play, GST_STATE_PAUSED);

  bvw->priv->cache_errors = TRUE;
  gst_element_seek (bvw->priv->play, 1.0,
		    GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
		    GST_SEEK_TYPE_SET, time * GST_MSECOND,
		    GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  bvw->priv->cache_errors = FALSE;

  if (was_playing)
    gst_element_set_state (bvw->priv->play, GST_STATE_PLAYING);

  return TRUE;
}

gboolean
bacon_video_widget_seek (BaconVideoWidget *bvw, float position, GError **gerror)
{
  gint64 seek_time, length_nanos;

  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  length_nanos = (gint64) (bvw->priv->stream_length * GST_MSECOND);
  seek_time = (gint64) (length_nanos * position);

  return bacon_video_widget_seek_time (bvw, seek_time / GST_MSECOND, gerror);
}

void
bacon_video_widget_stop (BaconVideoWidget * bvw)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  gst_element_set_state (GST_ELEMENT (bvw->priv->play), GST_STATE_READY);
}

void
bacon_video_widget_close (BaconVideoWidget * bvw)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  gst_element_set_state (GST_ELEMENT (bvw->priv->play), GST_STATE_READY);
  
  if (bvw->priv->mrl) {
    g_free (bvw->priv->mrl);
    bvw->priv->mrl = NULL;
  }

  g_signal_emit (bvw, bvw_table_signals[SIGNAL_CHANNELS_CHANGE], 0);
}

void
bacon_video_widget_dvd_event (BaconVideoWidget * bvw,
			      BaconVideoWidgetDVDEvent type)
{
#if 0
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  switch (type) {
    case BVW_DVD_ROOT_MENU:
    case BVW_DVD_TITLE_MENU:
    case BVW_DVD_SUBPICTURE_MENU:
    case BVW_DVD_AUDIO_MENU:
    case BVW_DVD_ANGLE_MENU:
    case BVW_DVD_CHAPTER_MENU:
      /* FIXME */
      break;
    case BVW_DVD_NEXT_CHAPTER:
    case BVW_DVD_PREV_CHAPTER:
    case BVW_DVD_NEXT_TITLE:
    case BVW_DVD_PREV_TITLE:
    case BVW_DVD_NEXT_ANGLE:
    case BVW_DVD_PREV_ANGLE: {
      GstFormat fmt;
      gint64 val;
      gint dir;

      if (type == BVW_DVD_NEXT_CHAPTER ||
          type == BVW_DVD_NEXT_TITLE ||
          type == BVW_DVD_NEXT_ANGLE)
        dir = 1;
      else
        dir = -1;

      if (type == BVW_DVD_NEXT_CHAPTER || type == BVW_DVD_PREV_CHAPTER)
        fmt = gst_format_get_by_nick ("chapter");
      else if (type == BVW_DVD_NEXT_TITLE || type == BVW_DVD_PREV_TITLE)
        fmt == gst_format_get_by_nick ("title");
      else
        fmt = gst_format_get_by_nick ("angle");

      if (gst_element_query (bvw->priv->play,
          GST_QUERY_POSITION, &fmt, &val)) {
        val += dir;
        gst_element_seek (bvw->priv->play,
            fmt | GST_SEEK_METHOD_SET | GST_SEEK_FLAG_FLUSH, val);
      }
      break;
    }
    default:
      break;
  }
#endif
}

void
bacon_video_widget_set_logo (BaconVideoWidget * bvw, gchar * filename)
{
  GError *error = NULL;
  
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  bvw->priv->logo_pixbuf = gdk_pixbuf_new_from_file (filename, &error);

  if (error) {
    g_warning ("An error occured trying to open logo %s: %s",
               filename, error->message);
    g_error_free (error);
  } else {
    gint w, h;

    shrink_toplevel (bvw);
    get_media_size (bvw, &w, &h);
    totem_widget_set_preferred_size (GTK_WIDGET (bvw), w, h);
  }
}

void
bacon_video_widget_set_logo_mode (BaconVideoWidget * bvw, gboolean logo_mode)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  /* this probably tells us to show logo or not? We already know
   * that from the media...? */
  bvw->priv->logo_mode = logo_mode;
}

gboolean
bacon_video_widget_get_logo_mode (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);

  return bvw->priv->logo_mode;
}

void
bacon_video_widget_pause (BaconVideoWidget * bvw)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  gst_element_set_state (GST_ELEMENT (bvw->priv->play), GST_STATE_PAUSED);
}

void
bacon_video_widget_set_proprietary_plugins_path (BaconVideoWidget * bvw,
						 const char *path)
{
}

void
bacon_video_widget_set_subtitle_font (BaconVideoWidget * bvw,
    				      const gchar * font)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  g_object_set (G_OBJECT (bvw->priv->play), "subtitle-font-desc", font, NULL);
}

gboolean
bacon_video_widget_can_set_volume (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);
  
  return TRUE;
}

void
bacon_video_widget_set_volume (BaconVideoWidget * bvw, int volume)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  if (bacon_video_widget_can_set_volume (bvw) != FALSE)
  {
    volume = CLAMP (volume, 0, 100);
    g_object_set (G_OBJECT (bvw->priv->play), "volume",
	(gdouble) (1. * volume / 100), NULL);
  }
}

int
bacon_video_widget_get_volume (BaconVideoWidget * bvw)
{
  gdouble vol;

  g_return_val_if_fail (bvw != NULL, -1);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), -1);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), -1);

  g_object_get (G_OBJECT (bvw->priv->play), "volume", &vol, NULL);

  return (gint) (vol * 100 + 0.5);
}

gboolean
bacon_video_widget_fullscreen_mode_available (BaconVideoWidget *bvw,
		TvOutType tvout) 
{
	switch(tvout) {
	case TV_OUT_NONE:
		/* Asume that ordinary fullscreen always works */
		return TRUE;
	case TV_OUT_NVTV_NTSC:
	case TV_OUT_NVTV_PAL:
#ifdef HAVE_NVTV
		/* Make sure nvtv is initialized, it will not do any harm 
		 * if it is done twice any way */
		if (!(nvtv_simple_init() && nvtv_enable_autoresize(TRUE))) {
			nvtv_simple_enable(FALSE);
		}
		return (nvtv_simple_is_available());
#else
		return FALSE;
#endif
	case TV_OUT_DXR3:
		/* FIXME: Add DXR3 detection code */
		return FALSE;
	}
	return FALSE;
}

void
bacon_video_widget_set_fullscreen (BaconVideoWidget * bvw,
				   gboolean fullscreen)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  bvw->priv->fullscreen_mode = fullscreen;

#ifdef HAVE_NVTV
  if (bvw->priv->tv_out_type != TV_OUT_NVTV_NTSC &&
      bvw->priv->tv_out_type != TV_OUT_NVTV_PAL)
    return;

  if (fullscreen == FALSE)
  {
    /* If NVTV is used */
    if (nvtv_simple_get_state() == NVTV_SIMPLE_TV_ON) {
      nvtv_simple_switch(NVTV_SIMPLE_TV_OFF,0,0);

    }
    /* Turn fullscreen on with NVTV if that option is on */
  } else if ((bvw->priv->tv_out_type == TV_OUT_NVTV_NTSC) ||
      (bvw->priv->tv_out_type == TV_OUT_NVTV_PAL)) {
    nvtv_simple_switch(NVTV_SIMPLE_TV_ON,
	bvw->priv->video_width,
	bvw->priv->video_height);
  }
#endif
}

void
bacon_video_widget_set_show_cursor (BaconVideoWidget * bvw,
				    gboolean show_cursor)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  if (show_cursor == FALSE)
  {
    totem_gdk_window_set_invisible_cursor (bvw->priv->video_window);
  } else {
    gdk_window_set_cursor (bvw->priv->video_window, NULL);
  }

  bvw->priv->cursor_shown = show_cursor;
}

gboolean
bacon_video_widget_get_show_cursor (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);

  return bvw->priv->cursor_shown;
}

void
bacon_video_widget_set_media_device (BaconVideoWidget * bvw, const char *path)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));
  
  g_free (bvw->priv->media_device);
  bvw->priv->media_device = g_strdup (path);
}

static void
get_visualization_size (BaconVideoWidget *bvw,
			int *w, int *h, int *fps)
{
  /* now see how close we can go */
  switch (bvw->priv->visq) {
    case VISUAL_SMALL:
      *fps = 10;
      *w = 200;
      *h = 150;
      break;
    case VISUAL_NORMAL:
      *fps = 20;
      *w = 320;
      *h = 240;
      break;
    case VISUAL_LARGE:
      *fps = 25;
      *w = 640;
      *h = 480;
      break;
    case VISUAL_EXTRA_LARGE:
      *fps = 30;
      *w = 800;
      *h = 600;
      break;
    default:
      /* shut up warnings */
      *fps = *w = *h = 0;
      g_assert_not_reached ();
  }
}

static void
change_visualization_quality (BaconVideoWidget *bvw)
{
  GstPad *pad, *spad;
  GstCaps *caps;
  GstStructure *s;
  int fps, w, h;

  /* get size */
  get_visualization_size (bvw, &w, &h, &fps);

  /* first unset any old filter, otherwise _get_allowed_caps takes the
   * old one into account. */
  g_object_set (G_OBJECT (bvw->priv->vis_capsfilter),
		"filter-caps", gst_caps_new_any (), NULL);

  /* see what we can do */
  pad = gst_element_get_pad (bvw->priv->vis_element, "src");
  spad = gst_element_get_pad (bvw->priv->vis_element, "sink");
  caps = gst_pad_get_allowed_caps (pad);

  /* fixate */
  s = gst_caps_get_structure (caps, 0);
  gst_caps_structure_fixate_field_nearest_int (s, "width", w);
  gst_caps_structure_fixate_field_nearest_int (s, "height", h);
  gst_caps_structure_fixate_field_nearest_double (s, "framerate", fps);

  /* set this */
  g_object_set (G_OBJECT (bvw->priv->vis_capsfilter),
		"filter-caps", caps, NULL);

  /* re-negotiate */
  GST_STREAM_LOCK (spad);
  gst_pad_set_caps (pad, NULL);
  GST_STREAM_UNLOCK (spad);

  gst_object_unref (GST_OBJECT (spad));
  gst_object_unref (GST_OBJECT (pad));
}

static void
setup_vis (BaconVideoWidget * bvw)
{
  const GstElement *vis = NULL;

  if (bvw->priv->show_vfx && bvw->priv->vis_element) {
    GstElement *bin;
    GstPad *pad;

    /* unparent */
    g_object_set (G_OBJECT (bvw->priv->play),
		  "vis-plugin", NULL, NULL);

    bin = gst_bin_new ("vis-bin");
    gst_object_ref (GST_OBJECT (bvw->priv->vis_capsfilter));
    gst_object_ref (GST_OBJECT (bvw->priv->vis_element));
    gst_bin_add_many (GST_BIN (bin), bvw->priv->vis_element,
		      bvw->priv->vis_capsfilter, NULL);
    vis = bin;

    pad = gst_element_get_pad (bvw->priv->vis_element, "sink");
    gst_element_add_pad (bin, gst_ghost_pad_new ("sink", pad));
    gst_object_unref (GST_OBJECT (pad));

    pad = gst_element_get_pad (bvw->priv->vis_capsfilter, "src");
    gst_element_add_pad (bin, gst_ghost_pad_new ("src", pad));
    gst_object_unref (GST_OBJECT (pad));

    gst_element_link_pads (bvw->priv->vis_element, "src",
			   bvw->priv->vis_capsfilter, "sink");

    change_visualization_quality (bvw);
  }

  g_object_set (G_OBJECT (bvw->priv->play), "vis-plugin", vis, NULL);
}

gboolean
bacon_video_widget_set_show_visuals (BaconVideoWidget * bvw,
				     gboolean show_visuals)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  bvw->priv->show_vfx = show_visuals;
  gconf_client_set_bool (bvw->priv->gc,
      GCONF_PREFIX"/show_vfx", show_visuals, NULL);

  setup_vis (bvw);

  return TRUE;
}

static gboolean
filter_features (GstPluginFeature * feature, gpointer data)
{
  GstElementFactory *f;

  if (!GST_IS_ELEMENT_FACTORY (feature))
    return FALSE;
  f = GST_ELEMENT_FACTORY (feature);
  if (!g_strrstr (gst_element_factory_get_klass (f), "Visualization"))
    return FALSE;

  return TRUE;
}

static GList *
get_visualization_features (void)
{
  return gst_registry_pool_feature_filter (
      (GstPluginFeatureFilter) filter_features, FALSE, NULL);
}

static void
add_longname (GstElementFactory *f, GList ** to)
{
  *to = g_list_append (*to, gst_element_factory_get_longname (f));
}

GList *
bacon_video_widget_get_visuals_list (BaconVideoWidget * bvw)
{
  GList *features, *names = NULL;

  g_return_val_if_fail (bvw != NULL, NULL);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), NULL);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), NULL);

  /* Cache */
  if (bvw->priv->vis_plugins_list) {
    return bvw->priv->vis_plugins_list;
  }

  features = get_visualization_features ();
  g_list_foreach (features, (GFunc) add_longname, &names);
  g_list_free (features);
  bvw->priv->vis_plugins_list = names;

  return names;
}

gboolean
bacon_video_widget_set_visuals (BaconVideoWidget * bvw, const char *name)
{
  GList *features, *item;
  GstElementFactory *fac = NULL;
  GstElement *old_vis = bvw->priv->vis_element;

  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  /* find */
  features = get_visualization_features ();
  for (item = features; item != NULL; item = item->next) {
    GstElementFactory *f = item->data;

    if (!strcmp (name, gst_element_factory_get_longname (f))) {
      fac = f;
      break;
    }
  }
  g_list_free (features);
  /* could be the user has an outdated preference, etc -- fail silently */
  if (!fac)
    return FALSE;

  /* now create */
  bvw->priv->vis_element = gst_element_factory_create (fac, "vis-plugin");
  if (!bvw->priv->vis_element) {
    bvw->priv->vis_element = old_vis;
    return FALSE;
  }

  gconf_client_set_string (bvw->priv->gc,
      GCONF_PREFIX"/visual", name, NULL);

  setup_vis (bvw);
  if (old_vis) {
    gst_object_unref (GST_OBJECT (old_vis));
  }

  return TRUE;
}

void
bacon_video_widget_set_visuals_quality (BaconVideoWidget * bvw,
					VisualsQuality quality)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  if (bvw->priv->visq == quality)
    return;
  bvw->priv->visq = quality;
  gconf_client_set_int (bvw->priv->gc,
      GCONF_PREFIX"/visual_quality", quality, NULL);

  if (bvw->priv->show_vfx && bvw->priv->vis_element) {
    change_visualization_quality (bvw);
  }
}

gboolean
bacon_video_widget_get_auto_resize (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);

  return bvw->priv->auto_resize;
}

void
bacon_video_widget_set_auto_resize (BaconVideoWidget * bvw,
				    gboolean auto_resize)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  bvw->priv->auto_resize = auto_resize;

  /* this will take effect when the next media file loads */
}

void
bacon_video_widget_set_aspect_ratio (BaconVideoWidget *bvw,
				BaconVideoWidgetAspectRatio ratio)
{
  gfloat pixel_ratio, factor = 1.0;

  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  bvw->priv->ratio_type = ratio;
  pixel_ratio = (gfloat) bvw->priv->video_width_pixels /
      bvw->priv->video_height_pixels;

  switch (ratio) {
    case BVW_RATIO_AUTO:
      factor = bvw->priv->movie_ratio;
      break;
    case BVW_RATIO_SQUARE:
      factor = 1.0;
      break;
    case BVW_RATIO_FOURBYTHREE:
      factor = 4.0 / 3.0;
      break;
    case BVW_RATIO_ANAMORPHIC:
      factor = 16.0 / 9.0;
      break;
    case BVW_RATIO_DVB:
      factor = 2.11;
      break;
  }

  factor /= pixel_ratio;
  bvw->priv->video_width = bvw->priv->video_width_pixels;
  bvw->priv->video_height = bvw->priv->video_height_pixels;
  if (factor > 1.0) {
    bvw->priv->video_width *= factor;
  } else {
    bvw->priv->video_height *= 1.0 / factor;
  }

  got_video_size (bvw);
}

BaconVideoWidgetAspectRatio
bacon_video_widget_get_aspect_ratio (BaconVideoWidget *bvw)
{
  g_return_val_if_fail (bvw != NULL, 0);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), 0);

  return bvw->priv->ratio_type;
}

void
bacon_video_widget_set_scale_ratio (BaconVideoWidget * bvw, gfloat ratio)
{
  gint w, h;

  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  get_media_size (bvw, &w, &h);
  if (ratio == 0) {
    if (totem_ratio_fits_screen (bvw->priv->video_window, w, h, 2.0))
      ratio = 2.0;
    else if (totem_ratio_fits_screen (bvw->priv->video_window, w, h, 1.0))
      ratio = 1.0;
    else if (totem_ratio_fits_screen (bvw->priv->video_window, w, h, 0.5))
      ratio = 0.5;
    else
      return;
  } else {
    if (!totem_ratio_fits_screen (bvw->priv->video_window, w, h, ratio))
      return;
  }
  w = (gfloat) w * ratio;
  h = (gfloat) h * ratio;
  shrink_toplevel (bvw);
  totem_widget_set_preferred_size (GTK_WIDGET (bvw), w, h);
}

gboolean
bacon_video_widget_can_set_zoom (BaconVideoWidget *bvw)
{
  return FALSE;
}

void
bacon_video_widget_set_zoom (BaconVideoWidget *bvw,
			     int               zoom)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));

  /* implement me */
}

int
bacon_video_widget_get_zoom (BaconVideoWidget *bvw)
{
  g_return_val_if_fail (bvw != NULL, 100);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), 100);

  return 100;
}

int
bacon_video_widget_get_video_property (BaconVideoWidget *bvw,
                                       BaconVideoWidgetVideoProperty type)
{
  int value = 65535 / 2;
  g_return_val_if_fail (bvw != NULL, value);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), value);
  
  if (bvw->priv->balance && GST_IS_COLOR_BALANCE (bvw->priv->balance))
    {
      const GList *channels_list = NULL;
      GstColorBalanceChannel *found_channel = NULL;
      
      channels_list = gst_color_balance_list_channels (bvw->priv->balance);
      
      while (channels_list != NULL && found_channel == NULL)
        { /* We search for the right channel corresponding to type */
          GstColorBalanceChannel *channel = channels_list->data;

          if (type == BVW_VIDEO_BRIGHTNESS && channel &&
              g_strrstr (channel->label, "BRIGHTNESS"))
            {
              g_object_ref (channel);
              found_channel = channel;
            }
          else if (type == BVW_VIDEO_CONTRAST && channel &&
              g_strrstr (channel->label, "CONTRAST"))
            {
              g_object_ref (channel);
              found_channel = channel;
            }
          else if (type == BVW_VIDEO_SATURATION && channel &&
              g_strrstr (channel->label, "SATURATION"))
            {
              g_object_ref (channel);
              found_channel = channel;
            }
          else if (type == BVW_VIDEO_HUE && channel &&
              g_strrstr (channel->label, "HUE"))
            {
              g_object_ref (channel);
              found_channel = channel;
            }
          channels_list = g_list_next (channels_list);
        }
        
      if (found_channel && GST_IS_COLOR_BALANCE_CHANNEL (found_channel))
        {
          value = gst_color_balance_get_value (bvw->priv->balance,
                                               found_channel);
          value = ((double) value - found_channel->min_value) * 65535 /
                  ((double) found_channel->max_value - found_channel->min_value);
          g_object_unref (found_channel);

	  return value;
        }
    }

  /* value wasn't found, get from gconf */
  return gconf_client_get_int (bvw->priv->gc, video_props_str[type], NULL);
}

void
bacon_video_widget_set_video_property (BaconVideoWidget *bvw,
                                       BaconVideoWidgetVideoProperty type,
                                       int value)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  
  if ( !(value < 65535 && value > 0) )
    return;

  if (bvw->priv->balance && GST_IS_COLOR_BALANCE (bvw->priv->balance))
    {
      const GList *channels_list = NULL;
      GstColorBalanceChannel *found_channel = NULL;
      
      channels_list = gst_color_balance_list_channels (bvw->priv->balance);

      while (found_channel == NULL && channels_list != NULL) {
          /* We search for the right channel corresponding to type */
          GstColorBalanceChannel *channel = channels_list->data;

          if (type == BVW_VIDEO_BRIGHTNESS && channel &&
              g_strrstr (channel->label, "BRIGHTNESS"))
            {
              g_object_ref (channel);
              found_channel = channel;
            }
          else if (type == BVW_VIDEO_CONTRAST && channel &&
              g_strrstr (channel->label, "CONTRAST"))
            {
              g_object_ref (channel);
              found_channel = channel;
            }
          else if (type == BVW_VIDEO_SATURATION && channel &&
              g_strrstr (channel->label, "SATURATION"))
            {
              g_object_ref (channel);
              found_channel = channel;
            }
          else if (type == BVW_VIDEO_HUE && channel &&
              g_strrstr (channel->label, "HUE"))
            {
              g_object_ref (channel);
              found_channel = channel;
            }
          channels_list = g_list_next (channels_list);
        }

      if (found_channel && GST_IS_COLOR_BALANCE_CHANNEL (found_channel))
        {
          int i_value = value * ((double) found_channel->max_value -
	      found_channel->min_value) / 65535 + found_channel->min_value;
          gst_color_balance_set_value (bvw->priv->balance, found_channel,
                                       i_value);
          g_object_unref (found_channel);
        }
    }

  /* save in gconf */
  gconf_client_set_int (bvw->priv->gc, video_props_str[type], value, NULL);
}

float
bacon_video_widget_get_position (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, -1);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), -1);
  return bvw->priv->current_position;
}

gint64
bacon_video_widget_get_current_time (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, -1);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), -1);
  return bvw->priv->current_time;
}

gint64
bacon_video_widget_get_stream_length (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, -1);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), -1);
  return bvw->priv->stream_length;
}

gboolean
bacon_video_widget_is_playing (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  if (GST_STATE (GST_ELEMENT (bvw->priv->play)) == GST_STATE_PLAYING)
    return TRUE;

  return FALSE;
}

gboolean
bacon_video_widget_is_seekable (BaconVideoWidget * bvw)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  /* hmm... */
  if (bvw->priv->stream_length)
    return TRUE;
  else
    return FALSE;
}

gboolean
bacon_video_widget_can_play (BaconVideoWidget * bvw, MediaType type)
{
  gboolean res;

  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  switch (type) {
    case MEDIA_TYPE_CDDA:
    case MEDIA_TYPE_VCD:
    case MEDIA_TYPE_DVD:
      res = TRUE;
      break;
    default:
      res = FALSE;
      break;
  }

  return res;
}

gchar **
bacon_video_widget_get_mrls (BaconVideoWidget * bvw, MediaType type)
{
  gchar **mrls;

  g_return_val_if_fail (bvw != NULL, NULL);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), NULL);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), NULL);

  switch (type) {
    case MEDIA_TYPE_CDDA: {
#if 0
      GstElement *cdda;
      gint64 tracks;
      GstFormat fmt;

      cdda = gst_element_make_from_uri (GST_URI_SRC, "cdda://", NULL);
      if (!cdda)
       return NULL;
      fmt = gst_format_get_by_nick ("track");
      if (!fmt) {
	gst_object_unref (GST_OBJECT (cdda));
        return NULL;
      }
      if (gst_element_set_state (cdda, GST_STATE_PAUSED) !=
              GST_STATE_SUCCESS) {
	gst_object_unref (GST_OBJECT (cdda));
	return NULL;
      }
      if (!gst_pad_query (gst_element_get_pad (cdda, "src"),
              GST_QUERY_TOTAL, &fmt, &tracks)) {
	gst_element_set_state (cdda, GST_STATE_NULL);
	gst_object_unref (GST_OBJECT (cdda));
        return NULL;
      }
      gst_element_set_state (cdda, GST_STATE_NULL);
      gst_object_unref (GST_OBJECT (cdda));
      mrls = g_new0 (gchar *, tracks + 1);
      while (tracks-- > 0) {
	mrls[tracks] = g_strdup_printf ("cdda://%d", (gint) tracks + 1);
      }
#endif
      gchar *uri[] = { "cdda://", NULL };
      mrls = g_strdupv (uri);
      break;
    }
    case MEDIA_TYPE_VCD: {
      gchar *uri[] = { "vcd://", NULL };
      mrls = g_strdupv (uri);
      break;
    }
    case MEDIA_TYPE_DVD: {
      gchar *uri[] = { "dvd://", NULL };
      mrls = g_strdupv (uri);
      break;
    }
    default:
      mrls = NULL;
      break;
  }

  return mrls;
}

static void
bacon_video_widget_get_metadata_string (BaconVideoWidget * bvw,
					BaconVideoWidgetMetadataType type,
					GValue * value)
{
  char *string = NULL;
  gboolean res = FALSE;

  g_value_init (value, G_TYPE_STRING);

  if (bvw->priv->play == NULL || bvw->priv->tagcache == NULL)
    {
      g_value_set_string (value, NULL);
      return;
    }

  switch (type)
    {
    case BVW_INFO_TITLE:
      res = gst_tag_list_get_string_index (bvw->priv->tagcache,
					   GST_TAG_TITLE, 0, &string);
      break;
    case BVW_INFO_ARTIST:
      res = gst_tag_list_get_string_index (bvw->priv->tagcache,
					   GST_TAG_ARTIST, 0, &string);
      break;
    case BVW_INFO_YEAR: {
      guint julian;
      if ((res = gst_tag_list_get_uint (bvw->priv->tagcache,
					GST_TAG_DATE, &julian))) {
	GDate *d = g_date_new_julian (julian);
	string = g_strdup_printf ("%d", g_date_get_year (d));
      }
      break;
    }
    case BVW_INFO_ALBUM:
      res = gst_tag_list_get_string_index (bvw->priv->tagcache,
					   GST_TAG_ALBUM, 0, &string);
      break;
    case BVW_INFO_VIDEO_CODEC:
      res = gst_tag_list_get_string (bvw->priv->tagcache,
				     GST_TAG_VIDEO_CODEC, &string);
      break;
    case BVW_INFO_AUDIO_CODEC:
      res = gst_tag_list_get_string (bvw->priv->tagcache,
				     GST_TAG_AUDIO_CODEC, &string);
      break;
    case BVW_INFO_CDINDEX:
      res = gst_tag_list_get_string (bvw->priv->tagcache,
				     "musicbrainz-discid", &string);
      break;
    default:
      g_assert_not_reached ();
    }

  if (res)
    g_value_take_string (value, string);
  else
    g_value_set_string (value, NULL);

  return;
}

static void
bacon_video_widget_get_metadata_int (BaconVideoWidget * bvw,
				     BaconVideoWidgetMetadataType type,
				     GValue * value)
{
  int integer = 0;

  g_value_init (value, G_TYPE_INT);

  if (bvw->priv->play == NULL)
    {
      g_value_set_int (value, 0);
      return;
    }

  switch (type)
    {
    case BVW_INFO_DURATION:
      integer = bacon_video_widget_get_stream_length (bvw) / 1000;
      break;
    case BVW_INFO_DIMENSION_X:
      integer = bvw->priv->video_width;
      break;
    case BVW_INFO_DIMENSION_Y:
      integer = bvw->priv->video_height;
      break;
    case BVW_INFO_FPS:
      if (bvw->priv->video_fps - (int)bvw->priv->video_fps >= 0.5)
        integer = bvw->priv->video_fps + 1;
      else
        integer = bvw->priv->video_fps;
      break;
    case BVW_INFO_AUDIO_BITRATE:
      if (bvw->priv->audiotags == NULL)
        break;
      if (gst_tag_list_get_uint (bvw->priv->audiotags,
				 GST_TAG_BITRATE, (guint *)&integer)) {
	integer /= 1000;
      }
      break;
    case BVW_INFO_VIDEO_BITRATE:
      if (bvw->priv->videotags == NULL)
	break;
      if (gst_tag_list_get_uint (bvw->priv->videotags,
				 GST_TAG_BITRATE, (guint *)&integer)) {
	integer /= 1000;
      }
      break;
    default:
      g_assert_not_reached ();
    }

  g_value_set_int (value, integer);

  return;
}

static void
bacon_video_widget_get_metadata_bool (BaconVideoWidget * bvw,
				      BaconVideoWidgetMetadataType type,
				      GValue * value)
{
  gboolean boolean = FALSE;

  g_value_init (value, G_TYPE_BOOLEAN);

  if (bvw->priv->play == NULL)
    {
      g_value_set_boolean (value, FALSE);
      return;
    }

  switch (type)
    {
    case BVW_INFO_HAS_VIDEO:
      boolean = bvw->priv->media_has_video;
      break;
    case BVW_INFO_HAS_AUDIO:
      boolean = bvw->priv->media_has_audio;
      break;
    default:
      g_assert_not_reached ();
    }

  g_value_set_boolean (value, boolean);

  return;
}

void
bacon_video_widget_get_metadata (BaconVideoWidget * bvw,
				 BaconVideoWidgetMetadataType type,
				 GValue * value)
{
  g_return_if_fail (bvw != NULL);
  g_return_if_fail (BACON_IS_VIDEO_WIDGET (bvw));
  g_return_if_fail (GST_IS_ELEMENT (bvw->priv->play));

  switch (type)
    {
    case BVW_INFO_TITLE:
    case BVW_INFO_ARTIST:
    case BVW_INFO_YEAR:
    case BVW_INFO_ALBUM:
    case BVW_INFO_VIDEO_CODEC:
    case BVW_INFO_AUDIO_CODEC:
    case BVW_INFO_CDINDEX:
      bacon_video_widget_get_metadata_string (bvw, type, value);
      break;
    case BVW_INFO_DURATION:
    case BVW_INFO_DIMENSION_X:
    case BVW_INFO_DIMENSION_Y:
    case BVW_INFO_FPS:
    case BVW_INFO_AUDIO_BITRATE:
    case BVW_INFO_VIDEO_BITRATE:
      bacon_video_widget_get_metadata_int (bvw, type, value);
      break;
    case BVW_INFO_HAS_VIDEO:
    case BVW_INFO_HAS_AUDIO:
      bacon_video_widget_get_metadata_bool (bvw, type, value);
      break;
    default:
      g_assert_not_reached ();
    }

  return;
}

/* Screenshot functions */
gboolean
bacon_video_widget_can_get_frames (BaconVideoWidget * bvw, GError ** error)
{
  g_return_val_if_fail (bvw != NULL, FALSE);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), FALSE);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), FALSE);

  /* check for version */
  if (!g_object_class_find_property (
           G_OBJECT_GET_CLASS (bvw->priv->play), "frame")) {
    g_set_error (error, BVW_ERROR, BVW_ERROR_GENERIC,
        _("Too old version of GStreamer installed."));
    return FALSE;
  }

  /* check for video */
  if (!bvw->priv->media_has_video) {
    g_set_error (error, BVW_ERROR, BVW_ERROR_GENERIC,
        _("Media contains no supported video streams."));
  }

  return bvw->priv->media_has_video;
}

static void
destroy_pixbuf (guchar *pix, gpointer data)
{
  gst_buffer_unref (GST_BUFFER (data));
}

GdkPixbuf *
bacon_video_widget_get_current_frame (BaconVideoWidget * bvw)
{
  GstBuffer *buf = NULL;
  GList *streaminfo = NULL;
  GstCaps *from = NULL;
  GdkPixbuf *pixbuf;

  g_return_val_if_fail (bvw != NULL, NULL);
  g_return_val_if_fail (BACON_IS_VIDEO_WIDGET (bvw), NULL);
  g_return_val_if_fail (GST_IS_ELEMENT (bvw->priv->play), NULL);

  /* no video info */
  if (!bvw->priv->video_width || !bvw->priv->video_height)
    return NULL;

  /* get frame */
  g_object_get (G_OBJECT (bvw->priv->play), "frame", &buf, NULL);
  if (!buf)
    return NULL;
  gst_buffer_ref (buf);

  /* get video size etc. */
  g_object_get (G_OBJECT (bvw->priv->play),
      "stream-info", &streaminfo, NULL);
  streaminfo = g_list_copy (streaminfo);
  g_list_foreach (streaminfo, (GFunc) g_object_ref, NULL);
  for (; streaminfo != NULL; streaminfo = streaminfo->next) {
    GObject *info = streaminfo->data;
    gint type;
    GParamSpec *pspec;
    GEnumValue *val;

    if (!info)
      continue;
    g_object_get (info, "type", &type, NULL);
    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (info), "type");
    val = g_enum_get_value (G_PARAM_SPEC_ENUM (pspec)->enum_class, type);

    if (strstr (val->value_name, "VIDEO")) {
      GstPad *pad = NULL;

      g_object_get (info, "object", &pad, NULL);
      g_assert (GST_IS_PAD (pad));
      from = gst_pad_get_negotiated_caps (pad);
      break;
    }
  }
  g_list_foreach (streaminfo, (GFunc) g_object_unref, NULL);
  g_list_free (streaminfo);
  if (!from)
    return NULL;

  /* convert to our own wanted format */
  buf = bvw_frame_conv_convert (buf, from,
      gst_caps_new_simple ("video/x-raw-rgb",
          "bpp", G_TYPE_INT, 24,
          "depth", G_TYPE_INT, 24,
          "width", G_TYPE_INT, bvw->priv->video_width,
          "height", G_TYPE_INT, bvw->priv->video_height,
          "framerate", G_TYPE_DOUBLE, bvw->priv->video_fps,
          "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          "endianness", G_TYPE_INT, G_BIG_ENDIAN,
          "red_mask", G_TYPE_INT, 0xff0000,
          "green_mask", G_TYPE_INT, 0x00ff00,
          "blue_mask", G_TYPE_INT, 0x0000ff, NULL));
  if (!buf)
    return NULL;

  /* create pixbuf from that - use our own destroy function */
  pixbuf = gdk_pixbuf_new_from_data (GST_BUFFER_DATA (buf),
			GDK_COLORSPACE_RGB, FALSE,
			8, bvw->priv->video_width,
			bvw->priv->video_height,
			(3 * bvw->priv->video_width + 3) &~ 3,
			destroy_pixbuf, buf);
  if (!pixbuf)
    gst_buffer_unref (buf);

  /* that's all */
  return pixbuf;
}

static void
cb_gconf (GConfClient * client,
	  guint connection_id,
	  GConfEntry * entry,
	  gpointer data)
{
  BaconVideoWidget *bvw = data;

  if (!strcmp (entry->key, "/apps/totem/network-buffer-threshold")) {
    g_object_set (G_OBJECT (bvw->priv->play), "queue-threshold",
        (guint64) GST_SECOND * gconf_value_get_float (entry->value), NULL);
  } else if (!strcmp (entry->key, "/apps/totem/buffer-size")) {
    g_object_set (G_OBJECT (bvw->priv->play), "queue-threshold",
        (guint64) GST_SECOND * gconf_value_get_float (entry->value), NULL);
  }
}

/* =========================================== */
/*                                             */
/*          Widget typing & Creation           */
/*                                             */
/* =========================================== */

G_DEFINE_TYPE(BaconVideoWidget, bacon_video_widget, GTK_TYPE_BOX)

struct poptOption *
bacon_video_widget_get_popt_table (void)
{
  /* Initializing GStreamer backend and parse our options from the command 
     line options */
  return (struct poptOption *) gst_init_get_popt_table ();
}

void
bacon_video_widget_init_backend (int *argc, char ***argv)
{
  gst_init (argc, argv);
}

GQuark
bacon_video_widget_error_quark (void)
{
  static GQuark q = 0;
  if (q == 0) {
    q = g_quark_from_static_string ("bvw-error-quark");
  }
  return q;
}

/* fold function to pick the best colorspace element */
static gboolean
find_colorbalance_element (GstElement *element, GValue * ret, GstElement **cb)
{
  GstColorBalanceClass *cb_class;

  if (!GST_IS_COLOR_BALANCE (element))
    return TRUE;
  
  cb_class = GST_COLOR_BALANCE_GET_CLASS (element);
  if (GST_COLOR_BALANCE_TYPE (cb_class) == GST_COLOR_BALANCE_HARDWARE) {
    gst_object_replace ((GstObject **) cb, (GstObject *) element);
    /* shortcuts the fold */
    return FALSE;
  } else if (*cb == NULL) {
    gst_object_replace ((GstObject **) cb, (GstObject *) element);
    return TRUE;
  } else {
    return TRUE;
  }
}

GtkWidget *
bacon_video_widget_new (int width, int height,
			BvwUseType type, GError ** err)
{
  GConfValue *confvalue;
  BaconVideoWidget *bvw;
  GstElement *audio_sink = NULL, *video_sink = NULL;
  GstBus *bus;
#if 0
  gulong sig1, sig2;
#endif
  gint i;
  GError *local_err = NULL;

  bvw = BACON_VIDEO_WIDGET (g_object_new
                            (bacon_video_widget_get_type (), NULL));
  
  bvw->priv->play = gst_element_factory_make ("playbin", "play");
  if (!bvw->priv->play) {
    g_set_error (err, BVW_ERROR, BVW_ERROR_PLUGIN_LOAD,
		 _("Failed to create a GStreamer play object. "
		   "Please check your GStreamer installation."));
    g_object_unref (G_OBJECT (bvw));
    return NULL;
  }

  bus = gst_pipeline_get_bus (GST_PIPELINE (bvw->priv->play));
  gst_bus_add_watch (bus, (GstBusHandler) bacon_video_widget_bus_callback, bvw);
  gst_object_unref (bus);

  /* bla die bla ... */
  bvw->priv->vis_capsfilter = gst_element_factory_make ("capsfilter",
							"vis-capsfilter");
  
  /* create GStreamer playback object, do some initial signal
   * connections and such. */
  bvw->priv->speakersetup = BVW_AUDIO_SOUND_STEREO;
  bvw->priv->media_device = g_strdup ("/dev/dvd");
  bvw->priv->init_width = 240;
  bvw->priv->init_height = 180;
  bvw->priv->visq = VISUAL_SMALL;
  bvw->priv->show_vfx = FALSE;
  bvw->priv->vis_plugins_list = NULL;
  bvw->priv->vis_element = NULL;
  bvw->priv->tv_out_type = TV_OUT_NONE;
  bvw->priv->connection_speed = 0;
  bvw->priv->ratio_type = BVW_RATIO_AUTO;

  bvw->priv->cursor_shown = TRUE;
  bvw->priv->logo_mode = TRUE;
  bvw->priv->auto_resize = TRUE;

  /* gconf setting in backend */
  bvw->priv->gc = gconf_client_get_default ();
  gconf_client_notify_add (bvw->priv->gc, "/apps/totem",
      cb_gconf, bvw, NULL, NULL);

  /* setup outputs */
  if (type == BVW_USE_TYPE_VIDEO || type == BVW_USE_TYPE_AUDIO) {
    GstPad *pad;

    audio_sink = gst_element_factory_make ("gconfaudiosink", "audio-sink");
    if (!GST_IS_ELEMENT (audio_sink)) {
      g_set_error (err, 0, 0,
		   _("Failed to retrieve a video output - please run gstreamer-properties"));
      g_object_unref (G_OBJECT (bvw));
      return NULL;
    }
    bvw->priv->audiosink = audio_sink;
    bvw->priv->audiocapsfilter =
        gst_element_factory_make ("capsfilter", "audiofilter");
    bvw->priv->audioconvert =
        gst_element_factory_make ("audioconvert", "audioconvert");
    audio_sink = gst_bin_new ("audiosinkbin");
    gst_bin_add_many (GST_BIN (audio_sink),
	bvw->priv->audioconvert, bvw->priv->audiocapsfilter,
	bvw->priv->audiosink, NULL);
    gst_element_link_pads (bvw->priv->audioconvert, "src",
	bvw->priv->audiocapsfilter, "sink");
    gst_element_link_pads (bvw->priv->audiocapsfilter, "src",
	bvw->priv->audiosink, "sink");

    pad = gst_element_get_pad (bvw->priv->audioconvert, "sink");
    gst_element_add_pad (audio_sink, gst_ghost_pad_new ("sink", pad));
    gst_object_unref (pad);
  }

  if (type == BVW_USE_TYPE_VIDEO) {
    video_sink = gst_element_factory_make ("gconfvideosink", "video-sink");
    if (!GST_IS_ELEMENT (video_sink)) {
      g_set_error (err, 0, 0,
		   _("Failed to retrieve a audio output - please run gstreamer-properties"));
      g_object_unref (G_OBJECT (bvw));
      if (audio_sink)
	gst_object_unref (GST_OBJECT (audio_sink));
      return NULL;
    } else {
      GDate d;

      g_date_clear (&d, 1);
      g_date_set_time (&d, time (NULL));
      if (g_date_day (&d) == 1 && g_date_month (&d) == G_DATE_APRIL) {
        confvalue = gconf_client_get_without_default (bvw->priv->gc,
            GCONF_PREFIX"/puzzle_year", NULL);

        if (!confvalue ||
            gconf_value_get_int (confvalue) != g_date_year (&d)) {
          GstElement *puzzle;

          gconf_client_set_int (bvw->priv->gc, GCONF_PREFIX"/puzzle_year",
              g_date_year (&d), NULL);

          puzzle = gst_element_factory_make ("puzzle", NULL);
          if (puzzle) {
            GstElement *bin = gst_bin_new ("videosinkbin");
	    GstPad *pad;

            gst_bin_add_many (GST_BIN (bin), puzzle, video_sink, NULL);
            gst_element_link (puzzle, video_sink);
	    pad = gst_element_get_pad (puzzle, "sink");
	    gst_element_add_pad (bin, gst_ghost_pad_new ("sink", pad));
	    gst_object_unref (GST_OBJECT (pad));
            video_sink = bin;
          }
        }

        if (confvalue)
          gconf_value_free (confvalue);
      }
    }
  }

  if (video_sink == NULL) {
    video_sink = gst_element_factory_make ("fakesink", "fakevideosink");
  }
  if (audio_sink == NULL) {
    audio_sink = gst_element_factory_make ("fakesink", "fakeaudiosink");
  }

  /* check video */
#if 0
  sig1 = g_signal_connect (video_sink, "error", G_CALLBACK (out_error),
      			   &local_err);
#endif
  if (gst_element_set_state (video_sink,
			     GST_STATE_READY) != GST_STATE_SUCCESS) {
    /* Videosink not available; give user feedback on what to do now */
    if (err) {
      if (local_err && local_err->domain == GST_RESOURCE_ERROR) {
	switch (local_err->code) {
	  case GST_RESOURCE_ERROR_NOT_FOUND:
	  case GST_RESOURCE_ERROR_OPEN_WRITE:
	    /* can use the default below... */
	    break;
	  case GST_RESOURCE_ERROR_BUSY:
	    g_set_error (err, BVW_ERROR, BVW_ERROR_VIDEO_PLUGIN,
		_("The video output is in use by another application. "
		  "Please close other video applications, or select another video output in the Multimedia Systems Selector."));
	    break;
	  default:
	    break;
	}
      }
      if (!*err) {
	/* "default" */
	g_set_error (err, BVW_ERROR, BVW_ERROR_VIDEO_PLUGIN,
	    _("Failed to open video output. It may not be available. "
	      "Please select another video output in the Multimedia Systems Selector."));
      }
      if (local_err)
	g_error_free (local_err);
    }
    gst_object_unref (video_sink);
    gst_object_unref (audio_sink);
    g_object_unref (G_OBJECT (bvw));
    return NULL;
  }
#if 0
  g_signal_handler_disconnect (video_sink, sig1);
#endif

  /* check audio */
#if 0
  sig2 = g_signal_connect (audio_sink, "error", G_CALLBACK (out_error),
			   &local_err);
#endif
  if (gst_element_set_state (audio_sink,
			     GST_STATE_READY) != GST_STATE_SUCCESS) {
    if (err) {
      if (local_err && local_err->domain == GST_RESOURCE_ERROR) {
	switch (local_err->code) {
	  case GST_RESOURCE_ERROR_OPEN_WRITE:
	  case GST_RESOURCE_ERROR_OPEN_READ_WRITE:
	    /* use default below */
	    break;
	  case GST_RESOURCE_ERROR_BUSY:
	    g_set_error (err, BVW_ERROR, BVW_ERROR_AUDIO_BUSY,
		_("The audio output is in use by another application. "
		  "Please select another audio output in the Multimedia Systems Selector. "
		  "You may want to consider using a sound server."));
	    break;
	  case GST_RESOURCE_ERROR_NOT_FOUND:
	    g_set_error (err, BVW_ERROR, BVW_ERROR_AUDIO_PLUGIN,
		_("The requested audio output was not found. "
		  "Please select another audio output in the Multimedia Systems Selector"));
	    break;
	  default:
	    break;
	}
      }
      if (!*err) {
	/* "default" */
        g_set_error (err, BVW_ERROR, BVW_ERROR_AUDIO_PLUGIN,
	    _("Failed to open audio output. You may not have permission to open the sound device, or the sound server may not be running. "
	      "Please select another audio output in the Multimedia Systems Selector."));
      }
      if (local_err)
	g_error_free (local_err);
    }
    gst_object_unref (video_sink);
    gst_object_unref (audio_sink);
    g_object_unref (G_OBJECT (bvw));
    return NULL;
  }
#if 0
  g_signal_handler_disconnect (audio_sink, sig2);
#endif

  /* now tell playbin */
  g_object_set (G_OBJECT (bvw->priv->play), "video-sink",
		video_sink, NULL);
  g_object_set (G_OBJECT (bvw->priv->play), "audio-sink",
		audio_sink, NULL);

  g_signal_connect (G_OBJECT (bvw->priv->play), "notify::source",
		    G_CALLBACK (got_source), (gpointer) bvw);
  g_signal_connect (G_OBJECT (bvw->priv->play), "notify::stream-info",
		    G_CALLBACK (stream_info_set), (gpointer) bvw);

  if (type == BVW_USE_TYPE_VIDEO) {
    /* Try to get an element supporting the XOverlay interface */
    if (GST_IS_BIN (video_sink)) {
      GstElement *element;

      element = gst_bin_get_by_interface (GST_BIN (video_sink),
					  GST_TYPE_X_OVERLAY);
      if (element)
        bvw->priv->xoverlay = GST_X_OVERLAY (element);
    } else {
      if (GST_IS_X_OVERLAY (video_sink))
        bvw->priv->xoverlay = GST_X_OVERLAY (video_sink);
    }
    
    /* Get a colorbalance-supporting element too, preferring hardware support */
    {
      GstIterator *iter;
      GstElement *element = NULL;

      iter = gst_bin_iterate_all_by_interface (GST_BIN (bvw->priv->play),
                                               GST_TYPE_COLOR_BALANCE);
      /* naively assume no resync */
      gst_iterator_fold (iter,
          (GstIteratorFoldFunction) find_colorbalance_element, NULL, &element);
      gst_iterator_free (iter);

      if (element)
        bvw->priv->balance = GST_COLOR_BALANCE (element);
    }
  }

  /* Setup brightness and contrast */
  for (i = 0; i < 4; i++) {
    confvalue = gconf_client_get_without_default (bvw->priv->gc,
        video_props_str[i], NULL);
    if (confvalue != NULL) {
      bacon_video_widget_set_video_property (bvw, i,
        gconf_value_get_int (confvalue));
      gconf_value_free (confvalue);
    }
  }

  /* audio out, if any */
  confvalue = gconf_client_get_without_default (bvw->priv->gc,
      GCONF_PREFIX"/audio_output_type", NULL);
  if (confvalue != NULL &&
      (type != BVW_USE_TYPE_METADATA && type != BVW_USE_TYPE_CAPTURE)) {
    bvw->priv->speakersetup = gconf_value_get_int (confvalue) - 1;
    bacon_video_widget_set_audio_out_type (bvw, bvw->priv->speakersetup + 1);
    gconf_value_free (confvalue);
  } else {
    bvw->priv->speakersetup = -1;
    bacon_video_widget_set_audio_out_type (bvw, BVW_AUDIO_SOUND_STEREO);
  }

  /* visualization */
  confvalue = gconf_client_get_without_default (bvw->priv->gc,
      GCONF_PREFIX"/show_vfx", NULL);
  if (confvalue != NULL) {
    bvw->priv->show_vfx = gconf_value_get_bool (confvalue);
    gconf_value_free (confvalue);
  }
  confvalue = gconf_client_get_without_default (bvw->priv->gc,
      GCONF_PREFIX"/visual_quality", NULL);
  if (confvalue != NULL) {
    bvw->priv->visq = gconf_value_get_int (confvalue);
    gconf_value_free (confvalue);
  }
#if 0
  confvalue = gconf_client_get_without_default (bvw->priv->gc,
      GCONF_PREFIX"/visual", NULL);
  if (confvalue != NULL) {
    bvw->priv->vis_element = 
        gst_element_factory_make (gconf_value_get_string (confvalue), NULL);
    gconf_value_free (confvalue);
  }
  setup_vis (bvw);
#endif

  /* tv/conn (not used yet) */
  confvalue = gconf_client_get_without_default (bvw->priv->gc,
      GCONF_PREFIX"/tv_out_type", NULL);
  if (confvalue != NULL) {
    bvw->priv->tv_out_type = gconf_value_get_int (confvalue);
    gconf_value_free (confvalue);
  }
  confvalue = gconf_client_get_without_default (bvw->priv->gc,
      GCONF_PREFIX"/connection_speed", NULL);
  if (confvalue != NULL) {
    bvw->priv->connection_speed = gconf_value_get_int (confvalue);
    gconf_value_free (confvalue);
  }

  /* those are private to us, i.e. not Xine-compatible */
  confvalue = gconf_client_get_without_default (bvw->priv->gc,
      GCONF_PREFIX"/buffer-size", NULL);
  if (confvalue != NULL) {
    g_object_set (G_OBJECT (bvw->priv->play), "queue-size",
        (guint64) GST_SECOND * gconf_value_get_float (confvalue), NULL);
    gconf_value_free (confvalue);
  }
  confvalue = gconf_client_get_without_default (bvw->priv->gc,
      GCONF_PREFIX"/network-buffer-threshold", NULL);
  if (confvalue != NULL) {
    g_object_set (G_OBJECT (bvw->priv->play), "queue-threshold",
        (guint64) GST_SECOND * gconf_value_get_float (confvalue), NULL);
    gconf_value_free (confvalue);
  }

  return GTK_WIDGET (bvw);
}

/*
 * vim: sw=2 ts=8 cindent noai bs=2
 */
