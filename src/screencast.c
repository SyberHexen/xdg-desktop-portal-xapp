/*
 * Copyright © 2017 Red Hat, Inc
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

#include "config.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <stdint.h>

#include "xdg-desktop-portal-dbus.h"

#include "screencast.h"
#include "screencastwidget.h"
#include "screencastdialog.h"
#include "muffinscreencast.h"
#include "remotedesktop.h"
#include "displaystatetracker.h"
#include "shellintrospect.h"
#include "sharingindicator.h"
#include "externalwindow.h"
#include "request.h"
#include "session.h"
#include "utils.h"

typedef struct _ScreenCastDialogHandle ScreenCastDialogHandle;

/* restore_data: (RESTORE_VENDOR, RESTORE_VERSION, a(ua{sv})), a source type
 * with what finds the source again:
 *   monitor: "match" (s) vendor:product:serial, "connector" (s)
 *   window: "app-id" (s), "title" (s), "wm-class" (s), "window-id" (t) */
#define RESTORE_VENDOR "xapp"
#define RESTORE_VERSION 1

typedef struct _ScreenCastSession
{
  Session parent;

  MuffinScreenCastSession *muffin_screen_cast_session;
  gulong session_ready_handler_id;
  gulong session_closed_handler_id;

  char *parent_window;

  ScreenCastSelection select;

  char *app_id;
  char *what;   /* what is shared, for the sharing indicator */

  /* Persistence the application asked for, and the sources to restore */
  uint32_t persist_mode;
  GVariant *restore_data;
  GVariant *restore_sources;

  GDBusMethodInvocation *start_invocation;
  ScreenCastDialogHandle *dialog_handle;
} ScreenCastSession;

typedef struct _ScreenCastSessionClass
{
  SessionClass parent_class;
} ScreenCastSessionClass;

typedef struct _ScreenCastDialogHandle
{
  Request *request;
  ScreenCastSession *session;

  GtkWidget *dialog;
  ExternalWindow *external_parent;

  int response;
} ScreenCastDialogHandle;

static GDBusConnection *impl_connection;
static GDBusInterfaceSkeleton *impl;

static MuffinScreenCast *muffin_screen_cast;

GType screen_cast_session_get_type (void);
G_DEFINE_TYPE (ScreenCastSession, screen_cast_session, session_get_type ())

static void
start_done (ScreenCastSession *session);

static gboolean
start_session (ScreenCastSession *session,
               GVariant *selections,
               GError **error);

static void
cancel_start_session (ScreenCastSession *session,
                      int response);

static gboolean
is_screen_cast_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session, screen_cast_session_get_type ());
}

static void
screen_cast_dialog_handle_free (ScreenCastDialogHandle *dialog_handle)
{
  g_clear_pointer (&dialog_handle->dialog, gtk_widget_destroy);
  g_clear_object (&dialog_handle->external_parent);
  g_object_unref (dialog_handle->request);

  g_free (dialog_handle);
}

static void
screen_cast_dialog_handle_close (ScreenCastDialogHandle *dialog_handle)
{
  screen_cast_dialog_handle_free (dialog_handle);
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              ScreenCastDialogHandle *dialog_handle)
{
  cancel_start_session (dialog_handle->session, 2);

  screen_cast_dialog_handle_close (dialog_handle);

  return FALSE;
}

static void
screen_cast_dialog_done (GtkWidget *widget,
                         int dialog_response,
                         GVariant *selections,
                         ScreenCastDialogHandle *dialog_handle)
{
  int response;

  switch (dialog_response)
    {
    default:
      g_warning ("Unexpected response: %d", dialog_response);
      /* Fall through */
    case GTK_RESPONSE_DELETE_EVENT:
      response = 2;
      break;

    case GTK_RESPONSE_CANCEL:
      response = 1;
      break;

    case GTK_RESPONSE_OK:
      response = 0;
      break;
    }

  if (response == 0)
    {
      g_autoptr(GError) error = NULL;
      gboolean persist = FALSE;

      if (!g_variant_lookup (selections, "persist", "b", &persist) || !persist)
        dialog_handle->session->persist_mode = 0;

      if (!start_session (dialog_handle->session, selections, &error))
        {
          g_warning ("Failed to start session: %s", error->message);
          response = 2;
        }
    }

  if (response != 0)
    cancel_start_session (dialog_handle->session, response);

  if (dialog_handle->request->exported)
    request_unexport (dialog_handle->request);
}

