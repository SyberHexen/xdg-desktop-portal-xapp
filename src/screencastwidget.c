/*
 * Copyright © 2017-2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * The windows and monitors that can be shared, as cards with a preview that
 * Cinnamon paints (org.cinnamon.PortalHandlers GetWindowThumbnail and
 * GetMonitorThumbnail), refreshed about five times a second while the
 * picker is shown.  Without Cinnamon's previews the cards show the icons.
 */

#include "config.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "screencastwidget.h"
#include "displaystatetracker.h"
#include "shellintrospect.h"

/* Size of the previews, in application pixels */
#define THUMBNAIL_WIDTH 224
#define THUMBNAIL_HEIGHT 126
#define THUMBNAIL_REFRESH_MS 200

enum
{
  HAS_SELECTION_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ScreenCastWidget
{
  GtkBox parent;

  GtkWidget *source_type_switcher;
  GtkWidget *source_type;
  GtkWidget *window_selection;
  GtkWidget *monitor_selection;

  GtkWidget *monitor_heading;
  GtkWidget *monitor_list;
  GtkWidget *monitor_list_scrolled;

  GtkWidget *window_heading;
  GtkWidget *window_list;
  GtkWidget *window_list_scrolled;

  DisplayStateTracker *display_state_tracker;
  gulong monitors_changed_handler_id;

  ShellIntrospect *shell_introspect;
  gulong windows_changed_handler_id;

  guint selection_changed_timeout_id;

  guint thumbnail_timeout_id;
  GCancellable *thumbnail_cancellable;
  /* Previews in flight; a new round waits for the last one */
  struct _ThumbnailRound *thumbnail_round;
};

typedef struct _ThumbnailRound
{
  grefcount ref_count;
  int pending;
} ThumbnailRound;

static GQuark quark_monitor_widget_data;
static GQuark quark_window_widget_data;   /* window ID (uint64_t *) */
static GQuark quark_title_label;
static GQuark quark_thumbnail_image;

G_DEFINE_TYPE (ScreenCastWidget, screen_cast_widget, GTK_TYPE_BOX)

static GIcon *
get_app_icon (const char *app_id)
{
  g_autoptr(GDesktopAppInfo) info = NULL;
  GIcon *icon = NULL;

  if (app_id && *app_id)
    info = g_desktop_app_info_new (app_id);
  if (info)
    icon = g_app_info_get_icon (G_APP_INFO (info));
  if (icon)
    return g_object_ref (icon);

  return g_themed_icon_new ("application-x-executable");
}

static GtkWidget *
new_icon_image (GIcon *icon,
                int    size)
{
  GtkWidget *image = gtk_image_new_from_gicon (icon, GTK_ICON_SIZE_DIALOG);

  /* Icons given as files (some launchers) are not scaled to the icon size */
  gtk_image_set_pixel_size (GTK_IMAGE (image), size);
  return image;
}

/* A card: a preview (an icon until Cinnamon's preview arrives) above a row
 * with an icon and the title, and an optional second line */
static GtkWidget *
create_card (GIcon      *icon,
             const char *title,
             const char *subtitle)
{
  GtkWidget *card;
  GtkWidget *thumbnail;
  GtkWidget *row;
  GtkWidget *label;

  card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (card, 6);
  gtk_widget_set_margin_end (card, 6);
  gtk_widget_set_margin_top (card, 6);
  gtk_widget_set_margin_bottom (card, 6);

  thumbnail = new_icon_image (icon, 64);
  gtk_widget_set_size_request (thumbnail, THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT);
  gtk_box_pack_start (GTK_BOX (card), thumbnail, FALSE, FALSE, 0);
  g_object_set_qdata (G_OBJECT (card), quark_thumbnail_image, thumbnail);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start (GTK_BOX (row), new_icon_image (icon, 16),
                      FALSE, FALSE, 0);
  label = gtk_label_new (title);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 24);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_tooltip_text (label, title);
  gtk_box_pack_start (GTK_BOX (row), label, TRUE, TRUE, 0);
  gtk_box_pack_start (GTK_BOX (card), row, FALSE, FALSE, 0);
  g_object_set_qdata (G_OBJECT (card), quark_title_label, label);

