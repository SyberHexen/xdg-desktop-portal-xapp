/*
 * Copyright © 2019 Alberto Fanjul <albfan@gnome.org>
 * Copyright © 2019 Red Hat, Inc
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

/* The windows that can be shared, from Cinnamon's org.cinnamon.PortalHandlers
 * GetWindows (Cinnamon's counterpart of org.gnome.Shell.Introspect) */

#include "config.h"

#include <limits.h>
#include <string.h>

#include "common-dbus.h"
#include "shellintrospect.h"

enum
{
  WINDOWS_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _Window
{
  uint64_t id;
  char *title;
  char *app_id;
};

struct _ShellIntrospect
{
  GObject parent;

  guint watch_name_id;
  GCancellable *cancellable;

  OrgCinnamonPortalHandlers *proxy;

  GList *windows;

  int num_listeners;
};

G_DEFINE_TYPE (ShellIntrospect, shell_introspect, G_TYPE_OBJECT)

static ShellIntrospect *_shell_introspect;

static void
window_free (Window *window)
{
  g_free (window->title);
  g_free (window->app_id);
  g_free (window);
}

const char *
window_get_title (Window *window)
{
  return window->title;
}

const char *
window_get_app_id (Window *window)
{
  return window->app_id;
}

const uint64_t
window_get_id (Window *window)
{
  return window->id;
}

static void
get_windows_cb (GObject      *source_object,
                GAsyncResult *res,
                gpointer      user_data)
{
  ShellIntrospect *shell_introspect = user_data;
  g_autoptr(GVariant) windows_variant = NULL;
  g_autoptr(GError) error = NULL;
  GVariantIter iter;
  uint64_t id;
  GVariant *params = NULL;
  GList *windows = NULL;

  if (!org_cinnamon_portal_handlers_call_get_windows_finish (ORG_CINNAMON_PORTAL_HANDLERS (source_object),
                                                             &windows_variant,
                                                             res,
                                                             &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to get window list: %s", error->message);
      return;
    }

  g_list_free_full (shell_introspect->windows, (GDestroyNotify) window_free);
  shell_introspect->windows = NULL;

  g_variant_iter_init (&iter, windows_variant);
  while (g_variant_iter_loop (&iter, "{t@a{sv}}", &id, &params))
    {
      Window *window = g_new0 (Window, 1);

      window->id = id;
      g_variant_lookup (params, "app-id", "s", &window->app_id);
      g_variant_lookup (params, "title", "s", &window->title);
      windows = g_list_prepend (windows, window);
    }

  shell_introspect->windows = g_list_reverse (windows);
  g_signal_emit (shell_introspect, signals[WINDOWS_CHANGED], 0);
}

static void
sync_state (ShellIntrospect *shell_introspect)
{
  org_cinnamon_portal_handlers_call_get_windows (shell_introspect->proxy,
                                                 shell_introspect->cancellable,
                                                 get_windows_cb,
                                                 shell_introspect);
}

static GVariant *
get_windows_sync (ShellIntrospect *shell_introspect)
{
  g_autoptr(GError) error = NULL;
  GVariant *windows = NULL;

  if (!shell_introspect->proxy ||
      !org_cinnamon_portal_handlers_call_get_windows_sync (shell_introspect->proxy,
                                                          &windows, NULL,
                                                          &error))
    {
      if (error)
        g_warning ("Failed to get window list: %s", error->message);
      return NULL;
    }

  return windows;
}

/* The app ID and title of the window @id, if it still exists */
gboolean
shell_introspect_lookup_window (ShellIntrospect  *shell_introspect,
                                uint64_t          id,
                                char            **app_id,
                                char            **title)
{
  g_autoptr(GVariant) windows = get_windows_sync (shell_introspect);
  GVariantIter iter;
  uint64_t window_id;
  GVariant *props;

  if (!windows)
    return FALSE;

  g_variant_iter_init (&iter, windows);
  while (g_variant_iter_next (&iter, "{t@a{sv}}", &window_id, &props))
    {
      if (window_id == id)
        {
          if (!g_variant_lookup (props, "app-id", "s", app_id))
            *app_id = g_strdup ("");
          if (!g_variant_lookup (props, "title", "s", title))
            *title = g_strdup ("");
          g_variant_unref (props);
          return TRUE;
        }
      g_variant_unref (props);
    }

  return FALSE;
}

/* A window to restore a screen cast of: @id if it still is the same app's,
 * else the app's window titled @title, else its only window */
gboolean
shell_introspect_find_window (ShellIntrospect *shell_introspect,
                              uint64_t         id,
                              const char      *app_id,
                              const char      *title,
                              uint64_t        *out_id)
{
  g_autoptr(GVariant) windows = get_windows_sync (shell_introspect);
  GVariantIter iter;
  uint64_t window_id;
  GVariant *props;
  uint64_t by_title = 0, only = 0;
  int n_app_windows = 0;
  gboolean found_id = FALSE;

  if (!windows || !app_id || !*app_id)
    return FALSE;

  g_variant_iter_init (&iter, windows);
  while (g_variant_iter_next (&iter, "{t@a{sv}}", &window_id, &props))
    {
      const char *window_app_id = "", *window_title = "";

      g_variant_lookup (props, "app-id", "&s", &window_app_id);
      g_variant_lookup (props, "title", "&s", &window_title);

      if (g_strcmp0 (window_app_id, app_id) == 0)
        {
          n_app_windows++;
          only = window_id;
          if (window_id == id)
            found_id = TRUE;
          if (!by_title && g_strcmp0 (window_title, title) == 0)
            by_title = window_id;
        }
      g_variant_unref (props);
    }

  if (found_id)
    *out_id = id;
  else if (by_title)
    *out_id = by_title;
  else if (n_app_windows == 1)
    *out_id = only;
  else
    return FALSE;

  return TRUE;
}

typedef struct
{
  ThumbnailCallback callback;
  gpointer user_data;
} ThumbnailRequest;

static void
thumbnail_cb (GObject      *source_object,
              GAsyncResult *res,
              gpointer      user_data)
{
  ThumbnailRequest *request = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) pixels = NULL;
  g_autoptr(GError) error = NULL;
  cairo_surface_t *surface = NULL;
  int32_t width, height, stride;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (!reply)
    {
      /* The callback still runs (with NULL) to release what it holds */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Failed to get thumbnail: %s", error->message);
    }
  else
    {
      g_variant_get (reply, "(iii@ay)", &width, &height, &stride, &pixels);
      if (width > 0 && height > 0 &&
          stride == cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, width) &&
          g_variant_get_size (pixels) == (gsize) stride * height)
        {
          surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                                width, height);
          cairo_surface_flush (surface);
          memcpy (cairo_image_surface_get_data (surface),
                  g_variant_get_data (pixels), (gsize) stride * height);
          cairo_surface_mark_dirty (surface);
        }
    }

  request->callback (surface, request->user_data);
  g_clear_pointer (&surface, cairo_surface_destroy);
  g_free (request);
}

