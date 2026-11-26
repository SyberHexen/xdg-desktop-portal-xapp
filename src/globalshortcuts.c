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
 * org.freedesktop.impl.portal.GlobalShortcuts for Cinnamon.
 *
 * An application binds shortcuts (an id, a description and maybe a preferred
 * key combination); the first time, the user confirms or changes the keys in
 * a dialog, and the choice is remembered per application. The keys are
 * grabbed through Cinnamon's org.cinnamon.PortalHandlers (GrabShortcuts),
 * which only takes grabs from this portal, and pressing one is reported back
 * as ShortcutActivated, which becomes the Activated signal.
 *
 * Keys are kept as GTK accelerators ("<Control><Shift>h"), which Muffin's
 * keybindings parse as well. Releasing a key isn't reported (no Deactivated):
 * Muffin's keybindings only see the press.
 */

#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "xdg-desktop-portal-dbus.h"
#include "common-dbus.h"

#include "globalshortcuts.h"
#include "externalwindow.h"
#include "request.h"
#include "session.h"
#include "utils.h"

#define SHORTCUTS_FILE_DIR "xdg-desktop-portal-xapp"
#define SHORTCUTS_FILE_NAME "global-shortcuts.conf"
#define NO_APP_ID_GROUP "(no app id)"

typedef struct
{
  char *id;
  char *description;
  char *accelerator;   /* GTK accelerator, "" when the shortcut has no keys */
} Shortcut;

typedef struct _GlobalShortcutsDialog GlobalShortcutsDialog;

typedef struct _GlobalShortcutsSession
{
  Session parent;

  char *app_id;
  GPtrArray *shortcuts;   /* Shortcut, in the application's order */

  GlobalShortcutsDialog *dialog;
} GlobalShortcutsSession;

typedef struct _GlobalShortcutsSessionClass
{
  SessionClass parent_class;
} GlobalShortcutsSessionClass;

/* An open confirmation dialog: for BindShortcuts (with a request and the
 * invocation to answer) or for ConfigureShortcuts (neither) */
struct _GlobalShortcutsDialog
{
  GlobalShortcutsSession *session;
  GPtrArray *shortcuts;   /* the keys being edited, applied on Allow */

  Request *request;
  GDBusMethodInvocation *invocation;

  GtkWidget *window;
  GtkWidget *fake_parent;
  ExternalWindow *external_parent;
  GPtrArray *buttons;     /* GtkButton per shortcut */
  int capturing;          /* shortcut whose keys are being typed, or -1 */
};

static GDBusConnection *impl_connection;
static XdpImplGlobalShortcuts *impl;
static OrgCinnamonPortalHandlers *portal_handlers;
static GCancellable *portal_handlers_cancellable;
static GHashTable *sessions;   /* session handle -> GlobalShortcutsSession */

GType global_shortcuts_session_get_type (void);
G_DEFINE_TYPE (GlobalShortcutsSession, global_shortcuts_session, session_get_type ())

/* ---- Shortcuts ---- */

static Shortcut *
shortcut_new (const char *id,
              const char *description,
              const char *accelerator)
{
  Shortcut *shortcut = g_new0 (Shortcut, 1);

  shortcut->id = g_strdup (id);
  shortcut->description = g_strdup (description ? description : "");
  shortcut->accelerator = g_strdup (accelerator ? accelerator : "");

  return shortcut;
}

static void
shortcut_free (Shortcut *shortcut)
{
  g_free (shortcut->id);
  g_free (shortcut->description);
  g_free (shortcut->accelerator);
  g_free (shortcut);
}

static GPtrArray *
shortcuts_copy (GPtrArray *shortcuts)
{
  GPtrArray *copy = g_ptr_array_new_with_free_func ((GDestroyNotify) shortcut_free);

  for (guint i = 0; i < shortcuts->len; i++)
    {
      Shortcut *shortcut = g_ptr_array_index (shortcuts, i);
      g_ptr_array_add (copy, shortcut_new (shortcut->id,
                                           shortcut->description,
                                           shortcut->accelerator));
    }

  return copy;
}

/* A GTK accelerator from key and modifiers, in the "<Control><Shift>h" form
 * both GTK and Muffin parse */
