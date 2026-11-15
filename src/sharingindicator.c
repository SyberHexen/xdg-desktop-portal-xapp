/*
 * Copyright © 2026 Andrew Hexen
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
 */

/*
 * While anything shares the screen or controls the computer through the
 * portal, a status icon says so (Cinnamon shows it in the panel), and its
 * menu stops them.  An application whose choice was remembered starts
 * without a dialog, so this is where the user sees it and can end it.
 */

#include "config.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libxapp/xapp-status-icon.h>

#include "sharingindicator.h"

typedef struct
{
  Session *session;
  char *app_name;
  char *what;
  gboolean remote_control;
} SharingEntry;

static GList *entries;
static XAppStatusIcon *status_icon;
static guint update_id;
static gulong theme_changed_id;

/* The badge: a glyph in white on the theme's warning color */
#define MINT_WARNING_COLOR "#F27835"   /* Mint-Y's warning_color */

static const char screen_glyph[] =
  "<rect x=\"3\" y=\"3.75\" width=\"10\" height=\"6.5\" rx=\"1\" fill=\"none\" "
  "stroke=\"#ffffff\" stroke-width=\"1.5\"/>"
  "<path d=\"M6 12.5h4\" stroke=\"#ffffff\" stroke-width=\"1.5\" "
  "stroke-linecap=\"round\"/>";
static const char pointer_glyph[] =
  "<path d=\"M5 3.25v9l2.4-2.3 1.7 3.6 1.6-0.75-1.7-3.55h3.3z\" fill=\"#ffffff\" "
  "stroke=\"#ffffff\" stroke-width=\"0.6\" stroke-linejoin=\"round\"/>";

static void schedule_update (void);

static char *
get_app_name (const char *app_id)
{
  g_autoptr(GDesktopAppInfo) info = NULL;

  if (app_id && *app_id)
    {
      g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);

      info = g_desktop_app_info_new (desktop_id);
    }

  if (info)
    return g_strdup (g_app_info_get_display_name (G_APP_INFO (info)));
  if (app_id && *app_id)
    return g_strdup (app_id);

  return g_strdup (_("An application"));
}

static char *
describe_entry (SharingEntry *entry)
{
  if (entry->remote_control && entry->what)
    return g_strdup_printf (_("%s is controlling this computer and sees %s"),
                            entry->app_name, entry->what);
  if (entry->remote_control)
    return g_strdup_printf (_("%s is controlling this computer"),
                            entry->app_name);
  if (entry->what)
    return g_strdup_printf (_("%s is sharing %s"), entry->app_name,
                            entry->what);

  return g_strdup_printf (_("%s is sharing the screen"), entry->app_name);
}

static void
stop_session (Session *session)
{
  if (!session->closed)
    session_close (session);
}

static void
on_stop_activate (GtkMenuItem *item,
                  Session     *session)
{
  g_object_ref (session);
  stop_session (session);
  g_object_unref (session);
}

static void
on_stop_all_activate (GtkMenuItem *item,
                      gpointer     user_data)
{
  g_autoptr(GList) sessions = NULL;
  GList *l;

  for (l = entries; l; l = l->next)
    sessions = g_list_prepend (sessions,
                               g_object_ref (((SharingEntry *) l->data)->session));

  for (l = sessions; l; l = l->next)
    {
      stop_session (l->data);
      g_object_unref (l->data);
    }
}

static GtkWidget *
build_menu (void)
{
  GtkWidget *menu = gtk_menu_new ();
  GList *l;

  for (l = entries; l; l = l->next)
    {
      SharingEntry *entry = l->data;
      g_autofree char *description = describe_entry (entry);
      g_autofree char *stop_label = NULL;
      GtkWidget *item;

      item = gtk_menu_item_new_with_label (description);
      gtk_widget_set_sensitive (item, FALSE);
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

      stop_label = g_strdup_printf (entry->remote_control ?
                                      _("Stop %s") :
                                      _("Stop sharing with %s"),
                                    entry->app_name);
      item = gtk_menu_item_new_with_label (stop_label);
      g_signal_connect_object (item, "activate",
                               G_CALLBACK (on_stop_activate),
                               entry->session, 0);
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

      gtk_menu_shell_append (GTK_MENU_SHELL (menu),
                             gtk_separator_menu_item_new ());
    }

  if (g_list_length (entries) > 1)
    {
      GtkWidget *item = gtk_menu_item_new_with_label (_("Stop all"));

      g_signal_connect (item, "activate",
                        G_CALLBACK (on_stop_all_activate), NULL);
      gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    }

  gtk_widget_show_all (menu);
  return menu;
}

/* The theme's warning color (as GTK themes define it, warning_color), else
 * Mint's: how the desktop already says "pay attention" */
