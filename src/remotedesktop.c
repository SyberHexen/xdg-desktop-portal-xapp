/*
 * Copyright © 2018 Red Hat, Inc
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
#include "common-dbus.h"

#include "remotedesktop.h"
#include "sharingindicator.h"
#include "remotedesktopdialog.h"
#include "screencastdialog.h"
#include "muffinscreencast.h"
#include "externalwindow.h"
#include "request.h"
#include "session.h"
#include "utils.h"

typedef enum _MuffinRemoteDesktopDeviceType
{
  MUFFIN_REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD = 1 << 0,
  MUFFIN_REMOTE_DESKTOP_DEVICE_TYPE_POINTER = 1 << 1,
  MUFFIN_REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN = 1 << 2,
} MuffinRemoteDesktopDeviceType;

enum _MuffinRemoteDesktopNotifyAxisFlags
{
  MUFFIN_REMOTE_DESKTOP_NOTIFY_AXIS_FLAGS_FINISH = 1 << 0,
} MuffinRemoteDesktopNotifyAxisFlags;

typedef struct _RemoteDesktopDialogHandle RemoteDesktopDialogHandle;

/* restore_data: (RESTORE_VENDOR, RESTORE_VERSION, a{sv}) with "devices" (u),
 * the device types allowed, and "sources" (a(ua{sv})), the screen cast
 * sources as ScreenCast stores them */
#define RESTORE_VENDOR "xapp"
#define RESTORE_VERSION 1

typedef struct _RemoteDesktopSession
{
  Session parent;

  char *parent_window;
  char *muffin_session_path;
  OrgCinnamonMuffinRemoteDesktopSession *muffin_session_proxy;
  gulong closed_handler_id;

  MuffinScreenCastSession *muffin_screen_cast_session;
  gulong session_ready_handler_id;

  struct {
    RemoteDesktopDeviceType device_types;

    gboolean screen_cast_enable;
    ScreenCastSelection screen_cast;
  } select;

  struct {
    RemoteDesktopDeviceType device_types;
  } shared;

  char *app_id;
  char *what;   /* what it sees, for the sharing indicator */

  /* Persistence the application asked for, and what to restore */
  uint32_t persist_mode;
  GVariant *restore_data;
  GVariant *restore_sources;

  GDBusMethodInvocation *start_invocation;
  RemoteDesktopDialogHandle *dialog_handle;
} RemoteDesktopSession;

typedef struct _RemoteDesktopSessionClass
{
  SessionClass parent_class;
} RemoteDesktopSessionClass;

typedef struct _RemoteDesktopDialogHandle
{
  Request *request;
  RemoteDesktopSession *session;

  GtkWidget *dialog;
  ExternalWindow *external_parent;

  int response;
} RemoteDesktopDialogHandle;

static GDBusConnection *impl_connection;
static guint remote_desktop_name_watch;
static GDBusInterfaceSkeleton *impl;
static OrgCinnamonMuffinRemoteDesktop *remote_desktop;
static MuffinScreenCast *muffin_screen_cast;

GType remote_desktop_session_get_type (void);
G_DEFINE_TYPE (RemoteDesktopSession, remote_desktop_session, session_get_type ())

static void
start_done (RemoteDesktopSession *session);

static gboolean
start_session (RemoteDesktopSession *session,
               GVariant *selections,
               GError **error);

static void
cancel_start_session (RemoteDesktopSession *session,
                      int response);

gboolean
is_remote_desktop_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session, remote_desktop_session_get_type ());
}

void
remote_desktop_session_sources_selected (RemoteDesktopSession *session,
                                         ScreenCastSelection *selection)
{
  session->select.screen_cast_enable = TRUE;
  session->select.screen_cast = *selection;
}

static void
remote_desktop_dialog_handle_free (RemoteDesktopDialogHandle *dialog_handle)
{
  g_clear_pointer (&dialog_handle->dialog, gtk_widget_destroy);
  g_clear_object (&dialog_handle->external_parent);
  g_object_unref (dialog_handle->request);

  g_free (dialog_handle);
}

static void
remote_desktop_dialog_handle_close (RemoteDesktopDialogHandle *dialog_handle)
{
  remote_desktop_dialog_handle_free (dialog_handle);
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              RemoteDesktopDialogHandle *dialog_handle)
{
  cancel_start_session (dialog_handle->session, 2);

  remote_desktop_dialog_handle_close (dialog_handle);

  return FALSE;
}