static char *
accelerator_from_key (guint keyval,
                      GdkModifierType mods)
{
  GString *accelerator = g_string_new (NULL);
  const char *name;

  if (mods & GDK_CONTROL_MASK)
    g_string_append (accelerator, "<Control>");
  if (mods & GDK_SHIFT_MASK)
    g_string_append (accelerator, "<Shift>");
  if (mods & GDK_MOD1_MASK)
    g_string_append (accelerator, "<Alt>");
  if (mods & (GDK_SUPER_MASK | GDK_MOD4_MASK))
    g_string_append (accelerator, "<Super>");

  name = gdk_keyval_name (keyval);
  if (!name)
    {
      g_string_free (accelerator, TRUE);
      return g_strdup ("");
    }
  g_string_append (accelerator, name);

  return g_string_free (accelerator, FALSE);
}

/* The XDG shortcuts form ("CTRL+SHIFT+h": modifiers CTRL, ALT, SHIFT, NUM,
 * LOGO, then an XKB keysym name) as a GTK accelerator, or "" */
static char *
accelerator_from_trigger (const char *trigger)
{
  g_auto(GStrv) parts = NULL;
  GdkModifierType mods = 0;
  guint keyval = 0;
  guint n;

  if (!trigger || !*trigger)
    return g_strdup ("");

  parts = g_strsplit (trigger, "+", -1);
  n = g_strv_length (parts);

  /* a trailing "+" is the plus key itself */
  if (n >= 2 && parts[n - 1][0] == '\0')
    {
      g_free (parts[n - 1]);
      parts[n - 1] = NULL;
      n--;
      g_free (parts[n - 1]);
      parts[n - 1] = g_strdup ("plus");
    }

  for (guint i = 0; i + 1 < n; i++)
    {
      if (g_ascii_strcasecmp (parts[i], "CTRL") == 0 ||
          g_ascii_strcasecmp (parts[i], "CONTROL") == 0)
        mods |= GDK_CONTROL_MASK;
      else if (g_ascii_strcasecmp (parts[i], "ALT") == 0)
        mods |= GDK_MOD1_MASK;
      else if (g_ascii_strcasecmp (parts[i], "SHIFT") == 0)
        mods |= GDK_SHIFT_MASK;
      else if (g_ascii_strcasecmp (parts[i], "LOGO") == 0 ||
               g_ascii_strcasecmp (parts[i], "SUPER") == 0)
        mods |= GDK_SUPER_MASK;
      else if (g_ascii_strcasecmp (parts[i], "NUM") == 0)
        ; /* Num Lock isn't a shortcut modifier here */
      else
        return g_strdup ("");
    }

  if (n > 0)
    {
      keyval = gdk_keyval_from_name (parts[n - 1]);
      if (keyval == GDK_KEY_VoidSymbol || keyval == 0)
        {
          g_autofree char *lower = g_ascii_strdown (parts[n - 1], -1);
          keyval = gdk_keyval_from_name (lower);
        }
    }

  if (keyval == GDK_KEY_VoidSymbol || keyval == 0)
    return g_strdup ("");

  return accelerator_from_key (gdk_keyval_to_lower (keyval), mods);
}

/* What the user reads for an accelerator, such as "Ctrl+Shift+H" */
static char *
accelerator_label (const char *accelerator)
{
  guint keyval = 0;
  GdkModifierType mods = 0;

  if (!accelerator || !*accelerator)
    return g_strdup ("");

  gtk_accelerator_parse (accelerator, &keyval, &mods);
  if (keyval == 0)
    return g_strdup ("");

  return gtk_accelerator_get_label (keyval, mods);
}

static GVariant *
shortcuts_to_variant (GPtrArray *shortcuts)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));
  for (guint i = 0; i < shortcuts->len; i++)
    {
      Shortcut *shortcut = g_ptr_array_index (shortcuts, i);
      g_autofree char *label = accelerator_label (shortcut->accelerator);
      GVariantBuilder props;

      g_variant_builder_init (&props, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&props, "{sv}", "description",
                             g_variant_new_string (shortcut->description));
      g_variant_builder_add (&props, "{sv}", "trigger_description",
                             g_variant_new_string (label));
      g_variant_builder_add (&builder, "(sa{sv})", shortcut->id, &props);
    }

  return g_variant_builder_end (&builder);
}