static ScreenCastDialogHandle *
create_screen_cast_dialog (ScreenCastSession *session,
                           GDBusMethodInvocation *invocation,
                           Request *request,
                           const char *parent_window)
{
  ScreenCastDialogHandle *dialog_handle;
  ExternalWindow *external_parent;
  GdkScreen *screen;
  GdkDisplay *display;
  GtkWidget *fake_parent;
  GtkWidget *dialog;

  if (parent_window && *parent_window)
    {
      external_parent = create_external_window_from_handle (parent_window);
      if (!external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   parent_window);
    }
  else
    {
      external_parent = NULL;
    }

  if (external_parent)
    display = external_window_get_display (external_parent);
  else
    display = gdk_display_get_default ();
  screen = gdk_display_get_default_screen (display);
  fake_parent = g_object_new (GTK_TYPE_WINDOW,
                              "type", GTK_WINDOW_TOPLEVEL,
                              "screen", screen,
                              NULL);
  g_object_ref_sink (fake_parent);

  dialog = GTK_WIDGET (screen_cast_dialog_new (request->app_id,
                                               &session->select,
                                               session->persist_mode != 0));
  gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (fake_parent));
  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

  dialog_handle = g_new0 (ScreenCastDialogHandle, 1);
  dialog_handle->session = session;
  dialog_handle->request = g_object_ref (request);
  dialog_handle->external_parent = external_parent;
  dialog_handle->dialog = dialog;

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (handle_close), dialog_handle);

  g_signal_connect (dialog, "done",
                    G_CALLBACK (screen_cast_dialog_done), dialog_handle);

  gtk_widget_realize (dialog);

  if (external_parent)
    external_window_set_parent_of (external_parent, gtk_widget_get_window (dialog));

  gtk_widget_show (dialog);

  return dialog_handle;
}

static ScreenCastSession *
screen_cast_session_new (const char *app_id,
                         const char *session_handle)
{
  ScreenCastSession *screen_cast_session;

  screen_cast_session = g_object_new (screen_cast_session_get_type (),
                                      "id", session_handle,
                                      NULL);
  screen_cast_session->app_id = g_strdup (app_id);

  return screen_cast_session;
}

static gboolean
handle_create_session (XdpImplScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  g_autoptr(GError) error = NULL;
  int response;
  Session *session;
  GVariantBuilder results_builder;

  session = (Session *)screen_cast_session_new (arg_app_id,
                                                arg_session_handle);

  if (!session_export (session,
                       g_dbus_method_invocation_get_connection (invocation),
                       &error))
    {
      g_clear_object (&session);
      g_warning ("Failed to create screen cast session: %s", error->message);
      response = 2;
      goto out;
    }

  response = 0;

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_screen_cast_complete_create_session (object,
                                                invocation,
                                                response,
                                                g_variant_builder_end (&results_builder));

  return TRUE;
}