  if (subtitle)
    {
      label = gtk_label_new (subtitle);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_style_context_add_class (gtk_widget_get_style_context (label),
                                   "dim-label");
      gtk_box_pack_start (GTK_BOX (card), label, FALSE, FALSE, 0);
    }

  gtk_widget_show_all (card);
  return card;
}

static GtkWidget *
create_window_widget (Window *window)
{
  g_autoptr(GIcon) icon = get_app_icon (window_get_app_id (window));
  GtkWidget *card;

  card = create_card (icon, window_get_title (window), NULL);
  /* The ID, not the Window: the window list is replaced when it changes */
  g_object_set_qdata_full (G_OBJECT (card), quark_window_widget_data,
                           g_memdup2 (&(uint64_t) { window_get_id (window) },
                                      sizeof (uint64_t)),
                           g_free);
  return card;
}

static gboolean
card_get_window_id (GtkWidget *card,
                    uint64_t  *id)
{
  uint64_t *window_id = g_object_get_qdata (G_OBJECT (card),
                                            quark_window_widget_data);

  if (!window_id)
    return FALSE;

  *id = *window_id;
  return TRUE;
}

static GtkWidget *
create_monitor_widget (LogicalMonitor *logical_monitor)
{
  g_autoptr(GIcon) icon = g_themed_icon_new ("video-display");
  Monitor *monitor;
  g_autofree char *title = NULL;
  g_autofree char *subtitle = NULL;
  GString *names = g_string_new (NULL);
  GList *l;
  int width, height;
  GtkWidget *card;

  /* Mirrored monitors share a logical monitor; it is recorded through the
   * first */
  monitor = logical_monitor_get_monitors (logical_monitor)->data;
  for (l = logical_monitor_get_monitors (logical_monitor); l; l = l->next)
    {
      if (names->len)
        g_string_append (names, " + ");
      g_string_append (names, monitor_get_display_name (l->data));
    }
  title = g_string_free (names, FALSE);

  if (monitor_get_size (monitor, &width, &height))
    subtitle = g_strdup_printf ("%d × %d%s", width, height,
                                logical_monitor_is_primary (logical_monitor) ?
                                _(" · Primary") : "");
  else if (logical_monitor_is_primary (logical_monitor))
    subtitle = g_strdup (_("Primary"));

  card = create_card (icon, title, subtitle);
  g_object_set_qdata (G_OBJECT (card), quark_monitor_widget_data, monitor);
  return card;
}

/* ---- previews ----------------------------------------------------------- */

static void
thumbnail_round_unref (ThumbnailRound *round)
{
  if (g_ref_count_dec (&round->ref_count))
    g_free (round);
}

/* Outlives the widget if it goes while previews are in flight */
typedef struct
{
  ThumbnailRound *round;
  GtkImage *image;
} ThumbnailTarget;

static void
on_thumbnail (cairo_surface_t *surface,
              gpointer         user_data)
{
  ThumbnailTarget *target = user_data;
  GtkImage *image = target->image;

  target->round->pending--;
  thumbnail_round_unref (target->round);

  if (surface && gtk_widget_get_parent (GTK_WIDGET (image)))
    {
      int scale = gtk_widget_get_scale_factor (GTK_WIDGET (image));

      cairo_surface_set_device_scale (surface, scale, scale);
      gtk_image_set_from_surface (image, surface);
    }
  g_object_unref (image);
  g_free (target);
}

static gpointer
new_target (ScreenCastWidget *widget,
            GtkWidget        *image)
{
  ThumbnailTarget *target = g_new0 (ThumbnailTarget, 1);

  g_ref_count_inc (&widget->thumbnail_round->ref_count);
  target->round = widget->thumbnail_round;
  target->round->pending++;
  target->image = g_object_ref (GTK_IMAGE (image));
  return target;
}