/* ---- The remembered keys, per application ---- */

static char *
shortcuts_file_path (void)
{
  return g_build_filename (g_get_user_config_dir (),
                           SHORTCUTS_FILE_DIR, SHORTCUTS_FILE_NAME, NULL);
}

static const char *
app_group (const char *app_id)
{
  return (app_id && *app_id) ? app_id : NO_APP_ID_GROUP;
}

static GKeyFile *
load_shortcuts_file (void)
{
  g_autofree char *path = shortcuts_file_path ();
  GKeyFile *key_file = g_key_file_new ();

  g_key_file_load_from_file (key_file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

  return key_file;
}

static void
save_shortcuts (const char *app_id,
                GPtrArray *shortcuts)
{
  g_autoptr(GKeyFile) key_file = load_shortcuts_file ();
  g_autofree char *path = shortcuts_file_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr(GError) error = NULL;

  for (guint i = 0; i < shortcuts->len; i++)
    {
      Shortcut *shortcut = g_ptr_array_index (shortcuts, i);
      g_key_file_set_string (key_file, app_group (app_id),
                             shortcut->id, shortcut->accelerator);
    }

  g_mkdir_with_parents (dir, 0700);
  if (!g_key_file_save_to_file (key_file, path, &error))
    g_warning ("Failed to save the global shortcuts: %s", error->message);
}

/* ---- Grabbing the keys in Cinnamon ---- */

typedef struct
{
  GlobalShortcutsSession *session;
  GDBusMethodInvocation *invocation;   /* BindShortcuts to answer, or NULL */
  gboolean changed;                    /* emit ShortcutsChanged */
} GrabData;

static void
grab_data_free (GrabData *data)
{
  g_object_unref (data->session);
  g_free (data);
}

static void
finish_grab (GrabData *data)
{
  GlobalShortcutsSession *session = data->session;

  if (data->invocation)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&results, "{sv}", "shortcuts",
                             shortcuts_to_variant (session->shortcuts));
      xdp_impl_global_shortcuts_complete_bind_shortcuts (impl,
                                                         data->invocation,
                                                         0,
                                                         g_variant_builder_end (&results));
    }

  if (data->changed && !((Session *) session)->closed)
    xdp_impl_global_shortcuts_emit_shortcuts_changed (impl,
                                                      ((Session *) session)->id,
                                                      shortcuts_to_variant (session->shortcuts));

  grab_data_free (data);
}

static void
on_grab_done (GObject *source,
              GAsyncResult *result,
              gpointer user_data)
{
  GrabData *data = user_data;
  g_auto(GStrv) failed = NULL;
  g_autoptr(GError) error = NULL;

  if (!org_cinnamon_portal_handlers_call_grab_shortcuts_finish (ORG_CINNAMON_PORTAL_HANDLERS (source),
                                                                &failed,
                                                                result,
                                                                &error))
    {
      g_warning ("Failed to grab global shortcuts: %s", error->message);
    }
  else
    {
      for (guint i = 0; failed && failed[i]; i++)
        g_warning ("Global shortcut \"%s\" of %s couldn't be grabbed (the keys are taken or invalid)",
                   failed[i], app_group (data->session->app_id));
    }

  finish_grab (data);
}

/* Grabs the session's keys (in place of what it grabbed before), then answers
 * the invocation if there is one */
