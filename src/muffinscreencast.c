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

#include "muffinscreencast.h"
#include "screencastwidget.h"
#include "common-dbus.h"

#include <stdint.h>

enum
{
  STREAM_SIGNAL_READY,

  N_STREAM_SIGNALS
};

guint stream_signals[N_STREAM_SIGNALS];

enum
{
  SESSION_SIGNAL_READY,
  SESSION_SIGNAL_CLOSED,

  N_SESSION_SIGNALS
};

guint session_signals[N_SESSION_SIGNALS];

enum
{
  ENABLED,
  DISABLED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct _MuffinScreenCastStream
{
  GObject parent;

  MuffinScreenCastSession *session;

  ScreenCastSourceType source_type;

  char *path;
  OrgCinnamonMuffinScreenCastStream *proxy;

  uint32_t pipewire_node_id;

  gboolean has_position;
  int x;
  int y;

  gboolean has_size;
  int width;
  int height;
} MuffinScreenCastStream;

typedef struct _MuffinScreenCastStreamClass
{
  GObjectClass parent_class;
} MuffinScreenCastStreamClass;

typedef struct _MuffinScreenCastSession
{
  GObject parent;

  char *path;
  OrgCinnamonMuffinScreenCastSession *proxy;
  gulong closed_handler_id;

  GList *streams;
  int n_needed_stream_node_ids;

  /* Muffin ended the session itself (its streams went away) */
  gboolean closed;
} MuffinScreenCastSession;

typedef struct _MuffinScreenCastSessionClass
{
  GObjectClass parent_class;
} MuffinScreenCastSessionClass;

typedef struct _MuffinScreenCast
{
  GObject parent;

  int api_version;

  guint screen_cast_name_watch;
  OrgCinnamonMuffinScreenCast *proxy;
} MuffinScreenCast;

typedef struct _MuffinScreenCastClass
{
  GObjectClass parent_class;
} MuffinScreenCastClass;

static GType muffin_screen_cast_stream_get_type (void);
G_DEFINE_TYPE (MuffinScreenCastStream, muffin_screen_cast_stream, G_TYPE_OBJECT)

static GType muffin_screen_cast_session_get_type (void);
G_DEFINE_TYPE (MuffinScreenCastSession, muffin_screen_cast_session, G_TYPE_OBJECT)

static GType muffin_screen_cast_get_type (void);
G_DEFINE_TYPE (MuffinScreenCast, muffin_screen_cast, G_TYPE_OBJECT)

static uint32_t
muffin_screen_cast_stream_get_pipewire_node_id (MuffinScreenCastStream *stream)
{
  return stream->pipewire_node_id;
}

static gboolean
muffin_screen_cast_stream_get_position (MuffinScreenCastStream *stream,
                                       int *x,
                                       int *y)
{
  if (!stream->has_position)
    return FALSE;

  *x = stream->x;
  *y = stream->y;

  return TRUE;
}

static gboolean
muffin_screen_cast_stream_get_size (MuffinScreenCastStream *stream,
                                   int *width,
                                   int *height)
{
  if (!stream->has_size)
    return FALSE;

  *width = stream->width;
  *height = stream->height;

  return TRUE;
}

static void
muffin_screen_cast_stream_finalize (GObject *object)
{
  MuffinScreenCastStream *stream = (MuffinScreenCastStream *)object;

  g_clear_object (&stream->proxy);
  g_free (stream->path);

  G_OBJECT_CLASS (muffin_screen_cast_stream_parent_class)->finalize (object);
}

static void
on_pipewire_stream_added (OrgCinnamonMuffinScreenCastStream *stream_proxy,
                          unsigned int arg_node_id,
                          MuffinScreenCastStream *stream)
{
  stream->pipewire_node_id = arg_node_id;
  g_signal_emit (stream, stream_signals[STREAM_SIGNAL_READY], 0);

  stream->session->n_needed_stream_node_ids--;
  if (stream->session->n_needed_stream_node_ids == 0)
    g_signal_emit (stream->session, session_signals[SESSION_SIGNAL_READY], 0);
}

static void
muffin_screen_cast_stream_init (MuffinScreenCastStream *stream)
{
}

static void
muffin_screen_cast_stream_class_init (MuffinScreenCastStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = muffin_screen_cast_stream_finalize;

  stream_signals[STREAM_SIGNAL_READY] = g_signal_new ("ready",
                                                      G_TYPE_FROM_CLASS (klass),
                                                      G_SIGNAL_RUN_LAST,
                                                      0,
                                                      NULL, NULL,
                                                      NULL,
                                                      G_TYPE_NONE, 0);
}

const char *
muffin_screen_cast_session_get_stream_path_from_id (MuffinScreenCastSession *muffin_screen_cast_session,
                                                   uint32_t stream_id)
{
  GList *l;

  for (l = muffin_screen_cast_session->streams; l; l = l->next)
    {
      MuffinScreenCastStream *stream = l->data;

      if (stream->pipewire_node_id == stream_id)
        return stream->path;
    }

  return NULL;
}

void
muffin_screen_cast_session_add_stream_properties (MuffinScreenCastSession *muffin_screen_cast_session,
                                                 GVariantBuilder *streams_builder)
{
  GList *streams;
  GList *l;

  streams = muffin_screen_cast_session->streams;
  for (l = streams; l; l = l->next)
    {
      MuffinScreenCastStream *stream = l->data;
      GVariantBuilder stream_properties_builder;
      int x, y;
      int width, height;
      uint32_t pipewire_node_id;


      g_variant_builder_init (&stream_properties_builder, G_VARIANT_TYPE_VARDICT);

      g_variant_builder_add (&stream_properties_builder, "{sv}",
                             "source_type",
                             g_variant_new ("u", stream->source_type));

      if (muffin_screen_cast_stream_get_position (stream, &x, &y))
        g_variant_builder_add (&stream_properties_builder, "{sv}",
                               "position",
                               g_variant_new ("(ii)", x, y));
      if (muffin_screen_cast_stream_get_size (stream, &width, &height))
        g_variant_builder_add (&stream_properties_builder, "{sv}",
                               "size",
                               g_variant_new ("(ii)", width, height));

      pipewire_node_id = muffin_screen_cast_stream_get_pipewire_node_id (stream);
      g_variant_builder_add (streams_builder, "(ua{sv})",
                             pipewire_node_id,
                             &stream_properties_builder);
    }
}

static uint32_t
cursor_mode_to_muffin_cursor_mode (ScreenCastCursorMode cursor_mode)
{
  switch (cursor_mode)
    {
    case SCREEN_CAST_CURSOR_MODE_NONE:
      g_assert_not_reached ();
      return -1;
    case SCREEN_CAST_CURSOR_MODE_HIDDEN:
      return 0;
    case SCREEN_CAST_CURSOR_MODE_EMBEDDED:
      return 1;
    case SCREEN_CAST_CURSOR_MODE_METADATA:
      return 2;
    }

  g_assert_not_reached ();
}

static gboolean
muffin_screen_cast_session_record_window (MuffinScreenCastSession *muffin_screen_cast_session,
                                         const guint64 id,
                                         ScreenCastSelection *select,
                                         GError **error)
{
  OrgCinnamonMuffinScreenCastSession *session_proxy =
    muffin_screen_cast_session->proxy;
  GVariantBuilder properties_builder;
  GVariant *properties;
  g_autofree char *stream_path = NULL;
  GDBusConnection *connection;
  OrgCinnamonMuffinScreenCastStream *stream_proxy;
  MuffinScreenCastStream *stream;
  GVariant *parameters;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&properties_builder, "{sv}",
                         "window-id",
                         g_variant_new_uint64 (id));
  if (select->cursor_mode)
    {
      uint32_t muffin_cursor_mode;

      muffin_cursor_mode = cursor_mode_to_muffin_cursor_mode (select->cursor_mode);
      g_variant_builder_add (&properties_builder, "{sv}",
                             "cursor-mode",
                             g_variant_new_uint32 (muffin_cursor_mode));
    }
  properties = g_variant_builder_end (&properties_builder);