static gboolean
handle_select_sources (XdpImplScreenCast *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  Session *session;
  int response;
  uint32_t types;
  gboolean multiple;
  ScreenCastCursorMode cursor_mode;
  ScreenCastSelection select;
  GVariantBuilder results_builder;
  GVariant *results;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to select sources on non-existing %s", arg_session_handle);
      response = 2;
      goto out;
    }

  if (!g_variant_lookup (arg_options, "multiple", "b", &multiple))
    multiple = FALSE;

  if (!g_variant_lookup (arg_options, "types", "u", &types))
    types = SCREEN_CAST_SOURCE_TYPE_MONITOR;

  if (!(types & (SCREEN_CAST_SOURCE_TYPE_MONITOR |
                 SCREEN_CAST_SOURCE_TYPE_WINDOW)))
    {
      g_warning ("Unknown screen cast source type");
      response = 2;
      goto out;
    }

  if (!g_variant_lookup (arg_options, "cursor_mode", "u", &cursor_mode))
    cursor_mode = SCREEN_CAST_CURSOR_MODE_HIDDEN;

  switch (cursor_mode)
    {
    case SCREEN_CAST_CURSOR_MODE_HIDDEN:
    case SCREEN_CAST_CURSOR_MODE_EMBEDDED:
    case SCREEN_CAST_CURSOR_MODE_METADATA:
      break;
    default:
      g_warning ("Unknown screen cast cursor mode");
      response = 2;
      goto out;
    }

  select.multiple = multiple;
  select.source_types = types;
  select.cursor_mode = cursor_mode;

  if (is_screen_cast_session (session))
    {
      ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;
      uint32_t persist_mode;
      GVariant *restore_data;

      screen_cast_session->select = select;

      if (g_variant_lookup (arg_options, "persist_mode", "u", &persist_mode) &&
          persist_mode <= 2)
        screen_cast_session->persist_mode = persist_mode;

      restore_data = g_variant_lookup_value (arg_options, "restore_data",
                                             G_VARIANT_TYPE ("(suv)"));
      g_clear_pointer (&screen_cast_session->restore_data, g_variant_unref);
      screen_cast_session->restore_data = restore_data;

      response = 0;
    }
  else if (is_remote_desktop_session (session))
    {
      RemoteDesktopSession *remote_desktop_session =
        (RemoteDesktopSession *)session;

      remote_desktop_session_sources_selected (remote_desktop_session, &select);
      response = 0;
    }
  else
    {
      g_warning ("Tried to select sources on invalid session type");
      response = 2;
    }

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  results = g_variant_builder_end (&results_builder);
  xdp_impl_screen_cast_complete_select_sources (object, invocation,
                                                response, results);

  return TRUE;
}

static void
start_done (ScreenCastSession *screen_cast_session)
{
  MuffinScreenCastSession *muffin_screen_cast_session;
  GVariantBuilder streams_builder;
  GVariantBuilder results_builder;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&streams_builder, G_VARIANT_TYPE ("a(ua{sv})"));

  muffin_screen_cast_session = screen_cast_session->muffin_screen_cast_session;
  muffin_screen_cast_session_add_stream_properties (muffin_screen_cast_session,
                                                   &streams_builder);

  g_variant_builder_add (&results_builder, "{sv}",
                         "streams",
                         g_variant_builder_end (&streams_builder));

  if (screen_cast_session->persist_mode != 0 &&
      screen_cast_session->restore_sources)
    {
      g_variant_builder_add (&results_builder, "{sv}", "restore_data",
                             g_variant_new ("(suv)", RESTORE_VENDOR,
                                            RESTORE_VERSION,
                                            screen_cast_session->restore_sources));
    }
  else
    {
      screen_cast_session->persist_mode = 0;
    }
  g_variant_builder_add (&results_builder, "{sv}", "persist_mode",
                         g_variant_new_uint32 (screen_cast_session->persist_mode));

  xdp_impl_screen_cast_complete_start (XDP_IMPL_SCREEN_CAST (impl),
                                       screen_cast_session->start_invocation, 0,
                                       g_variant_builder_end (&results_builder));
  screen_cast_session->start_invocation = NULL;

  sharing_indicator_add ((Session *) screen_cast_session,
                         screen_cast_session->app_id,
                         screen_cast_session->what, FALSE);
}

static void
on_muffin_screen_cast_session_ready (MuffinScreenCastSession *muffin_screen_cast_session,
                                    ScreenCastSession *screen_cast_session)
{
  start_done (screen_cast_session);
}

static void
on_muffin_screen_cast_session_closed (MuffinScreenCastSession *muffin_screen_cast_session,
                                     ScreenCastSession *screen_cast_session)
{
  session_close ((Session *)screen_cast_session);
}