static void
grab_session (GlobalShortcutsSession *session,
              GDBusMethodInvocation *invocation,
              gboolean changed)
{
  GrabData *data = g_new0 (GrabData, 1);
  GVariantBuilder builder;

  data->session = g_object_ref (session);
  data->invocation = invocation;
  data->changed = changed;

  if (!portal_handlers)
    {
      g_warning ("Cinnamon isn't running, global shortcuts are grabbed once it is");
      finish_grab (data);
      return;
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
  for (guint i = 0; i < session->shortcuts->len; i++)
    {
      Shortcut *shortcut = g_ptr_array_index (session->shortcuts, i);
      if (*shortcut->accelerator)
        g_variant_builder_add (&builder, "(ss)", shortcut->id, shortcut->accelerator);
    }

  org_cinnamon_portal_handlers_call_grab_shortcuts (portal_handlers,
                                                    ((Session *) session)->id,
                                                    g_variant_builder_end (&builder),
                                                    NULL,
                                                    on_grab_done,
                                                    data);
}

static void
ungrab_session (GlobalShortcutsSession *session)
{
  if (portal_handlers)
    org_cinnamon_portal_handlers_call_ungrab_shortcuts (portal_handlers,
                                                        ((Session *) session)->id,
                                                        NULL, NULL, NULL);
}

static void
on_shortcut_activated (OrgCinnamonPortalHandlers *proxy,
                       const char *owner,
                       const char *shortcut_id,
                       guint timestamp,
                       gpointer user_data)
{
  GVariantBuilder options;

  if (!g_hash_table_contains (sessions, owner))
    return;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  xdp_impl_global_shortcuts_emit_activated (impl, owner, shortcut_id,
                                            (guint64) timestamp,
                                            g_variant_builder_end (&options));
}

static void
on_portal_handlers_acquired (GObject *source,
                             GAsyncResult *result,
                             gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  OrgCinnamonPortalHandlers *proxy;
  GHashTableIter iter;
  gpointer value;

  proxy = org_cinnamon_portal_handlers_proxy_new_for_bus_finish (result, &error);
  if (!proxy)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to acquire org.cinnamon.PortalHandlers proxy: %s",
                   error->message);
      return;
    }

  portal_handlers = proxy;
  g_signal_connect (portal_handlers, "shortcut-activated",
                    G_CALLBACK (on_shortcut_activated), NULL);

  /* Cinnamon (re)started: grab the keys of the open sessions again */
  g_hash_table_iter_init (&iter, sessions);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    grab_session (value, NULL, FALSE);
}

static void
on_cinnamon_appeared (GDBusConnection *connection,
                      const char *name,
                      const char *name_owner,
                      gpointer user_data)
{
  portal_handlers_cancellable = g_cancellable_new ();
  org_cinnamon_portal_handlers_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                  G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                  "org.Cinnamon",
                                                  "/org/Cinnamon",
                                                  portal_handlers_cancellable,
                                                  on_portal_handlers_acquired,
                                                  NULL);
}

static void
on_cinnamon_vanished (GDBusConnection *connection,
                      const char *name,
                      gpointer user_data)
{
  if (portal_handlers_cancellable)
    {
      g_cancellable_cancel (portal_handlers_cancellable);
      g_clear_object (&portal_handlers_cancellable);
    }
  g_clear_object (&portal_handlers);
}

/* ---- The dialog ---- */

static void dialog_close (GlobalShortcutsDialog *dialog);

static char *
app_display_name (const char *app_id)
{
  g_autofree char *desktop_id = NULL;
  g_autoptr(GDesktopAppInfo) info = NULL;

  if (!app_id || !*app_id)
    return g_strdup (_("An application"));

  desktop_id = g_strconcat (app_id, ".desktop", NULL);
  info = g_desktop_app_info_new (desktop_id);
  if (info)
    return g_strdup (g_app_info_get_display_name (G_APP_INFO (info)));

  return g_strdup (app_id);
}

static void
update_button (GlobalShortcutsDialog *dialog,
               guint index)
{
  Shortcut *shortcut = g_ptr_array_index (dialog->shortcuts, index);
  GtkWidget *button = g_ptr_array_index (dialog->buttons, index);
  g_autofree char *label = NULL;

  if ((int) index == dialog->capturing)
    {
      gtk_button_set_label (GTK_BUTTON (button), _("Press the keys…"));
      return;
    }

  label = accelerator_label (shortcut->accelerator);
  gtk_button_set_label (GTK_BUTTON (button), *label ? label : _("Disabled"));
}

static void
stop_capturing (GlobalShortcutsDialog *dialog)
{
  int index = dialog->capturing;

  if (index < 0)
    return;

  dialog->capturing = -1;
  gtk_grab_remove (dialog->window);
  update_button (dialog, index);
}

