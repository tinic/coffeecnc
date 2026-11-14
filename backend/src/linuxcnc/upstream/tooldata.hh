/*
** The part of LinuxCNC's src/emc/tooldata/tooldata.hh that the adapter uses, as LinuxCNC 2.9.4
** declares it (Copyright 2021 Dewey Garrett, GPL-2.0-or-later). linuxcnc-uspace-dev 2.9.4
** installs libtooldata.so but not this header. CMake uses this file only where the installed
** development files have no tooldata.hh of their own, and as a SYSTEM header like LinuxCNC's.
** Nothing here may differ from upstream: the functions are LinuxCNC's.
*/
#ifndef TOOL_DATA_H
#define TOOL_DATA_H

#include "emctool.h"

typedef enum {
    IDX_OK = 0,
    IDX_NEW,
    IDX_FAIL,
} toolidx_t;

toolidx_t tooldata_get(CANON_TOOL_TABLE* pdata,int idx);
int    tooldata_last_index_get(void);

int  tool_mmap_user(void);

#endif
