/*
 * This file is part of libaacs
 * Copyright (C) 2011  VideoLAN
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "win32.h"

#include "util/logging.h"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include <shlobj.h>
#include <w32api.h>
#include <limits.h>
#include <direct.h>

int win32_mkdir(const char *dir)
{
    wchar_t wdir[MAX_PATH];

    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, MAX_PATH);
    return _wmkdir(wdir);
}

const char *win32_get_config_home(void)
{
    static char appdir[PATH_MAX] = "";
    wchar_t wdir[MAX_PATH];

    if (*appdir)
        return appdir;

    /* Get the "Application Data" folder for the user */
    if (S_OK == SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE,
                NULL, SHGFP_TYPE_CURRENT, wdir)) {
        WideCharToMultiByte (CP_UTF8, 0, wdir, -1, appdir, PATH_MAX, NULL, NULL);
        return appdir;
    }

    DEBUG(DBG_FILE, "Can't find user configuration directory !\n");
    return NULL;
}

const char *win32_get_config_system(const char *dir)
{
    static char appdir[PATH_MAX] = "";
    wchar_t wdir[MAX_PATH];

    if (!dir) {
        // first call

        if (*appdir)
            return appdir;

        /* Get the "Application Data" folder for all users */
        if (S_OK == SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA | CSIDL_FLAG_CREATE,
                    NULL, SHGFP_TYPE_CURRENT, wdir)) {
            WideCharToMultiByte (CP_UTF8, 0, wdir, -1, appdir, PATH_MAX, NULL, NULL);
            return appdir;
        } else {
            DEBUG(DBG_FILE, "Can't find common configuration directory !\n");
            return NULL;
        }
    } else {
        // next call
        return NULL;
    }

    return dir;
}