/* What is shared, for the sharing indicator: monitor names and window
 * titles */
char *
screen_cast_describe_sources (GVariant *source_selections)
{
  DisplayStateTracker *tracker = display_state_tracker_get ();
  ShellIntrospect *shell_introspect = shell_introspect_get ();
  GString *what = g_string_new (NULL);
  GVariantIter iter;
  uint32_t source_type;
  GVariant *value;

  g_variant_iter_init (&iter, source_selections);
  while (g_variant_iter_next (&iter, "(u@?)", &source_type, &value))
    {
      g_autofree char *name = NULL;

      if (source_type == SCREEN_CAST_SOURCE_TYPE_MONITOR)
        {
          Monitor *monitor =
            display_state_tracker_find_monitor (tracker, NULL,
                                                g_variant_get_string (value, NULL));

          name = g_strdup (monitor ? monitor_get_display_name (monitor)
                                   : g_variant_get_string (value, NULL));
        }
      else if (source_type == SCREEN_CAST_SOURCE_TYPE_WINDOW)
        {
          g_autofree char *app_id = NULL;
          g_autofree char *title = NULL;

          if (shell_introspect_lookup_window (shell_introspect,
                                              g_variant_get_uint64 (value),
                                              &app_id, &title, NULL))
            name = g_strdup_printf ("“%s”", title);
          else
            name = g_strdup ("a window");
        }
      g_variant_unref (value);

      if (name)
        {
          if (what->len)
            g_string_append (what, ", ");
          g_string_append (what, name);
        }
    }

  return g_string_free (what, what->len == 0);
}

/* What finds the selected sources again in a later session */
GVariant *
screen_cast_build_restore_sources (GVariant *source_selections)
{
  DisplayStateTracker *tracker = display_state_tracker_get ();
  ShellIntrospect *shell_introspect = shell_introspect_get ();
  GVariantBuilder sources_builder;
  GVariantIter iter;
  uint32_t source_type;
  GVariant *value;

  g_variant_builder_init (&sources_builder, G_VARIANT_TYPE ("a(ua{sv})"));
  g_variant_iter_init (&iter, source_selections);
  while (g_variant_iter_next (&iter, "(u@?)", &source_type, &value))
    {
      GVariantBuilder props;

      g_variant_builder_init (&props, G_VARIANT_TYPE_VARDICT);
      if (source_type == SCREEN_CAST_SOURCE_TYPE_MONITOR)
        {
          const char *connector = g_variant_get_string (value, NULL);
          Monitor *monitor =
            display_state_tracker_find_monitor (tracker, NULL, connector);

          g_variant_builder_add (&props, "{sv}", "connector",
                                 g_variant_new_string (connector));
          if (monitor)
            g_variant_builder_add (&props, "{sv}", "match",
                                   g_variant_new_string (monitor_get_match_string (monitor)));
        }
      else if (source_type == SCREEN_CAST_SOURCE_TYPE_WINDOW)
        {
          uint64_t id = g_variant_get_uint64 (value);
          g_autofree char *app_id = NULL;
          g_autofree char *title = NULL;
          g_autofree char *wm_class = NULL;

          g_variant_builder_add (&props, "{sv}", "window-id",
                                 g_variant_new_uint64 (id));
          if (shell_introspect_lookup_window (shell_introspect, id,
                                              &app_id, &title, &wm_class))
            {
              g_variant_builder_add (&props, "{sv}", "app-id",
                                     g_variant_new_string (app_id));
              g_variant_builder_add (&props, "{sv}", "title",
                                     g_variant_new_string (title));
              g_variant_builder_add (&props, "{sv}", "wm-class",
                                     g_variant_new_string (wm_class));
            }
        }
      g_variant_builder_add (&sources_builder, "(u@a{sv})", source_type,
                             g_variant_builder_end (&props));
      g_variant_unref (value);
    }

  return g_variant_builder_end (&sources_builder);
}