static void
refresh_flow_box_thumbnails (ScreenCastWidget *widget,
                             GtkWidget        *flow_box)
{
  int scale = gtk_widget_get_scale_factor (flow_box);
  g_autoptr(GList) children = NULL;
  GList *l;

  children = gtk_container_get_children (GTK_CONTAINER (flow_box));
  for (l = children; l; l = l->next)
    {
      GtkWidget *card = gtk_bin_get_child (GTK_BIN (l->data));
      GtkWidget *image = g_object_get_qdata (G_OBJECT (card),
                                             quark_thumbnail_image);
      Monitor *monitor = g_object_get_qdata (G_OBJECT (card),
                                             quark_monitor_widget_data);
      uint64_t window_id;

      if (card_get_window_id (card, &window_id))
        shell_introspect_get_thumbnail (widget->shell_introspect,
                                        SCREEN_CAST_SOURCE_TYPE_WINDOW,
                                        window_id, NULL,
                                        THUMBNAIL_WIDTH * scale,
                                        THUMBNAIL_HEIGHT * scale,
                                        widget->thumbnail_cancellable,
                                        on_thumbnail, new_target (widget, image));
      else if (monitor)
        shell_introspect_get_thumbnail (widget->shell_introspect,
                                        SCREEN_CAST_SOURCE_TYPE_MONITOR,
                                        0, monitor_get_connector (monitor),
                                        THUMBNAIL_WIDTH * scale,
                                        THUMBNAIL_HEIGHT * scale,
                                        widget->thumbnail_cancellable,
                                        on_thumbnail, new_target (widget, image));
    }
}

static gboolean
refresh_thumbnails (gpointer user_data)
{
  ScreenCastWidget *widget = user_data;
  GtkWidget *visible_child =
    gtk_stack_get_visible_child (GTK_STACK (widget->source_type));

  if (!widget->thumbnail_round || widget->thumbnail_round->pending > 0)
    return G_SOURCE_CONTINUE;

  if (visible_child == widget->window_selection)
    refresh_flow_box_thumbnails (widget, widget->window_list);
  else if (visible_child == widget->monitor_selection)
    refresh_flow_box_thumbnails (widget, widget->monitor_list);

  return G_SOURCE_CONTINUE;
}

static void
start_thumbnails (ScreenCastWidget *widget)
{
  if (widget->thumbnail_timeout_id)
    return;

  widget->thumbnail_cancellable = g_cancellable_new ();
  widget->thumbnail_round = g_new0 (ThumbnailRound, 1);
  g_ref_count_init (&widget->thumbnail_round->ref_count);
  refresh_thumbnails (widget);
  widget->thumbnail_timeout_id = g_timeout_add (THUMBNAIL_REFRESH_MS,
                                                refresh_thumbnails, widget);
}

static void
stop_thumbnails (ScreenCastWidget *widget)
{
  g_clear_handle_id (&widget->thumbnail_timeout_id, g_source_remove);
  if (widget->thumbnail_cancellable)
    {
      g_cancellable_cancel (widget->thumbnail_cancellable);
      g_clear_object (&widget->thumbnail_cancellable);
    }
  g_clear_pointer (&widget->thumbnail_round, thumbnail_round_unref);
}

/* ---- lists -------------------------------------------------------------- */

static gboolean
should_skip_window (Window    *window,
                    GtkWindow *toplevel)
{
  g_autofree char *processed_app_id = NULL;

  if (g_strcmp0 (window_get_title (window),
                 gtk_window_get_title (toplevel)) != 0)
    return FALSE;

  processed_app_id = g_strdup (window_get_app_id (window));
  if (g_str_has_suffix (processed_app_id, ".desktop"))
    processed_app_id[strlen (processed_app_id) -
                     strlen (".desktop")] = '\0';

  if (g_strcmp0 (processed_app_id, g_get_prgname ()) != 0 &&
      g_strcmp0 (processed_app_id, gdk_get_program_class ()) != 0)
    return FALSE;

  return TRUE;
}

static void
clear_flow_box (GtkWidget *flow_box)
{
  g_autoptr(GList) children = NULL;
  GList *l;

  children = gtk_container_get_children (GTK_CONTAINER (flow_box));
  for (l = children; l; l = l->next)
    gtk_container_remove (GTK_CONTAINER (flow_box), l->data);
}

/* Updates the cards in place as windows open, close or change their titles,
 * so the selection and the previews stay */
