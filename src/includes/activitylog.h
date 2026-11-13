/*
  Hatari - activitylog.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.
*/

#ifndef HATARI_ACTIVITYLOG_H
#define HATARI_ACTIVITYLOG_H

#include <stdbool.h>

extern void ActLog_Init(void);
extern void ActLog_UnInit(void);
extern bool ActLog_IsEnabled(void);
extern void ActLog_Printf(const char *category, const char *fmt, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 2, 3)))
#endif
	;

#endif