  if (!org_cinnamon_muffin_screen_cast_session_call_record_window_sync (session_proxy,
                                                                     properties,
                                                                     &stream_path,
                                                                     NULL,
                                                                     error))
    return FALSE;

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (session_proxy));
  stream_proxy =
    org_cinnamon_muffin_screen_cast_stream_proxy_new_sync (connection,
                                                        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                        "org.cinnamon.Muffin.ScreenCast",
                                                        stream_path,
                                                        NULL,
                                                        error);
  if (!stream_proxy)
    return FALSE;

  stream = g_object_new (muffin_screen_cast_stream_get_type (), NULL);
  stream->source_type = SCREEN_CAST_SOURCE_TYPE_WINDOW;
  stream->session = muffin_screen_cast_session;
  stream->path = g_strdup (stream_path);
  stream->proxy = stream_proxy;

  parameters = org_cinnamon_muffin_screen_cast_stream_get_parameters (stream->proxy);
  if (parameters)
    {
      if (g_variant_lookup (parameters, "position", "(ii)",
                            &stream->x, &stream->y))
        stream->has_position = TRUE;
      if (g_variant_lookup (parameters, "size", "(ii)",
                            &stream->width, &stream->height))
        stream->has_size = TRUE;
    }
  else
    {
      g_warning ("Screen cast stream %s missing parameters",
                 stream->path);
    }

  g_signal_connect (stream_proxy, "pipewire-stream-added",
                    G_CALLBACK (on_pipewire_stream_added),
                    stream);

  muffin_screen_cast_session->streams =
    g_list_prepend (muffin_screen_cast_session->streams, stream);
  muffin_screen_cast_session->n_needed_stream_node_ids++;

  return TRUE;
}