static void
remote_desktop_dialog_done (GtkWidget *widget,
                            int dialog_response,
                            GVariant *selections,
                            RemoteDesktopDialogHandle *dialog_handle)
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
}

static RemoteDesktopDialogHandle *
create_remote_desktop_dialog (RemoteDesktopSession *session,
                              GDBusMethodInvocation *invocation,
                              Request *request,
                              const char *parent_window)
{
  RemoteDesktopDialogHandle *dialog_handle;
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

  dialog =
    GTK_WIDGET (remote_desktop_dialog_new (request->app_id,
                                           session->select.device_types,
                                           session->select.screen_cast_enable ?
                                             &session->select.screen_cast : NULL,
                                           session->persist_mode != 0));
  gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (fake_parent));
  gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

  dialog_handle = g_new0 (RemoteDesktopDialogHandle, 1);
  dialog_handle->session = session;
  dialog_handle->request = g_object_ref (request);
  dialog_handle->external_parent = external_parent;
  dialog_handle->dialog = dialog;

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (handle_close), dialog_handle);

  g_signal_connect (dialog, "done",
                    G_CALLBACK (remote_desktop_dialog_done), dialog_handle);

  gtk_widget_realize (dialog);

  if (external_parent)
    external_window_set_parent_of (external_parent, gtk_widget_get_window (dialog));

  gtk_widget_show (dialog);

  return dialog_handle;
}

static void
on_muffin_session_closed (OrgCinnamonMuffinRemoteDesktopSession *muffin_session_proxy,
                          RemoteDesktopSession *remote_desktop_session)
{
  session_close ((Session *)remote_desktop_session);
}

static RemoteDesktopSession *
remote_desktop_session_new (const char *app_id,
                            const char *session_handle,
                            const char *muffin_session_path,
                            OrgCinnamonMuffinRemoteDesktopSession *muffin_session_proxy)
{
  RemoteDesktopSession *remote_desktop_session;

  remote_desktop_session = g_object_new (remote_desktop_session_get_type (),
                                         "id", session_handle,
                                         NULL);
  remote_desktop_session->app_id = g_strdup (app_id);
  remote_desktop_session->muffin_session_path =
    g_strdup (muffin_session_path);
  remote_desktop_session->muffin_session_proxy =
    g_object_ref (muffin_session_proxy);

  remote_desktop_session->closed_handler_id =
    g_signal_connect (remote_desktop_session->muffin_session_proxy,
                      "closed", G_CALLBACK (on_muffin_session_closed),
                      remote_desktop_session);

  return remote_desktop_session;
}

static gboolean
handle_create_session (XdpImplRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  g_autofree char *muffin_session_path = NULL;
  g_autoptr(GError) error = NULL;
  int response;
  OrgCinnamonMuffinRemoteDesktopSession *muffin_session_proxy = NULL;
  Session *session;
  GVariantBuilder results_builder;

  sender = g_dbus_method_invocation_get_sender (invocation);

  request = request_new (sender, arg_app_id, arg_handle);

  if (!org_cinnamon_muffin_remote_desktop_call_create_session_sync (remote_desktop,
                                                                 &muffin_session_path,
                                                                 NULL,
                                                                 &error))
    {
      g_warning ("Failed to create remote desktop session: %s", error->message);
      response = 2;
      goto out;
    }

  muffin_session_proxy =
    org_cinnamon_muffin_remote_desktop_session_proxy_new_sync (impl_connection,
                                                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                            "org.cinnamon.Muffin.RemoteDesktop",
                                                            muffin_session_path,
                                                            NULL,
                                                            &error);
  if (!muffin_session_proxy)
    {
      g_warning ("Failed to get remote desktop session proxy: %s", error->message);
      response = 2;
      goto out;
    }

  session = (Session *)remote_desktop_session_new (arg_app_id,
                                                   arg_session_handle,
                                                   muffin_session_path,
                                                   muffin_session_proxy);

  if (!session_export (session,
                       g_dbus_method_invocation_get_connection (invocation),
                       &error))
    {
      g_clear_object (&session);
      g_warning ("Failed to create remote desktop session: %s", error->message);
      response = 2;
      goto out;
    }

  response = 0;

out:
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_remote_desktop_complete_create_session (object,
                                                   invocation,
                                                   response,
                                                   g_variant_builder_end (&results_builder));

  g_clear_object (&muffin_session_proxy);

  return TRUE;
}

