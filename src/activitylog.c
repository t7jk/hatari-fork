/*
  Hatari - activitylog.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Critical-activity log written to text files in <HatariHome>/logs/.
  Enabled with ConfigureParams.Log.bLogToFile.
  - one file per day: hatari-YYYY-MM-DD.log (rotated to hatari-YYYY-MM-DD-N.log
    when a file exceeds ACTLOG_MAX_FILE_SIZE)
  - old files are removed so that at most ACTLOG_MAX_FILES files and
    ACTLOG_MAX_TOTAL_SIZE bytes remain
  - identical consecutive lines are collapsed ("repeated N times"),
    and a hard limit of ACTLOG_MAX_LINES_PER_SEC protects against floods
  Line format: "HH:MM:SS.mmm [VBL=<frame>] CATEGORY: message"
*/
const char ActivityLog_fileid[] = "Hatari activitylog.c";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>

#include "main.h"
#include "configuration.h"
#include "paths.h"
#include "video.h"
#include "activitylog.h"

#define ACTLOG_MAX_FILE_SIZE      (1024L * 1024L)
#define ACTLOG_MAX_TOTAL_SIZE     (5L * 1024L * 1024L)
#define ACTLOG_MAX_FILES          5
#define ACTLOG_MAX_LINES_PER_SEC  500
#define ACTLOG_LINE_MAX           1024

static FILE *actFile;
static char actDir[FILENAME_MAX];
static char actCurName[FILENAME_MAX];
static char actCurDay[16];
static long actFileSize;
static bool actEnabled;

static char actLastLine[ACTLOG_LINE_MAX];
static unsigned long actRepeat;
static time_t actSecond;
static int actSecCount;
static unsigned long actDropped;

struct logfile { char name[FILENAME_MAX]; time_t mtime; long size; };

static int cmp_mtime(const void *a, const void *b)
{
	const struct logfile *x = a, *y = b;
	if (x->mtime != y->mtime)
		return x->mtime < y->mtime ? -1 : 1;
	return strcmp(x->name, y->name);
}

/* Remove oldest files so that limits hold. 'keep' is never removed. */
static void ActLog_Cleanup(const char *keep)
{
	DIR *dir = opendir(actDir);
	struct dirent *ent;
	struct logfile *list = NULL;
	int count = 0, cap = 0, i, remaining;
	long total = 0;

	if (!dir)
		return;
	while ((ent = readdir(dir)) != NULL)
	{
		struct stat st;
		size_t len = strlen(ent->d_name);
		char path[FILENAME_MAX];

		if (strncmp(ent->d_name, "hatari-", 7) != 0 || len < 12 ||
		    strcmp(ent->d_name + len - 4, ".log") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", actDir, ent->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (count == cap)
		{
			struct logfile *n;
			cap = cap ? cap * 2 : 16;
			n = realloc(list, cap * sizeof(*list));
			if (!n)
				break;
			list = n;
		}
		snprintf(list[count].name, sizeof(list[count].name), "%s", path);
		list[count].mtime = st.st_mtime;
		list[count].size = (long)st.st_size;
		total += (long)st.st_size;
		count++;
	}
	closedir(dir);
	if (!list)
		return;
	remaining = count;
	qsort(list, count, sizeof(*list), cmp_mtime);
	for (i = 0; i < count && (remaining > ACTLOG_MAX_FILES || total > ACTLOG_MAX_TOTAL_SIZE); i++)
	{
		if (keep && strcmp(list[i].name, keep) == 0)
			continue;
		if (remove(list[i].name) == 0)
		{
			remaining--;
			total -= list[i].size;
		}
	}
	free(list);
}

static void ActLog_Close(void)
{
	if (actFile)
	{
		fclose(actFile);
		actFile = NULL;
	}
}

/* Open (or switch to) the file for current day, honouring the size limit */
static bool ActLog_Open(const struct tm *tm)
{
	char day[16];
	int n;

	strftime(day, sizeof(day), "%Y-%m-%d", tm);
	if (actFile && strcmp(day, actCurDay) == 0 && actFileSize < ACTLOG_MAX_FILE_SIZE)
		return true;

	ActLog_Close();
	snprintf(actCurDay, sizeof(actCurDay), "%s", day);
	for (n = 0; n < 1000; n++)
	{
		struct stat st;
		if (n == 0)
			snprintf(actCurName, sizeof(actCurName), "%s/hatari-%s.log", actDir, day);
		else
			snprintf(actCurName, sizeof(actCurName), "%s/hatari-%s-%d.log", actDir, day, n);
		actFileSize = 0;
		if (stat(actCurName, &st) == 0)
			actFileSize = (long)st.st_size;
		if (actFileSize < ACTLOG_MAX_FILE_SIZE)
			break;
	}
	actFile = fopen(actCurName, "a");
	if (!actFile)
		return false;
	ActLog_Cleanup(actCurName);
	return true;
}

static void ActLog_Write(const char *category, const char *text, unsigned long repeat)
{
	struct timeval tv;
	struct tm tm;
	time_t t;
	int len;

	gettimeofday(&tv, NULL);
	t = tv.tv_sec;
	localtime_r(&t, &tm);
	if (!ActLog_Open(&tm))
		return;
	len = fprintf(actFile, "%02d:%02d:%02d.%03d [VBL=%d] %s: %s\n",
	              tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000),
	              nVBLs, category, text);
	if (repeat)
		len += fprintf(actFile, "%02d:%02d:%02d.%03d [VBL=%d] %s: (last message repeated %lu times)\n",
		               tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000),
		               nVBLs, category, repeat);
	fflush(actFile);
	if (len > 0)
		actFileSize += len;
}

