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

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <stdint.h>

#include "screencast.h"

typedef struct _MuffinScreenCast MuffinScreenCast;
typedef struct _MuffinScreenCastSession MuffinScreenCastSession;
typedef struct _MuffinScreenCastStream MuffinScreenCastStream;

const char * muffin_screen_cast_session_get_stream_path_from_id (MuffinScreenCastSession *muffin_screen_cast_session,
                                                                uint32_t stream_id);

void muffin_screen_cast_session_add_stream_properties (MuffinScreenCastSession *muffin_screen_cast_session,
                                                      GVariantBuilder *streams_builder);

gboolean muffin_screen_cast_session_record_selections (MuffinScreenCastSession *muffin_screen_cast_session,
                                                      GVariant *selections,
                                                      ScreenCastSelection *select,
                                                      GError **error);

gboolean muffin_screen_cast_session_stop (MuffinScreenCastSession *muffin_screen_cast_session,
                                         GError **error);

gboolean muffin_screen_cast_session_start (MuffinScreenCastSession *muffin_screen_cast_session,
                                          GError **error);

MuffinScreenCastSession *muffin_screen_cast_create_session (MuffinScreenCast *muffin_screen_cast,
                                                          const char *remote_desktop_session_id,
                                                          GError **error);

int muffin_screen_cast_get_api_version (MuffinScreenCast *muffin_screen_cast);

MuffinScreenCast *muffin_screen_cast_new (GDBusConnection *connection);
