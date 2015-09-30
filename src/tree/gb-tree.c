/* gb-tree.c
 *
 * Copyright (C) 2011 Christian Hergert <chris@dronelabs.com>
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

#define G_LOG_DOMAIN "tree"

#include <glib/gi18n.h>
#include <ide.h>

#include "gb-tree.h"
#include "gb-tree-node.h"
#include "gb-tree-private.h"
#include "gb-widget.h"

typedef struct
{
  GPtrArray         *builders;
  GbTreeNode        *root;
  GbTreeNode        *selection;
  GtkTreeViewColumn *column;
  GtkCellRenderer   *cell_pixbuf;
  GtkCellRenderer   *cell_text;
  GtkTreeStore      *store;
  GdkRGBA            dim_foreground;
  guint              show_icons : 1;
} GbTreePrivate;

typedef struct
{
  gpointer    key;
  GEqualFunc  equal_func;
  GbTreeNode *result;
} NodeLookup;

typedef struct
{
  GbTree           *self;
  GbTreeFilterFunc  filter_func;
  gpointer          filter_data;
  GDestroyNotify    filter_data_destroy;
} FilterFunc;

static void gb_tree_buildable_init (GtkBuildableIface *iface);

G_DEFINE_TYPE_WITH_CODE (GbTree, gb_tree, GTK_TYPE_TREE_VIEW,
                         G_ADD_PRIVATE (GbTree)
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_BUILDABLE, gb_tree_buildable_init))

enum {
  PROP_0,
  PROP_ROOT,
  PROP_SELECTION,
  PROP_SHOW_ICONS,
  LAST_PROP
};

enum {
  ACTION,
  POPULATE_POPUP,
  LAST_SIGNAL
};

static GtkBuildableIface *gb_tree_parent_buildable_iface;
static GParamSpec *gParamSpecs [LAST_PROP];
static guint gSignals [LAST_SIGNAL];

void
_gb_tree_build_node (GbTree     *self,
                     GbTreeNode *node)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  gsize i;

  g_assert (GB_IS_TREE (self));
  g_assert (GB_IS_TREE_NODE (node));

  _gb_tree_node_set_needs_build (node, FALSE);
  _gb_tree_node_remove_dummy_child (node);

  for (i = 0; i < priv->builders->len; i++)
    {
      GbTreeBuilder *builder;

      builder = g_ptr_array_index (priv->builders, i);
      _gb_tree_builder_build_node (builder, node);
    }
}

static void
gb_tree_unselect (GbTree *self)
{
  GtkTreeSelection *selection;

  IDE_ENTRY;

  g_return_if_fail (GB_IS_TREE (self));

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  gtk_tree_selection_unselect_all (selection);

  IDE_EXIT;
}

static void
gb_tree_select (GbTree     *self,
                GbTreeNode *node)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeSelection *selection;
  GtkTreePath *path;

  IDE_ENTRY;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));

  if (priv->selection)
    {
      gb_tree_unselect (self);
      g_assert (!priv->selection);
    }

  priv->selection = node;

  path = gb_tree_node_get_path (node);
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  gtk_tree_selection_select_path (selection, path);
  gtk_tree_path_free (path);

  IDE_EXIT;
}

static guint
gb_tree_get_row_height (GbTree *self)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  guint extra_padding;
  gint pix_min_height;
  gint pix_nat_height;
  gint text_min_height;
  gint text_nat_height;

  g_assert (GB_IS_TREE (self));

  gtk_widget_style_get (GTK_WIDGET (self),
                        "vertical-separator", &extra_padding,
                        NULL);

  gtk_cell_renderer_get_preferred_height (priv->cell_pixbuf,
                                          GTK_WIDGET (self),
                                          &pix_min_height,
                                          &pix_nat_height);
  gtk_cell_renderer_get_preferred_height (priv->cell_text,
                                          GTK_WIDGET (self),
                                          &text_min_height,
                                          &text_nat_height);

  return MAX (pix_nat_height, text_nat_height) + extra_padding;
}

static void
gb_tree_menu_position_func (GtkMenu  *menu,
                            gint     *x,
                            gint     *y,
                            gboolean *push_in,
                            gpointer  user_data)
{
  GdkPoint *loc = user_data;
  GtkRequisition req;
  GdkRectangle rect;
  GdkScreen *screen;
  gint monitor;

  g_return_if_fail (loc != NULL);

  gtk_widget_get_preferred_size (GTK_WIDGET (menu), NULL, &req);
  screen = gtk_widget_get_screen (GTK_WIDGET (menu));
  monitor = gdk_screen_get_monitor_at_point (screen, *x, *y);
  gdk_screen_get_monitor_geometry (screen, monitor, &rect);

  if ((loc->x != -1) && (loc->y != -1))
    {
      if ((loc->y + req.height) <= (rect.y + rect.height))
        {
          *x = loc->x;
          *y = loc->y;
        }
      else
        {
          GtkWidget *attached;
          guint row_height;

          attached = gtk_menu_get_attach_widget (menu);
          row_height = gb_tree_get_row_height (GB_TREE (attached));

          *x = loc->x;
          *y = loc->y + row_height - req.height;
        }
    }
}

static void
check_visible_foreach (GtkWidget *widget,
                       gpointer   user_data)
{
  gboolean *at_least_one_visible = user_data;

  if (*at_least_one_visible == FALSE)
    *at_least_one_visible = gtk_widget_get_visible (widget);
}

static GMenu *
gb_tree_create_menu (GbTree     *self,
                     GbTreeNode *node)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GMenu *menu;
  guint i;

  g_return_val_if_fail (GB_IS_TREE (self), NULL);
  g_return_val_if_fail (GB_IS_TREE_NODE (node), NULL);

  menu = g_menu_new ();

  for (i = 0; i < priv->builders->len; i++)
    {
      GbTreeBuilder *builder;

      builder = g_ptr_array_index (priv->builders, i);
      _gb_tree_builder_node_popup (builder, node, menu);
    }

  return menu;
}

static void
gb_tree_popup (GbTree         *self,
               GbTreeNode     *node,
               GdkEventButton *event,
               gint            target_x,
               gint            target_y)
{
  GdkPoint loc = { -1, -1 };
  gboolean at_least_one_visible = FALSE;
  GtkWidget *menu_widget;
  GMenu *menu;
  gint button;
  gint event_time;

  IDE_ENTRY;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));

  menu = gb_tree_create_menu (self, node);
  menu_widget = gtk_menu_new_from_model (G_MENU_MODEL (menu));
  g_clear_object (&menu);

  g_signal_emit (self, gSignals [POPULATE_POPUP], 0, menu_widget);

  if ((target_x >= 0) && (target_y >= 0))
    {
      gdk_window_get_root_coords (gtk_widget_get_window (GTK_WIDGET (self)),
                                  target_x, target_y, &loc.x, &loc.y);
      loc.x -= 12;
    }

  gtk_container_foreach (GTK_CONTAINER (menu_widget),
                         check_visible_foreach,
                         &at_least_one_visible);

  if (event != NULL)
    {
      button = event->button;
      event_time = event->time;
    }
  else
    {
      button = 0;
      event_time = gtk_get_current_event_time ();
    }

  if (at_least_one_visible)
    {
      gtk_menu_attach_to_widget (GTK_MENU (menu_widget),
                                 GTK_WIDGET (self),
                                 NULL);
      gtk_menu_popup (GTK_MENU (menu_widget), NULL, NULL,
                      gb_tree_menu_position_func, &loc,
                      button, event_time);
    }

  IDE_EXIT;
}

static gboolean
gb_tree_popup_menu (GtkWidget *widget)
{
  GbTree *self = (GbTree *)widget;
  GbTreeNode *node;
  GdkRectangle area;

  g_assert (GB_IS_TREE (self));

  if (!(node = gb_tree_get_selected (self)))
    return FALSE;

  gb_tree_node_get_area (node, &area);
  gb_tree_popup (self, node, NULL, area.x + area.width, area.y - 1);

  return TRUE;
}

static void
gb_tree_selection_changed (GbTree           *self,
                           GtkTreeSelection *selection)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GbTreeBuilder *builder;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GbTreeNode *node;
  GbTreeNode *unselection;
  gint i;

  IDE_ENTRY;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GTK_IS_TREE_SELECTION (selection));

  if ((unselection = priv->selection))
    {
      priv->selection = NULL;
      for (i = 0; i < priv->builders->len; i++)
        {
          builder = g_ptr_array_index (priv->builders, i);
          _gb_tree_builder_node_unselected (builder, unselection);
        }
    }

  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    {
      gtk_tree_model_get (model, &iter, 0, &node, -1);
      if (node)
        {
          for (i = 0; i < priv->builders->len; i++)
            {
              builder = g_ptr_array_index (priv->builders, i);
              _gb_tree_builder_node_selected (builder, node);
            }
          g_object_unref (node);
        }
    }

  g_object_notify_by_pspec (G_OBJECT (self), gParamSpecs [PROP_SELECTION]);

  IDE_EXIT;
}

static gboolean
gb_tree_add_builder_foreach_cb (GtkTreeModel *model,
                                GtkTreePath  *path,
                                GtkTreeIter  *iter,
                                gpointer      user_data)
{
  GbTreeBuilder *builder = user_data;
  GbTreeNode *node = NULL;

  IDE_ENTRY;

  g_return_val_if_fail (GTK_IS_TREE_MODEL (model), FALSE);
  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (iter != NULL, FALSE);

  gtk_tree_model_get (model, iter, 0, &node, -1);
  if (!_gb_tree_node_get_needs_build (node))
    _gb_tree_builder_build_node (builder, node);
  g_clear_object (&node);

  IDE_RETURN (FALSE);
}

static gboolean
gb_tree_foreach (GbTree                  *self,
                 GtkTreeIter             *iter,
                 GtkTreeModelForeachFunc  func,
                 gpointer                 user_data)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreePath *path;
  GtkTreeIter child;
  gboolean ret;

  g_assert (GB_IS_TREE (self));
  g_assert (iter != NULL);
  g_assert (gtk_tree_store_iter_is_valid (priv->store, iter));
  g_assert (func != NULL);

  model = GTK_TREE_MODEL (priv->store);
  path = gtk_tree_model_get_path (model, iter);
  ret = func (model, path, iter, user_data);
  gtk_tree_path_free (path);

  if (ret)
    return TRUE;

  if (gtk_tree_model_iter_children (model, &child, iter))
    {
      do
        {
          if (gb_tree_foreach (self, &child, func, user_data))
            return TRUE;
        }
      while (gtk_tree_model_iter_next (model, &child));
    }

  return FALSE;
}

static void
pixbuf_func (GtkCellLayout   *cell_layout,
             GtkCellRenderer *cell,
             GtkTreeModel    *tree_model,
             GtkTreeIter     *iter,
             gpointer         data)
{
  const gchar *icon_name;
  GbTreeNode *node;

  g_assert (GTK_IS_CELL_LAYOUT (cell_layout));
  g_assert (GTK_IS_CELL_RENDERER_PIXBUF (cell));
  g_assert (GTK_IS_TREE_MODEL (tree_model));
  g_assert (iter != NULL);

  gtk_tree_model_get (tree_model, iter, 0, &node, -1);
  icon_name = node ? gb_tree_node_get_icon_name (node) : NULL;
  g_object_set (cell, "icon-name", icon_name, NULL);
  g_clear_object (&node);
}

static void
text_func (GtkCellLayout   *cell_layout,
           GtkCellRenderer *cell,
           GtkTreeModel    *tree_model,
           GtkTreeIter     *iter,
           gpointer         data)
{
  GbTree *self = data;
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GbTreeNode *node = NULL;

  g_assert (GB_IS_TREE (self));
  g_assert (GTK_IS_CELL_LAYOUT (cell_layout));
  g_assert (GTK_IS_CELL_RENDERER_TEXT (cell));
  g_assert (GTK_IS_TREE_MODEL (tree_model));
  g_assert (iter != NULL);

  gtk_tree_model_get (tree_model, iter, 0, &node, -1);

  if (node)
    {
      GdkRGBA *rgba = NULL;
      const gchar *text;
      gboolean use_markup;

      text = gb_tree_node_get_text (node);
      use_markup = gb_tree_node_get_use_markup (node);

      if (gb_tree_node_get_use_dim_label (node))
        rgba = &priv->dim_foreground;

      g_object_set (cell,
                    use_markup ? "markup" : "text", text,
                    "foreground-rgba", rgba,
                    NULL);
    }
}

static void
gb_tree_add (GbTree     *self,
             GbTreeNode *node,
             GbTreeNode *child,
             gboolean    prepend)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreePath *path;
  GtkTreeIter *parentptr;
  GtkTreeIter iter;
  GtkTreeIter parent;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));
  g_return_if_fail (GB_IS_TREE_NODE (child));

  _gb_tree_node_set_tree (child, self);
  _gb_tree_node_set_parent (child, node);

  g_object_ref_sink (child);

  if (node == priv->root)
    {
      parentptr = NULL;
    }
  else
    {
      path = gb_tree_node_get_path (node);
      gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &parent, path);
      parentptr = &parent;
      g_clear_pointer (&path, gtk_tree_path_free);
    }

  if (prepend)
    gtk_tree_store_prepend (priv->store, &iter, parentptr);
  else
    gtk_tree_store_append (priv->store, &iter, parentptr);

  gtk_tree_store_set (priv->store, &iter, 0, child, -1);

  if (node == priv->root)
    _gb_tree_build_node (self, child);

  g_object_unref (child);
}

void
_gb_tree_insert_sorted (GbTree                *self,
                        GbTreeNode            *node,
                        GbTreeNode            *child,
                        GbTreeNodeCompareFunc  compare_func,
                        gpointer               user_data)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreeIter *parent = NULL;
  GtkTreeIter node_iter;
  GtkTreeIter children;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));
  g_return_if_fail (GB_IS_TREE_NODE (child));
  g_return_if_fail (compare_func != NULL);

  model = GTK_TREE_MODEL (priv->store);

  _gb_tree_node_set_tree (child, self);
  _gb_tree_node_set_parent (child, node);

  g_object_ref_sink (child);

  if (gb_tree_node_get_iter (node, &node_iter))
    parent = &node_iter;

  if (gtk_tree_model_iter_children (model, &children, parent))
    {
      do
        {
          g_autoptr(GbTreeNode) sibling = NULL;
          GtkTreeIter that;

          gtk_tree_model_get (model, &children, 0, &sibling, -1);

          if (compare_func (sibling, child, user_data) > 0)
            {
              gtk_tree_store_insert_before (priv->store, &that, parent, &children);
              gtk_tree_store_set (priv->store, &that, 0, child, -1);
              goto inserted;
            }
        }
      while (gtk_tree_model_iter_next (model, &children));
    }

  gtk_tree_store_append (priv->store, &children, parent);
  gtk_tree_store_set (priv->store, &children, 0, child, -1);

inserted:
  if (node == priv->root)
    _gb_tree_build_node (self, child);

  g_object_unref (child);
}

static void
gb_tree_row_activated (GtkTreeView       *tree_view,
                       GtkTreePath       *path,
                       GtkTreeViewColumn *column)
{
  GbTree *self = (GbTree *)tree_view;
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GbTreeBuilder *builder;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GbTreeNode *node = NULL;
  gboolean handled = FALSE;
  gint i;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (path != NULL);

  model = gtk_tree_view_get_model (tree_view);

  if (gtk_tree_model_get_iter (model, &iter, path))
    {
      gtk_tree_model_get (model, &iter, 0, &node, -1);
      for (i = 0; i < priv->builders->len; i++)
        {
          builder = g_ptr_array_index (priv->builders, i);
          if ((handled = _gb_tree_builder_node_activated (builder, node)))
            break;
        }
      g_clear_object (&node);
    }

  if (!handled)
    {
      if (gtk_tree_view_row_expanded (tree_view, path))
        gtk_tree_view_collapse_row (tree_view, path);
      else
        gtk_tree_view_expand_to_path (tree_view, path);
    }
}

static void
gb_tree_row_expanded (GtkTreeView *tree_view,
                      GtkTreeIter *iter,
                      GtkTreePath *path)
{
  GbTree *self = (GbTree *)tree_view;
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeModel *model;
  GbTreeNode *node;

  g_assert (GB_IS_TREE (self));
  g_assert (iter != NULL);
  g_assert (path != NULL);

  model = GTK_TREE_MODEL (priv->store);

  gtk_tree_model_get (model, iter, 0, &node, -1);

  /*
   * If we are expanding a row that has a dummy child, we might need to
   * build the node immediately, and re-expand it.
   */
  if (_gb_tree_node_get_needs_build (node))
    {
      _gb_tree_build_node (self, node);
      gb_tree_node_expand (node, FALSE);
      gb_tree_node_select (node);
    }

  g_clear_object (&node);
}