static void
on_shortcut_button_clicked (GtkButton *button,
                            GlobalShortcutsDialog *dialog)
{
  guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "index"));

  stop_capturing (dialog);
  dialog->capturing = index;
  gtk_grab_add (dialog->window);
  update_button (dialog, index);
}

static gboolean
is_function_key (guint keyval)
{
  return keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35;
}

/* While a shortcut is being changed, the next key combination becomes its
 * keys: Escape cancels, Backspace disables the shortcut. A key alone (other
 * than a function key) isn't taken, since it would stop that key from
 * working anywhere else. */
static gboolean
on_key_press (GtkWidget *widget,
              GdkEventKey *event,
              GlobalShortcutsDialog *dialog)
{
  Shortcut *shortcut;
  GdkModifierType mods;
  guint keyval;

  if (dialog->capturing < 0)
    return GDK_EVENT_PROPAGATE;

  if (event->is_modifier)
    return GDK_EVENT_STOP;

  shortcut = g_ptr_array_index (dialog->shortcuts, dialog->capturing);
  mods = event->state & gtk_accelerator_get_default_mod_mask ();
  keyval = gdk_keyval_to_lower (event->keyval);
  if (keyval == GDK_KEY_ISO_Left_Tab)
    keyval = GDK_KEY_Tab;

  if (mods == 0 && keyval == GDK_KEY_Escape)
    {
      stop_capturing (dialog);
      return GDK_EVENT_STOP;
    }

  if (mods == 0 && keyval == GDK_KEY_BackSpace)
    {
      g_free (shortcut->accelerator);
      shortcut->accelerator = g_strdup ("");
      stop_capturing (dialog);
      return GDK_EVENT_STOP;
    }

  if ((mods == 0 && !is_function_key (keyval)) ||
      !gtk_accelerator_valid (keyval, mods))
    {
      gtk_widget_error_bell (widget);
      return GDK_EVENT_STOP;
    }

  g_free (shortcut->accelerator);
  shortcut->accelerator = accelerator_from_key (keyval, mods);
  stop_capturing (dialog);

  return GDK_EVENT_STOP;
}

static void
dialog_respond (GlobalShortcutsDialog *dialog,
                gboolean allowed)
{
  GlobalShortcutsSession *session = dialog->session;
  GDBusMethodInvocation *invocation = dialog->invocation;
  gboolean configure = dialog->request == NULL;

  dialog->invocation = NULL;

  if (allowed)
    {
      g_ptr_array_unref (session->shortcuts);
      session->shortcuts = g_ptr_array_ref (dialog->shortcuts);
      save_shortcuts (session->app_id, session->shortcuts);
      dialog_close (dialog);
      grab_session (session, invocation, configure);
    }
  else
    {
      dialog_close (dialog);
      if (invocation)
        {
          GVariantBuilder results;

          g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
          xdp_impl_global_shortcuts_complete_bind_shortcuts (impl, invocation, 1,
                                                             g_variant_builder_end (&results));
        }
    }
}

static void
on_dialog_response (GtkDialog *window,
                    int response_id,
                    GlobalShortcutsDialog *dialog)
{
  dialog_respond (dialog, response_id == GTK_RESPONSE_OK);
}

static gboolean
on_request_close (XdpImplRequest *object,
                  GDBusMethodInvocation *invocation,
                  GlobalShortcutsDialog *dialog)
{
  dialog_respond (dialog, FALSE);
  xdp_impl_request_complete_close (object, invocation);

  return TRUE;
}

static void
dialog_close (GlobalShortcutsDialog *dialog)
{
  GlobalShortcutsSession *session = dialog->session;

  if (session->dialog == dialog)
    session->dialog = NULL;

  if (dialog->request)
    {
      g_signal_handlers_disconnect_by_data (dialog->request, dialog);
      request_unexport (dialog->request);
      g_clear_object (&dialog->request);
    }

  if (dialog->invocation)
    {
      GVariantBuilder results;

      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      xdp_impl_global_shortcuts_complete_bind_shortcuts (impl, dialog->invocation, 2,
                                                         g_variant_builder_end (&results));
      dialog->invocation = NULL;
    }

  g_clear_pointer (&dialog->window, gtk_widget_destroy);
  g_clear_pointer (&dialog->fake_parent, gtk_widget_destroy);
  g_clear_object (&dialog->external_parent);
  g_clear_pointer (&dialog->buttons, g_ptr_array_unref);
  g_clear_pointer (&dialog->shortcuts, g_ptr_array_unref);
  g_object_unref (session);
  g_free (dialog);
}