static void
update_windows_list (ScreenCastWidget *widget)
{
  g_autoptr(GHashTable) cards = NULL;
  g_autoptr(GList) children = NULL;
  GtkWidget *toplevel;
  GHashTableIter iter;
  gpointer child;
  GList *l;

  toplevel = gtk_widget_get_ancestor (GTK_WIDGET (widget), GTK_TYPE_WINDOW);
  if (!toplevel)
    return;

  cards = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  children = gtk_container_get_children (GTK_CONTAINER (widget->window_list));
  for (l = children; l; l = l->next)
    {
      uint64_t id;

      if (card_get_window_id (gtk_bin_get_child (GTK_BIN (l->data)), &id))
        g_hash_table_insert (cards, g_memdup2 (&id, sizeof (id)), l->data);
    }

  for (l = shell_introspect_get_windows (widget->shell_introspect); l; l = l->next)
    {
      Window *window = l->data;
      uint64_t id = window_get_id (window);
      GtkWidget *existing;

      if (should_skip_window (window, GTK_WINDOW (toplevel)))
        continue;

      existing = g_hash_table_lookup (cards, &id);
      if (existing)
        {
          GtkWidget *label =
            g_object_get_qdata (G_OBJECT (gtk_bin_get_child (GTK_BIN (existing))),
                                quark_title_label);

          gtk_label_set_label (GTK_LABEL (label), window_get_title (window));
          gtk_widget_set_tooltip_text (label, window_get_title (window));
          g_hash_table_remove (cards, &id);
        }
      else
        {
          gtk_container_add (GTK_CONTAINER (widget->window_list),
                             create_window_widget (window));
        }
    }

  /* The windows that are gone */
  g_hash_table_iter_init (&iter, cards);
  while (g_hash_table_iter_next (&iter, NULL, &child))
    gtk_container_remove (GTK_CONTAINER (widget->window_list), child);

  if (widget->thumbnail_timeout_id)
    refresh_thumbnails (widget);
}

static void
update_monitors_list (ScreenCastWidget *widget)
{
  GList *l;

  clear_flow_box (widget->monitor_list);

  for (l = display_state_tracker_get_logical_monitors (widget->display_state_tracker);
       l; l = l->next)
    gtk_container_add (GTK_CONTAINER (widget->monitor_list),
                       create_monitor_widget (l->data));

  if (widget->thumbnail_timeout_id)
    refresh_thumbnails (widget);
}

static void
on_windows_changed (ShellIntrospect  *shell_introspect,
                    ScreenCastWidget *widget)
{
  update_windows_list (widget);
}

static void
connect_windows_changed_listener (ScreenCastWidget *widget)
{
  g_assert (!widget->windows_changed_handler_id);
  widget->windows_changed_handler_id =
    g_signal_connect (widget->shell_introspect,
                      "windows-changed",
                      G_CALLBACK (on_windows_changed),
                      widget);
  shell_introspect_ref_listeners (widget->shell_introspect);
}

static void
disconnect_windows_changed_listener (ScreenCastWidget *widget)
{
  g_assert (widget->windows_changed_handler_id);
  g_signal_handler_disconnect (widget->shell_introspect,
                               widget->windows_changed_handler_id);
  widget->windows_changed_handler_id = 0;
  shell_introspect_unref_listeners (widget->shell_introspect);
}

static void
on_stack_switch (GtkStack   *stack,
                 GParamSpec *pspec,
                 gpointer    data)
{
  ScreenCastWidget *widget = data;
  GtkWidget *visible_child;

  /* Only the shown page's selection is shared */
  gtk_flow_box_unselect_all (GTK_FLOW_BOX (widget->monitor_list));
  gtk_flow_box_unselect_all (GTK_FLOW_BOX (widget->window_list));

  visible_child = gtk_stack_get_visible_child (stack);
  if (visible_child == widget->window_selection)
    {
      if (!widget->windows_changed_handler_id)
        connect_windows_changed_listener (widget);
    }
  else
    {
      if (widget->windows_changed_handler_id)
        disconnect_windows_changed_listener (widget);
    }

  if (widget->thumbnail_timeout_id)
    refresh_thumbnails (widget);
}