static gboolean
gb_tree_button_press_event (GtkWidget      *widget,
                            GdkEventButton *button)
{
  GbTree *self = (GbTree *)widget;
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkAllocation alloc;
  GtkTreePath *tree_path = NULL;
  GtkTreeIter iter;
  GbTreeNode *node = NULL;
  gint cell_y;

  g_assert (GB_IS_TREE (self));
  g_assert (button != NULL);

  if ((button->type == GDK_BUTTON_PRESS) && (button->button == GDK_BUTTON_SECONDARY))
    {
      if (!gtk_widget_has_focus (GTK_WIDGET (self)))
        gtk_widget_grab_focus (GTK_WIDGET (self));

      gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (self),
                                     button->x,
                                     button->y,
                                     &tree_path,
                                     NULL,
                                     NULL,
                                     &cell_y);

      if (!tree_path)
        {
          gb_tree_unselect (self);
        }
      else
        {
          gtk_widget_get_allocation (GTK_WIDGET (self), &alloc);
          gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, tree_path);
          gtk_tree_model_get (GTK_TREE_MODEL (priv->store), &iter, 0, &node, -1);
          gb_tree_select (self, node);
          gb_tree_popup (self, node, button,
                         alloc.x + alloc.width,
                         button->y - cell_y);
          g_object_unref (node);
          gtk_tree_path_free (tree_path);
        }

      return GDK_EVENT_STOP;
    }

  return GTK_WIDGET_CLASS (gb_tree_parent_class)->button_press_event (widget, button);
}