static char *
get_warning_color (void)
{
  g_autoptr(GtkStyleContext) context = gtk_style_context_new ();
  g_autoptr(GtkWidgetPath) path = gtk_widget_path_new ();
  GdkRGBA rgba;

  gtk_widget_path_append_type (path, GTK_TYPE_WINDOW);
  gtk_style_context_set_path (context, path);
  gtk_style_context_set_screen (context, gdk_screen_get_default ());

  if (!gtk_style_context_lookup_color (context, "warning_color", &rgba))
    return g_strdup (MINT_WARNING_COLOR);

  return g_strdup_printf ("#%02x%02x%02x",
                          (int) (rgba.red * 255 + 0.5),
                          (int) (rgba.green * 255 + 0.5),
                          (int) (rgba.blue * 255 + 0.5));
}

/* An icon file in that color (the panel shows files as they are instead of
 * tinting them like the theme's icons) */
static char *
get_badge_path (gboolean remote_control)
{
  g_autofree char *color = get_warning_color ();
  g_autofree char *dir = g_build_filename (g_get_user_runtime_dir (),
                                           "xdg-desktop-portal-xapp", NULL);
  g_autofree char *name = g_strdup_printf ("%s-%s.svg",
                                           remote_control ? "remote-control" :
                                                            "screen-sharing",
                                           color + 1);
  char *path = g_build_filename (dir, name, NULL);

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      g_autofree char *svg =
        g_strdup_printf ("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" "
                         "height=\"16\" viewBox=\"0 0 16 16\">"
                         "<rect x=\"0\" y=\"0.5\" width=\"16\" height=\"15\" "
                         "rx=\"3.5\" fill=\"%s\"/>%s</svg>\n",
                         color, remote_control ? pointer_glyph : screen_glyph);

      g_mkdir_with_parents (dir, 0700);
      if (!g_file_set_contents (path, svg, -1, NULL))
        {
          g_free (path);
          return g_strdup (remote_control ?
                             "preferences-desktop-remote-desktop-symbolic" :
                             "screen-shared-symbolic");
        }
    }

  return path;
}

static void
on_theme_changed (GtkSettings *settings,
                  GParamSpec  *pspec,
                  gpointer     user_data)
{
  if (entries)
    schedule_update ();
}

static gboolean
update_status_icon (gpointer user_data)
{
  g_autofree char *badge_path = NULL;
  GString *tooltip;
  gboolean remote_control = FALSE;
  GList *l;

  update_id = 0;

  if (!entries)
    {
      if (status_icon)
        xapp_status_icon_set_visible (status_icon, FALSE);
      g_clear_object (&status_icon);
      return G_SOURCE_REMOVE;
    }

  /* Needs a display; the portal also runs without one */
  if (!gdk_display_get_default ())
    return G_SOURCE_REMOVE;

  if (!status_icon)
    {
      status_icon = xapp_status_icon_new ();
      xapp_status_icon_set_name (status_icon, "xdg-desktop-portal-xapp-sharing");
    }

  if (!theme_changed_id)
    theme_changed_id = g_signal_connect (gtk_settings_get_default (),
                                         "notify::gtk-theme-name",
                                         G_CALLBACK (on_theme_changed), NULL);

  tooltip = g_string_new (NULL);
  for (l = entries; l; l = l->next)
    {
      SharingEntry *entry = l->data;
      g_autofree char *description = describe_entry (entry);

      remote_control |= entry->remote_control;
      if (tooltip->len)
        g_string_append_c (tooltip, '\n');
      g_string_append (tooltip, description);
    }

  /* In the theme's warning color, as other desktops warn of it */
  badge_path = get_badge_path (remote_control);
  xapp_status_icon_set_icon_name (status_icon, badge_path);
  xapp_status_icon_set_tooltip_text (status_icon, tooltip->str);
  g_string_free (tooltip, TRUE);

  /* Either button opens the menu */
  xapp_status_icon_set_primary_menu (status_icon, GTK_MENU (build_menu ()));
  xapp_status_icon_set_secondary_menu (status_icon, GTK_MENU (build_menu ()));
  xapp_status_icon_set_visible (status_icon, TRUE);

  return G_SOURCE_REMOVE;
}

/* Not right away: a session may end from a menu item of the menu that is
 * about to be replaced */
static void
schedule_update (void)
{
  if (!update_id)
    update_id = g_idle_add (update_status_icon, NULL);
}

void
sharing_indicator_add (Session    *session,
                       const char *app_id,
                       const char *what,
                       gboolean    remote_control)
{
  SharingEntry *entry;

  sharing_indicator_remove (session);

  entry = g_new0 (SharingEntry, 1);
  entry->session = g_object_ref (session);
  entry->app_name = get_app_name (app_id);
  entry->what = g_strdup (what);
  entry->remote_control = remote_control;
  entries = g_list_append (entries, entry);

  schedule_update ();
}

void
sharing_indicator_remove (Session *session)
{
  GList *l;

  for (l = entries; l; l = l->next)
    {
      SharingEntry *entry = l->data;

      if (entry->session == session)
        {
          entries = g_list_delete_link (entries, l);
          g_object_unref (entry->session);
          g_free (entry->app_name);
          g_free (entry->what);
          g_free (entry);
          schedule_update ();
          return;
        }
    }
}
