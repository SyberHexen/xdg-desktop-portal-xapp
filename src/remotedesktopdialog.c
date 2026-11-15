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

/*
 * Asks whether an application may control the computer: which input devices
 * (ticked as asked for, the user can untick them), what it may see when it
 * also asks for that, and whether to remember the choice.
 */

#include "config.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

#include "remotedesktop.h"
#include "remotedesktopdialog.h"
#include "screencastwidget.h"

struct _RemoteDesktopDialog
{
  GtkWindow parent;

  GtkWidget *accept_button;
  GtkWidget *app_image;
  GtkWidget *heading;
  GtkWidget *description;
  GtkWidget *device_box;
  GtkWidget *screen_cast_widget;
  GtkWidget *persist_check;

  RemoteDesktopDeviceType device_types;

  gboolean screen_cast_enable;
  ScreenCastSelection screen_cast;

  gboolean is_screen_cast_sources_selected;
};

struct _RemoteDesktopDialogClass
{
  GtkWindowClass *parent_class;
};

enum
{
  DONE,

  N_SIGNAL
};

static guint signals[N_SIGNAL];

static GQuark quark_device_type;

G_DEFINE_TYPE (RemoteDesktopDialog, remote_desktop_dialog, GTK_TYPE_WINDOW)

static RemoteDesktopDeviceType
get_selected_device_types (RemoteDesktopDialog *dialog)
{
  RemoteDesktopDeviceType device_types = 0;
  g_autoptr(GList) children = NULL;
  GList *l;

  children = gtk_container_get_children (GTK_CONTAINER (dialog->device_box));
  for (l = children; l; l = l->next)
    {
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (l->data)))
        device_types |= GPOINTER_TO_UINT (g_object_get_qdata (G_OBJECT (l->data),
                                                              quark_device_type));
    }

  return device_types;
}

static void
button_clicked (GtkWidget           *button,
                RemoteDesktopDialog *dialog)
{
  int response;
  GVariant *selections;

  gtk_widget_hide (GTK_WIDGET (dialog));

  if (button == dialog->accept_button)
    {
      GVariantBuilder selections_builder;

      response = GTK_RESPONSE_OK;

      g_variant_builder_init (&selections_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&selections_builder, "{sv}",
                             "selected_device_types",
                             g_variant_new_uint32 (get_selected_device_types (dialog)));
      if (dialog->screen_cast_enable)
        screen_cast_widget_add_selections (SCREEN_CAST_WIDGET (dialog->screen_cast_widget),
                                           &selections_builder);
      g_variant_builder_add (&selections_builder, "{sv}", "persist",
                             g_variant_new_boolean (
                               gtk_widget_get_visible (dialog->persist_check) &&
                               gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (dialog->persist_check))));
      selections = g_variant_builder_end (&selections_builder);
    }
  else
    {
      response = GTK_RESPONSE_CANCEL;
      selections = NULL;
    }

  g_signal_emit (dialog, signals[DONE], 0, response, selections);
}

/* Allowed once something is allowed, and what it may see is chosen when it
 * asked to see something */
static void
update_button_sensitivity (RemoteDesktopDialog *dialog)
{
  gboolean can_accept;

  if (dialog->screen_cast_enable)
    can_accept = dialog->is_screen_cast_sources_selected;
  else
    can_accept = get_selected_device_types (dialog) != 0;

  gtk_widget_set_sensitive (dialog->accept_button, can_accept);
}

static void
on_device_toggled (GtkToggleButton     *button,
                   RemoteDesktopDialog *dialog)
{
  update_button_sensitivity (dialog);
}

static void
add_device (RemoteDesktopDialog     *dialog,
            RemoteDesktopDeviceType  device_type,
            const char              *icon_name,
            const char              *name)
{
  GtkWidget *check;
  GtkWidget *box;

  if (!(dialog->device_types & device_type))
    return;

  check = gtk_check_button_new ();
  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start (GTK_BOX (box),
                      gtk_image_new_from_icon_name (icon_name,
                                                    GTK_ICON_SIZE_LARGE_TOOLBAR),
                      FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), gtk_label_new (name), FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (check), box);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), TRUE);
  g_object_set_qdata (G_OBJECT (check), quark_device_type,
                      GUINT_TO_POINTER (device_type));
  g_signal_connect (check, "toggled", G_CALLBACK (on_device_toggled), dialog);
  gtk_widget_show_all (check);
  gtk_container_add (GTK_CONTAINER (dialog->device_box), check);
}