static gboolean
emit_selection_change_in_idle_cb (gpointer data)
{
  ScreenCastWidget *widget = data;
  g_autoptr(GList) selected_monitors = NULL;
  g_autoptr(GList) selected_windows = NULL;

  selected_monitors =
    gtk_flow_box_get_selected_children (GTK_FLOW_BOX (widget->monitor_list));
  selected_windows =
    gtk_flow_box_get_selected_children (GTK_FLOW_BOX (widget->window_list));
  g_signal_emit (widget, signals[HAS_SELECTION_CHANGED], 0,
                 selected_monitors || selected_windows);

  widget->selection_changed_timeout_id = 0;
  return G_SOURCE_REMOVE;
}

static void
on_selected_children_changed (GtkFlowBox       *flow_box,
                              ScreenCastWidget *widget)
{
  if (widget->selection_changed_timeout_id > 0)
    return;

  widget->selection_changed_timeout_id =
    g_idle_add (emit_selection_change_in_idle_cb, widget);
}

static void
on_monitors_changed (DisplayStateTracker *display_state_tracker,
                     ScreenCastWidget    *widget)
{
  update_monitors_list (widget);
}

static gboolean
add_selections (ScreenCastWidget *widget,
                GVariantBuilder  *source_selections_builder)
{
  g_autoptr(GList) selected_monitors = NULL;
  g_autoptr(GList) selected_windows = NULL;
  GList *l;

  selected_monitors =
    gtk_flow_box_get_selected_children (GTK_FLOW_BOX (widget->monitor_list));
  selected_windows =
    gtk_flow_box_get_selected_children (GTK_FLOW_BOX (widget->window_list));
  if (!selected_monitors && !selected_windows)
    return FALSE;

  for (l = selected_monitors; l; l = l->next)
    {
      GtkWidget *card = gtk_bin_get_child (GTK_BIN (l->data));
      Monitor *monitor = g_object_get_qdata (G_OBJECT (card),
                                             quark_monitor_widget_data);

      g_variant_builder_add (source_selections_builder, "(us)",
                             SCREEN_CAST_SOURCE_TYPE_MONITOR,
                             monitor_get_connector (monitor));
    }
  for (l = selected_windows; l; l = l->next)
    {
      GtkWidget *card = gtk_bin_get_child (GTK_BIN (l->data));
      uint64_t window_id;

      if (!card_get_window_id (card, &window_id))
        continue;
      g_variant_builder_add (source_selections_builder, "(ut)",
                             SCREEN_CAST_SOURCE_TYPE_WINDOW,
                             window_id);
    }

  return TRUE;
}

void
screen_cast_widget_add_selections (ScreenCastWidget *widget,
                                   GVariantBuilder  *selections_builder)
{
  GVariantBuilder source_selections_builder;

  g_variant_builder_init (&source_selections_builder, G_VARIANT_TYPE ("a(u?)"));
  if (!add_selections (widget, &source_selections_builder))
    {
      g_variant_builder_clear (&source_selections_builder);
    }
  else
    {
      g_variant_builder_add (selections_builder, "{sv}",
                             "selected_screen_cast_sources",
                             g_variant_builder_end (&source_selections_builder));
    }
}

void
screen_cast_widget_set_app_id (ScreenCastWidget *widget,
                               const char       *app_id)
{
  g_autofree char *monitor_heading = NULL;
  g_autofree char *window_heading = NULL;
  g_autoptr(GAppInfo) info = NULL;

  if (app_id && *app_id)
    {
      g_autofree char *id = g_strconcat (app_id, ".desktop", NULL);

      info = G_APP_INFO (g_desktop_app_info_new (id));
    }

  if (info)
    {
      const char *display_name = g_app_info_get_display_name (info);

      monitor_heading = g_strdup_printf (_("Select a screen to share with %s"),
                                         display_name);
      window_heading = g_strdup_printf (_("Select a window to share with %s"),
                                        display_name);
    }
  else
    {
      monitor_heading = g_strdup (_("Select a screen to share with the requesting application"));
      window_heading = g_strdup (_("Select a window to share with the requesting application"));
    }

  gtk_label_set_label (GTK_LABEL (widget->monitor_heading), monitor_heading);
  gtk_label_set_label (GTK_LABEL (widget->window_heading), window_heading);
}