/* Shows the dialog for the session's shortcuts; for BindShortcuts it has the
 * request and the invocation to answer */
static void
dialog_open (GlobalShortcutsSession *session,
             GPtrArray *shortcuts,
             Request *request,
             GDBusMethodInvocation *invocation,
             const char *parent_window)
{
  GlobalShortcutsDialog *dialog = g_new0 (GlobalShortcutsDialog, 1);
  g_autofree char *app_name = app_display_name (session->app_id);
  g_autofree char *heading = NULL;
  GdkDisplay *display;
  GtkWidget *content;
  GtkWidget *box;
  GtkWidget *label;
  GtkWidget *grid;
  gboolean use_header = FALSE;

  dialog->session = g_object_ref (session);
  dialog->shortcuts = shortcuts;
  dialog->request = request ? g_object_ref (request) : NULL;
  dialog->invocation = invocation;
  dialog->buttons = g_ptr_array_new ();
  dialog->capturing = -1;
  session->dialog = dialog;

  if (parent_window && *parent_window)
    {
      dialog->external_parent = create_external_window_from_handle (parent_window);
      if (!dialog->external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   parent_window);
    }

  display = dialog->external_parent ?
            external_window_get_display (dialog->external_parent) :
            gdk_display_get_default ();
  dialog->fake_parent = g_object_new (GTK_TYPE_WINDOW,
                                      "type", GTK_WINDOW_TOPLEVEL,
                                      "screen", gdk_display_get_default_screen (display),
                                      NULL);
  g_object_ref_sink (dialog->fake_parent);

  g_object_get (gtk_settings_get_default (), "gtk-dialogs-use-header", &use_header, NULL);
  dialog->window = g_object_new (GTK_TYPE_DIALOG,
                                 "title", _("Keyboard Shortcuts"),
                                 "use-header-bar", use_header,
                                 "transient-for", dialog->fake_parent,
                                 "modal", TRUE,
                                 "resizable", FALSE,
                                 NULL);
  gtk_dialog_add_buttons (GTK_DIALOG (dialog->window),
                          _("_Cancel"), GTK_RESPONSE_CANCEL,
                          request ? _("_Allow") : _("_Save"), GTK_RESPONSE_OK,
                          NULL);
  gtk_dialog_set_default_response (GTK_DIALOG (dialog->window), GTK_RESPONSE_OK);

  content = gtk_dialog_get_content_area (GTK_DIALOG (dialog->window));
  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (box), 18);
  gtk_container_add (GTK_CONTAINER (content), box);

  heading = request ?
            g_strdup_printf (_("%s wants to use these keyboard shortcuts"), app_name) :
            g_strdup_printf (_("Keyboard shortcuts of %s"), app_name);
  label = gtk_label_new (heading);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 50);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "title");
  {
    PangoAttrList *attrs = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (label), attrs);
    pango_attr_list_unref (attrs);
  }
  gtk_container_add (GTK_CONTAINER (box), label);

  label = gtk_label_new (_("They work in every window, also when the application isn't focused. "
                           "Click a shortcut to change it; Backspace disables it."));
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 50);
  gtk_style_context_add_class (gtk_widget_get_style_context (label), "dim-label");
  gtk_container_add (GTK_CONTAINER (box), label);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 18);
  gtk_container_add (GTK_CONTAINER (box), grid);

  for (guint i = 0; i < shortcuts->len; i++)
    {
      Shortcut *shortcut = g_ptr_array_index (shortcuts, i);
      GtkWidget *button;

      label = gtk_label_new (*shortcut->description ? shortcut->description : shortcut->id);
      gtk_label_set_xalign (GTK_LABEL (label), 0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_grid_attach (GTK_GRID (grid), label, 0, i, 1, 1);

      button = gtk_button_new ();
      gtk_widget_set_size_request (button, 160, -1);
      g_object_set_data (G_OBJECT (button), "index", GUINT_TO_POINTER (i));
      g_signal_connect (button, "clicked",
                        G_CALLBACK (on_shortcut_button_clicked), dialog);
      gtk_grid_attach (GTK_GRID (grid), button, 1, i, 1, 1);
      g_ptr_array_add (dialog->buttons, button);
      update_button (dialog, i);
    }

  g_signal_connect (dialog->window, "key-press-event",
                    G_CALLBACK (on_key_press), dialog);
  g_signal_connect (dialog->window, "response",
                    G_CALLBACK (on_dialog_response), dialog);
  if (request)
    g_signal_connect (request, "handle-close",
                      G_CALLBACK (on_request_close), dialog);

  gtk_widget_show_all (content);
  gtk_widget_realize (dialog->window);
  if (dialog->external_parent)
    external_window_set_parent_of (dialog->external_parent,
                                   gtk_widget_get_window (dialog->window));
  gtk_window_present (GTK_WINDOW (dialog->window));
}

