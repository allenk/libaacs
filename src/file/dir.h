/*
 * This file is part of libbluray
 * Copyright (C) 2009-2010  John Stebbins
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DIR_H_
#define DIR_H_

#include <stdint.h>

#define dir_open dir_open_posix

#define dir_close(X) X->close(X)
#define dir_read(X,Y) X->read(X,Y)

// Our dirent struct only contains the parts we care about.
typedef struct
{
    char    d_name[256];
} DIRENT;

typedef struct dir DIR_H;
struct dir
{
    void* internal;
    void (*close)(DIR_H *dir);
    int (*read)(DIR_H *dir, DIRENT *entry);
};

extern DIR_H *dir_open_posix(const char* dirname);

#endif /* DIR_H_ */