/* The selections stored sources (a(ua{sv}), see RESTORE_VENDOR) stand for
 * now, or NULL if any of them is gone: the user is asked then */
GVariant *
screen_cast_restore_sources (GVariant            *data,
                             ScreenCastSelection *select)
{
  DisplayStateTracker *tracker = display_state_tracker_get ();
  ShellIntrospect *shell_introspect = shell_introspect_get ();
  GVariantBuilder sources_builder;
  GVariantBuilder selections_builder;
  GVariantIter iter;
  uint32_t source_type;
  GVariant *props;
  int n_sources = 0;

  if (!g_variant_is_of_type (data, G_VARIANT_TYPE ("a(ua{sv})")))
    return NULL;

  g_variant_builder_init (&sources_builder, G_VARIANT_TYPE ("a(u?)"));
  g_variant_iter_init (&iter, data);
  while (g_variant_iter_next (&iter, "(u@a{sv})", &source_type, &props))
    {
      g_autoptr(GVariant) props_ref = props;

      if (source_type == SCREEN_CAST_SOURCE_TYPE_MONITOR &&
          (select->source_types & SCREEN_CAST_SOURCE_TYPE_MONITOR))
        {
          const char *match = NULL, *connector = NULL;
          Monitor *monitor;

          g_variant_lookup (props, "match", "&s", &match);
          g_variant_lookup (props, "connector", "&s", &connector);
          monitor = display_state_tracker_find_monitor (tracker, match,
                                                        connector);
          if (!monitor)
            goto fail;
          g_variant_builder_add (&sources_builder, "(us)", source_type,
                                 monitor_get_connector (monitor));
        }
      else if (source_type == SCREEN_CAST_SOURCE_TYPE_WINDOW &&
               (select->source_types & SCREEN_CAST_SOURCE_TYPE_WINDOW))
        {
          const char *app_id = NULL, *title = NULL, *wm_class = NULL;
          uint64_t id = 0, found;

          g_variant_lookup (props, "app-id", "&s", &app_id);
          g_variant_lookup (props, "title", "&s", &title);
          g_variant_lookup (props, "wm-class", "&s", &wm_class);
          g_variant_lookup (props, "window-id", "t", &id);
          if (!shell_introspect_find_window (shell_introspect, id, app_id,
                                             title, wm_class, &found))
            goto fail;
          g_variant_builder_add (&sources_builder, "(ut)", source_type,
                                 found);
        }
      else
        {
          goto fail;
        }
      n_sources++;
    }

  if (n_sources == 0 ||
      (n_sources > 1 && !select->multiple))
    goto fail;

  g_variant_builder_init (&selections_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&selections_builder, "{sv}",
                         "selected_screen_cast_sources",
                         g_variant_builder_end (&sources_builder));
  return g_variant_ref_sink (g_variant_builder_end (&selections_builder));

fail:
  g_variant_builder_clear (&sources_builder);
  return NULL;
}

/* The selections restore_data stands for now, or NULL if any of its sources
 * is gone (or the data is not ours): the user is asked then */
static GVariant *
restore_selections (ScreenCastSession *screen_cast_session)
{
  const char *vendor;
  uint32_t version;
  g_autoptr(GVariant) data = NULL;

  if (!screen_cast_session->restore_data)
    return NULL;

  g_variant_get (screen_cast_session->restore_data, "(&suv)",
                 &vendor, &version, &data);
  if (g_strcmp0 (vendor, RESTORE_VENDOR) != 0 || version != RESTORE_VERSION)
    return NULL;

  return screen_cast_restore_sources (data, &screen_cast_session->select);
}