static gboolean
handle_select_devices (XdpImplRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  RemoteDesktopSession *remote_desktop_session;
  int response;
  uint32_t device_types;
  GVariantBuilder results_builder;
  GVariant *results;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  if (!remote_desktop_session)
    {
      g_warning ("Tried to select sources on non-existing %s", arg_session_handle);
      response = 2;
      goto out;
    }

  if (!g_variant_lookup (arg_options, "types", "u", &device_types))
    device_types = REMOTE_DESKTOP_DEVICE_TYPE_ALL;

  remote_desktop_session->select.device_types = device_types;

  {
    uint32_t persist_mode;

    if (g_variant_lookup (arg_options, "persist_mode", "u", &persist_mode) &&
        persist_mode <= 2)
      remote_desktop_session->persist_mode = persist_mode;
    g_clear_pointer (&remote_desktop_session->restore_data, g_variant_unref);
    remote_desktop_session->restore_data =
      g_variant_lookup_value (arg_options, "restore_data",
                              G_VARIANT_TYPE ("(suv)"));
  }
  response = 0;

out:
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  results = g_variant_builder_end (&results_builder);
  xdp_impl_remote_desktop_complete_select_devices (object, invocation,
                                                   response, results);

  return TRUE;
}

static void
start_done (RemoteDesktopSession *remote_desktop_session)
{
  GVariantBuilder results_builder;
  RemoteDesktopDeviceType shared_device_types;
  MuffinScreenCastSession *muffin_screen_cast_session;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  shared_device_types = remote_desktop_session->shared.device_types;
  g_variant_builder_add (&results_builder, "{sv}",
                         "devices", g_variant_new_uint32 (shared_device_types));

  muffin_screen_cast_session = remote_desktop_session->muffin_screen_cast_session;
  if (muffin_screen_cast_session)
    {
      GVariantBuilder streams_builder;

      g_variant_builder_init (&streams_builder, G_VARIANT_TYPE ("a(ua{sv})"));
      muffin_screen_cast_session_add_stream_properties (muffin_screen_cast_session,
                                                       &streams_builder);
      g_variant_builder_add (&results_builder, "{sv}",
                             "streams",
                             g_variant_builder_end (&streams_builder));
    }

  if (remote_desktop_session->persist_mode != 0)
    {
      GVariantBuilder data_builder;

      g_variant_builder_init (&data_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&data_builder, "{sv}", "devices",
                             g_variant_new_uint32 (shared_device_types));
      if (remote_desktop_session->restore_sources)
        g_variant_builder_add (&data_builder, "{sv}", "sources",
                               remote_desktop_session->restore_sources);
      g_variant_builder_add (&results_builder, "{sv}", "restore_data",
                             g_variant_new ("(suv)", RESTORE_VENDOR,
                                            RESTORE_VERSION,
                                            g_variant_builder_end (&data_builder)));
    }
  g_variant_builder_add (&results_builder, "{sv}", "persist_mode",
                         g_variant_new_uint32 (remote_desktop_session->persist_mode));

  xdp_impl_remote_desktop_complete_start (XDP_IMPL_REMOTE_DESKTOP (impl),
                                          remote_desktop_session->start_invocation,
                                          0,
                                          g_variant_builder_end (&results_builder));
  remote_desktop_session->start_invocation = NULL;

  sharing_indicator_add ((Session *) remote_desktop_session,
                         remote_desktop_session->app_id,
                         remote_desktop_session->what,
                         shared_device_types != 0);
}

static void
on_muffin_screen_cast_session_ready (MuffinScreenCastSession *muffin_screen_cast_session,
                                    RemoteDesktopSession *remote_desktop_session)
{
  start_done (remote_desktop_session);
}

static gboolean
open_screen_cast_session (RemoteDesktopSession *remote_desktop_session,
                          GVariant *source_selections,
                          GError **error)
{
  OrgCinnamonMuffinRemoteDesktopSession *session_proxy =
    remote_desktop_session->muffin_session_proxy;
  MuffinScreenCastSession *muffin_screen_cast_session;
  const char *remote_desktop_session_id;

  remote_desktop_session_id =
    org_cinnamon_muffin_remote_desktop_session_get_session_id (session_proxy);
  muffin_screen_cast_session =
    muffin_screen_cast_create_session (muffin_screen_cast,
                                      remote_desktop_session_id,
                                      error);
  if (!muffin_screen_cast_session)
    return FALSE;

  remote_desktop_session->muffin_screen_cast_session = muffin_screen_cast_session;
  remote_desktop_session->session_ready_handler_id =
    g_signal_connect (muffin_screen_cast_session, "ready",
                      G_CALLBACK (on_muffin_screen_cast_session_ready),
                      remote_desktop_session);

  if (!muffin_screen_cast_session_record_selections (muffin_screen_cast_session,
                                                    source_selections,
                                                    &remote_desktop_session->select.screen_cast,
                                                    error))
    return FALSE;

  return TRUE;
}

