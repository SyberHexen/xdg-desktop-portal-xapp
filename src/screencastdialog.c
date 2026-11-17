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

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

#include "screencastdialog.h"
#include "screencastwidget.h"
#include "displaystatetracker.h"

struct _ScreenCastDialog
{
  GtkWindow parent;

  GtkWidget *accept_button;
  GtkWidget *screen_cast_widget;
  GtkWidget *persist_check;

  gboolean multiple;
};

struct _ScreenCastDialogClass
{
  GtkWindowClass *parent_class;
};

enum
{
  DONE,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (ScreenCastDialog, screen_cast_dialog, GTK_TYPE_WINDOW)

static void
button_clicked (GtkWidget *button,
                ScreenCastDialog *dialog)
{
  int response;
  GVariant *selections;

  gtk_widget_hide (GTK_WIDGET (dialog));

  if (button == dialog->accept_button)
    {
      ScreenCastWidget *screen_cast_widget =
        SCREEN_CAST_WIDGET (dialog->screen_cast_widget);
      GVariantBuilder selections_builder;

      response = GTK_RESPONSE_OK;

      g_variant_builder_init (&selections_builder, G_VARIANT_TYPE ("a{sv}"));
      screen_cast_widget_add_selections (screen_cast_widget,
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

/* A double-clicked source is shared at once, as with the Share button */
static void
on_source_activated (ScreenCastWidget *screen_cast_widget,
                     ScreenCastDialog *dialog)
{
  if (gtk_widget_get_sensitive (dialog->accept_button))
    button_clicked (dialog->accept_button, dialog);
}

static void
on_has_selection_changed (ScreenCastWidget *screen_cast_widget,
                          gboolean has_selection,
                          ScreenCastDialog *dialog)
{
  if (has_selection)
    gtk_widget_set_sensitive (dialog->accept_button, TRUE);
  else
    gtk_widget_set_sensitive (dialog->accept_button, FALSE);
}

ScreenCastDialog *
screen_cast_dialog_new (const char *app_id,
                        ScreenCastSelection *select,
                        gboolean offer_persist)
{
  ScreenCastDialog *dialog;
  ScreenCastWidget *screen_cast_widget;

  dialog = g_object_new (SCREEN_CAST_TYPE_DIALOG, NULL);
  screen_cast_widget = SCREEN_CAST_WIDGET (dialog->screen_cast_widget);
  screen_cast_widget_set_app_id (screen_cast_widget, app_id);
  screen_cast_widget_set_allow_multiple (screen_cast_widget, select->multiple);
  screen_cast_widget_set_source_types (screen_cast_widget,
                                       select->source_types);

  /* Offered when the application asked to keep the selection */
  gtk_widget_set_visible (dialog->persist_check, offer_persist);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (dialog->persist_check),
                                offer_persist);

  return dialog;
}

static void
screen_cast_dialog_init (ScreenCastDialog *dialog)
{
  gtk_widget_init_template (GTK_WIDGET (dialog));

  g_signal_connect (dialog->screen_cast_widget, "has-selection-changed",
                    G_CALLBACK (on_has_selection_changed), dialog);
  g_signal_connect (dialog->screen_cast_widget, "source-activated",
                    G_CALLBACK (on_source_activated), dialog);
  gtk_widget_show (dialog->screen_cast_widget);
}

static gboolean
screen_cast_dialog_delete_event (GtkWidget *dialog, GdkEventAny *event)
{
  gtk_widget_hide (dialog);

  g_signal_emit (dialog, signals[DONE], 0, GTK_RESPONSE_CANCEL, NULL);

  return TRUE;
}

static void
screen_cast_dialog_class_init (ScreenCastDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  widget_class->delete_event = screen_cast_dialog_delete_event;

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

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/xapp/screencastdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, ScreenCastDialog, accept_button);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastDialog, screen_cast_widget);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastDialog, persist_check);
  gtk_widget_class_bind_template_callback (widget_class, button_clicked);
}