static gboolean
gb_tree_find_item_foreach_cb (GtkTreeModel *model,
                              GtkTreePath  *path,
                              GtkTreeIter  *iter,
                              gpointer      user_data)
{
  GbTreeNode *node = NULL;
  NodeLookup *lookup = user_data;
  gboolean ret = FALSE;

  g_assert (GTK_IS_TREE_MODEL (model));
  g_assert (path != NULL);
  g_assert (iter != NULL);
  g_assert (lookup != NULL);

  gtk_tree_model_get (model, iter, 0, &node, -1);

  if (node != NULL)
    {
      GObject *item;

      item = gb_tree_node_get_item (node);

      if (lookup->equal_func (lookup->key, item))
        {
          lookup->result = node;
          ret = TRUE;
        }
    }

  g_clear_object (&node);

  return ret;
}

static void
gb_tree_real_action (GbTree      *self,
                     const gchar *prefix,
                     const gchar *action_name,
                     const gchar *param)
{
  GVariant *variant = NULL;

  g_assert (GB_IS_TREE (self));

  if (*param != 0)
    {
      GError *error = NULL;

      variant = g_variant_parse (NULL, param, NULL, NULL, &error);

      if (variant == NULL)
        {
          g_warning ("can't parse keybinding parameters \"%s\": %s",
                     param, error->message);
          g_clear_error (&error);
          return;
        }
    }

  gb_widget_activate_action (GTK_WIDGET (self), prefix, action_name, variant);
}

