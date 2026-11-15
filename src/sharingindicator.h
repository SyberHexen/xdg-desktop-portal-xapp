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

#pragma once

#include <glib.h>

#include "session.h"

/* A status icon shown while sessions share the screen or control the
 * computer, with a menu to stop them */

/* @what: what is shared ("HISENSE", a window title), or NULL;
 * @remote_control: whether the application can also send input */
void sharing_indicator_add (Session    *session,
                            const char *app_id,
                            const char *what,
                            gboolean    remote_control);

void sharing_indicator_remove (Session *session);