void
shell_introspect_get_thumbnail (ShellIntrospect   *shell_introspect,
                                uint32_t           source_type,
                                uint64_t           id,
                                const char        *connector,
                                int                max_width,
                                int                max_height,
                                GCancellable      *cancellable,
                                ThumbnailCallback  callback,
                                gpointer           user_data)
{
  ThumbnailRequest *request;

  if (!shell_introspect->proxy)
    {
      callback (NULL, user_data);
      return;
    }

  request = g_new0 (ThumbnailRequest, 1);
  request->callback = callback;
  request->user_data = user_data;

  /* Thumbnails need Cinnamon's thumbnail support; without it the call fails
   * and the picker shows icons only */
  if (source_type == 1)
    g_dbus_proxy_call (G_DBUS_PROXY (shell_introspect->proxy),
                       "GetMonitorThumbnail",
                       g_variant_new ("(sii)", connector, max_width, max_height),
                       G_DBUS_CALL_FLAGS_NONE, 2000, cancellable,
                       thumbnail_cb, request);
  else
    g_dbus_proxy_call (G_DBUS_PROXY (shell_introspect->proxy),
                       "GetWindowThumbnail",
                       g_variant_new ("(tii)", id, max_width, max_height),
                       G_DBUS_CALL_FLAGS_NONE, 2000, cancellable,
                       thumbnail_cb, request);
}