static gboolean
gb_tree_default_search_equal_func (GtkTreeModel *model,
                                   gint          column,
                                   const gchar  *key,
                                   GtkTreeIter  *iter,
                                   gpointer      user_data)
{
  GbTreeNode *node = NULL;
  gboolean ret = TRUE;

  g_assert (GTK_IS_TREE_MODEL (model));
  g_assert (column == 0);
  g_assert (key != NULL);
  g_assert (iter != NULL);

  gtk_tree_model_get (model, iter, 0, &node, -1);

  if (node != NULL)
    {
      const gchar *text;

      text = gb_tree_node_get_text (node);
      ret = !(strstr (key, text) != NULL);
      g_object_unref (node);
    }

  return ret;
}

static void
gb_tree_add_child (GtkBuildable *buildable,
                   GtkBuilder   *builder,
                   GObject      *child,
                   const gchar  *type)
{
  GbTree *self = (GbTree *)buildable;

  g_assert (GB_IS_TREE (self));
  g_assert (GTK_IS_BUILDER (builder));
  g_assert (G_IS_OBJECT (child));

  if (g_strcmp0 (type, "builder") == 0)
    {
      if (!GB_IS_TREE_BUILDER (child))
        {
          g_warning ("Attempt to add invalid builder of type %s to GbTree.",
                     g_type_name (G_OBJECT_TYPE (child)));
          return;
        }

      gb_tree_add_builder (self, GB_TREE_BUILDER (child));
      return;
    }

  gb_tree_parent_buildable_iface->add_child (buildable, builder, child, type);
}

