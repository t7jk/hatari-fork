/*
  Hatari - dlgBrowser.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Disk-image Browser dialog (SDL GUI).

  The window is made of three panes in the style of the original Hatari
  SDL GUI: the file list on the left, information about the current item
  on the upper right and a scrollable text description on the lower right.
  Under them there is a message line, a key hint line and a row of buttons.

  The dialog runs its own event loop (instead of SDLGui_DoDialogExt) so that
  cursor keys, Tab, Space and Enter can be used for list navigation.  The
  SDL GUI objects are only used for drawing (SDLGui_DrawDialog).

  Data (tree, favorites, settings, descriptions) come from browser_data.c,
  local copies/launching from browser_local.c; see includes/browser.h.
*/
const char DlgBrowser_fileid[] = "Hatari dlgBrowser.c";

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "main.h"
#include "configuration.h"
#include "dialog.h"
#include "screen.h"
#include "screen_sdl.h"
#include "sdlgui.h"
#include "file.h"
#include "str.h"
#include "log.h"
#include "browser.h"

#if ENABLE_SDL3
#define ev2key(e) (e).key.key
#define ev2mod(e) (e).key.mod
#else
#define ev2key(e) (e).key.keysym.sym
#define ev2mod(e) (e).key.keysym.mod
#endif

#define MAXDEPTH     32
#define LIST_MINROWS  5

typedef enum { V_ARCHIVE = 0, V_FAV, V_LOCAL, V_COUNT } ViewId;
static const char *const view_names[V_COUNT] = { "Archive", "Favorites", "Local" };

/* buttons, in the order they are placed in the bottom row */
enum { BTN_RUN, BTN_MARK, BTN_FAV, BTN_COPY, BTN_DEL, BTN_RESCAN, BTN_SETTINGS, BTN_CLOSE, BTN_COUNT };
static char *const btn_text[BTN_COUNT] = {
	"_Run", "_Mark", "_Fav", "Copy _local", "_Delete", "R_escan", "_Settings", "_Close"
};
/* Alt+<letter> for each button (must match the underscores above) */
static const char btn_key[BTN_COUNT] = { 'R', 'M', 'F', 'L', 'D', 'E', 'S', 'C' };

typedef struct {
	char label[200];   /* label of the folder entered from this level */
	int cur, top;      /* cursor and first visible row at this level */
} Frame;

typedef struct {
	Frame fr[MAXDEPTH + 1];
	BrNode *nodes[MAXDEPTH + 1];
	int depth;
	const BrItem *items;
	int n;
} View;

/* scrollbars that can be dragged */
enum { SB_NONE = 0, SB_LIST, SB_DESC };

static struct {
	/* dialog */
	SGOBJ *obj;
	int nobj, cap;
	int fw, fh;
	int W, H, LW, LH, LBH, IH, IW, DH, xr, RW, yl;
	int nrows, ndesc;
	int lay;           /* 0: list only, 1: list + Disk info, 2: list + Description */
	int list_trackrows, desc_trackrows;
	int o_title, o_crumb, o_up, o_down, o_lsb, o_cursor, o_row0;
	int o_info0, o_dtitle, o_drow0, o_dsb, o_status, o_hint, o_btn0;
	SDL_Surface *bg;
	SDL_Rect bgrect;

	/* browsing state */
	View views[V_COUNT];
	ViewId view;
	char **marks;
	int nmarks, capmarks;
	char msg[256];

	/* description pane */
	char *desc;
	int *dl_off, *dl_len;
	int ndl, dl_cap;
	int dscroll;
	bool desc_dirty;

	/* info pane */
	bool info_dirty;
	bool size_ok;
	long long fsize;

	/* mouse */
	Uint32 last_click;
	int last_click_row;
	int pressed;       /* pressed button object or -1 */
	int drag;          /* SB_* */
	int drag_off;
	int repeat;        /* -1/0/+1: auto-repeating list scroll arrow */
	Uint32 repeat_t;
	Uint32 last_draw;
} B;


/* ------------------------------------------------------------------ */
/* small helpers */

static void set_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(B.msg, sizeof(B.msg), fmt, ap);
	va_end(ap);
}

/** Copy src to dst (max w chars) with only printable ASCII left. */
static void put_text(char *dst, int w, const char *src)
{
	int i = 0;
	for (; src && src[i] && i < w; i++)
	{
		unsigned char c = (unsigned char)src[i];
		dst[i] = (c < 32 || c >= 127) ? '?' : (char)c;
	}
	dst[i] = '\0';
}