GList *
shell_introspect_get_windows (ShellIntrospect *shell_introspect)
{
  return shell_introspect->windows;
}

void
shell_introspect_ref_listeners (ShellIntrospect *shell_introspect)
{
  shell_introspect->num_listeners++;

  if (shell_introspect->proxy)
    sync_state (shell_introspect);
}

void
shell_introspect_unref_listeners (ShellIntrospect *shell_introspect)
{
  g_return_if_fail (shell_introspect->num_listeners > 0);

  shell_introspect->num_listeners--;
  if (shell_introspect->num_listeners == 0)
    {
      g_list_free_full (shell_introspect->windows,
                        (GDestroyNotify) window_free);
      shell_introspect->windows = NULL;
    }
}

gboolean
shell_introspect_are_animations_enabled (ShellIntrospect *shell_introspect,
                                         gboolean        *out_animations_enabled)
{
  return FALSE;
}

/* Windows opened, closed or retitled while the picker shows them */
static void
on_windows_changed (OrgCinnamonPortalHandlers *proxy,
                    ShellIntrospect           *shell_introspect)
{
  if (shell_introspect->num_listeners > 0)
    sync_state (shell_introspect);
}

static void
on_proxy_acquired (GObject      *object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  ShellIntrospect *shell_introspect = user_data;
  OrgCinnamonPortalHandlers *proxy;
  g_autoptr(GError) error = NULL;

  proxy = org_cinnamon_portal_handlers_proxy_new_for_bus_finish (result, &error);
  if (!proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to acquire org.cinnamon.PortalHandlers proxy: %s",
                   error->message);
      return;
    }

  shell_introspect->proxy = proxy;
  g_signal_connect (proxy, "windows-changed",
                    G_CALLBACK (on_windows_changed), shell_introspect);

  if (shell_introspect->num_listeners > 0)
    sync_state (shell_introspect);
}

static void
on_name_appeared (GDBusConnection *connection,
                  const char      *name,
                  const char      *name_owner,
                  gpointer         user_data)
{
  ShellIntrospect *shell_introspect = user_data;

  shell_introspect->cancellable = g_cancellable_new ();
  org_cinnamon_portal_handlers_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                  G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                  "org.Cinnamon",
                                                  "/org/Cinnamon",
                                                  shell_introspect->cancellable,
                                                  on_proxy_acquired,
                                                  shell_introspect);
}

static void
on_name_vanished (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  ShellIntrospect *shell_introspect = user_data;

  if (shell_introspect->cancellable)
    {
      g_cancellable_cancel (shell_introspect->cancellable);
      g_clear_object (&shell_introspect->cancellable);
    }
  g_clear_object (&shell_introspect->proxy);
}

ShellIntrospect *
shell_introspect_get (void)
{
  ShellIntrospect *shell_introspect;

  if (_shell_introspect)
    return _shell_introspect;

  shell_introspect = g_object_new (shell_introspect_get_type (), NULL);
  shell_introspect->watch_name_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      "org.Cinnamon",
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_name_appeared,
                      on_name_vanished,
                      shell_introspect, NULL);
  _shell_introspect = shell_introspect;
  return shell_introspect;
}

static void
shell_introspect_init (ShellIntrospect *shell_introspect)
{
}

static void
shell_introspect_class_init (ShellIntrospectClass *klass)
{
  signals[WINDOWS_CHANGED] = g_signal_new ("windows-changed",
                                           G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL, NULL, NULL,
                                           G_TYPE_NONE, 0);
}