static void
gb_tree_style_updated (GtkWidget *widget)
{
  GbTree *self = (GbTree *)widget;
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkStyleContext *style_context;
  GtkStateFlags flags;

  g_assert (GB_IS_TREE (self));

  GTK_WIDGET_CLASS (gb_tree_parent_class)->style_updated (widget);

  flags = gtk_widget_get_state_flags (widget);

  style_context = gtk_widget_get_style_context (widget);
  gtk_style_context_save (style_context);
  gtk_style_context_add_class (style_context, "dim-label");
  gtk_style_context_get_color (style_context, flags, &priv->dim_foreground);
  gtk_style_context_restore (style_context);
}

static void
gb_tree_finalize (GObject *object)
{
  GbTree *self = GB_TREE (object);
  GbTreePrivate *priv = gb_tree_get_instance_private (self);

  g_ptr_array_unref (priv->builders);
  g_clear_object (&priv->store);
  g_clear_object (&priv->root);

  G_OBJECT_CLASS (gb_tree_parent_class)->finalize (object);
}

static void
gb_tree_get_property (GObject    *object,
                      guint       prop_id,
                      GValue     *value,
                      GParamSpec *pspec)
{
  GbTree *self = GB_TREE (object);
  GbTreePrivate *priv = gb_tree_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_ROOT:
      g_value_set_object (value, priv->root);
      break;

    case PROP_SELECTION:
      g_value_set_object (value, priv->selection);
      break;

    case PROP_SHOW_ICONS:
      g_value_set_boolean (value, priv->show_icons);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_tree_set_property (GObject      *object,
                      guint         prop_id,
                      const GValue *value,
                      GParamSpec   *pspec)
{
  GbTree *self = GB_TREE (object);

  switch (prop_id)
    {
    case PROP_ROOT:
      gb_tree_set_root (self, g_value_get_object (value));
      break;

    case PROP_SELECTION:
      gb_tree_select (self, g_value_get_object (value));
      break;

    case PROP_SHOW_ICONS:
      gb_tree_set_show_icons (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_tree_buildable_init (GtkBuildableIface *iface)
{
  gb_tree_parent_buildable_iface = g_type_interface_peek_parent (iface);

  iface->add_child = gb_tree_add_child;
}

static void
gb_tree_class_init (GbTreeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkTreeViewClass *tree_view_class = GTK_TREE_VIEW_CLASS (klass);

  object_class->finalize = gb_tree_finalize;
  object_class->get_property = gb_tree_get_property;
  object_class->set_property = gb_tree_set_property;

  widget_class->popup_menu = gb_tree_popup_menu;
  widget_class->button_press_event = gb_tree_button_press_event;
  widget_class->style_updated = gb_tree_style_updated;

  tree_view_class->row_activated = gb_tree_row_activated;
  tree_view_class->row_expanded = gb_tree_row_expanded;

  klass->action = gb_tree_real_action;

  gParamSpecs[PROP_ROOT] =
    g_param_spec_object ("root",
                         "Root",
                         "The root object of the tree.",
                         GB_TYPE_TREE_NODE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gParamSpecs[PROP_SELECTION] =
    g_param_spec_object ("selection",
                         "Selection",
                         "The node selection.",
                         GB_TYPE_TREE_NODE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  gParamSpecs [PROP_SHOW_ICONS] =
    g_param_spec_boolean ("show-icons",
                          "Show Icons",
                          "Show Icons",
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, LAST_PROP, gParamSpecs);

  gSignals [ACTION] =
    g_signal_new ("action",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                  G_STRUCT_OFFSET (GbTreeClass, action),
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  3,
                  G_TYPE_STRING,
                  G_TYPE_STRING,
                  G_TYPE_STRING);

  gSignals [POPULATE_POPUP] =
    g_signal_new ("populate-popup",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbTreeClass, populate_popup),
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1,
                  GTK_TYPE_WIDGET);
}

static void
gb_tree_init (GbTree *self)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeSelection *selection;
  GtkCellRenderer *cell;
  GtkCellLayout *column;

  priv->builders = g_ptr_array_new ();
  g_ptr_array_set_free_func (priv->builders, g_object_unref);
  priv->store = gtk_tree_store_new (1, GB_TYPE_TREE_NODE);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  g_signal_connect_object (selection, "changed",
                           G_CALLBACK (gb_tree_selection_changed),
                           self,
                           G_CONNECT_SWAPPED);

  column = g_object_new (GTK_TYPE_TREE_VIEW_COLUMN,
                         "title", "Node",
                         NULL);
  priv->column = GTK_TREE_VIEW_COLUMN (column);

  cell = g_object_new (GTK_TYPE_CELL_RENDERER_PIXBUF,
                       "xpad", 3,
                       "visible", priv->show_icons,
                       NULL);
  priv->cell_pixbuf = cell;
  g_object_bind_property (self, "show-icons", cell, "visible", 0);
  gtk_cell_layout_pack_start (column, cell, FALSE);
  gtk_cell_layout_set_cell_data_func (column, cell, pixbuf_func, NULL, NULL);

  cell = g_object_new (GTK_TYPE_CELL_RENDERER_TEXT,
                       "ellipsize", PANGO_ELLIPSIZE_NONE,
                       NULL);
  priv->cell_text = cell;
  gtk_cell_layout_pack_start (column, cell, TRUE);
  gtk_cell_layout_set_cell_data_func (column, cell, text_func, self, NULL);

  gtk_tree_view_append_column (GTK_TREE_VIEW (self),
                               GTK_TREE_VIEW_COLUMN (column));

  gtk_tree_view_set_model (GTK_TREE_VIEW (self),
                           GTK_TREE_MODEL (priv->store));

  gtk_tree_view_set_search_equal_func (GTK_TREE_VIEW (self),
                                       gb_tree_default_search_equal_func,
                                       NULL, NULL);
  gtk_tree_view_set_search_column (GTK_TREE_VIEW (self), 0);
}

void
gb_tree_expand_to_node (GbTree     *self,
                        GbTreeNode *node)
{
  g_assert (GB_IS_TREE (self));
  g_assert (GB_IS_TREE_NODE (node));

  if (gb_tree_node_get_expanded (node))
    {
      gb_tree_node_expand (node, TRUE);
    }
  else
    {
      gb_tree_node_expand (node, TRUE);
      gb_tree_node_collapse (node);
    }
}

gboolean
gb_tree_get_show_icons (GbTree *self)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);

  g_return_val_if_fail (GB_IS_TREE (self), FALSE);

  return priv->show_icons;
}

void
gb_tree_set_show_icons (GbTree   *self,
                        gboolean  show_icons)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);

  g_return_if_fail (GB_IS_TREE (self));

  show_icons = !!show_icons;

  if (show_icons != priv->show_icons)
    {
      priv->show_icons = show_icons;
      g_object_set (priv->cell_pixbuf, "visible", show_icons, NULL);
      /*
       * WORKAROUND:
       *
       * Changing the visibility of the cell does not force a redraw of the
       * tree view. So to force it, we will hide/show our entire pixbuf/text
       * column.
       */
      gtk_tree_view_column_set_visible (priv->column, FALSE);
      gtk_tree_view_column_set_visible (priv->column, TRUE);
      g_object_notify_by_pspec (G_OBJECT (self),
                                gParamSpecs [PROP_SHOW_ICONS]);
    }
}