static void ActLog_FlushRepeat(void)
{
	if (actRepeat && actFile)
	{
		struct timeval tv;
		struct tm tm;
		time_t t;
		gettimeofday(&tv, NULL);
		t = tv.tv_sec;
		localtime_r(&t, &tm);
		fprintf(actFile, "%02d:%02d:%02d.%03d [VBL=%d] LOG: (last message repeated %lu times)\n",
		        tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), nVBLs, actRepeat);
		fflush(actFile);
	}
	actRepeat = 0;
}

bool ActLog_IsEnabled(void)
{
	return actEnabled;
}

void ActLog_Init(void)
{
	const char *home;

	actEnabled = false;
	if (!ConfigureParams.Log.bLogToFile)
		return;
	home = Paths_GetHatariHome();
	if (!home || !home[0])
		return;
	snprintf(actDir, sizeof(actDir), "%s/logs", home);
#ifdef WIN32
	mkdir(actDir);
#else
	mkdir(actDir, 0755);
#endif
	actLastLine[0] = 0;
	actRepeat = 0;
	actDropped = 0;
	actEnabled = true;
	ActLog_Cleanup(NULL);		/* clean old files at startup */
	ActLog_Printf("LOG", "---- Hatari activity log started ----");
}

void ActLog_UnInit(void)
{
	if (actEnabled)
	{
		ActLog_FlushRepeat();
		ActLog_Printf("LOG", "---- Hatari activity log closed ----");
	}
	actEnabled = false;
	ActLog_Close();
}

void ActLog_Printf(const char *category, const char *fmt, ...)
{
	char text[ACTLOG_LINE_MAX];
	char key[ACTLOG_LINE_MAX + 32];
	va_list ap;
	time_t now;

	if (!actEnabled)
		return;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);

	snprintf(key, sizeof(key), "%s|%s", category, text);
	if (actLastLine[0] && strncmp(key, actLastLine, sizeof(actLastLine)) == 0)
	{
		actRepeat++;			/* collapse identical consecutive lines */
		return;
	}
	ActLog_FlushRepeat();
	snprintf(actLastLine, sizeof(actLastLine), "%s", key);

	/* flood protection */
	now = time(NULL);
	if (now != actSecond)
	{
		if (actDropped && actFile)
		{
			fprintf(actFile, "LOG: %lu lines dropped (rate limit)\n", actDropped);
			fflush(actFile);
		}
		actDropped = 0;
		actSecond = now;
		actSecCount = 0;
	}
	if (++actSecCount > ACTLOG_MAX_LINES_PER_SEC)
	{
		actDropped++;
		return;
	}
	ActLog_Write(category, text, 0);
}