static gboolean
start_session (ScreenCastSession *screen_cast_session,
               GVariant *selections,
               GError **error)
{
  MuffinScreenCastSession *muffin_screen_cast_session;
  g_autoptr(GVariant) source_selections = NULL;

  muffin_screen_cast_session =
    muffin_screen_cast_create_session (muffin_screen_cast, NULL, error);
  if (!muffin_screen_cast_session)
    return FALSE;

  screen_cast_session->muffin_screen_cast_session = muffin_screen_cast_session;

  screen_cast_session->session_ready_handler_id =
    g_signal_connect (muffin_screen_cast_session, "ready",
                      G_CALLBACK (on_muffin_screen_cast_session_ready),
                      screen_cast_session);
  screen_cast_session->session_closed_handler_id =
    g_signal_connect (muffin_screen_cast_session, "closed",
                      G_CALLBACK (on_muffin_screen_cast_session_closed),
                      screen_cast_session);

  g_variant_lookup (selections, "selected_screen_cast_sources", "@a(u?)",
                    &source_selections);

  g_clear_pointer (&screen_cast_session->what, g_free);
  if (source_selections)
    screen_cast_session->what = screen_cast_describe_sources (source_selections);

  g_clear_pointer (&screen_cast_session->restore_sources, g_variant_unref);
  if (screen_cast_session->persist_mode != 0 && source_selections)
    screen_cast_session->restore_sources =
      g_variant_ref_sink (screen_cast_build_restore_sources (source_selections));
  if (!muffin_screen_cast_session_record_selections (muffin_screen_cast_session,
                                                    source_selections,
                                                    &screen_cast_session->select,
                                                    error))
    return FALSE;

  if (!muffin_screen_cast_session_start (muffin_screen_cast_session, error))
    return FALSE;

  return TRUE;
}

static void
cancel_start_session (ScreenCastSession *screen_cast_session,
                      int response)
{
  GVariantBuilder results_builder;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_screen_cast_complete_start (XDP_IMPL_SCREEN_CAST (impl),
                                       screen_cast_session->start_invocation,
                                       response,
                                       g_variant_builder_end (&results_builder));
}

static gboolean
handle_start (XdpImplScreenCast *object,
              GDBusMethodInvocation *invocation,
              const char *arg_handle,
              const char *arg_session_handle,
              const char *arg_app_id,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  ScreenCastSession *screen_cast_session;
  ScreenCastDialogHandle *dialog_handle;
  GVariantBuilder results_builder;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);
  request_export (request,
                  g_dbus_method_invocation_get_connection (invocation));

  screen_cast_session =
    (ScreenCastSession *)lookup_session (arg_session_handle);
  if (!screen_cast_session)
    {
      g_warning ("Attempted to start non existing screen cast session");
      goto err;
    }

  if (screen_cast_session->dialog_handle)
    {
      g_warning ("Screen cast dialog already open");
      goto err;
    }

  /* A selection the user asked to remember is started without asking */
  {
    g_autoptr(GVariant) selections = restore_selections (screen_cast_session);

    if (selections)
      {
        g_autoptr(GError) error = NULL;

        screen_cast_session->start_invocation = invocation;
        request_unexport (request);
        if (!start_session (screen_cast_session, selections, &error))
          {
            g_warning ("Failed to restore screen cast session: %s",
                       error->message);
            cancel_start_session (screen_cast_session, 2);
          }
        return TRUE;
      }
  }

  dialog_handle = create_screen_cast_dialog (screen_cast_session,
                                             invocation,
                                             request,
                                             arg_parent_window);


  screen_cast_session->start_invocation = invocation;
  screen_cast_session->dialog_handle = dialog_handle;

  return TRUE;

err:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE ("a(ua{sv}"));
  xdp_impl_screen_cast_complete_start (object, invocation, 2,
                                       g_variant_builder_end (&results_builder));

  return TRUE;
}