/**
 * gb_tree_get_selected:
 * @self: (in): A #GbTree.
 *
 * Gets the currently selected node in the tree.
 *
 * Returns: (transfer none): A #GbTreeNode.
 */
GbTreeNode *
gb_tree_get_selected (GbTree *self)
{
  GtkTreeSelection *selection;
  GtkTreeModel *model;
  GtkTreeIter iter;
  GbTreeNode *ret = NULL;

  g_return_val_if_fail (GB_IS_TREE (self), NULL);

  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  if (gtk_tree_selection_get_selected (selection, &model, &iter))
    {
      gtk_tree_model_get (model, &iter, 0, &ret, -1);

      /*
       * We incurred an extra reference when extracting the value from
       * the treemodel. Since it owns the reference, we can drop it here
       * so that we don't transfer the ownership to the caller.
       */
      g_object_unref (ret);
    }

  return ret;
}

void
gb_tree_scroll_to_node (GbTree     *self,
                        GbTreeNode *node)
{
  GtkTreePath *path;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));

  path = gb_tree_node_get_path (node);
  gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (self), path, NULL, FALSE, 0, 0);
  gtk_tree_path_free (path);
}

GtkTreePath *
_gb_tree_get_path (GbTree *self,
                   GList  *list)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreeIter iter;
  GtkTreeIter *iter_ptr;
  GList *list_iter;

  g_assert (GB_IS_TREE (self));

  model = GTK_TREE_MODEL (priv->store);

  if ((list == NULL) || (list->data != priv->root) || (list->next == NULL))
    return NULL;

  iter_ptr = NULL;

  for (list_iter = list->next; list_iter; list_iter = list_iter->next)
    {
      GtkTreeIter children;

      if (gtk_tree_model_iter_children (model, &children, iter_ptr))
        {
          gboolean found = FALSE;

          do
            {
              g_autoptr(GbTreeNode) item = NULL;

              gtk_tree_model_get (model, &children, 0, &item, -1);
              found = (item == (GbTreeNode *)list_iter->data);
            }
          while (!found && gtk_tree_model_iter_next (model, &children));

          if (found)
            {
              iter = children;
              iter_ptr = &iter;
              continue;
            }
        }

      return NULL;
    }

  return gtk_tree_model_get_path (model, &iter);
}

/**
 * gb_tree_add_builder:
 * @self: (in): A #GbTree.
 * @builder: (in) (transfer full): A #GbTreeBuilder to add.
 *
 * Removes a builder from the tree.
 */
void
gb_tree_add_builder (GbTree        *self,
                     GbTreeBuilder *builder)
{
  GtkTreeIter iter;
  GbTreePrivate *priv = gb_tree_get_instance_private (self);

  IDE_ENTRY;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_BUILDER (builder));

  g_ptr_array_add (priv->builders, g_object_ref_sink (builder));

  _gb_tree_builder_set_tree (builder, self);
  _gb_tree_builder_added (builder, self);

  if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (priv->store), &iter))
    gb_tree_foreach (self, &iter, gb_tree_add_builder_foreach_cb, builder);

  IDE_EXIT;
}

/**
 * gb_tree_remove_builder:
 * @self: (in): A #GbTree.
 * @builder: (in): A #GbTreeBuilder to remove.
 *
 * Removes a builder from the tree.
 */
void
gb_tree_remove_builder (GbTree        *self,
                        GbTreeBuilder *builder)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  gsize i;

  IDE_ENTRY;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_BUILDER (builder));

  for (i = 0; i < priv->builders->len; i++)
    {
      if (builder == g_ptr_array_index (priv->builders, i))
        {
          g_object_ref (builder);
          g_ptr_array_remove_index (priv->builders, i);
          _gb_tree_builder_removed (builder, self);
          g_object_unref (builder);
        }
    }

  IDE_EXIT;
}