static gboolean
start_session (RemoteDesktopSession *remote_desktop_session,
               GVariant *selections,
               GError **error)
{
  OrgCinnamonMuffinRemoteDesktopSession *session_proxy;
  RemoteDesktopDeviceType device_types = 0;
  g_autoptr(GVariant) source_selections = NULL;
  gboolean need_streams;

  g_variant_lookup (selections, "selected_device_types", "u", &device_types);
  remote_desktop_session->shared.device_types = device_types;

  /* Monitors (us) and windows (ut) */
  if (g_variant_lookup (selections, "selected_screen_cast_sources", "@a(u?)",
                        &source_selections))
    {
      g_clear_pointer (&remote_desktop_session->what, g_free);
      remote_desktop_session->what =
        screen_cast_describe_sources (source_selections);
      g_clear_pointer (&remote_desktop_session->restore_sources,
                       g_variant_unref);
      if (remote_desktop_session->persist_mode != 0)
        remote_desktop_session->restore_sources =
          g_variant_ref_sink (screen_cast_build_restore_sources (source_selections));

      if (!open_screen_cast_session (remote_desktop_session,
                                     source_selections, error))
        return FALSE;

      need_streams = TRUE;
    }
  else
    {
      need_streams = FALSE;
    }

  session_proxy = remote_desktop_session->muffin_session_proxy;
  if (!org_cinnamon_muffin_remote_desktop_session_call_start_sync (session_proxy,
                                                                NULL,
                                                                error))
    return FALSE;

  if (!need_streams)
    start_done (remote_desktop_session);

  return TRUE;
}

static void
cancel_start_session (RemoteDesktopSession *remote_desktop_session,
                      int response)
{
  GVariantBuilder results_builder;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_remote_desktop_complete_start (XDP_IMPL_REMOTE_DESKTOP (impl),
                                          remote_desktop_session->start_invocation,
                                          response,
                                          g_variant_builder_end (&results_builder));
}

/* The selections restore_data stands for now, or NULL if something is gone
 * (or the data is not ours): the user is asked then */
static GVariant *
restore_selections (RemoteDesktopSession *remote_desktop_session)
{
  const char *vendor;
  uint32_t version;
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) sources = NULL;
  g_autoptr(GVariant) source_selections = NULL;
  GVariantBuilder selections_builder;
  uint32_t device_types = 0;

  if (!remote_desktop_session->restore_data)
    return NULL;

  g_variant_get (remote_desktop_session->restore_data, "(&suv)",
                 &vendor, &version, &data);
  if (g_strcmp0 (vendor, RESTORE_VENDOR) != 0 ||
      version != RESTORE_VERSION ||
      !g_variant_is_of_type (data, G_VARIANT_TYPE_VARDICT))
    return NULL;

  g_variant_lookup (data, "devices", "u", &device_types);
  device_types &= remote_desktop_session->select.device_types;

  g_variant_builder_init (&selections_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&selections_builder, "{sv}", "selected_device_types",
                         g_variant_new_uint32 (device_types));

  if (remote_desktop_session->select.screen_cast_enable)
    {
      sources = g_variant_lookup_value (data, "sources",
                                        G_VARIANT_TYPE ("a(ua{sv})"));
      if (sources)
        source_selections =
          screen_cast_restore_sources (sources,
                                       &remote_desktop_session->select.screen_cast);
      if (!source_selections)
        {
          g_variant_builder_clear (&selections_builder);
          return NULL;
        }
      g_variant_builder_add (&selections_builder, "{sv}",
                             "selected_screen_cast_sources",
                             g_variant_lookup_value (source_selections,
                                                     "selected_screen_cast_sources",
                                                     NULL));
    }
  else if (device_types == 0)
    {
      g_variant_builder_clear (&selections_builder);
      return NULL;
    }

  return g_variant_ref_sink (g_variant_builder_end (&selections_builder));
}