static gboolean
muffin_screen_cast_session_record_monitor (MuffinScreenCastSession *muffin_screen_cast_session,
                                          const char *connector,
                                          ScreenCastSelection *select,
                                          GError **error)
{
  OrgCinnamonMuffinScreenCastSession *session_proxy =
    muffin_screen_cast_session->proxy;
  GVariantBuilder properties_builder;
  GVariant *properties;
  g_autofree char *stream_path = NULL;
  GDBusConnection *connection;
  OrgCinnamonMuffinScreenCastStream *stream_proxy;
  MuffinScreenCastStream *stream;
  GVariant *parameters;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE_VARDICT);
  if (select->cursor_mode)
    {
      uint32_t muffin_cursor_mode;

      muffin_cursor_mode = cursor_mode_to_muffin_cursor_mode (select->cursor_mode);
      g_variant_builder_add (&properties_builder, "{sv}",
                             "cursor-mode",
                             g_variant_new_uint32 (muffin_cursor_mode));
    }
  properties = g_variant_builder_end (&properties_builder);

  if (!org_cinnamon_muffin_screen_cast_session_call_record_monitor_sync (session_proxy,
                                                                      connector,
                                                                      properties,
                                                                      &stream_path,
                                                                      NULL,
                                                                      error))
    return FALSE;

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (session_proxy));
  stream_proxy =
    org_cinnamon_muffin_screen_cast_stream_proxy_new_sync (connection,
                                                        G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                        "org.cinnamon.Muffin.ScreenCast",
                                                        stream_path,
                                                        NULL,
                                                        error);
  if (!stream_proxy)
    return FALSE;

  stream = g_object_new (muffin_screen_cast_stream_get_type (), NULL);
  stream->source_type = SCREEN_CAST_SOURCE_TYPE_MONITOR;
  stream->session = muffin_screen_cast_session;
  stream->path = g_strdup (stream_path);
  stream->proxy = stream_proxy;

  parameters = org_cinnamon_muffin_screen_cast_stream_get_parameters (stream->proxy);
  if (parameters)
    {
      if (g_variant_lookup (parameters, "position", "(ii)",
                            &stream->x, &stream->y))
        stream->has_position = TRUE;
      if (g_variant_lookup (parameters, "size", "(ii)",
                            &stream->width, &stream->height))
        stream->has_size = TRUE;
    }
  else
    {
      g_warning ("Screen cast stream %s missing parameters",
                 stream->path);
    }

  g_signal_connect (stream_proxy, "pipewire-stream-added",
                    G_CALLBACK (on_pipewire_stream_added),
                    stream);

  muffin_screen_cast_session->streams =
    g_list_prepend (muffin_screen_cast_session->streams, stream);
  muffin_screen_cast_session->n_needed_stream_node_ids++;

  return TRUE;
}