/**
 * gb_tree_get_root:
 *
 * Retrieves the root node of the tree. The root node is not a visible node
 * in the self, but a placeholder for all other builders to build upon.
 *
 * Returns: (transfer none) (nullable): A #GbTreeNode or %NULL.
 */
GbTreeNode *
gb_tree_get_root (GbTree *self)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);

  g_return_val_if_fail (GB_IS_TREE (self), NULL);

  return priv->root;
}

/**
 * gb_tree_set_root:
 * @self: (in): A #GbTree.
 * @node: (in): A #GbTreeNode.
 *
 * Sets the root node of the #GbTree widget. This is used to build
 * the items within the treeview. The item itself will not be added
 * to the self, but the direct children will be.
 */
void
gb_tree_set_root (GbTree     *self,
                  GbTreeNode *root)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);

  IDE_ENTRY;

  g_return_if_fail (GB_IS_TREE (self));

  if (priv->root != root)
    {
      GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));

      gtk_tree_selection_unselect_all (selection);

      if (priv->root != NULL)
        {
          _gb_tree_node_set_parent (priv->root, NULL);
          _gb_tree_node_set_tree (priv->root, NULL);
          gtk_tree_store_clear (priv->store);
          g_clear_object (&priv->root);
        }

      if (root != NULL)
        {
          priv->root = g_object_ref_sink (root);
          _gb_tree_node_set_parent (priv->root, NULL);
          _gb_tree_node_set_tree (priv->root, self);
          _gb_tree_build_node (self, priv->root);
        }

      g_object_notify_by_pspec (G_OBJECT (self), gParamSpecs [PROP_ROOT]);
    }

  IDE_EXIT;
}

void
gb_tree_rebuild (GbTree *self)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeSelection *selection;

  g_return_if_fail (GB_IS_TREE (self));

  /*
   * We don't want notification of selection changes while rebuilding.
   */
  selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (self));
  gtk_tree_selection_unselect_all (selection);

  if (priv->root != NULL)
    {
      gtk_tree_store_clear (priv->store);
      _gb_tree_build_node (self, priv->root);
    }
}

/**
 * gb_tree_find_custom:
 * @self: A #GbTree
 * @equal_func: (scope call): A #GEqualFunc
 * @key: the key for @equal_func
 *
 * Walks the entire tree looking for the first item that matches given
 * @equal_func and @key.
 *
 * The first parameter to @equal_func will always be @key.
 * The second parameter will be the nodes #GbTreeNode:item property.
 *
 * Returns: (nullable) (transfer none): A #GbTreeNode or %NULL.
 */
GbTreeNode *
gb_tree_find_custom (GbTree     *self,
                     GEqualFunc  equal_func,
                     gpointer    key)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  NodeLookup lookup;

  g_return_val_if_fail (GB_IS_TREE (self), NULL);
  g_return_val_if_fail (equal_func != NULL, NULL);

  lookup.key = key;
  lookup.equal_func = equal_func;
  lookup.result = NULL;

  gtk_tree_model_foreach (GTK_TREE_MODEL (priv->store),
                          gb_tree_find_item_foreach_cb,
                          &lookup);

  return lookup.result;
}

/**
 * gb_tree_find_item:
 * @self: A #GbTree.
 * @item: (allow-none): A #GObject or %NULL.
 *
 * Finds a #GbTreeNode with an item property matching @item.
 *
 * Returns: (transfer none) (nullable): A #GbTreeNode or %NULL.
 */
GbTreeNode *
gb_tree_find_item (GbTree  *self,
                   GObject *item)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  NodeLookup lookup;

  g_return_val_if_fail (GB_IS_TREE (self), NULL);
  g_return_val_if_fail (!item || G_IS_OBJECT (item), NULL);

  lookup.key = item;
  lookup.equal_func = g_direct_equal;
  lookup.result = NULL;

  gtk_tree_model_foreach (GTK_TREE_MODEL (priv->store),
                          gb_tree_find_item_foreach_cb,
                          &lookup);

  return lookup.result;
}

void
_gb_tree_append (GbTree     *self,
                 GbTreeNode *node,
                 GbTreeNode *child)
{
  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));
  g_return_if_fail (GB_IS_TREE_NODE (child));

  gb_tree_add (self, node, child, FALSE);
}

void
_gb_tree_prepend (GbTree     *self,
                  GbTreeNode *node,
                  GbTreeNode *child)
{
  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));
  g_return_if_fail (GB_IS_TREE_NODE (child));

  gb_tree_add (self, node, child, TRUE);
}

void
_gb_tree_invalidate (GbTree     *self,
                     GbTreeNode *node)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreePath *path;
  GbTreeNode *parent;
  GtkTreeIter iter;
  GtkTreeIter child;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));

  model = GTK_TREE_MODEL (priv->store);
  path = gb_tree_node_get_path (node);
  gtk_tree_model_get_iter (model, &iter, path);

  if (gtk_tree_model_iter_children (model, &child, &iter))
    {
      while (gtk_tree_store_remove (priv->store, &child))
        {
        }
    }

  _gb_tree_node_set_needs_build (node, TRUE);

  parent = gb_tree_node_get_parent (node);

  if ((parent == NULL) || gb_tree_node_get_expanded (parent))
    _gb_tree_build_node (self, node);

  gtk_tree_path_free (path);
}