static void
on_has_selection_changed (ScreenCastWidget    *screen_cast_widget,
                          gboolean             has_selection,
                          RemoteDesktopDialog *dialog)
{
  dialog->is_screen_cast_sources_selected = has_selection;
  update_button_sensitivity (dialog);
}

RemoteDesktopDialog *
remote_desktop_dialog_new (const char              *app_id,
                           RemoteDesktopDeviceType  device_types,
                           ScreenCastSelection     *screen_cast_select,
                           gboolean                 offer_persist)
{
  RemoteDesktopDialog *dialog;
  g_autoptr(GAppInfo) info = NULL;
  g_autofree char *heading = NULL;
  const char *description;

  dialog = g_object_new (REMOTE_DESKTOP_TYPE_DIALOG, NULL);
  dialog->device_types = device_types;
  dialog->screen_cast_enable = screen_cast_select != NULL;
  if (screen_cast_select)
    dialog->screen_cast = *screen_cast_select;

  if (app_id && *app_id)
    {
      g_autofree char *id = g_strconcat (app_id, ".desktop", NULL);

      info = G_APP_INFO (g_desktop_app_info_new (id));
    }

  if (info)
    {
      GIcon *icon = g_app_info_get_icon (info);

      heading = g_strdup_printf (_("%s wants to control this computer"),
                                 g_app_info_get_display_name (info));
      if (icon)
        gtk_image_set_from_gicon (GTK_IMAGE (dialog->app_image), icon,
                                  GTK_ICON_SIZE_DIALOG);
    }
  else
    {
      heading = g_strdup (_("An application wants to control this computer"));
    }

  if (dialog->screen_cast_enable)
    description = _("It will be able to use the devices allowed below, and to "
                    "see the screen or window you choose. Only allow "
                    "applications you trust.");
  else
    description = _("It will be able to use the devices allowed below. Only "
                    "allow applications you trust.");

  gtk_label_set_label (GTK_LABEL (dialog->heading), heading);
  gtk_label_set_label (GTK_LABEL (dialog->description), description);

  add_device (dialog, REMOTE_DESKTOP_DEVICE_TYPE_POINTER,
              "input-mouse", _("Mouse"));
  add_device (dialog, REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD,
              "input-keyboard", _("Keyboard"));
  add_device (dialog, REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN,
              "input-tablet", _("Touch screen"));

  if (dialog->screen_cast_enable)
    {
      ScreenCastWidget *screen_cast_widget =
        SCREEN_CAST_WIDGET (dialog->screen_cast_widget);

      screen_cast_widget_set_app_id (screen_cast_widget, app_id);
      screen_cast_widget_set_compact (screen_cast_widget);
      screen_cast_widget_set_allow_multiple (screen_cast_widget,
                                             screen_cast_select->multiple);
      screen_cast_widget_set_source_types (screen_cast_widget,
                                           screen_cast_select->source_types);
      g_signal_connect (screen_cast_widget, "has-selection-changed",
                        G_CALLBACK (on_has_selection_changed), dialog);
      gtk_widget_show (GTK_WIDGET (screen_cast_widget));
      gtk_window_set_default_size (GTK_WINDOW (dialog), 840, 620);
    }

  gtk_widget_set_visible (dialog->persist_check, offer_persist);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dialog->persist_check),
                                offer_persist);

  update_button_sensitivity (dialog);

  return dialog;
}

static void
remote_desktop_dialog_init (RemoteDesktopDialog *dialog)
{
  gtk_widget_init_template (GTK_WIDGET (dialog));
}

static gboolean
remote_desktop_dialog_delete_event (GtkWidget   *dialog,
                                    GdkEventAny *event)
{
  gtk_widget_hide (dialog);

  g_signal_emit (dialog, signals[DONE], 0, GTK_RESPONSE_CANCEL, NULL);

  return TRUE;
}

static void
remote_desktop_dialog_class_init (RemoteDesktopDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->delete_event = remote_desktop_dialog_delete_event;

  signals[DONE] = g_signal_new ("done",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                0,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 2,
                                G_TYPE_INT,
                                G_TYPE_VARIANT);

  init_screen_cast_widget ();

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/xapp/remotedesktopdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, accept_button);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, app_image);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, heading);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, description);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, device_box);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, screen_cast_widget);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, persist_check);
  gtk_widget_class_bind_template_callback (widget_class, button_clicked);

  quark_device_type = g_quark_from_static_string ("-remote-desktop-device-type");
}
