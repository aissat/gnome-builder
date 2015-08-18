/* ide-box-theatric.c
 *
 * Copyright (C) 2014 Christian Hergert <christian@hergert.me>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "theatrics"

#include <glib/gi18n.h>

#include "egg-animation.h"
#include "ide-box-theatric.h"
#include "ide-cairo.h"

struct _IdeBoxTheatric
{
  GObject      parent_instance;

  GtkWidget   *target;
  GtkWidget   *toplevel;
  GdkRectangle area;
  GdkRectangle last_area;
  GdkRGBA      background_rgba;

  guint        draw_handler;
};

enum {
  PROP_0,
  PROP_ALPHA,
  PROP_BACKGROUND,
  PROP_HEIGHT,
  PROP_TARGET,
  PROP_WIDTH,
  PROP_X,
  PROP_Y,
  LAST_PROP
};

G_DEFINE_TYPE (IdeBoxTheatric, ide_box_theatric, G_TYPE_OBJECT)

static GParamSpec *gParamSpecs[LAST_PROP];

static void
get_toplevel_rect (IdeBoxTheatric *theatric,
                   GdkRectangle  *area)
{
  gtk_widget_translate_coordinates (theatric->target, theatric->toplevel,
                                    theatric->area.x, theatric->area.y,
                                    &area->x, &area->y);

  area->width = theatric->area.width;
  area->height = theatric->area.height;
}

static gboolean
on_toplevel_draw (GtkWidget     *widget,
                  cairo_t       *cr,
                  IdeBoxTheatric *theatric)
{
  GdkRectangle area;

  get_toplevel_rect (theatric, &area);

#if 0
  g_print ("Drawing on %s at %d,%d %dx%d\n",
           g_type_name (G_TYPE_FROM_INSTANCE (widget)),
           area.x, area.y, area.width, area.height);
#endif

  ide_cairo_rounded_rectangle (cr, &area, 3, 3);
  gdk_cairo_set_source_rgba (cr, &theatric->background_rgba);
  cairo_fill (cr);

  theatric->last_area = area;

  return FALSE;
}

static void
ide_box_theatric_notify (GObject    *object,
                        GParamSpec *pspec)
{
  IdeBoxTheatric *self = IDE_BOX_THEATRIC (object);

  if (G_OBJECT_CLASS (ide_box_theatric_parent_class)->notify)
    G_OBJECT_CLASS (ide_box_theatric_parent_class)->notify (object, pspec);

  if (self->target && self->toplevel)
    {
      GdkWindow *window;
      GdkRectangle area;

      get_toplevel_rect (IDE_BOX_THEATRIC (object), &area);

#if 0
      g_print ("  Invalidate : %d,%d %dx%d\n",
               area.x, area.y, area.width, area.height);
#endif

      window = gtk_widget_get_window (self->toplevel);

      if (window != NULL)
        {
          gdk_window_invalidate_rect (window, &self->last_area, TRUE);
          gdk_window_invalidate_rect (window, &area, TRUE);
        }
    }
}

static void
ide_box_theatric_dispose (GObject *object)
{
  IdeBoxTheatric *self =  IDE_BOX_THEATRIC (object);

  if (self->target)
    {
      if (self->draw_handler && self->toplevel)
        {
          g_signal_handler_disconnect (self->toplevel, self->draw_handler);
          self->draw_handler = 0;
        }
      g_object_remove_weak_pointer (G_OBJECT (self->target),
                                    (gpointer *) &self->target);
      self->target = NULL;
    }

  G_OBJECT_CLASS (ide_box_theatric_parent_class)->dispose (object);
}

static void
ide_box_theatric_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  IdeBoxTheatric *theatric = IDE_BOX_THEATRIC (object);

  switch (prop_id)
    {
    case PROP_ALPHA:
      g_value_set_double (value, theatric->background_rgba.alpha);
      break;

    case PROP_BACKGROUND:
      g_value_take_string (value,
                           gdk_rgba_to_string (&theatric->background_rgba));
      break;

    case PROP_HEIGHT:
      g_value_set_int (value, theatric->area.height);
      break;

    case PROP_TARGET:
      g_value_set_object (value, theatric->target);
      break;

    case PROP_WIDTH:
      g_value_set_int (value, theatric->area.width);
      break;

    case PROP_X:
      g_value_set_int (value, theatric->area.x);
      break;

    case PROP_Y:
      g_value_set_int (value, theatric->area.y);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ide_box_theatric_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  IdeBoxTheatric *theatric = IDE_BOX_THEATRIC (object);

  switch (prop_id)
    {
    case PROP_ALPHA:
      theatric->background_rgba.alpha = g_value_get_double (value);
      break;

    case PROP_BACKGROUND:
      {
        gdouble old_alpha = theatric->background_rgba.alpha;

        gdk_rgba_parse (&theatric->background_rgba,
                        g_value_get_string (value));
        theatric->background_rgba.alpha = old_alpha;
      }
      break;

    case PROP_HEIGHT:
      theatric->area.height = g_value_get_int (value);
      break;

    case PROP_TARGET:
      theatric->target = g_value_get_object (value);
      theatric->toplevel = gtk_widget_get_toplevel (theatric->target);
      g_object_add_weak_pointer (G_OBJECT (theatric->target),
                                 (gpointer *) &theatric->target);
      theatric->draw_handler =
        g_signal_connect_after (theatric->toplevel,
                                "draw",
                                G_CALLBACK (on_toplevel_draw),
                                theatric);
      break;

    case PROP_WIDTH:
      theatric->area.width = g_value_get_int (value);
      break;

    case PROP_X:
      theatric->area.x = g_value_get_int (value);
      break;

    case PROP_Y:
      theatric->area.y = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }

  g_object_notify_by_pspec (object, pspec);
}

static void
ide_box_theatric_class_init (IdeBoxTheatricClass *klass)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = ide_box_theatric_dispose;
  object_class->notify = ide_box_theatric_notify;
  object_class->get_property = ide_box_theatric_get_property;
  object_class->set_property = ide_box_theatric_set_property;

  gParamSpecs[PROP_ALPHA] =
    g_param_spec_double ("alpha",
                         _("Alpha"),
                         _("Alpha"),
                         0.0,
                         1.0,
                         1.0,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gParamSpecs[PROP_BACKGROUND] =
    g_param_spec_string ("background",
                         _("background"),
                         _("background"),
                         "#000000",
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gParamSpecs[PROP_HEIGHT] =
    g_param_spec_int ("height",
                      _("height"),
                      _("height"),
                      0,
                      G_MAXINT,
                      0,
                      (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gParamSpecs[PROP_TARGET] =
    g_param_spec_object ("target",
                         _("Target"),
                         _("Target"),
                         GTK_TYPE_WIDGET,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  gParamSpecs[PROP_WIDTH] =
    g_param_spec_int ("width",
                      _("width"),
                      _("width"),
                      0,
                      G_MAXINT,
                      0,
                      (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gParamSpecs[PROP_X] =
    g_param_spec_int ("x",
                      _("x"),
                      _("x"),
                      G_MININT,
                      G_MAXINT,
                      0,
                      (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gParamSpecs[PROP_Y] =
    g_param_spec_int ("y",
                      _("y"),
                      _("y"),
                      G_MININT,
                      G_MAXINT,
                      0,
                      (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, gParamSpecs);
}

static void
ide_box_theatric_init (IdeBoxTheatric *theatric)
{
}