/**
 * gb_tree_find_child_node:
 * @self: A #GbTree
 * @node: A #GbTreeNode
 * @find_func: (scope call): A callback to locate the child
 * @user_data: user data for @find_func
 *
 * Searches through the direct children of @node for a matching child.
 * @find_func should return %TRUE if the child matches, otherwise %FALSE.
 *
 * Returns: (transfer none) (nullable): A #GbTreeNode or %NULL.
 */
GbTreeNode *
gb_tree_find_child_node (GbTree         *self,
                         GbTreeNode     *node,
                         GbTreeFindFunc  find_func,
                         gpointer        user_data)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreeModel *model;
  GtkTreePath *path;
  GtkTreeIter iter;
  GtkTreeIter children;

  g_return_val_if_fail (GB_IS_TREE (self), NULL);
  g_return_val_if_fail (!node || GB_IS_TREE_NODE (node), NULL);
  g_return_val_if_fail (find_func, NULL);

  if (node == NULL)
    node = priv->root;

  if (node == NULL)
    {
      g_warning ("Cannot find node. No root node has been set on %s.",
                 g_type_name (G_OBJECT_TYPE (self)));
      return NULL;
    }

  if (_gb_tree_node_get_needs_build (node))
    _gb_tree_build_node (self, node);

  model = GTK_TREE_MODEL (priv->store);
  path = gb_tree_node_get_path (node);

  if (path != NULL)
    {
      if (!gtk_tree_model_get_iter (model, &iter, path))
        goto failure;

      if (!gtk_tree_model_iter_children (model, &children, &iter))
        goto failure;
    }
  else
    {
      if (!gtk_tree_model_iter_children (model, &children, NULL))
        goto failure;
    }

  do
    {
      GbTreeNode *child = NULL;

      gtk_tree_model_get (model, &children, 0, &child, -1);

      if (find_func (self, node, child, user_data))
        {
          /*
           * We want to returned a borrowed reference to the child node.
           * It is safe to unref the child here before we return.
           */
          g_object_unref (child);
          return child;
        }

      g_clear_object (&child);
    }
  while (gtk_tree_model_iter_next (model, &children));

failure:
  g_clear_pointer (&path, gtk_tree_path_free);

  return NULL;
}

void
_gb_tree_remove (GbTree     *self,
                 GbTreeNode *node)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreePath *path;
  GtkTreeIter iter;

  g_return_if_fail (GB_IS_TREE (self));
  g_return_if_fail (GB_IS_TREE_NODE (node));

  path = gb_tree_node_get_path (node);

  if (gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), &iter, path))
    gtk_tree_store_remove (priv->store, &iter);

  gtk_tree_path_free (path);
}

gboolean
_gb_tree_get_iter (GbTree      *self,
                   GbTreeNode  *node,
                   GtkTreeIter *iter)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);
  GtkTreePath *path;
  gboolean ret = FALSE;

  g_return_val_if_fail (GB_IS_TREE (self), FALSE);
  g_return_val_if_fail (GB_IS_TREE_NODE (node), FALSE);
  g_return_val_if_fail (iter, FALSE);

  if ((path = gb_tree_node_get_path (node)) != NULL)
    {
      ret = gtk_tree_model_get_iter (GTK_TREE_MODEL (priv->store), iter, path);
      gtk_tree_path_free (path);
    }

  return ret;
}

static void
filter_func_free (gpointer user_data)
{
  FilterFunc *data = user_data;

  if (data->filter_data_destroy)
    data->filter_data_destroy (data->filter_data);

  g_free (data);
}

static gboolean
gb_tree_model_filter_visible_func (GtkTreeModel *model,
                                   GtkTreeIter  *iter,
                                   gpointer      data)
{
  GbTreeNode *node = NULL;
  FilterFunc *filter = data;
  gboolean ret;

  gtk_tree_model_get (model, iter, 0, &node, -1);
  ret = filter->filter_func (filter->self, node, filter->filter_data);
  g_clear_object (&node);

  return ret;
}

/**
 * gb_tree_set_filter:
 * @self: A #GbTree
 * @filter_func: (allow-none) (scope notified): A callback to determien visibility.
 * @filter_data: User data for @filter_func.
 * @filter_data_destroy: Destroy notify for @filter_data.
 *
 * Sets the filter function to be used to determine visability of a tree node.
 */
void
gb_tree_set_filter (GbTree           *self,
                    GbTreeFilterFunc  filter_func,
                    gpointer          filter_data,
                    GDestroyNotify    filter_data_destroy)
{
  GbTreePrivate *priv = gb_tree_get_instance_private (self);

  g_return_if_fail (GB_IS_TREE (self));

  if (filter_func == NULL)
    {
      gtk_tree_view_set_model (GTK_TREE_VIEW (self), GTK_TREE_MODEL (priv->store));
    }
  else
    {
      FilterFunc *data;
      GtkTreeModel *filter;

      data = g_new0 (FilterFunc, 1);
      data->filter_func = filter_func;
      data->filter_data = filter_data;
      data->filter_data_destroy = filter_data_destroy;

      filter = gtk_tree_model_filter_new (GTK_TREE_MODEL (priv->store), NULL);
      gtk_tree_model_filter_set_visible_func (GTK_TREE_MODEL_FILTER (filter),
                                              gb_tree_model_filter_visible_func,
                                              data,
                                              filter_func_free);
      gtk_tree_view_set_model (GTK_TREE_VIEW (self), GTK_TREE_MODEL (filter));
      g_clear_object (&filter);
    }
}