static gboolean
handle_start (XdpImplRemoteDesktop *object,
              GDBusMethodInvocation *invocation,
              const char *arg_handle,
              const char *arg_session_handle,
              const char *arg_app_id,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  RemoteDesktopSession *remote_desktop_session;
  g_autoptr(GError) error = NULL;
  RemoteDesktopDialogHandle *dialog_handle;
  GVariantBuilder results_builder;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);
  request_export (request,
                  g_dbus_method_invocation_get_connection (invocation));

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  if (!remote_desktop_session)
    {
      g_warning ("Attempted to start non existing remote desktop session");
      goto err;
    }

  if (remote_desktop_session->dialog_handle)
    {
      g_warning ("Screen cast dialog already open");
      goto err;
    }

  /* A choice the user asked to remember is started without asking */
  {
    g_autoptr(GVariant) selections = restore_selections (remote_desktop_session);

    if (selections)
      {
        remote_desktop_session->start_invocation = invocation;
        request_unexport (request);
        if (!start_session (remote_desktop_session, selections, &error))
          {
            g_warning ("Failed to restore remote desktop session: %s",
                       error->message);
            cancel_start_session (remote_desktop_session, 2);
          }
        return TRUE;
      }
  }

  dialog_handle = create_remote_desktop_dialog (remote_desktop_session,
                                                invocation,
                                                request,
                                                arg_parent_window);

  remote_desktop_session->start_invocation = invocation;
  remote_desktop_session->dialog_handle = dialog_handle;

  return TRUE;

err:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE ("a(ua{sv}"));
  xdp_impl_remote_desktop_complete_start (object, invocation, 2,
                                          g_variant_builder_end (&results_builder));

  return TRUE;
}