gboolean
muffin_screen_cast_session_record_selections (MuffinScreenCastSession *muffin_screen_cast_session,
                                             GVariant *selections,
                                             ScreenCastSelection *select,
                                             GError **error)
{
  GVariantIter selections_iter;
  GVariant *selection;

  g_variant_iter_init (&selections_iter, selections);
  while ((selection = g_variant_iter_next_value (&selections_iter)))
    {
      ScreenCastSourceType source_type;
      g_autofree char *key = NULL;
      g_autoptr(GVariant) variant = NULL;
      guint64 id;

      g_variant_get (selection, "(u?)",
                     &source_type,
                     &variant);

      switch (source_type)
        {
        case SCREEN_CAST_SOURCE_TYPE_MONITOR:
          key = g_variant_dup_string (variant, NULL);
          if (!muffin_screen_cast_session_record_monitor (muffin_screen_cast_session,
                                                         key,
                                                         select,
                                                         error))
            return FALSE;
          break;
        case SCREEN_CAST_SOURCE_TYPE_WINDOW:
          id = g_variant_get_uint64 (variant);
          if (!muffin_screen_cast_session_record_window (muffin_screen_cast_session,
                                                        id,
                                                        select,
                                                        error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
muffin_screen_cast_session_stop (MuffinScreenCastSession *muffin_screen_cast_session,
                                GError **error)
{
  OrgCinnamonMuffinScreenCastSession *session_proxy =
    muffin_screen_cast_session->proxy;

  g_signal_handler_disconnect (muffin_screen_cast_session->proxy,
                               muffin_screen_cast_session->closed_handler_id);

  if (muffin_screen_cast_session->closed)
    return TRUE;

  if (!org_cinnamon_muffin_screen_cast_session_call_stop_sync (session_proxy,
                                                            NULL,
                                                            error))
    return FALSE;

  return TRUE;
}

gboolean
muffin_screen_cast_session_start (MuffinScreenCastSession *muffin_screen_cast_session,
                                 GError **error)
{
  OrgCinnamonMuffinScreenCastSession *session_proxy =
    muffin_screen_cast_session->proxy;

  if (!org_cinnamon_muffin_screen_cast_session_call_start_sync (session_proxy,
                                                             NULL,
                                                             error))
    return FALSE;

  return TRUE;
}

static void
muffin_screen_cast_session_finalize (GObject *object)
{
  MuffinScreenCastSession *session = (MuffinScreenCastSession *)object;

  g_list_free_full (session->streams, g_object_unref);
  g_clear_object (&session->proxy);
  g_free (session->path);

  G_OBJECT_CLASS (muffin_screen_cast_session_parent_class)->finalize (object);
}

static void
muffin_screen_cast_session_init (MuffinScreenCastSession *muffin_screen_cast_session)
{
}

static void
muffin_screen_cast_session_class_init (MuffinScreenCastSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = muffin_screen_cast_session_finalize;

  session_signals[SESSION_SIGNAL_READY] = g_signal_new ("ready",
                                                        G_TYPE_FROM_CLASS (klass),
                                                        G_SIGNAL_RUN_LAST,
                                                        0,
                                                        NULL, NULL,
                                                        NULL,
                                                        G_TYPE_NONE, 0);
  session_signals[SESSION_SIGNAL_CLOSED] = g_signal_new ("closed",
                                                         G_TYPE_FROM_CLASS (klass),
                                                         G_SIGNAL_RUN_LAST,
                                                         0,
                                                         NULL, NULL,
                                                         NULL,
                                                         G_TYPE_NONE, 0);
}

static void
on_muffin_session_closed (OrgCinnamonMuffinScreenCastSession *session_proxy,
                          MuffinScreenCastSession *muffin_screen_cast_session)
{
  muffin_screen_cast_session->closed = TRUE;
  g_signal_emit (muffin_screen_cast_session,
                 session_signals[SESSION_SIGNAL_CLOSED], 0);
}

MuffinScreenCastSession *
muffin_screen_cast_create_session (MuffinScreenCast *muffin_screen_cast,
                                  const char *remote_desktop_session_id,
                                  GError **error)
{
  GVariantBuilder properties_builder;
  GVariant *properties;
  g_autofree char *session_path = NULL;
  GDBusConnection *connection;
  OrgCinnamonMuffinScreenCastSession *session_proxy;
  MuffinScreenCastSession *muffin_screen_cast_session;

  g_variant_builder_init (&properties_builder, G_VARIANT_TYPE_VARDICT);
  if (remote_desktop_session_id)
    {
      g_variant_builder_add (&properties_builder, "{sv}",
                             "remote-desktop-session-id",
                             g_variant_new_string (remote_desktop_session_id));
    }
  properties = g_variant_builder_end (&properties_builder);
  if (!org_cinnamon_muffin_screen_cast_call_create_session_sync (muffin_screen_cast->proxy,
                                                              properties,
                                                              &session_path,
                                                              NULL,
                                                              error))
    return NULL;

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (muffin_screen_cast->proxy));
  session_proxy =
    org_cinnamon_muffin_screen_cast_session_proxy_new_sync (connection,
                                                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                         "org.cinnamon.Muffin.ScreenCast",
                                                         session_path,
                                                         NULL,
                                                         error);
  if (!session_proxy)
    return NULL;

  muffin_screen_cast_session =
    g_object_new (muffin_screen_cast_session_get_type (), NULL);
  muffin_screen_cast_session->path = g_steal_pointer (&session_path);
  muffin_screen_cast_session->proxy = g_steal_pointer (&session_proxy);
  muffin_screen_cast_session->closed_handler_id =
    g_signal_connect (muffin_screen_cast_session->proxy,
                      "closed", G_CALLBACK (on_muffin_session_closed),
                      muffin_screen_cast_session);

  return muffin_screen_cast_session;
}

int
muffin_screen_cast_get_api_version (MuffinScreenCast *muffin_screen_cast)
{
  return muffin_screen_cast->api_version;
}

static void
muffin_screen_cast_name_appeared (GDBusConnection *connection,
                                 const char *name,
                                 const char *name_owner,
                                 gpointer user_data)
{
  MuffinScreenCast *muffin_screen_cast = user_data;
  g_autoptr(GError) error = NULL;

  muffin_screen_cast->proxy =
    org_cinnamon_muffin_screen_cast_proxy_new_sync (connection,
                                                 G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                 "org.cinnamon.Muffin.ScreenCast",
                                                 "/org/cinnamon/Muffin/ScreenCast",
                                                 NULL,
                                                 &error);
  if (!muffin_screen_cast->proxy)
    {
      g_warning ("Failed to acquire org.cinnamon.Muffin.ScreenCast proxy: %s",
                 error->message);
      return;
    }

  muffin_screen_cast->api_version =
    org_cinnamon_muffin_screen_cast_get_version (muffin_screen_cast->proxy);

  g_signal_emit (muffin_screen_cast, signals[ENABLED], 0);
}

static void
muffin_screen_cast_name_vanished (GDBusConnection *connection,
                                 const char *name,
                                 gpointer user_data)
{
  MuffinScreenCast *muffin_screen_cast = user_data;

  g_clear_object (&muffin_screen_cast->proxy);

  g_signal_emit (muffin_screen_cast, signals[DISABLED], 0);
}

static void
muffin_screen_cast_init (MuffinScreenCast *muffin_screen_cast)
{
}

static void
muffin_screen_cast_class_init (MuffinScreenCastClass *klass)
{
  signals[ENABLED] = g_signal_new ("enabled",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL,
                                   NULL,
                                   G_TYPE_NONE, 0);
  signals[DISABLED] = g_signal_new ("disabled",
                                    G_TYPE_FROM_CLASS (klass),
                                    G_SIGNAL_RUN_LAST,
                                    0,
                                    NULL, NULL,
                                    NULL,
                                    G_TYPE_NONE, 0);
}

MuffinScreenCast *
muffin_screen_cast_new (GDBusConnection *connection)
{
  MuffinScreenCast *muffin_screen_cast;

  muffin_screen_cast = g_object_new (muffin_screen_cast_get_type (), NULL);
  muffin_screen_cast->screen_cast_name_watch =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      "org.cinnamon.Muffin.ScreenCast",
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      muffin_screen_cast_name_appeared,
                      muffin_screen_cast_name_vanished,
                      muffin_screen_cast,
                      NULL);

  return muffin_screen_cast;
}
