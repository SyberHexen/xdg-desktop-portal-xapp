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

#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <stdint.h>

typedef struct _Window Window;

G_DECLARE_FINAL_TYPE (ShellIntrospect, shell_introspect,
                      SHELL, INTROSPECT, GObject)

const char * window_get_app_id (Window *window);

const char * window_get_title (Window *window);

const uint64_t window_get_id (Window *window);

GList * shell_introspect_get_windows (ShellIntrospect *shell_introspect);

gboolean shell_introspect_are_animations_enabled (ShellIntrospect *introspect,
                                                  gboolean        *enable_animations);

void shell_introspect_ref_listeners (ShellIntrospect *shell_introspect);

void shell_introspect_unref_listeners (ShellIntrospect *shell_introspect);

ShellIntrospect * shell_introspect_get (void);

#include <cairo.h>

/* Fetches a preview of a window (source type 2, @id) or a monitor (source
 * type 1, @connector); @callback gets a new surface, or NULL */
typedef void (* ThumbnailCallback) (cairo_surface_t *surface,
                                    gpointer         user_data);

void shell_introspect_get_thumbnail (ShellIntrospect   *shell_introspect,
                                     uint32_t           source_type,
                                     uint64_t           id,
                                     const char        *connector,
                                     int                max_width,
                                     int                max_height,
                                     GCancellable      *cancellable,
                                     ThumbnailCallback  callback,
                                     gpointer           user_data);

gboolean shell_introspect_lookup_window (ShellIntrospect  *shell_introspect,
                                         uint64_t          id,
                                         char            **app_id,
                                         char            **title,
                                         char            **wm_class);

gboolean shell_introspect_find_window (ShellIntrospect *shell_introspect,
                                       uint64_t         id,
                                       const char      *app_id,
                                       const char      *title,
                                       const char      *wm_class,
                                       uint64_t        *out_id);