/* ---- The portal ---- */

static GlobalShortcutsSession *
lookup_global_shortcuts_session (const char *session_handle)
{
  Session *session = lookup_session (session_handle);

  if (!session ||
      !G_TYPE_CHECK_INSTANCE_TYPE (session, global_shortcuts_session_get_type ()))
    return NULL;

  return (GlobalShortcutsSession *) session;
}

static gboolean
handle_create_session (XdpImplGlobalShortcuts *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  g_autoptr(GError) error = NULL;
  GlobalShortcutsSession *session;
  GVariantBuilder results;
  int response = 0;

  session = g_object_new (global_shortcuts_session_get_type (),
                          "id", arg_session_handle,
                          NULL);
  session->app_id = g_strdup (arg_app_id);

  if (!session_export ((Session *) session,
                       g_dbus_method_invocation_get_connection (invocation),
                       &error))
    {
      g_warning ("Failed to create global shortcuts session: %s", error->message);
      g_object_unref (session);
      response = 2;
    }
  else
    {
      g_hash_table_insert (sessions, g_strdup (arg_session_handle), session);
    }

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  xdp_impl_global_shortcuts_complete_create_session (object, invocation, response,
                                                     g_variant_builder_end (&results));

  return TRUE;
}

static gboolean
handle_bind_shortcuts (XdpImplGlobalShortcuts *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       GVariant *arg_shortcuts,
                       const char *arg_parent_window,
                       GVariant *arg_options)
{
  GlobalShortcutsSession *session;
  g_autoptr(GKeyFile) key_file = NULL;
  g_autoptr(GPtrArray) shortcuts = NULL;
  g_autoptr(Request) request = NULL;
  gboolean all_remembered = TRUE;
  GVariantIter iter;
  const char *id;
  GVariant *props;

  session = lookup_global_shortcuts_session (arg_session_handle);
  if (!session || session->dialog)
    {
      GVariantBuilder results;

      g_warning ("BindShortcuts on a missing session or while its dialog is open");
      g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
      xdp_impl_global_shortcuts_complete_bind_shortcuts (object, invocation, 2,
                                                         g_variant_builder_end (&results));
      return TRUE;
    }

  key_file = load_shortcuts_file ();
  shortcuts = g_ptr_array_new_with_free_func ((GDestroyNotify) shortcut_free);

  g_variant_iter_init (&iter, arg_shortcuts);
  while (g_variant_iter_next (&iter, "(&s@a{sv})", &id, &props))
    {
      const char *description = NULL;
      const char *trigger = NULL;
      g_autofree char *accelerator = NULL;

      g_variant_lookup (props, "description", "&s", &description);
      g_variant_lookup (props, "preferred_trigger", "&s", &trigger);

      if (g_key_file_has_key (key_file, app_group (session->app_id), id, NULL))
        accelerator = g_key_file_get_string (key_file, app_group (session->app_id), id, NULL);
      else
        {
          all_remembered = FALSE;
          accelerator = accelerator_from_trigger (trigger);
        }

      g_ptr_array_add (shortcuts, shortcut_new (id, description, accelerator));
      g_variant_unref (props);
    }

  /* Keys the user already chose for this application are used as they are */
  if (all_remembered)
    {
      g_ptr_array_unref (session->shortcuts);
      session->shortcuts = g_steal_pointer (&shortcuts);
      grab_session (session, invocation, FALSE);
      return TRUE;
    }

  request = request_new (g_dbus_method_invocation_get_sender (invocation),
                         session->app_id, arg_handle);
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  dialog_open (session, g_steal_pointer (&shortcuts), request, invocation,
               arg_parent_window);

  return TRUE;
}