void
screen_cast_widget_set_allow_multiple (ScreenCastWidget *widget,
                                       gboolean          multiple)
{
  GtkSelectionMode mode = multiple ? GTK_SELECTION_MULTIPLE
                                   : GTK_SELECTION_SINGLE;

  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (widget->monitor_list), mode);
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (widget->window_list), mode);
}

void
screen_cast_widget_set_source_types (ScreenCastWidget     *screen_cast_widget,
                                     ScreenCastSourceType  source_types)
{
  if (source_types & SCREEN_CAST_SOURCE_TYPE_MONITOR)
    gtk_widget_show (screen_cast_widget->monitor_selection);

  if (source_types & SCREEN_CAST_SOURCE_TYPE_WINDOW)
    gtk_widget_show (screen_cast_widget->window_selection);

  if (__builtin_popcount (source_types) > 1)
    gtk_widget_show (screen_cast_widget->source_type_switcher);
}

/* Smaller lists, for dialogs that show more than the picker */
void
screen_cast_widget_set_compact (ScreenCastWidget *widget)
{
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (widget->window_list_scrolled),
                                              180);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (widget->monitor_list_scrolled),
                                              180);
}

static void
screen_cast_widget_map (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (screen_cast_widget_parent_class)->map (widget);

  start_thumbnails (SCREEN_CAST_WIDGET (widget));
}

static void
screen_cast_widget_unmap (GtkWidget *widget)
{
  stop_thumbnails (SCREEN_CAST_WIDGET (widget));

  GTK_WIDGET_CLASS (screen_cast_widget_parent_class)->unmap (widget);
}

static void
screen_cast_widget_finalize (GObject *object)
{
  ScreenCastWidget *widget = SCREEN_CAST_WIDGET (object);

  stop_thumbnails (widget);

  g_signal_handler_disconnect (widget->display_state_tracker,
                               widget->monitors_changed_handler_id);

  if (widget->windows_changed_handler_id)
    disconnect_windows_changed_listener (widget);

  g_clear_handle_id (&widget->selection_changed_timeout_id, g_source_remove);

  G_OBJECT_CLASS (screen_cast_widget_parent_class)->finalize (object);
}

static void
screen_cast_widget_init (ScreenCastWidget *widget)
{
  gtk_widget_init_template (GTK_WIDGET (widget));

  screen_cast_widget_set_app_id (widget, NULL);
  screen_cast_widget_set_allow_multiple (widget, FALSE);

  g_signal_connect (widget->source_type, "notify::visible-child",
                    G_CALLBACK (on_stack_switch),
                    widget);
  g_signal_connect (widget->monitor_list, "selected-children-changed",
                    G_CALLBACK (on_selected_children_changed),
                    widget);
  g_signal_connect (widget->window_list, "selected-children-changed",
                    G_CALLBACK (on_selected_children_changed),
                    widget);

  widget->display_state_tracker = display_state_tracker_get ();
  widget->monitors_changed_handler_id =
    g_signal_connect (widget->display_state_tracker,
                      "monitors-changed",
                      G_CALLBACK (on_monitors_changed),
                      widget);
  widget->shell_introspect = shell_introspect_get ();

  update_monitors_list (widget);
  update_windows_list (widget);
}

static void
screen_cast_widget_class_init (ScreenCastWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = screen_cast_widget_finalize;
  widget_class->map = screen_cast_widget_map;
  widget_class->unmap = screen_cast_widget_unmap;

  signals[HAS_SELECTION_CHANGED] = g_signal_new ("has-selection-changed",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL, NULL,
                                                 NULL,
                                                 G_TYPE_NONE, 1,
                                                 G_TYPE_BOOLEAN);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/xapp/screencastwidget.ui");
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, source_type_switcher);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, source_type);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_selection);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_selection);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_heading);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_list);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_list_scrolled);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_heading);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_list);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_list_scrolled);

  quark_monitor_widget_data = g_quark_from_static_string ("-monitor-widget-connector-quark");
  quark_window_widget_data = g_quark_from_static_string ("-window-widget-connector-quark");
  quark_title_label = g_quark_from_static_string ("-title-label-quark");
  quark_thumbnail_image = g_quark_from_static_string ("-thumbnail-image-quark");
}

void
init_screen_cast_widget (void)
{
  g_type_ensure (screen_cast_widget_get_type ());
}