static void
on_muffin_screen_cast_enabled (MuffinScreenCast *muffin_screen_cast)
{
  int muffin_api_version;
  ScreenCastSourceType available_source_types;
  ScreenCastCursorMode available_cursor_modes;
  g_autoptr(GError) error = NULL;

  impl = G_DBUS_INTERFACE_SKELETON (xdp_impl_screen_cast_skeleton_new ());

  g_signal_connect (impl, "handle-create-session",
                    G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (impl, "handle-select-sources",
                    G_CALLBACK (handle_select_sources), NULL);
  g_signal_connect (impl, "handle-start",
                    G_CALLBACK (handle_start), NULL);

  muffin_api_version = muffin_screen_cast_get_api_version (muffin_screen_cast);

  available_source_types = SCREEN_CAST_SOURCE_TYPE_MONITOR;
  if (muffin_api_version >= 2)
    available_source_types |= SCREEN_CAST_SOURCE_TYPE_WINDOW;
  g_object_set (G_OBJECT (impl),
                "available-source-types", available_source_types,
                "version", 4,
                NULL);

  available_cursor_modes = SCREEN_CAST_CURSOR_MODE_NONE;
  if (muffin_api_version >= 2)
    available_cursor_modes |= (SCREEN_CAST_CURSOR_MODE_HIDDEN |
                               SCREEN_CAST_CURSOR_MODE_EMBEDDED |
                               SCREEN_CAST_CURSOR_MODE_METADATA);
  g_object_set (G_OBJECT (impl),
                "available-cursor-modes", available_cursor_modes,
                NULL);

  if (!g_dbus_interface_skeleton_export (impl,
                                         impl_connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Failed to export screen cast portal implementation object: %s",
                 error->message);
      return;
    }

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (impl)->name);
}

static void
on_muffin_screen_cast_disabled (GDBusConnection *connection,
                               const char *name,
                               gpointer user_data)
{
  if (impl)
    {
      g_debug ("unproviding %s", g_dbus_interface_skeleton_get_info (impl)->name);

      g_dbus_interface_skeleton_unexport (impl);
      g_clear_object (&impl);
    }
}

gboolean
screen_cast_init (GDBusConnection *connection,
                  GError **error)
{
  impl_connection = connection;
  muffin_screen_cast = muffin_screen_cast_new (connection);

  g_signal_connect (muffin_screen_cast, "enabled",
                    G_CALLBACK (on_muffin_screen_cast_enabled), NULL);
  g_signal_connect (muffin_screen_cast, "disabled",
                    G_CALLBACK (on_muffin_screen_cast_disabled), NULL);

  return TRUE;
}

static void
screen_cast_session_close (Session *session)
{
  ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;
  MuffinScreenCastSession *muffin_screen_cast_session;
  g_autoptr(GError) error = NULL;

  sharing_indicator_remove (session);

  muffin_screen_cast_session = screen_cast_session->muffin_screen_cast_session;
  if (muffin_screen_cast_session)
    {
      g_signal_handler_disconnect (muffin_screen_cast_session,
                                   screen_cast_session->session_ready_handler_id);
      g_signal_handler_disconnect (muffin_screen_cast_session,
                                   screen_cast_session->session_closed_handler_id);
      if (!muffin_screen_cast_session_stop (muffin_screen_cast_session,
                                           &error))
        g_warning ("Failed to close Muffin screen cast session: %s",
                   error->message);
      g_clear_object (&screen_cast_session->muffin_screen_cast_session);
    }
}

static void
screen_cast_session_finalize (GObject *object)
{
  ScreenCastSession *screen_cast_session = (ScreenCastSession *)object;

  g_clear_object (&screen_cast_session->muffin_screen_cast_session);
  g_clear_pointer (&screen_cast_session->restore_data, g_variant_unref);
  g_clear_pointer (&screen_cast_session->restore_sources, g_variant_unref);
  g_free (screen_cast_session->app_id);
  g_free (screen_cast_session->what);

  G_OBJECT_CLASS (screen_cast_session_parent_class)->finalize (object);
}

static void
screen_cast_session_init (ScreenCastSession *screen_cast_session)
{
}

static void
screen_cast_session_class_init (ScreenCastSessionClass *klass)
{
  GObjectClass *gobject_class;
  SessionClass *session_class;

  gobject_class = (GObjectClass *)klass;
  gobject_class->finalize = screen_cast_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = screen_cast_session_close;
}