/** Write "..tail" version of src (keeping the end) into dst of width w. */
static void put_tail(char *dst, int w, const char *src)
{
	int len = (int)strlen(src);
	if (len <= w)
		put_text(dst, w, src);
	else if (w > 3)
	{
		dst[0] = dst[1] = dst[2] = '.';
		put_text(dst + 3, w - 3, src + len - (w - 3));
	}
	else
		put_text(dst, w, src + len - w);
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static bool has_ext(const char *path, const char *ext)
{
	size_t l = strlen(path), e = strlen(ext);
	return l >= e && strcasecmp(path + l - e, ext) == 0;
}

static bool is_zip(const char *path)
{
	return has_ext(path, ".zip");
}

/** Text files that can be shown in the description pane (local view). */
static bool is_textfile(const char *name)
{
	static const char *const exts[] = {
		".txt", ".inf", ".doc", ".nfo", ".diz", ".asc", ".me", ".1st", ".md", ".log",
		".cfg", ".ini", ".readme", ".hlp", ".lst", ".dsc", NULL
	};
	const char *dot = strrchr(name, '.');
	int i;
	if (!dot || dot == name)
		return true;               /* no extension */
	for (i = 0; exts[i]; i++)
		if (strcasecmp(dot, exts[i]) == 0)
			return true;
	return false;
}

static void strip_slash(char *s)
{
	size_t l = strlen(s);
	while (l > 1 && s[l - 1] == '/')
		s[--l] = '\0';
}


/* ------------------------------------------------------------------ */
/* marks */

static int mark_find(const char *p)
{
	int i;
	for (i = 0; i < B.nmarks; i++)
		if (strcmp(B.marks[i], p) == 0)
			return i;
	return -1;
}

static void marks_clear(void)
{
	int i;
	for (i = 0; i < B.nmarks; i++)
		free(B.marks[i]);
	B.nmarks = 0;
}

/** Toggle mark, returns true if the path is now marked. */
static bool mark_toggle(const char *p)
{
	int i = mark_find(p);
	if (i >= 0)
	{
		free(B.marks[i]);
		memmove(&B.marks[i], &B.marks[i + 1], (B.nmarks - i - 1) * sizeof(char *));
		B.nmarks--;
		return false;
	}
	if (B.nmarks == B.capmarks)
	{
		int ncap = B.capmarks ? B.capmarks * 2 : 32;
		char **nm = realloc(B.marks, ncap * sizeof(char *));
		if (!nm)
			return false;
		B.marks = nm;
		B.capmarks = ncap;
	}
	B.marks[B.nmarks] = strdup(p);
	if (!B.marks[B.nmarks])
		return false;
	B.nmarks++;
	return true;
}


/* ------------------------------------------------------------------ */
/* views */

static View *cur_view(void)
{
	return &B.views[B.view];
}

static const BrItem *cur_item(void)
{
	View *v = cur_view();
	Frame *f = &v->fr[v->depth];
	if (!v->items || f->cur < 0 || f->cur >= v->n)
		return NULL;
	return &v->items[f->cur];
}

static void view_clamp(View *v)
{
	Frame *f = &v->fr[v->depth];
	if (f->cur >= v->n)
		f->cur = v->n - 1;
	if (f->cur < 0)
		f->cur = 0;
	if (f->top > f->cur)
		f->top = f->cur;
	if (f->top < 0)
		f->top = 0;
}

static void view_settle(View *v)
{
	v->items = v->nodes[v->depth] ? BrNode_Items(v->nodes[v->depth], &v->n) : NULL;
	if (!v->items)
		v->n = 0;
	view_clamp(v);
}

/** Use a new root for the view, re-entering the folders of the current path. */
static void view_reload(View *v, BrNode *root)
{
	int d;
	v->nodes[0] = root;
	for (d = 0; d < v->depth; d++)
	{
		const BrItem *it;
		int i, n = 0;
		BrNode *next = NULL;
		it = v->nodes[d] ? BrNode_Items(v->nodes[d], &n) : NULL;
		for (i = 0; it && i < n; i++)
		{
			if (it[i].kind == BR_DIR && strcmp(it[i].label, v->fr[d].label) == 0)
			{
				next = it[i].sub;
				break;
			}
		}
		if (!next)
		{
			v->depth = d;
			break;
		}
		v->nodes[d + 1] = next;
	}
	view_settle(v);
}

static BrNode *view_root(ViewId id)
{
	switch (id)
	{
	 case V_ARCHIVE: return BrData_ArchiveRoot();
	 case V_FAV:     return BrData_FavRoot();
	 default:        return BrData_LocalRoot();
	}
}

/** The cursor moved or the content changed: invalidate lazily loaded panes. */
static void cursor_changed(void)
{
	B.desc_dirty = true;
	B.info_dirty = true;
	B.dscroll = 0;
}

static void set_cursor(int c)
{
	View *v = cur_view();
	Frame *f = &v->fr[v->depth];
	int old = f->cur;
	if (c >= v->n)
		c = v->n - 1;
	if (c < 0)
		c = 0;
	f->cur = c;
	if (c < f->top)
		f->top = c;
	if (c >= f->top + B.nrows)
		f->top = c - B.nrows + 1;
	if (f->top < 0)
		f->top = 0;
	if (c != old)
		cursor_changed();
}

/** Scroll the list view by delta rows, keeping the cursor visible. */
static void scroll_list(int delta)
{
	View *v = cur_view();
	Frame *f = &v->fr[v->depth];
	int maxtop = v->n - B.nrows;
	int oldcur = f->cur;
	if (maxtop < 0)
		maxtop = 0;
	f->top += delta;
	if (f->top > maxtop)
		f->top = maxtop;
	if (f->top < 0)
		f->top = 0;
	if (f->cur < f->top)
		f->cur = f->top;
	if (f->cur >= f->top + B.nrows)
		f->cur = f->top + B.nrows - 1;
	if (f->cur >= v->n)
		f->cur = v->n - 1;
	if (f->cur < 0)
		f->cur = 0;
	if (f->cur != oldcur)
		cursor_changed();
}

static void enter_dir(const BrItem *it)
{
	View *v = cur_view();
	if (v->depth >= MAXDEPTH || !it->sub)
		return;
	snprintf(v->fr[v->depth].label, sizeof(v->fr[0].label), "%s", it->label);
	v->nodes[v->depth + 1] = it->sub;
	v->depth++;
	v->fr[v->depth].cur = v->fr[v->depth].top = 0;
	view_settle(v);
	cursor_changed();
}

static void leave_dir(void)
{
	View *v = cur_view();
	if (v->depth == 0)
		return;
	v->depth--;
	view_settle(v);
	/* keep the folder we came from visible */
	set_cursor(v->fr[v->depth].cur);
	cursor_changed();
}

static void switch_view(int step)
{
	ViewId nv = (ViewId)((B.view + V_COUNT + step) % V_COUNT);
	if ((nv == V_LOCAL) != (B.view == V_LOCAL) && B.nmarks)
	{
		marks_clear();      /* archive and local paths do not mix */
		set_msg("Selection cleared.");
	}
	else
		B.msg[0] = '\0';
	B.view = nv;
	if (nv != V_ARCHIVE)
		view_reload(cur_view(), view_root(nv));
	else
		view_settle(cur_view());
	cursor_changed();
}

/** Path of the current item (relative for archive/favorites, absolute for local). */
static bool item_path(const BrItem *it, char *out, size_t sz)
{
	View *v = cur_view();
	if (!it || it->kind != BR_FILE)
		return false;
	return BrNode_ItemPath(v->nodes[v->depth], it, out, sz);
}

static void reset_archive_view(void)
{
	View *v = &B.views[V_ARCHIVE];
	memset(v, 0, sizeof(*v));
	v->nodes[0] = BrData_ArchiveRoot();
	view_settle(v);
	if (B.view == V_ARCHIVE)
		cursor_changed();
}

/** Restore the last position saved with BrData_SaveState(). */
static void restore_state(void)
{
	static char labels[MAXDEPTH][200];
	char sel[200] = "";
	View *v = &B.views[V_ARCHIVE];
	int n, d, i;

	n = BrData_LoadState(labels, MAXDEPTH, sel, sizeof(sel));
	for (d = 0; d < n && d < MAXDEPTH && v->nodes[d]; d++)
	{
		int cnt = 0;
		const BrItem *it = BrNode_Items(v->nodes[d], &cnt);
		for (i = 0; it && i < cnt; i++)
		{
			if (it[i].kind == BR_DIR && strcmp(it[i].label, labels[d]) == 0)
				break;
		}
		if (!it || i >= cnt)
			break;
		snprintf(v->fr[d].label, sizeof(v->fr[d].label), "%.199s", labels[d]);
		v->nodes[d + 1] = it[i].sub;
		v->fr[d].cur = i;
		v->depth = d + 1;
		v->fr[d + 1].cur = v->fr[d + 1].top = 0;
	}
	view_settle(v);
	if (sel[0])
	{
		for (i = 0; i < v->n; i++)
		{
			if (strcmp(v->items[i].label, sel) == 0)
			{
				v->fr[v->depth].cur = i;
				v->fr[v->depth].top = i > B.nrows / 2 ? i - B.nrows / 2 : 0;
				break;
			}
		}
	}
}

static void save_state(void)
{
	View *v = &B.views[V_ARCHIVE];
	const char *labels[MAXDEPTH];
	const char *sel = "";
	int d;
	if (!v->nodes[0])
		return;
	for (d = 0; d < v->depth; d++)
		labels[d] = v->fr[d].label;
	if (v->items && v->fr[v->depth].cur < v->n)
		sel = v->items[v->fr[v->depth].cur].label;
	BrData_SaveState(labels, v->depth, sel);
}


/* ------------------------------------------------------------------ */
/* description pane */

static void wrap_desc(void)
{
	int w = B.RW - 2;
	const char *t = B.desc;
	int pos = 0;

	B.ndl = 0;
	B.dscroll = 0;
	if (!t || w < 4)
		return;
	while (t[pos])
	{
		int len = 0, brk = -1;
		/* find the extent of this display line */
		while (t[pos + len] && t[pos + len] != '\n' && len < w)
		{
			if (t[pos + len] == ' ')
				brk = len;
			len++;
		}
		if (t[pos + len] && t[pos + len] != '\n' && brk > 0)
			len = brk;                 /* wrap at the last blank */
		if (B.ndl == B.dl_cap)
		{
			int ncap = B.dl_cap ? B.dl_cap * 2 : 256;
			int *a = realloc(B.dl_off, ncap * sizeof(int));
			int *b;
			if (a)
				B.dl_off = a;
			b = realloc(B.dl_len, ncap * sizeof(int));
			if (b)
				B.dl_len = b;
			if (!a || !b)
				return;
			B.dl_cap = ncap;
		}
		B.dl_off[B.ndl] = pos;
		B.dl_len[B.ndl] = len;
		B.ndl++;
		pos += len;
		if (t[pos] == '\n')
			pos++;
		else
			while (t[pos] == ' ')
				pos++;
	}
}

static void load_info_and_desc(void)
{
	const BrItem *it = cur_item();
	char path[1100], abs[1100];
	struct stat st;

	free(B.desc);
	B.desc = NULL;
	B.size_ok = false;
	if (it && it->kind == BR_FILE && item_path(it, path, sizeof(path)))
	{
		BrData_AbsPath(path, abs, sizeof(abs));
		if (stat(abs, &st) == 0)
		{
			B.size_ok = true;
			B.fsize = (long long)st.st_size;
		}
		if (B.view == V_LOCAL && !BrLocal_IsDisk(it->label))
		{
			if (is_textfile(it->label))
				B.desc = BrData_ReadTextFile(abs);
		}
		else
			B.desc = BrData_Description(path);
	}
	wrap_desc();
	B.desc_dirty = B.info_dirty = false;
}


/* ------------------------------------------------------------------ */
/* dialog construction and drawing */

static int add_obj(int type, int flags, int x, int y, int w, int h, char *txt, int bufsize)
{
	SGOBJ *o;
	int i;

	if (B.nobj >= B.cap - 1)
		return B.nobj - 1;         /* cannot happen, cap is computed with margin */
	i = B.nobj++;
	o = &B.obj[i];
	memset(o, 0, sizeof(*o));
	o->type = type;
	o->flags = flags;
	o->x = x;
	o->y = y;
	o->w = w;
	o->h = h;
	if (bufsize > 0)
	{
		o->txt = calloc(1, bufsize);
		if (o->txt && txt)
			snprintf(o->txt, bufsize, "%s", txt);
	}
	else
		o->txt = txt;
	return i;
}

static void dlg_free(void)
{
	int i;
	if (B.obj)
	{
		for (i = 0; i < B.nobj; i++)
		{
			/* only buffers we allocated; static strings are button labels */
			if (B.obj[i].type == SGTEXT && B.obj[i].txt)
				free(B.obj[i].txt);
		}
		free(B.obj);
	}
	B.obj = NULL;
	B.nobj = B.cap = 0;
}

/** Compute the layout from the screen size and create all dialog objects. */
static bool dlg_build(void)
{
	int cw, ch, i;

	SDLGui_GetFontSize(&B.fw, &B.fh);
	cw = sdlscrn->w / B.fw;
	ch = sdlscrn->h / B.fh;
	B.W = cw * 9 / 10;
	B.H = ch * 9 / 10;
	if (B.W < 64)
		B.W = cw < 64 ? cw : 64;
	if (B.H < 20)
		B.H = ch < 20 ? ch : 20;
	if (B.W < 48 || B.H < 14)
	{
		Log_Printf(LOG_ERROR, "Screen size too small for the Browser!\n");
		return false;
	}
	B.LW = B.W - 2;
	B.LH = B.H - 5;
	/* list on top (full width), one lower panel (info or description) below */
	B.LBH = B.LH * 60 / 100;
	B.IH = B.DH = B.LH - B.LBH;
	B.yl = B.LBH + 1;
	B.IW = B.LW;
	B.xr = 1;
	B.RW = B.LW;
	if (B.lay && (B.LBH - 1 < LIST_MINROWS || B.IH < 5))
		B.lay = 0;              /* too small for a lower panel */
	if (!B.lay)
		B.LBH = B.LH;
	B.nrows = B.LBH - 1;
	B.ndesc = B.DH - 1;
	B.list_trackrows = B.LBH - 3;
	B.desc_trackrows = B.ndesc;
	if (B.nrows < LIST_MINROWS)
		return false;

	B.cap = B.nrows + B.ndesc + 64;
	B.obj = calloc(B.cap, sizeof(SGOBJ));
	if (!B.obj)
		return false;
	B.nobj = 0;

	add_obj(SGBOX, 0, 0, 0, B.W, B.H, NULL, 0);
	B.o_title = add_obj(SGTEXT, 0, 1, 0, B.W - 2, 1, "", B.W + 1);
	add_obj(SGBOX, 0, 1, 1, B.LW, B.LBH, NULL, 0);
	B.o_crumb = add_obj(SGTEXT, 0, 1, 1, B.LW - 1, 1, "", B.LW + 1);
	B.o_up = add_obj(SGBUTTON, 0, B.LW, 2, 1, 1, "\001", 0);
	B.o_down = add_obj(SGBUTTON, 0, B.LW, B.LBH, 1, 1, "\002", 0);
	B.o_lsb = add_obj(SGSCROLLBAR, 0, B.LW, 3, 0, 0, NULL, 0);
	B.o_cursor = add_obj(SGBOX, SG_SELECTED, 1, 2, B.LW - 1, 1, NULL, 0);
	B.obj[B.o_cursor].state = SG_SELECTED | SG_FOCUSED;
	B.o_row0 = B.nobj;
	for (i = 0; i < B.nrows; i++)
		add_obj(SGTEXT, 0, 1, 2 + i, B.LW - 1, 1, "", B.LW + 1);

	if (B.lay == 1)
	{
		add_obj(SGBOX, 0, 1, B.yl, B.IW, B.IH, NULL, 0);
		B.o_info0 = B.nobj;
		for (i = 0; i < B.IH; i++)
			add_obj(SGTEXT, 0, 1, B.yl + i, B.IW, 1, "", B.IW + 1);
	}
	else if (B.lay == 2)
	{
		add_obj(SGBOX, 0, B.xr, B.yl, B.RW, B.DH, NULL, 0);
		B.o_dtitle = add_obj(SGTEXT, 0, B.xr, B.yl, B.RW, 1, "", B.RW + 1);
		B.o_drow0 = B.nobj;
		for (i = 0; i < B.ndesc; i++)
			add_obj(SGTEXT, 0, B.xr, B.yl + 1 + i, B.RW - 1, 1, "", B.RW + 1);
		B.o_dsb = add_obj(SGSCROLLBAR, 0, B.xr + B.RW - 1, B.yl + 1, 0, 0, NULL, 0);
	}

	B.o_status = add_obj(SGTEXT, 0, 1, B.LH + 1, B.W - 2, 1, "", B.W + 1);
	B.o_hint = add_obj(SGTEXT, 0, 1, B.LH + 2, B.W - 2, 1, "", B.W + 1);

	B.o_btn0 = B.nobj;
	{
		int sum = 0, gap, x, b, n;
		int wd[BTN_COUNT];
		for (b = 0; b < BTN_COUNT; b++)
		{
			wd[b] = (int)strlen(btn_text[b]) - 1 + 2;
			sum += wd[b];
		}
		gap = (B.W - 2 - sum) / (BTN_COUNT - 1);
		if (gap < 0)
			gap = 0;
		if (gap > 2)
			gap = 2;
		n = sum + gap * (BTN_COUNT - 1);
		x = 1 + (B.W - 2 - n) / 2;
		if (x < 0)
			x = 0;
		for (b = 0; b < BTN_COUNT; b++)
		{
			add_obj(SGBUTTON, 0, x, B.LH + 3, wd[b], 1, btn_text[b], 0);
			x += wd[b] + gap;
		}
	}
	B.obj[B.nobj].type = SGSTOP;

	B.obj[0].x = (cw - B.W) / 2;
	B.obj[0].y = (ch - B.H) / 2;
	return true;
}

/** Set scrollbar geometry: thumb for 'pos' of 'total' with 'vis' visible rows. */
static void sb_set(int o, int trackrows, int total, int vis, int pos)
{
	int trackpx = trackrows * B.fh;
	int thumb, off;

	if (trackrows < 1)
	{
		B.obj[o].w = B.obj[o].h = 0;
		return;
	}
	if (total <= vis)
	{
		thumb = trackpx;
		off = 0;
	}
	else
	{
		thumb = (int)((long long)trackpx * vis / total);
		if (thumb < 6)
			thumb = 6;
		if (thumb > trackpx)
			thumb = trackpx;
		off = (int)((long long)(trackpx - thumb) * pos / (total - vis));
	}
	B.obj[o].w = thumb;
	B.obj[o].h = off;
}

static void fill_row(int idx, const BrItem *it, bool marked, bool fav)
{
	char *dst = B.obj[B.o_row0 + idx].txt;
	int w = B.LW - 1;
	char lab[256], cnt[24] = "";
	int avail, cl;

	snprintf(lab, sizeof(lab), "%s", it->label);
	if (it->kind == BR_DIR && it->count > 0)
		snprintf(cnt, sizeof(cnt), " %d", it->count);
	cl = (int)strlen(cnt);
	avail = w - 3 - cl;
	if (avail < 4)
		avail = w - 3;
	/* marked rows get a dark band behind them (see SDLGui_DrawText) */
	if (marked)
	{
		B.obj[B.o_row0 + idx].flags |= SG_EXIT;
		B.obj[B.o_row0 + idx].state |= SG_SELECTED;
	}
	else
	{
		B.obj[B.o_row0 + idx].flags &= ~SG_EXIT;
		B.obj[B.o_row0 + idx].state &= ~(SG_SELECTED | SG_FOCUSED);
	}
	memset(dst, ' ', w);
	dst[w] = '\0';
	dst[0] = marked ? '*' : ' ';
	dst[1] = it->kind == BR_DIR ? (char)SGFOLDER : (fav ? '+' : ' ');
	{
		char tmp[256];
		int l;
		put_text(tmp, avail, lab);
		l = (int)strlen(tmp);
		if ((int)strlen(lab) > avail && avail > 2)
			tmp[avail - 1] = '~';
		memcpy(dst + 3, tmp, l);
	}
	if (cl && w - cl >= 3)
		memcpy(dst + w - cl, cnt, cl);
}

static void fmt_size(char *out, size_t sz, long long b)
{
	if (b >= 1024 * 1024)
		snprintf(out, sz, "%lld bytes (%.2f MB)", b, b / (1024.0 * 1024.0));
	else
		snprintf(out, sz, "%lld bytes (%lld KB)", b, (b + 1023) / 1024);
}

static const char *fmt_format(const char *name, long long size, bool ok)
{
	if (has_ext(name, ".st"))
	{
		if (ok && size == 368640)
			return "ST raw image, single sided DD";
		if (ok && size == 737280)
			return "ST raw image, double sided DD";
		if (ok && size == 819200)
			return "ST raw image, 800K";
		if (ok && size == 1474560)
			return "ST raw image, HD";
		return "ST raw image";
	}
	if (has_ext(name, ".msa"))
		return "MSA compressed image";
	if (has_ext(name, ".stx"))
		return "STX (Pasti) image";
	if (has_ext(name, ".dim"))
		return "DIM (FastCopy) image";
	if (is_zip(name))
		return "ZIP archive";
	return "Unknown";
}

/** Refresh all dialog texts from the current state. */
static void dlg_texts(void)
{
	View *v = cur_view();
	Frame *f = &v->fr[v->depth];
	const BrItem *it = cur_item();
	char buf[1400], path[1100];
	int i, d;
	int total = v->n;
	bool have_path = false;

	/* title: view tabs */
	buf[0] = '\0';
	for (i = 0; i < V_COUNT; i++)
	{
		char t[64];
		snprintf(t, sizeof(t), "%s%s%s ", i == (int)B.view ? "[" : " ",
		         view_names[i], i == (int)B.view ? "]" : " ");
		strncat(buf, t, sizeof(buf) - strlen(buf) - 1);
	}
	{
		char t[1500];
		snprintf(t, sizeof(t), "Disk Browser  %s", buf);
		put_text(B.obj[B.o_title].txt, B.W - 2, t);
	}

	/* breadcrumb */
	{
		char t[1600];
		snprintf(t, sizeof(t), "%s:", view_names[B.view]);
		for (d = 0; d < v->depth; d++)
		{
			strncat(t, "/", sizeof(t) - strlen(t) - 1);
			strncat(t, v->fr[d].label, sizeof(t) - strlen(t) - 1);
		}
		if (!v->depth)
			strncat(t, "/", sizeof(t) - strlen(t) - 1);
		put_tail(B.obj[B.o_crumb].txt, B.LW - 1, t);
	}

	/* list rows */
	for (i = 0; i < B.nrows; i++)
	{
		int idx = f->top + i;
		char *dst = B.obj[B.o_row0 + i].txt;
		if (idx < v->n)
		{
			const BrItem *r = &v->items[idx];
			bool marked = false, fav = false;
			if (r->kind == BR_FILE)
			{
				char p[1100];
				if (BrNode_ItemPath(v->nodes[v->depth], r, p, sizeof(p)))
				{
					marked = B.nmarks && mark_find(p) >= 0;
					if (B.view == V_ARCHIVE)
						fav = BrData_IsFav(p);
				}
			}
			fill_row(i, r, marked, fav);
		}
		else
		{
			dst[0] = '\0';
			B.obj[B.o_row0 + i].flags &= ~SG_EXIT;
			B.obj[B.o_row0 + i].state &= ~(SG_SELECTED | SG_FOCUSED);
		}
	}
	if (v->n == 0)
	{
		const char *e = B.view == V_ARCHIVE ?
			(BrData_HaveIndex() ? "(empty)" : "No index: Alt+E scans") :
			B.view == V_FAV ? "(none yet: Alt+F adds)" : "(no local copies)";
		put_text(B.obj[B.o_row0].txt, B.LW - 2, e);
	}

	/* cursor highlight box */
	if (v->n && f->cur >= f->top && f->cur < f->top + B.nrows)
	{
		B.obj[B.o_cursor].y = 2 + f->cur - f->top;
		B.obj[B.o_cursor].w = B.LW - 1;
		B.obj[B.o_cursor].h = 1;
		/* cursor on a marked row: keep both visible (brighter band) */
		if (B.obj[B.o_row0 + f->cur - f->top].state & SG_SELECTED)
			B.obj[B.o_row0 + f->cur - f->top].state |= SG_FOCUSED;
	}
	else
		B.obj[B.o_cursor].w = B.obj[B.o_cursor].h = 0;

	sb_set(B.o_lsb, B.list_trackrows, total, B.nrows, f->top);

	if (B.lay == 1)
	{
	/* info pane: the name is wrapped over several lines if it is long */
	int r = 1, nn = 1, nl, nw = B.IW - 8;
	const char *nm = it ? it->label : "";

#define INFO_LINE(...) \
	do { \
		if (r < B.IH) { snprintf(buf, sizeof(buf), __VA_ARGS__); put_text(B.obj[B.o_info0 + r].txt, B.IW, buf); } \
		r++; \
	} while (0)

	for (i = 0; i < B.IH; i++)
		B.obj[B.o_info0 + i].txt[0] = '\0';
	put_text(B.obj[B.o_info0].txt, B.IW, "Disk info");
	have_path = it && item_path(it, path, sizeof(path));
	if (it)
	{
		nl = ((int)strlen(nm) + nw - 1) / nw;
		if (nl > B.IH - 4)
			nl = B.IH - 4;
		for (nn = 0; nn < nl; nn++)
		{
			int len = (int)strlen(nm) - nn * nw;
			char part[256];
			if (len > nw)
				len = nw;
			if (len > (int)sizeof(part) - 1)
				len = (int)sizeof(part) - 1;
			memcpy(part, nm + nn * nw, len);
			part[len] = '\0';
			INFO_LINE("%s%s", nn ? "        " : "Name:   ", part);
		}
		if (nl < 1)
			INFO_LINE("Name:   %s", nm);
		if (have_path)
		{
			char abs[1100], *s, sz[64];
			BrData_AbsPath(path, abs, sizeof(abs));
			s = strrchr(abs, '/');
			if (s)
				*s = '\0';
			snprintf(buf, sizeof(buf), "Path:   %s", abs);
			put_tail(B.obj[B.o_info0 + r].txt, B.IW, buf);
			r++;
			if (B.size_ok)
			{
				fmt_size(sz, sizeof(sz), B.fsize);
				INFO_LINE("Size:   %s", sz);
			}
			else
				INFO_LINE("Size:   %s", B.info_dirty ? "..." : "?");
			if (r < B.IH - 1)
				INFO_LINE("Format: %s", fmt_format(it->label, B.fsize, B.size_ok));
		}
		else
			INFO_LINE("Folder%s, %d disk(s) inside", it->virt ? " (letter group)" : "", it->count);
	}
	if (B.IH >= 7)
		INFO_LINE("Marked: %d   Favorites: %d", B.nmarks, BrData_FavCount());
	else
		INFO_LINE("Marked: %d", B.nmarks);
	if (B.IH >= 8)
		INFO_LINE("Local copies %s", BrData_Settings()->copy_local ? "used for zip/multi-disk" : "off");
#undef INFO_LINE
	}

	/* description pane */
	if (B.lay == 2)
	{
		int maxscroll = B.ndl - B.ndesc;
		if (maxscroll < 0)
			maxscroll = 0;
		if (B.dscroll > maxscroll)
			B.dscroll = maxscroll;
		if (B.dscroll < 0)
			B.dscroll = 0;
		put_text(B.obj[B.o_dtitle].txt, B.RW, B.ndl > B.ndesc ?
		         "Description (Alt+Up/Dn, -/+)" : "Description");
		for (i = 0; i < B.ndesc; i++)
		{
			char *dst = B.obj[B.o_drow0 + i].txt;
			int li = B.dscroll + i;
			if (li < B.ndl)
			{
				int len = B.dl_len[li];
				if (len > B.RW - 2)
					len = B.RW - 2;
				memcpy(buf, B.desc + B.dl_off[li], len);
				buf[len] = '\0';
				put_text(dst, B.RW - 2, buf);
			}
			else
				dst[0] = '\0';
		}
		if (!B.desc && !B.desc_dirty && it && it->kind == BR_FILE)
			put_text(B.obj[B.o_drow0].txt, B.RW - 2, "(no description found)");
		sb_set(B.o_dsb, B.desc_trackrows, B.ndl, B.ndesc, B.dscroll);
	}

	/* status + hints; a message-free status shows the full name when the list cuts it */
	if (!B.msg[0] && it && (B.lay != 1 || (int)strlen(it->label) >
	    B.LW - 4 - (it->kind == BR_DIR && it->count > 0 ? 7 : 0)))
	{
		snprintf(buf, sizeof(buf), "Name: %s", it->label);
		put_text(B.obj[B.o_status].txt, B.W - 2, buf);
	}
	else
		put_text(B.obj[B.o_status].txt, B.W - 2, B.msg);
	{
		static const char *const lname[] = { "list", "list+info", "list+desc" };
		static const char *const fmt[] = {
			"Enter run  Space mark  W view:%s  Tab tab  Bksp up  Del rm  Esc clear/quit",
			"Enter run  Space mark  W view:%s  Tab tab  Bksp up  Esc quit",
			"Enter run Spc mark W:%s Tab Bksp up Esc quit",
			"Enter run Spc mark W Tab Bksp up Esc quit",
			"Enter Spc W Tab Bksp Esc"
		};
		char hint[128];
		int h = 0;
		for (;;)
		{
			snprintf(hint, sizeof(hint), fmt[h], lname[B.lay]);
			if (h >= 4 || (int)strlen(hint) <= B.W - 2)
				break;
			h++;
		}
		put_text(B.obj[B.o_hint].txt, B.W - 2, hint);
	}
}


static void draw_all(void)
{
	dlg_texts();
	SDLGui_DrawDialog(B.obj);
	B.last_draw = SDL_GetTicks();
}


/* ------------------------------------------------------------------ */
/* actions */

/** Message pump used by long operations; true if user wants to abort. */
static bool pump_abort(void)
{
	SDL_Event e;
	bool abort = false;
	while (SDL_PollEvent(&e))
	{
		if (e.type == SDL_QUIT)
		{
			bQuitProgram = true;
			abort = true;
		}
		else if (e.type == SDL_KEYDOWN && ev2key(e) == SDLK_ESCAPE)
			abort = true;
	}
	if (SDL_GetTicks() - B.last_draw > 100)
		draw_all();
	return abort;
}

static bool rescan_progress(int found)
{
	set_msg("Scanning archive... %d disks found (Esc = abort)", found);
	return !pump_abort();
}

static bool copy_progress(int done, int total)
{
	set_msg("Copying to local folder... %d/%d (Esc = abort)", done, total);
	return !pump_abort();
}

static void do_rescan(void)
{
	BrSettings *s = BrData_Settings();
	bool ok;

	if (access(s->archive_root, R_OK) != 0)
	{
		set_msg("Archive folder not accessible: %s", s->archive_root);
		return;
	}
	set_msg("Scanning archive...");
	draw_all();
	ok = BrData_Rescan(rescan_progress);
	/* the old tree is gone: start from the root of the new one */
	reset_archive_view();
	if (B.view == V_ARCHIVE)
		view_settle(cur_view());
	if (ok)
		set_msg("Archive index rebuilt.");
	else
		set_msg("Rescan aborted or failed.");
	cursor_changed();
}

/** Collect the paths to launch/copy: marked items plus the current file. */
static int collect_paths(char ***out)
{
	const BrItem *it = cur_item();
	char p[1100];
	char **list;
	int n = 0, i;

	list = calloc(B.nmarks + 1, sizeof(char *));
	if (!list)
		return 0;
	for (i = 0; i < B.nmarks; i++)
		list[n++] = strdup(B.marks[i]);
	if (item_path(it, p, sizeof(p)) && mark_find(p) < 0)
		list[n++] = strdup(p);
	*out = list;
	return n;
}

static void free_paths(char **l, int n)
{
	int i;
	for (i = 0; i < n; i++)
		free(l[i]);
	free(l);
}

static void show_in_pane(const BrItem *it)
{
	/* force reload (text of a local file) */
	(void)it;
	B.desc_dirty = true;
}

/** Run the marked/current disks; returns true if disks were inserted. */
static bool do_run(void)
{
	const BrItem *it = cur_item();
	char **paths = NULL;
	int n, i, inserted = 0;
	bool local = (B.view == V_LOCAL);
	BrSettings *s = BrData_Settings();

	if (!it)
		return false;
	if (it->kind == BR_DIR)
	{
		enter_dir(it);
		return false;
	}
	if (local && !B.nmarks && !BrLocal_IsDisk(it->label))
	{
		if (is_textfile(it->label))
		{
			show_in_pane(it);
			set_msg("Text shown in the description pane.");
		}
		else
			set_msg("Not a disk image or text file: %s", it->label);
		return false;
	}
	n = collect_paths(&paths);
	if (n == 0)
		return false;

	/* drop anything that is not a disk image */
	{
		int k = 0;
		for (i = 0; i < n; i++)
		{
			if (BrLocal_IsDisk(paths[i]))
				paths[k++] = paths[i];
			else
				free(paths[i]);
		}
		n = k;
	}
	if (n == 0)
	{
		set_msg("Not a disk image: %s", it->label);
		free(paths);
		return false;
	}

	if (!local)
	{
		char abs[1100];
		bool need_copy = false;
		BrData_AbsPath(paths[0], abs, sizeof(abs));
		if (access(abs, R_OK) != 0)
		{
			set_msg("Cannot read %s (archive not mounted?)", base_name(abs));
			free_paths(paths, n);
			return false;
		}
		for (i = 0; i < n; i++)
			if (is_zip(paths[i]))
				need_copy = true;
		if (n >= 2)
			need_copy = true;
		need_copy = need_copy && s->copy_local;

		if (need_copy)
		{
			char dir[1100], err[256] = "";
			char **files = NULL;
			int nfiles = 0;
			set_msg("Copying to local folder...");
			draw_all();
			if (!BrLocal_Copy((const char *const *)paths, n, dir, sizeof(dir), &files, &nfiles,
			                  err, sizeof(err), copy_progress) || nfiles <= 0)
			{
				set_msg("Copy failed: %s", err[0] ? err : "nothing copied");
				if (files)
					BrLocal_FreeList(files, nfiles);
				free_paths(paths, n);
				return false;
			}
			inserted = BrLocal_Insert(files, nfiles);
			BrLocal_FreeList(files, nfiles);
		}
		else
		{
			char **abss = calloc(n, sizeof(char *));
			if (abss)
			{
				for (i = 0; i < n; i++)
				{
					abss[i] = malloc(1100);
					if (abss[i])
						BrData_AbsPath(paths[i], abss[i], 1100);
					else
						abss[i] = strdup("");
				}
				inserted = BrLocal_Insert(abss, n);
				free_paths(abss, n);
			}
		}
	}
	else
		inserted = BrLocal_Insert(paths, n);

	if (inserted <= 0)
	{
		set_msg("Could not insert the disk image.");
		free_paths(paths, n);
		return false;
	}
	if (n > 2)
	{
		DlgAlert_Notice("More than 2 disks selected:\nonly the first one was\ninserted into drive A:.");
	}
	free_paths(paths, n);
	return true;
}

static void do_mark(void)
{
	const BrItem *it = cur_item();
	char p[1100];
	if (!it)
		return;
	if (!item_path(it, p, sizeof(p)))
	{
		set_msg("Only disks can be marked, not folders.");
		return;
	}
	if (mark_toggle(p))
		set_msg("Marked %d: pick next disk, Enter runs", B.nmarks);
	else
		set_msg("Unmarked.");
	B.info_dirty = true;
	set_cursor(cur_view()->fr[cur_view()->depth].cur + 1);
}

static void do_fav(void)
{
	const BrItem *it = cur_item();
	char p[1100];
	View *v = cur_view();
	if (B.view == V_LOCAL)
	{
		set_msg("Favorites are only for archive disks.");
		return;
	}
	if (!item_path(it, p, sizeof(p)))
	{
		set_msg("Favorites: select a disk (not a folder).");
		return;
	}
	if (BrData_ToggleFav(p))
		set_msg("Added to favorites: %s", it->label);
	else
		set_msg("Removed from favorites: %s", it->label);
	if (B.view == V_FAV)
	{
		view_reload(v, BrData_FavRoot());
		cursor_changed();
	}
}

static void do_copy_local(void)
{
	char **paths = NULL;
	int n, i;
	char dir[1100], err[256] = "";
	char **files = NULL;
	int nfiles = 0;

	if (B.view == V_LOCAL)
	{
		set_msg("Already local.");
		return;
	}
	if (!cur_item())
		return;
	if (cur_item()->kind == BR_DIR && !B.nmarks)
	{
		set_msg("Select a disk (or mark disks) to copy.");
		return;
	}
	n = collect_paths(&paths);
	if (n == 0)
		return;
	set_msg("Copying to local folder...");
	draw_all();
	if (BrLocal_Copy((const char *const *)paths, n, dir, sizeof(dir), &files, &nfiles,
	                 err, sizeof(err), copy_progress))
		set_msg("Copied %d file(s) to %s", nfiles, dir);
	else
		set_msg("Copy failed: %s", err[0] ? err : "unknown error");
	if (files)
		BrLocal_FreeList(files, nfiles);
	for (i = 0; i < n; i++)
		free(paths[i]);
	free(paths);
	if (B.view == V_LOCAL)
		view_reload(cur_view(), view_root(V_LOCAL));
}

static void do_delete(void)
{
	const BrItem *it = cur_item();
	View *v = cur_view();
	char path[1100], q[300];
	bool ok;

	if (B.view != V_LOCAL)
	{
		set_msg("Delete works only in the Local view.");
		return;
	}
	if (!it)
		return;
	if (it->kind == BR_FILE)
		ok = BrNode_ItemPath(v->nodes[v->depth], it, path, sizeof(path));
	else
		ok = BrNode_ItemDir(v->nodes[v->depth], it, path, sizeof(path));
	if (!ok)
	{
		set_msg("Cannot delete this entry.");
		return;
	}
	snprintf(q, sizeof(q), "Delete local %s\n%.40s\n%s?", it->kind == BR_DIR ? "folder" : "file",
	         base_name(path), it->kind == BR_DIR ? "(with all its content)" : "");
	if (!DlgAlert_Query(q))
		return;
	if (BrLocal_Delete(path))
	{
		int c = v->fr[v->depth].cur;
		set_msg("Deleted: %s", it->label);
		marks_clear();
		view_reload(v, BrData_LocalRoot());
		set_cursor(c);
		cursor_changed();
	}
	else
		set_msg("Delete refused or failed.");
}


/* ------------------------------------------------------------------ */
/* Settings dialog */

#define SET_ARCH    3
#define SET_LOCAL   5
#define SET_COPY    6
#define SET_BARCH   7
#define SET_BLOC    8
#define SET_RESCAN  9
#define SET_OK     10
#define SET_CANCEL 11

static void set_edit_text(char *edit, int w, const char *full)
{
	if ((int)strlen(full) <= w)
		snprintf(edit, 1024, "%.1023s", full);
	else
		edit[0] = '\0';
}

/** Browser settings; returns true if "Rescan" was requested. */
static bool do_settings(void)
{
	BrSettings *s = BrData_Settings();
	static char edit_arch[1024], edit_loc[1024], arch[FILENAME_MAX], loc[FILENAME_MAX];
	static char lab_arch[100], lab_loc[100];
	static char txt_copy[] = "_Use local copy for zip / multi-disk";
	int SW = B.W < 72 ? B.W : 72, SH = 15, ew;
	bool rescan = false, changed;
	SGOBJ dlg[13];
	int r, cw, ch;
	char old_arch[1024];

	if (SW < 50)
		SW = 50;
	ew = SW - 4;
	snprintf(arch, sizeof(arch), "%s", s->archive_root);
	snprintf(loc, sizeof(loc), "%s", s->local_dir);
	snprintf(old_arch, sizeof(old_arch), "%s", s->archive_root);
	set_edit_text(edit_arch, ew, arch);
	set_edit_text(edit_loc, ew, loc);

	memset(dlg, 0, sizeof(dlg));
#define SGO(i, t, f, xx, yy, ww, hh, tx) \
	do { dlg[i].type = t; dlg[i].flags = f; dlg[i].x = xx; dlg[i].y = yy; dlg[i].w = ww; dlg[i].h = hh; dlg[i].txt = tx; } while (0)
	SGO(0, SGBOX, 0, 0, 0, SW, SH, NULL);
	SGO(1, SGTEXT, 0, 2, 1, 20, 1, "Browser settings");
	snprintf(lab_arch, sizeof(lab_arch), "Archive folder (already mounted):");
	SGO(2, SGTEXT, 0, 2, 3, ew, 1, lab_arch);
	SGO(SET_ARCH, SGEDITFIELD, 0, 2, 4, ew, 1, edit_arch);
	snprintf(lab_loc, sizeof(lab_loc), "Local copies folder:");
	SGO(4, SGTEXT, 0, 2, 6, ew, 1, lab_loc);
	SGO(SET_LOCAL, SGEDITFIELD, 0, 2, 7, ew, 1, edit_loc);
	SGO(SET_COPY, SGCHECKBOX, 0, 2, 9, 40, 1, txt_copy);
	SGO(SET_BARCH, SGBUTTON, SG_EXIT, 2, 11, 17, 1, "_Browse archive");
	SGO(SET_BLOC, SGBUTTON, SG_EXIT, 21, 11, 15, 1, "Browse _local");
	SGO(SET_RESCAN, SGBUTTON, SG_EXIT, 38, 11, 17, 1, "_Rescan archive");
	SGO(SET_OK, SGBUTTON, SG_EXIT | SG_DEFAULT, 2, 13, 10, 1, "OK");
	SGO(SET_CANCEL, SGBUTTON, SG_EXIT | SG_CANCEL, 14, 13, 10, 1, "Cancel");
	SGO(12, SGSTOP, 0, 0, 0, 0, 0, NULL);
#undef SGO
	if (s->copy_local)
		dlg[SET_COPY].state |= SG_SELECTED;

	cw = sdlscrn->w / B.fw;
	ch = sdlscrn->h / B.fh;
	dlg[0].x = (cw - SW) / 2;
	dlg[0].y = (ch - SH) / 2;

	/* show a shortened form of paths that do not fit the edit fields */
	do
	{
		snprintf(lab_arch, sizeof(lab_arch), "Archive folder (already mounted):");
		snprintf(lab_loc, sizeof(lab_loc), "Local copies folder:");
		if ((int)strlen(arch) > ew)
			snprintf(lab_arch, sizeof(lab_arch), "Archive: ...%.*s", ew - 12, arch + strlen(arch) - (ew - 12));
		if ((int)strlen(loc) > ew)
			snprintf(lab_loc, sizeof(lab_loc), "Local: ...%.*s", ew - 10, loc + strlen(loc) - (ew - 10));
		r = SDLGui_DoDialog(dlg);
		/* fold edit fields into the full paths */
		if (edit_arch[0])
			snprintf(arch, sizeof(arch), "%s", edit_arch);
		if (edit_loc[0])
			snprintf(loc, sizeof(loc), "%s", edit_loc);
		if (r == SET_BARCH || r == SET_BLOC)
		{
			char name[96];
			char *target = (r == SET_BARCH) ? arch : loc;
			char *edit = (r == SET_BARCH) ? edit_arch : edit_loc;
			if (SDLGui_DirConfSelect(r == SET_BARCH ? "Archive folder:" : "Local copies folder:",
			                         name, target, ew))
			{
				strip_slash(target);
				set_edit_text(edit, ew, target);
			}
		}
	}
	while (r != SET_OK && r != SET_CANCEL && r != SET_RESCAN && r != SDLGUI_QUIT
	       && r != SDLGUI_ERROR && !bQuitProgram);

	if (r == SET_OK || r == SET_RESCAN)
	{
		strip_slash(arch);
		strip_slash(loc);
		snprintf(s->archive_root, sizeof(s->archive_root), "%.1023s", arch);
		snprintf(s->local_dir, sizeof(s->local_dir), "%.1023s", loc);
		s->copy_local = (dlg[SET_COPY].state & SG_SELECTED) != 0;
		BrData_SaveSettings();
		changed = strcmp(old_arch, s->archive_root) != 0;
		if (r == SET_RESCAN)
			rescan = true;
		else if (changed && DlgAlert_Query("Archive folder changed.\nRescan it now?"))
			rescan = true;
		set_msg("Settings saved.");
		marks_clear();
	}
	return rescan;
}


/* ------------------------------------------------------------------ */
/* event handling */

static void scale_mouse(int *x, int *y)
{
	if (!bInFullScreen || ConfigureParams.Screen.bStretchFullScreen)
		SDLGui_ScaleMouseStateCoordinates(x, y);
}

/** Which dialog object (button/arrow) is under pixel position, or -1. */
static int hit_button(int px, int py)
{
	int cx = px / B.fw - B.obj[0].x, cy = py / B.fh - B.obj[0].y;
	int i;
	for (i = B.o_up; i <= B.o_down; i++)
	{
		SGOBJ *o = &B.obj[i];
		if (cx >= o->x && cx < o->x + o->w && cy >= o->y && cy < o->y + o->h)
			return i;
	}
	for (i = B.o_btn0; i < B.o_btn0 + BTN_COUNT; i++)
	{
		SGOBJ *o = &B.obj[i];
		if (cx >= o->x && cx < o->x + o->w && cy >= o->y && cy < o->y + o->h)
			return i;
	}
	return -1;
}

/** List row under the pixel, or -1. */
static int hit_row(int px, int py)
{
	int cx = px / B.fw - B.obj[0].x, cy = py / B.fh - B.obj[0].y;
	if (cx >= 1 && cx < B.LW && cy >= 2 && cy < 2 + B.nrows)
		return cy - 2;
	return -1;
}

static bool in_desc(int px, int py)
{
	int cx = px / B.fw - B.obj[0].x, cy = py / B.fh - B.obj[0].y;
	return B.lay == 2 && cx >= B.xr && cx < B.xr + B.RW && cy >= B.yl && cy < B.yl + B.DH;
}

/** Is the pixel on the thumb/track of a scrollbar? returns SB_* and sets *onthumb, *rel. */
static int hit_scrollbar(int px, int py, bool *onthumb, int *rel)
{
	int cx = px / B.fw - B.obj[0].x;
	int ids[2] = { B.o_lsb, B.o_dsb };
	int kinds[2] = { SB_LIST, SB_DESC };
	int rows[2] = { B.list_trackrows, B.desc_trackrows };
	int i;
	for (i = 0; i < (B.lay == 2 ? 2 : 1); i++)
	{
		SGOBJ *o = &B.obj[ids[i]];
		int top = (B.obj[0].y + o->y) * B.fh;
		if (cx == o->x && py >= top && py < top + rows[i] * B.fh)
		{
			*rel = py - top;
			*onthumb = *rel >= o->h && *rel < o->h + o->w;
			return kinds[i];
		}
	}
	return SB_NONE;
}

static void sb_page(int kind, int dir)
{
	if (kind == SB_LIST)
		scroll_list(dir * (B.nrows - 1));
	else
		B.dscroll += dir * (B.ndesc - 1);
}

static void sb_drag_to(int kind, int rel)
{
	int o = kind == SB_LIST ? B.o_lsb : B.o_dsb;
	int trackrows = kind == SB_LIST ? B.list_trackrows : B.desc_trackrows;
	int track = trackrows * B.fh - B.obj[o].w;
	int total, vis, maxpos, pos;
	View *v = cur_view();

	if (track <= 0)
		return;
	if (kind == SB_LIST)
	{
		total = v->n;
		vis = B.nrows;
	}
	else
	{
		total = B.ndl;
		vis = B.ndesc;
	}
	maxpos = total - vis;
	if (maxpos <= 0)
		return;
	pos = (int)((long long)(rel - B.drag_off) * maxpos / track);
	if (kind == SB_LIST)
		scroll_list(pos - v->fr[v->depth].top);
	else
		B.dscroll = pos;
}

enum { ACT_NONE = 0, ACT_EXIT_RESET, ACT_EXIT };

static int do_button(int b)
{
	switch (b)
	{
	 case BTN_RUN:
		if (do_run())
			return ACT_EXIT_RESET;
		break;
	 case BTN_MARK:
		do_mark();
		break;
	 case BTN_FAV:
		do_fav();
		break;
	 case BTN_COPY:
		do_copy_local();
		break;
	 case BTN_DEL:
		do_delete();
		break;
	 case BTN_RESCAN:
		do_rescan();
		break;
	 case BTN_SETTINGS:
		if (do_settings())
			do_rescan();
		else
		{
			/* settings may have rebuilt the tree: do not keep old pointers */
			reset_archive_view();
			if (B.view == V_ARCHIVE)
				view_settle(cur_view());
		}
		break;
	 case BTN_CLOSE:
		return ACT_EXIT;
	}
	return ACT_NONE;
}

/** Cycle the layout: list only -> list+info -> list+description (cursor is kept). */
static void toggle_wide(void)
{
	int old = B.lay;

	dlg_free();
	B.lay = (B.lay + 1) % 3;
	if (!dlg_build())
	{
		B.lay = old;            /* cannot fail: same geometry as before */
		dlg_build();
	}
	cursor_changed();
	set_msg("%s", "");
}

static int handle_key(SDL_Event *ev)
{
	SDL_Keycode key = ev2key(*ev);
	int mod = ev2mod(*ev);
	View *v = cur_view();
	Frame *f = &v->fr[v->depth];

	if (mod & (KMOD_CTRL | KMOD_GUI))
		return ACT_NONE;
	if (mod & KMOD_ALT)
	{
		int b;
		if (key == SDLK_UP)
			B.dscroll -= 3;
		else if (key == SDLK_DOWN)
			B.dscroll += 3;
		else if (key >= 33 && key <= 126)
		{
			for (b = 0; b < BTN_COUNT; b++)
				if (toupper((int)key) == btn_key[b])
					return do_button(b);
		}
		return ACT_NONE;
	}

	B.msg[0] = '\0';
	switch (key)
	{
	 case SDLK_UP:
		set_cursor(f->cur - 1);
		break;
	 case SDLK_DOWN:
		set_cursor(f->cur + 1);
		break;
	 case SDLK_PAGEUP:
		set_cursor(f->cur - (B.nrows - 1));
		break;
	 case SDLK_PAGEDOWN:
		set_cursor(f->cur + (B.nrows - 1));
		break;
	 case SDLK_HOME:
		set_cursor(0);
		break;
	 case SDLK_END:
		set_cursor(v->n - 1);
		break;
	 case SDLK_RETURN:
	 case SDLK_KP_ENTER:
		return do_button(BTN_RUN);
	 case SDLK_SPACE:
		do_mark();
		break;
	 case SDLK_BACKSPACE:
	 case SDLK_LEFT:
		leave_dir();
		break;
	 case SDLK_RIGHT:
		if (cur_item() && cur_item()->kind == BR_DIR)
			enter_dir(cur_item());
		break;
	 case SDLK_w:
		toggle_wide();
		break;
	 case SDLK_TAB:
		switch_view((mod & KMOD_SHIFT) ? -1 : 1);
		break;
	 case SDLK_DELETE:
	 case SDLK_KP_PERIOD:
		do_delete();
		break;
	 case SDLK_MINUS:
	 case SDLK_KP_MINUS:
		B.dscroll -= 3;
		break;
	 case SDLK_EQUALS:
	 case SDLK_PLUS:
	 case SDLK_KP_PLUS:
		B.dscroll += 3;
		break;
	 case SDLK_ESCAPE:
		if (B.nmarks)
		{
			marks_clear();
			set_msg("Selection cleared.");
		}
		else
			return ACT_EXIT;
		break;
	 default:
		break;
	}
	return ACT_NONE;
}

static int handle_event(SDL_Event *ev)
{
	int x, y, act = ACT_NONE;

	switch (ev->type)
	{
	 case SDL_QUIT:
		bQuitProgram = true;
		return ACT_EXIT;

	 case SDL_KEYDOWN:
		return handle_key(ev);

	 case SDL_MOUSEWHEEL:
	 {
		int dir = ev->wheel.y > 0 ? -1 : (ev->wheel.y < 0 ? 1 : 0);
		SDL_GetMouseState(&x, &y);
		scale_mouse(&x, &y);
		if (in_desc(x, y))
			B.dscroll += dir * 3;
		else
			scroll_list(dir * 3);
		break;
	 }

	 case SDL_MOUSEBUTTONDOWN:
	 {
		bool onthumb = false;
		int rel = 0, kind, b, row;
		if (ev->button.button != SDL_BUTTON_LEFT)
			break;
		x = ev->button.x;
		y = ev->button.y;
		scale_mouse(&x, &y);
		b = hit_button(x, y);
		if (b >= 0)
		{
			B.obj[b].state |= SG_SELECTED;
			B.pressed = b;
			if (b == B.o_up || b == B.o_down)
			{
				B.repeat = (b == B.o_up) ? -1 : 1;
				B.repeat_t = SDL_GetTicks() + 300;
				scroll_list(B.repeat);
			}
			break;
		}
		kind = hit_scrollbar(x, y, &onthumb, &rel);
		if (kind != SB_NONE)
		{
			int o = kind == SB_LIST ? B.o_lsb : B.o_dsb;
			if (onthumb)
			{
				B.drag = kind;
				B.drag_off = rel - B.obj[o].h;
			}
			else
				sb_page(kind, rel < B.obj[o].h ? -1 : 1);
			break;
		}
		row = hit_row(x, y);
		if (row >= 0)
		{
			View *v = cur_view();
			int idx = v->fr[v->depth].top + row;
			Uint32 now = SDL_GetTicks();
			if (idx < v->n)
			{
				bool dbl = (idx == B.last_click_row && now - B.last_click < 400);
				set_cursor(idx);
				B.last_click = now;
				B.last_click_row = idx;
				if (dbl)
				{
					B.last_click_row = -1;
					act = do_button(BTN_RUN);
				}
			}
		}
		break;
	 }

	 case SDL_MOUSEMOTION:
		if (B.drag != SB_NONE)
		{
			int o = B.drag == SB_LIST ? B.o_lsb : B.o_dsb;
			x = ev->motion.x;
			y = ev->motion.y;
			scale_mouse(&x, &y);
			sb_drag_to(B.drag, y - (B.obj[0].y + B.obj[o].y) * B.fh);
		}
		break;

	 case SDL_MOUSEBUTTONUP:
		if (ev->button.button != SDL_BUTTON_LEFT)
			break;
		B.drag = SB_NONE;
		B.repeat = 0;
		if (B.pressed >= 0)
		{
			int p = B.pressed;
			x = ev->button.x;
			y = ev->button.y;
			scale_mouse(&x, &y);
			B.obj[p].state &= ~SG_SELECTED;
			B.pressed = -1;
			if (hit_button(x, y) == p && p >= B.o_btn0)
			{
				draw_all();
				act = do_button(p - B.o_btn0);
			}
		}
		break;

	 default:
		break;
	}
	return act;
}


/* ------------------------------------------------------------------ */
/* main */

static void leave_dialog(void)
{
	if (B.bg)
	{
		SDL_Rect from = { 0, 0, B.bgrect.w, B.bgrect.h };
		SDL_Rect to = { B.bgrect.x, B.bgrect.y, 0, 0 };
		SDL_BlitSurface(B.bg, &from, sdlscrn, &to);
		SDL_FreeSurface(B.bg);
		B.bg = NULL;
	}
	Screen_UpdateRect(sdlscrn, 0, 0, 0, 0);
}

/** Save the screen area under the dialog so that it can be restored later. */
static void save_background(void)
{
	SDL_Rect src;
	src.x = B.obj[0].x * B.fw - 2;
	src.y = B.obj[0].y * B.fh - 2;
	src.w = B.W * B.fw + 4;
	src.h = B.H * B.fh + 4;
	if (src.x < 0)
		src.x = 0;
	if (src.y < 0)
		src.y = 0;
	if (src.x + src.w > sdlscrn->w)
		src.w = sdlscrn->w - src.x;
	if (src.y + src.h > sdlscrn->h)
		src.h = sdlscrn->h - src.y;
	B.bgrect = src;
	B.bg = SDL_CreateRGBSurface(SDL_SWSURFACE, src.w, src.h, sdlscrn->format->BitsPerPixel,
	                            sdlscrn->format->Rmask, sdlscrn->format->Gmask,
	                            sdlscrn->format->Bmask, sdlscrn->format->Amask);
	if (B.bg)
	{
		if (sdlscrn->format->palette)
			SDL_SetPaletteColors(B.bg->format->palette, sdlscrn->format->palette->colors,
			                     0, sdlscrn->format->palette->ncolors - 1);
		SDL_Rect dst = { 0, 0, 0, 0 };
		SDL_BlitSurface(sdlscrn, &src, B.bg, &dst);
	}
}

static void free_state(void)
{
	marks_clear();
	free(B.marks);
	free(B.desc);
	free(B.dl_off);
	free(B.dl_len);
	if (B.bg)
		SDL_FreeSurface(B.bg);
	dlg_free();
	memset(&B, 0, sizeof(B));
}

/**
 * Show the Browser dialog.
 * Returns true if disk(s) were inserted and the machine should be reset.
 */
bool DlgBrowser_Main(void)
{
	bool reset = false, quit = false;
	SDL_Event ev;

	if (SDLGui_SetScreen(sdlscrn))
		return false;

	memset(&B, 0, sizeof(B));
	B.pressed = -1;
	B.last_click_row = -1;
	B.lay = 1;
	if (!BrData_Init())
		Log_Printf(LOG_WARN, "Browser: data initialisation reported a problem\n");
	if (!dlg_build())
	{
		DlgAlert_Notice("Screen too small for\nthe Browser dialog.");
		free_state();
		return false;
	}
	save_background();

	/* initial views */
	B.views[V_ARCHIVE].nodes[0] = BrData_ArchiveRoot();
	view_settle(&B.views[V_ARCHIVE]);
	B.view = V_ARCHIVE;
	if (B.views[V_ARCHIVE].nodes[0])
		restore_state();
	cursor_changed();
	draw_all();

	if (!B.views[V_ARCHIVE].nodes[0] && !BrData_HaveIndex())
	{
		if (access(BrData_Settings()->archive_root, R_OK) == 0)
		{
			char q[1300];
			snprintf(q, sizeof(q), "No archive index yet.\nScan %.40s now?", BrData_Settings()->archive_root);
			if (DlgAlert_Query(q))
				do_rescan();
		}
		else
			set_msg("Archive folder not found: open Settings (Alt+S)");
	}

	while (!quit && !bQuitProgram)
	{
		Uint32 tmo = 1000;
		bool got;

		if (B.desc_dirty || B.info_dirty)
			tmo = 100;
		if (B.repeat)
			tmo = 40;
		draw_all();
		got = SDL_WaitEventTimeout(&ev, tmo) != 0;
		if (!got)
		{
			if (B.repeat && SDL_GetTicks() >= B.repeat_t)
			{
				scroll_list(B.repeat);
				B.repeat_t = SDL_GetTicks() + 50;
			}
			if (B.desc_dirty || B.info_dirty)
				load_info_and_desc();
			continue;
		}
		do
		{
			int act = handle_event(&ev);
			if (act == ACT_EXIT_RESET)
			{
				reset = true;
				quit = true;
			}
			else if (act == ACT_EXIT)
				quit = true;
		}
		while (!quit && !bQuitProgram && SDL_PollEvent(&ev));
	}

	save_state();
	leave_dialog();
	free_state();
	return reset;
}