static gboolean
handle_list_shortcuts (XdpImplGlobalShortcuts *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle)
{
  GlobalShortcutsSession *session = lookup_global_shortcuts_session (arg_session_handle);
  GVariantBuilder results;

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);
  if (!session)
    {
      xdp_impl_global_shortcuts_complete_list_shortcuts (object, invocation, 2,
                                                         g_variant_builder_end (&results));
      return TRUE;
    }

  g_variant_builder_add (&results, "{sv}", "shortcuts",
                         shortcuts_to_variant (session->shortcuts));
  xdp_impl_global_shortcuts_complete_list_shortcuts (object, invocation, 0,
                                                     g_variant_builder_end (&results));

  return TRUE;
}

static gboolean
handle_configure_shortcuts (XdpImplGlobalShortcuts *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            const char *arg_parent_window,
                            GVariant *arg_options)
{
  GlobalShortcutsSession *session = lookup_global_shortcuts_session (arg_session_handle);

  if (session && !session->dialog && session->shortcuts->len > 0)
    dialog_open (session, shortcuts_copy (session->shortcuts), NULL, NULL,
                 arg_parent_window);

  xdp_impl_global_shortcuts_complete_configure_shortcuts (object, invocation);

  return TRUE;
}

gboolean
global_shortcuts_init (GDBusConnection *connection,
                       GError **error)
{
  impl_connection = connection;
  sessions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  impl = xdp_impl_global_shortcuts_skeleton_new ();
  g_object_set (impl, "version", 2, NULL);

  g_signal_connect (impl, "handle-create-session",
                    G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (impl, "handle-bind-shortcuts",
                    G_CALLBACK (handle_bind_shortcuts), NULL);
  g_signal_connect (impl, "handle-list-shortcuts",
                    G_CALLBACK (handle_list_shortcuts), NULL);
  g_signal_connect (impl, "handle-configure-shortcuts",
                    G_CALLBACK (handle_configure_shortcuts), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (impl),
                                         connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_bus_watch_name_on_connection (connection,
                                  "org.Cinnamon",
                                  G_BUS_NAME_WATCHER_FLAGS_NONE,
                                  on_cinnamon_appeared,
                                  on_cinnamon_vanished,
                                  NULL, NULL);

  g_debug ("providing %s",
           g_dbus_interface_skeleton_get_info (G_DBUS_INTERFACE_SKELETON (impl))->name);

  return TRUE;
}

/* ---- Session ---- */

static void
global_shortcuts_session_close (Session *session)
{
  GlobalShortcutsSession *shortcuts_session = (GlobalShortcutsSession *) session;

  if (shortcuts_session->dialog)
    dialog_respond (shortcuts_session->dialog, FALSE);

  ungrab_session (shortcuts_session);
  g_hash_table_remove (sessions, session->id);
}

static void
global_shortcuts_session_finalize (GObject *object)
{
  GlobalShortcutsSession *session = (GlobalShortcutsSession *) object;

  g_free (session->app_id);
  g_clear_pointer (&session->shortcuts, g_ptr_array_unref);

  G_OBJECT_CLASS (global_shortcuts_session_parent_class)->finalize (object);
}

static void
global_shortcuts_session_init (GlobalShortcutsSession *session)
{
  session->shortcuts = g_ptr_array_new_with_free_func ((GDestroyNotify) shortcut_free);
}

static void
global_shortcuts_session_class_init (GlobalShortcutsSessionClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  SessionClass *session_class = (SessionClass *) klass;

  gobject_class->finalize = global_shortcuts_session_finalize;
  session_class->close = global_shortcuts_session_close;
}