static gboolean
handle_notify_pointer_motion (XdpImplRemoteDesktop *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_session_handle,
                              GVariant *arg_options,
                              double dx,
                              double dy)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;

  org_cinnamon_muffin_remote_desktop_session_call_notify_pointer_motion_relative (proxy,
                                                                               dx, dy,
                                                                               NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_motion (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_pointer_motion_absolute (XdpImplRemoteDesktop *object,
                                       GDBusMethodInvocation *invocation,
                                       const char *arg_session_handle,
                                       GVariant *arg_options,
                                       uint32_t stream,
                                       double x,
                                       double y)
{
  RemoteDesktopSession *remote_desktop_session;
  MuffinScreenCastSession *muffin_screen_cast_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;
  const char *stream_path;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;
  muffin_screen_cast_session = remote_desktop_session->muffin_screen_cast_session;

  stream_path = muffin_screen_cast_session_get_stream_path_from_id (muffin_screen_cast_session,
                                                                   stream);
  org_cinnamon_muffin_remote_desktop_session_call_notify_pointer_motion_absolute (proxy,
                                                                               stream_path,
                                                                               x, y,
                                                                               NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_motion_absolute (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_pointer_button (XdpImplRemoteDesktop *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_session_handle,
                              GVariant *arg_options,
                              int32_t button,
                              uint32_t state)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;

  org_cinnamon_muffin_remote_desktop_session_call_notify_pointer_button (proxy,
                                                                      button, state,
                                                                      NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_button (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_pointer_axis (XdpImplRemoteDesktop *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            double dx,
                            double dy)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;
  gboolean finish;
  unsigned int flags = 0;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;

  if (g_variant_lookup (arg_options, "finish", "b", &finish))
    {
      if (finish)
        flags = MUFFIN_REMOTE_DESKTOP_NOTIFY_AXIS_FLAGS_FINISH;
    }

  org_cinnamon_muffin_remote_desktop_session_call_notify_pointer_axis (proxy,
                                                                    dx, dy, flags,
                                                                    NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_axis (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_pointer_axis_discrete (XdpImplRemoteDesktop *object,
                                     GDBusMethodInvocation *invocation,
                                     const char *arg_session_handle,
                                     GVariant *arg_options,
                                     uint32_t axis,
                                     int32_t steps)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;

  org_cinnamon_muffin_remote_desktop_session_call_notify_pointer_axis_discrete (proxy,
                                                                             axis, steps,
                                                                             NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_axis_discrete (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_keyboard_keycode (XdpImplRemoteDesktop *object,
                                GDBusMethodInvocation *invocation,
                                const char *arg_session_handle,
                                GVariant *arg_options,
                                int32_t keycode,
                                uint32_t state)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;

  org_cinnamon_muffin_remote_desktop_session_call_notify_keyboard_keycode (proxy,
                                                                        keycode, state,
                                                                        NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_keyboard_keycode (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_keyboard_keysym (XdpImplRemoteDesktop *object,
                               GDBusMethodInvocation *invocation,
                               const char *arg_session_handle,
                               GVariant *arg_options,
                               int32_t keysym,
                               uint32_t state)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;

  org_cinnamon_muffin_remote_desktop_session_call_notify_keyboard_keysym (proxy,
                                                                       keysym, state,
                                                                       NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_keyboard_keysym (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_touch_down (XdpImplRemoteDesktop *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_session_handle,
                          GVariant *arg_options,
                          uint32_t stream,
                          uint32_t slot,
                          double x,
                          double y)
{
  RemoteDesktopSession *remote_desktop_session;
  MuffinScreenCastSession *muffin_screen_cast_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;
  const char *stream_path;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;
  muffin_screen_cast_session = remote_desktop_session->muffin_screen_cast_session;

  stream_path = muffin_screen_cast_session_get_stream_path_from_id (muffin_screen_cast_session,
                                                                   stream);
  org_cinnamon_muffin_remote_desktop_session_call_notify_touch_down (proxy,
                                                                  stream_path,
                                                                  slot,
                                                                  x, y,
                                                                  NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_touch_down (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_touch_motion (XdpImplRemoteDesktop *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            uint32_t stream,
                            uint32_t slot,
                            double x,
                            double y)
{
  RemoteDesktopSession *remote_desktop_session;
  MuffinScreenCastSession *muffin_screen_cast_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;
  const char *stream_path;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;
  muffin_screen_cast_session = remote_desktop_session->muffin_screen_cast_session;

  stream_path = muffin_screen_cast_session_get_stream_path_from_id (muffin_screen_cast_session,
                                                                   stream);
  org_cinnamon_muffin_remote_desktop_session_call_notify_touch_motion (proxy,
                                                                    stream_path,
                                                                    slot,
                                                                    x, y,
                                                                    NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_touch_motion (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_touch_up (XdpImplRemoteDesktop *object,
                        GDBusMethodInvocation *invocation,
                        const char *arg_session_handle,
                        GVariant *arg_options,
                        uint32_t slot)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgCinnamonMuffinRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->muffin_session_proxy;

  org_cinnamon_muffin_remote_desktop_session_call_notify_touch_up (proxy,
                                                                slot,
                                                                NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_touch_up (object, invocation);
  return TRUE;
}

static unsigned int
muffin_device_types_xdp_device_types (unsigned int muffin_device_types)
{
  unsigned int supported_device_types = REMOTE_DESKTOP_DEVICE_TYPE_NONE;

  if (muffin_device_types & MUFFIN_REMOTE_DESKTOP_DEVICE_TYPE_POINTER)
    supported_device_types |= REMOTE_DESKTOP_DEVICE_TYPE_POINTER;
  if (muffin_device_types & MUFFIN_REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD)
    supported_device_types |= REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD;
  if (muffin_device_types & MUFFIN_REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN)
    supported_device_types |= REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN;

  return supported_device_types;
}

static void
remote_desktop_name_appeared (GDBusConnection *connection,
                              const char *name,
                              const char *name_owner,
                              gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  unsigned int supported_device_types;

  remote_desktop =
    org_cinnamon_muffin_remote_desktop_proxy_new_sync (impl_connection,
                                                    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                    "org.cinnamon.Muffin.RemoteDesktop",
                                                    "/org/cinnamon/Muffin/RemoteDesktop",
                                                    NULL,
                                                    &error);
  if (!remote_desktop)
    {
      g_warning ("Failed to acquire org.cinnamon.Muffin.RemoteDesktop proxy: %s",
                 error->message);
      return;
    }

  impl = G_DBUS_INTERFACE_SKELETON (xdp_impl_remote_desktop_skeleton_new ());

  g_signal_connect (impl, "handle-create-session",
                    G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (impl, "handle-select-devices",
                    G_CALLBACK (handle_select_devices), NULL);
  g_signal_connect (impl, "handle-start",
                    G_CALLBACK (handle_start), NULL);

  g_signal_connect (impl, "handle-notify-pointer-motion",
                    G_CALLBACK (handle_notify_pointer_motion), NULL);
  g_signal_connect (impl, "handle-notify-pointer-motion-absolute",
                    G_CALLBACK (handle_notify_pointer_motion_absolute), NULL);
  g_signal_connect (impl, "handle-notify-pointer-button",
                    G_CALLBACK (handle_notify_pointer_button), NULL);
  g_signal_connect (impl, "handle-notify-pointer-axis",
                    G_CALLBACK (handle_notify_pointer_axis), NULL);
  g_signal_connect (impl, "handle-notify-pointer-axis-discrete",
                    G_CALLBACK (handle_notify_pointer_axis_discrete), NULL);
  g_signal_connect (impl, "handle-notify-keyboard-keycode",
                    G_CALLBACK (handle_notify_keyboard_keycode), NULL);
  g_signal_connect (impl, "handle-notify-keyboard-keysym",
                    G_CALLBACK (handle_notify_keyboard_keysym), NULL);
  g_signal_connect (impl, "handle-notify-touch-down",
                    G_CALLBACK (handle_notify_touch_down), NULL);
  g_signal_connect (impl, "handle-notify-touch-motion",
                    G_CALLBACK (handle_notify_touch_motion), NULL);
  g_signal_connect (impl, "handle-notify-touch-up",
                    G_CALLBACK (handle_notify_touch_up), NULL);

  supported_device_types =
    org_cinnamon_muffin_remote_desktop_get_supported_device_types (remote_desktop);
  g_object_set (G_OBJECT (impl),
                "available-device-types",
                muffin_device_types_xdp_device_types (supported_device_types),
                "version", 2,
                NULL);

  if (!g_dbus_interface_skeleton_export (impl,
                                         impl_connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Failed to export remote desktop portal implementation object: %s",
                 error->message);
      return;
    }

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (impl)->name);
}

static void
remote_desktop_name_vanished (GDBusConnection *connection,
                              const char *name,
                              gpointer user_data)
{
  if (impl)
    {
      g_dbus_interface_skeleton_unexport (impl);
      g_clear_object (&impl);
    }

  g_clear_object (&remote_desktop);
}

gboolean
remote_desktop_init (GDBusConnection *connection,
                     GError **error)
{
  impl_connection = connection;
  muffin_screen_cast = muffin_screen_cast_new (connection);

  remote_desktop_name_watch = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                                "org.cinnamon.Muffin.RemoteDesktop",
                                                G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                remote_desktop_name_appeared,
                                                remote_desktop_name_vanished,
                                                NULL,
                                                NULL);

  return TRUE;
}

static void
remote_desktop_session_close (Session *session)
{
  RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)session;
  OrgCinnamonMuffinRemoteDesktopSession *session_proxy;
  MuffinScreenCastSession *muffin_screen_cast_session;
  g_autoptr(GError) error = NULL;

  sharing_indicator_remove (session);

  muffin_screen_cast_session = remote_desktop_session->muffin_screen_cast_session;
  if (muffin_screen_cast_session)
    {
      g_signal_handler_disconnect (muffin_screen_cast_session,
                                   remote_desktop_session->session_ready_handler_id);
      g_clear_object (&remote_desktop_session->muffin_screen_cast_session);
    }

  session_proxy = remote_desktop_session->muffin_session_proxy;
  g_signal_handler_disconnect (session_proxy,
                               remote_desktop_session->closed_handler_id);

  if (!org_cinnamon_muffin_remote_desktop_session_call_stop_sync (session_proxy,
                                                               NULL,
                                                               &error))
    {
      g_warning ("Failed to stop screen cast session: %s", error->message);
      return;
    }
}

static void
remote_desktop_session_finalize (GObject *object)
{
  RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)object;

  g_free (remote_desktop_session->muffin_session_path);
  g_clear_pointer (&remote_desktop_session->restore_data, g_variant_unref);
  g_clear_pointer (&remote_desktop_session->restore_sources, g_variant_unref);
  g_free (remote_desktop_session->app_id);
  g_free (remote_desktop_session->what);

  G_OBJECT_CLASS (remote_desktop_session_parent_class)->finalize (object);
}

static void
remote_desktop_session_init (RemoteDesktopSession *remote_desktop_session)
{
}

static void
remote_desktop_session_class_init (RemoteDesktopSessionClass *klass)
{
  GObjectClass *gobject_class;
  SessionClass *session_class;

  gobject_class = (GObjectClass *)klass;
  gobject_class->finalize = remote_desktop_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = remote_desktop_session_close;
}
