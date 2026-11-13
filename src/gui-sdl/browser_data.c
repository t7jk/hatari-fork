/*
  Hatari - browser_data.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Disk-image Browser data layer: settings, archive index, tree, favorites,
  descriptions and browsing state. Pure C + libc (no SDL), so that it can
  be tested standalone.
*/
const char BrowserData_fileid[] = "Hatari browser_data.c";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "browser.h"
#include "paths.h"

#define BR_PATHBUF   4096
#define BR_MAX_FLAT  30          /* more files than this => virtual letter groups */
#define BR_CAT_ROOT  "BY CATEGORY"
#define BR_TEXT_MAX  102400      /* bytes read from text files */
#define BR_MAX_DEPTH 64

/* ------------------------------------------------------------------ */
/* Internal types                                                      */

typedef struct {
	char       *label;     /* display text (ASCII safe) */
	const char *path;      /* archive-relative or absolute path */
} BrFile;

typedef struct {
	char   *name;          /* real folder name */
	BrNode *node;
} BrDirEnt;

struct BrNode {
	BrNode    *alloc_next; /* chain of all nodes of one tree */
	BrNode   **chain;
	BrDirEnt  *dirs;
	int        ndirs, capdirs, lastdir;
	BrFile    *files;
	int        nfiles, capfiles;
	bool       own_labels; /* free() the labels of files[] */
	bool       own_paths;  /* free() the paths of files[] */
	bool       flat;       /* never split into letter groups */
	bool       virt;       /* virtual letter group */
	int        plen;       /* prefix length for letter groups */
	int        count;      /* cached number of disks below (-1 = unknown) */
	char      *absdir;     /* local trees: real directory */
	BrItem    *items;      /* built lazily */
	const char **ipaths;   /* path of every item (files), parallel to items */
	int        nitems;
	bool       built;
};

static BrSettings g_cfg;
static bool       g_inited;
static char     **g_favs;
static int        g_nfavs, g_capfavs;

/* archive tree cache */
static BrNode    *g_arch_chain;
static BrNode    *g_arch_root;
static char      *g_arch_buf;
static long       g_arch_idx_size = -1, g_arch_cat_size = -1;
static long       g_arch_idx_mtime, g_arch_cat_mtime;
/* other trees */
static BrNode    *g_local_chain;
static BrNode    *g_fav_chain;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */

static const char *cfg_dir(void)
{
	static char dir[1024];
	const char *home = Paths_GetHatariHome();
	snprintf(dir, sizeof(dir), "%.1000s/browser", home ? home : ".");
	return dir;
}

static void br_path(char *out, size_t size, const char *name)
{
	snprintf(out, size, "%.1000s/%.200s", cfg_dir(), name);
}

static char *br_strdup(const char *s)
{
	size_t n = strlen(s);
	char *p = malloc(n + 1);
	if (p)
		memcpy(p, s, n + 1);
	return p;
}

static char *br_strndup(const char *s, size_t n)
{
	char *p = malloc(n + 1);
	if (p) {
		memcpy(p, s, n);
		p[n] = 0;
	}
	return p;
}

/** mkdir -p */
static bool br_mkdirs(const char *path)
{
	char tmp[BR_PATHBUF];
	size_t i, len = strlen(path);
	struct stat st;

	if (len == 0 || len >= sizeof(tmp))
		return false;
	memcpy(tmp, path, len + 1);
	for (i = 1; i <= len; i++) {
		if (tmp[i] == '/' || tmp[i] == 0) {
			char c = tmp[i];
			tmp[i] = 0;
			if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
				return false;
			tmp[i] = c;
		}
	}
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool br_is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/** Read a whole file (max maxsize bytes) into malloc()ed NUL-terminated buffer. */
static char *br_read_file(const char *path, size_t maxsize, size_t *outlen)
{
	FILE *fp = fopen(path, "rb");
	char *buf;
	size_t n, cap = 65536, len = 0;

	if (!fp)
		return NULL;
	buf = malloc(cap + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	while (len < maxsize && (n = fread(buf + len, 1, cap - len, fp)) > 0) {
		len += n;
		if (len >= cap && len < maxsize) {
			char *nb;
			cap *= 2;
			if (cap > maxsize)
				cap = maxsize;
			nb = realloc(buf, cap + 1);
			if (!nb) {
				free(buf);
				fclose(fp);
				return NULL;
			}
			buf = nb;
		}
		if (len >= cap)
			break;
	}
	fclose(fp);
	buf[len] = 0;
	if (outlen)
		*outlen = len;
	return buf;
}

static bool br_copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb"), *out;
	char buf[8192];
	size_t n;
	bool ok = true;

	if (!in)
		return false;
	out = fopen(dst, "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			ok = false;
			break;
		}
	}
	fclose(in);
	if (fclose(out) != 0)
		ok = false;
	return ok;
}

static bool br_file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int cmp_str_ptr(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ------------------------------------------------------------------ */
/* Text conversion: UTF-8 / cp1250 -> ASCII safe for the SDL GUI font  */

static const uint16_t cp1250_hi[128] = {
	0x20AC,0,0x201A,0,0x201E,0x2026,0x2020,0x2021,0,0x2030,0x0160,0x2039,0x015A,0x0164,0x017D,0x0179,
	0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0,0x2122,0x0161,0x203A,0x015B,0x0165,0x017E,0x017A,
	0x00A0,0x02C7,0x02D8,0x0141,0x00A4,0x0104,0x00A6,0x00A7,0x00A8,0x00A9,0x015E,0x00AB,0x00AC,0x00AD,0x00AE,0x017B,
	0x00B0,0x00B1,0x02DB,0x0142,0x00B4,0x00B5,0x00B6,0x00B7,0x00B8,0x0105,0x015F,0x00BB,0x013D,0x02DD,0x013E,0x017C,
	0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
	0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
	0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
	0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9
};

/* U+00C0..U+00FF base letters (AE/ae/ss/TH/th handled separately) */
static const char latin1_base[] =
	"AAAAAAAC" "EEEEIIII" "DNOOOOOx" "OUUUUYTs"
	"aaaaaaac" "eeeeiiii" "dnooooo/" "ouuuuyty";
/* U+0100..U+017F */
static const char latin_ext_a[] =
	"AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
	"lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

/** Map a Unicode code point to up to 3 ASCII chars. Returns number of chars. */
static int cp_to_ascii(uint32_t cp, char *out)
{
	const char *s = NULL;

	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp == 0xA0) {
		out[0] = ' ';
		return 1;
	}
	if (cp >= 0xC0 && cp <= 0xFF) {
		if (cp == 0xC6) s = "AE";
		else if (cp == 0xE6) s = "ae";
		else if (cp == 0xDF) s = "ss";
		else if (cp == 0xDE) s = "Th";
		else if (cp == 0xFE) s = "th";
		if (s) {
			size_t n = strlen(s);
			memcpy(out, s, n);
			return (int)n;
		}
		out[0] = latin1_base[cp - 0xC0];
		return 1;
	}
	if (cp >= 0x100 && cp <= 0x17F) {
		out[0] = latin_ext_a[cp - 0x100];
		return 1;
	}
	switch (cp) {
	case 0xA9: s = "(c)"; break;
	case 0xAE: s = "(R)"; break;
	case 0xB7: s = "."; break;
	case 0xAB: s = "<"; break;
	case 0xBB: s = ">"; break;
	case 0xB0: s = "o"; break;
	case 0x2013: case 0x2014: s = "-"; break;
	case 0x2018: case 0x2019: case 0x201A: s = "'"; break;
	case 0x201C: case 0x201D: case 0x201E: s = "\""; break;
	case 0x2022: s = "*"; break;
	case 0x2026: s = "..."; break;
	case 0x20AC: s = "E"; break;
	case 0x2122: s = "TM"; break;
	default: s = "?"; break;
	}
	{
		size_t n = strlen(s);
		memcpy(out, s, n);
		return (int)n;
	}
}

/** Decode one UTF-8 sequence; returns length (0 = invalid). */
static int utf8_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
	if (n == 0)
		return 0;
	if (s[0] < 0x80) {
		*cp = s[0];
		return 1;
	}
	if (s[0] >= 0xC2 && s[0] <= 0xDF && n >= 2 && (s[1] & 0xC0) == 0x80) {
		*cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
		return 2;
	}
	if ((s[0] & 0xF0) == 0xE0 && n >= 3 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		*cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		if (*cp < 0x800)
			return 0;
		return 3;
	}
	if ((s[0] & 0xF8) == 0xF0 && n >= 4 && (s[1] & 0xC0) == 0x80 &&
	    (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		*cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
		      ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		if (*cp < 0x10000 || *cp > 0x10FFFF)
			return 0;
		return 4;
	}
	return 0;
}

/** Valid UTF-8? An incomplete sequence at the very end (truncated read) is tolerated. */
static bool utf8_valid(const unsigned char *s, size_t n, bool tolerate_tail)
{
	size_t i = 0;
	uint32_t cp;
	int l;

	while (i < n) {
		l = utf8_decode(s + i, n - i, &cp);
		if (l == 0) {
			if (tolerate_tail && n - i < 4 && s[i] >= 0xC0)
				return true;
			return false;
		}
		i += l;
	}
	return true;
}

/**
 * Convert text to ASCII. out must hold at least 4*n+1 bytes (multiline) .
 * multiline: keep '\n', tab -> 4 spaces, drop CR. Otherwise every control char -> ' '.
 * Returns length.
 */
static size_t text_to_ascii(const unsigned char *in, size_t n, char *out, bool multiline,
                            bool tolerate_tail)
{
	bool utf8 = utf8_valid(in, n, tolerate_tail);
	size_t i = 0, o = 0;

	while (i < n) {
		uint32_t cp;
		if (utf8) {
			int l = utf8_decode(in + i, n - i, &cp);
			if (l == 0) {		/* truncated tail */
				break;
			}
			i += l;
		} else {
			unsigned char c = in[i++];
			cp = (c >= 0x80) ? cp1250_hi[c - 0x80] : c;
			if (c >= 0x80 && cp == 0)
				cp = '?';
		}
		if (cp < 0x20 || cp == 0x7F) {
			if (multiline) {
				if (cp == '\n')
					out[o++] = '\n';
				else if (cp == '\t') {
					memset(out + o, ' ', 4);
					o += 4;
				} else if (cp == '\r' || cp == 0) {
					/* dropped */
				} else
					out[o++] = ' ';
			} else
				out[o++] = ' ';
			continue;
		}
		o += cp_to_ascii(cp, out + o);
	}
	out[o] = 0;
	return o;
}

/** ASCII-safe copy of a name for display (malloc()ed). */
static char *label_dup(const char *s, size_t n)
{
	char *out = malloc(4 * n + 1);
	if (!out)
		return NULL;
	text_to_ascii((const unsigned char *)s, n, out, false, false);
	return out;
}

/* ------------------------------------------------------------------ */
/* Nodes                                                               */

static BrNode *node_new(BrNode **chain)
{
	BrNode *n = calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->count = -1;
	n->chain = chain;
	n->alloc_next = *chain;
	*chain = n;
	return n;
}

static void chain_free(BrNode **chain)
{
	BrNode *n = *chain, *next;
	int i;

	for (; n; n = next) {
		next = n->alloc_next;
		for (i = 0; i < n->ndirs; i++)
			free(n->dirs[i].name);
		free(n->dirs);
		for (i = 0; i < n->nfiles; i++) {
			if (n->own_labels)
				free(n->files[i].label);
			if (n->own_paths)
				free((char *)n->files[i].path);
		}
		free(n->files);
		free(n->absdir);
		free(n->items);
		free(n->ipaths);
		free(n);
	}
	*chain = NULL;
}

/** Find or create a subfolder (name/len is not NUL terminated). */
static BrNode *node_subdir(BrNode *parent, const char *name, size_t len)
{
	int i, start = parent->lastdir;
	BrNode *sub;

	for (i = 0; i < parent->ndirs; i++) {
		int k = (start + i) % parent->ndirs;
		const char *dn = parent->dirs[k].name;
		if (strncmp(dn, name, len) == 0 && dn[len] == 0) {
			parent->lastdir = k;
			return parent->dirs[k].node;
		}
	}
	if (parent->ndirs == parent->capdirs) {
		int cap = parent->capdirs ? parent->capdirs * 2 : 4;
		BrDirEnt *nd = realloc(parent->dirs, cap * sizeof(*nd));
		if (!nd)
			return NULL;
		parent->dirs = nd;
		parent->capdirs = cap;
	}
	sub = node_new(parent->chain);
	if (!sub)
		return NULL;
	sub->own_labels = true;
	parent->dirs[parent->ndirs].name = br_strndup(name, len);
	parent->dirs[parent->ndirs].node = sub;
	parent->lastdir = parent->ndirs;
	parent->ndirs++;
	return sub;
}

static bool node_addfile(BrNode *n, char *label, const char *path)
{
	if (n->nfiles == n->capfiles) {
		int cap = n->capfiles ? n->capfiles * 2 : 8;
		BrFile *nf = realloc(n->files, cap * sizeof(*nf));
		if (!nf)
			return false;
		n->files = nf;
		n->capfiles = cap;
	}
	n->files[n->nfiles].label = label;
	n->files[n->nfiles].path = path;
	n->nfiles++;
	return true;
}

static int node_count(BrNode *n)
{
	if (n->count < 0) {
		int i, c = n->nfiles;
		for (i = 0; i < n->ndirs; i++)
			c += node_count(n->dirs[i].node);
		n->count = c;
	}
	return n->count;
}

static int cmp_dirent(const void *a, const void *b)
{
	const BrDirEnt *x = a, *y = b;
	int rx = strcmp(x->name, BR_CAT_ROOT) != 0;
	int ry = strcmp(y->name, BR_CAT_ROOT) != 0;
	int r;
	if (rx != ry)
		return rx - ry;
	r = strcasecmp(x->name, y->name);
	return r ? r : strcmp(x->name, y->name);
}

static int cmp_file(const void *a, const void *b)
{
	const BrFile *x = a, *y = b;
	int r = strcasecmp(x->label, y->label);
	return r ? r : strcmp(x->label, y->label);
}

typedef struct {
	char key[16];
	int first, n;            /* range in sorted file array */
} Bucket;

static void bucket_key(const char *name, int plen, char *key)
{
	int i;
	if (!isalpha((unsigned char)name[0])) {
		strcpy(key, "0-9");
		return;
	}
	for (i = 0; i < plen && i < 15 && name[i]; i++)
		key[i] = (char)toupper((unsigned char)name[i]);
	key[i] = 0;
}

static int cmp_bucket(const void *a, const void *b)
{
	const Bucket *x = a, *y = b;
	int zx = strcmp(x->key, "0-9") != 0, zy = strcmp(y->key, "0-9") != 0;
	if (zx != zy)
		return zx - zy;
	return strcmp(x->key, y->key);
}

/** Collapse chains of folders that contain nothing but one subfolder. */
static BrNode *node_collapse(BrNode *node, char *label, size_t labsize, const char *name)
{
	char *l = label_dup(name, strlen(name));
	snprintf(label, labsize, "%s", l ? l : "?");
	free(l);
	while (node->nfiles == 0 && node->ndirs == 1) {
		size_t cur = strlen(label);
		l = label_dup(node->dirs[0].name, strlen(node->dirs[0].name));
		if (cur + 1 < labsize)
			snprintf(label + cur, labsize - cur, "/%s", l ? l : "?");
		free(l);
		node = node->dirs[0].node;
	}
	return node;
}

static void node_build(BrNode *node)
{
	int i, total, nd, nf;
	BrFile *sorted = NULL;
	BrItem *items;
	const char **ipaths;

	if (node->built)
		return;
	node->built = true;

	if (node->ndirs > 1)
		qsort(node->dirs, node->ndirs, sizeof(BrDirEnt), cmp_dirent);
	nd = node->ndirs;
	nf = node->nfiles;
	if (nf > 1) {
		sorted = malloc(nf * sizeof(BrFile));
		if (!sorted)
			return;
		memcpy(sorted, node->files, nf * sizeof(BrFile));
		qsort(sorted, nf, sizeof(BrFile), cmp_file);
	}
	{
		const BrFile *src = sorted ? sorted : node->files;
		Bucket *buckets = NULL;
		int nb = 0, plen = node->plen ? node->plen : 1;
		bool grouped = false;

		if (nf > BR_MAX_FLAT && !node->flat) {
			buckets = malloc(nf * sizeof(Bucket));
			if (buckets) {
				for (;;) {
					nb = 0;
					for (i = 0; i < nf; i++) {
						char key[16];
						bucket_key(src[i].label, plen, key);
						/* sorted by label, so equal keys are mostly adjacent;
						 * search backwards to be robust */
						int k, found = -1;
						for (k = nb - 1; k >= 0; k--)
							if (strcmp(buckets[k].key, key) == 0) {
								found = k;
								break;
							}
						if (found < 0) {
							strcpy(buckets[nb].key, key);
							buckets[nb].n = 0;
							found = nb++;
						}
						buckets[found].n++;
					}
					if (nb > 1 || plen >= 8)
						break;
					plen++;
				}
				grouped = nb > 1;
			}
		}

		total = nd + (grouped ? nb : nf);
		items = calloc(total ? total : 1, sizeof(BrItem));
		ipaths = calloc(total ? total : 1, sizeof(char *));
		if (!items || !ipaths) {
			free(items);
			free(ipaths);
			free(buckets);
			free(sorted);
			return;
		}
		for (i = 0; i < nd; i++) {
			BrNode *sub = node_collapse(node->dirs[i].node, items[i].label,
			                            sizeof(items[i].label), node->dirs[i].name);
			items[i].kind = BR_DIR;
			items[i].sub = sub;
			items[i].virt = false;
			items[i].count = node_count(sub);
		}
		if (!grouped) {
			for (i = 0; i < nf; i++) {
				BrItem *it = &items[nd + i];
				it->kind = BR_FILE;
				snprintf(it->label, sizeof(it->label), "%s", src[i].label);
				ipaths[nd + i] = src[i].path;
			}
		} else {
			int k;
			qsort(buckets, nb, sizeof(Bucket), cmp_bucket);
			for (k = 0; k < nb; k++) {
				BrNode *sub = node_new(node->chain);
				BrItem *it = &items[nd + k];
				int c = 0;
				if (sub) {
					sub->virt = true;
					sub->plen = plen + 1;
					sub->absdir = node->absdir ? br_strdup(node->absdir) : NULL;
					sub->files = malloc(buckets[k].n * sizeof(BrFile));
					sub->capfiles = buckets[k].n;
					for (i = 0; sub->files && i < nf; i++) {
						char key[16];
						bucket_key(src[i].label, plen, key);
						if (strcmp(key, buckets[k].key) == 0)
							sub->files[c++] = src[i];
					}
					sub->nfiles = c;
					sub->count = c;
				}
				it->kind = BR_DIR;
				snprintf(it->label, sizeof(it->label), "%s", buckets[k].key);
				it->sub = sub;
				it->virt = true;
				it->count = c;
			}
		}
		free(buckets);
	}
	free(sorted);
	node->items = items;
	node->ipaths = ipaths;
	node->nitems = total;
}

const BrItem *BrNode_Items(BrNode *node, int *count)
{
	if (!node) {
		if (count)
			*count = 0;
		return NULL;
	}
	node_build(node);
	if (count)
		*count = node->nitems;
	return node->items;
}

static int item_index(BrNode *node, const BrItem *item)
{
	long idx;
	if (!node || !item || !node->items)
		return -1;
	idx = item - node->items;
	if (idx < 0 || idx >= node->nitems)
		return -1;
	return (int)idx;
}

bool BrNode_ItemPath(BrNode *node, const BrItem *item, char *out, size_t outsize)
{
	int idx = item_index(node, item);
	if (idx < 0 || item->kind != BR_FILE || !node->ipaths[idx] || outsize == 0)
		return false;
	snprintf(out, outsize, "%s", node->ipaths[idx]);
	return true;
}

bool BrNode_ItemDir(BrNode *node, const BrItem *item, char *out, size_t outsize)
{
	int idx = item_index(node, item);
	if (idx < 0 || item->kind != BR_DIR || item->virt || !item->sub ||
	    !item->sub->absdir || outsize == 0)
		return false;
	snprintf(out, outsize, "%s", item->sub->absdir);
	return true;
}

void BrData_AbsPath(const char *path, char *out, size_t outsize)
{
	if (outsize == 0)
		return;
	if (path[0] == '/')
		snprintf(out, outsize, "%s", path);
	else
		snprintf(out, outsize, "%s/%s", g_cfg.archive_root, path);
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */

static void settings_defaults(void)
{
	const char *home = getenv("HOME");
	char def_local[BR_PATHBUF];

	memset(&g_cfg, 0, sizeof(g_cfg));
	if (br_is_dir("/home/t7jk/atarist/ATARI_ST"))
		snprintf(g_cfg.archive_root, sizeof(g_cfg.archive_root), "%s",
		         "/home/t7jk/atarist/ATARI_ST");
	else
		snprintf(g_cfg.archive_root, sizeof(g_cfg.archive_root), "%s",
		         home && home[0] ? home : "/");
	snprintf(def_local, sizeof(def_local), "%.1000s/local", cfg_dir());
	snprintf(g_cfg.local_dir, sizeof(g_cfg.local_dir), "%.1000s", def_local);
	g_cfg.copy_local = true;
}

static void settings_load(void)
{
	char path[BR_PATHBUF];
	FILE *fp;
	char line[BR_PATHBUF + 64];

	br_path(path, sizeof(path), "browser.cfg");
	fp = fopen(path, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *eq = strchr(line, '='), *val;
		size_t l;
		if (!eq || line[0] == '#')
			continue;
		*eq = 0;
		val = eq + 1;
		l = strlen(val);
		while (l > 0 && (val[l - 1] == '\n' || val[l - 1] == '\r'))
			val[--l] = 0;
		if (strcmp(line, "archive_root") == 0 && val[0])
			snprintf(g_cfg.archive_root, sizeof(g_cfg.archive_root), "%s", val);
		else if (strcmp(line, "local_dir") == 0 && val[0])
			snprintf(g_cfg.local_dir, sizeof(g_cfg.local_dir), "%s", val);
		else if (strcmp(line, "copy_local") == 0)
			g_cfg.copy_local = (val[0] == '1' || val[0] == 't' || val[0] == 'y');
	}
	fclose(fp);
}

BrSettings *BrData_Settings(void)
{
	return &g_cfg;
}

bool BrData_SaveSettings(void)
{
	char path[BR_PATHBUF];
	FILE *fp;

	if (!br_mkdirs(cfg_dir()))
		return false;
	br_path(path, sizeof(path), "browser.cfg");
	fp = fopen(path, "w");
	if (!fp)
		return false;
	fprintf(fp, "archive_root=%s\nlocal_dir=%s\ncopy_local=%d\n",
	        g_cfg.archive_root, g_cfg.local_dir, g_cfg.copy_local ? 1 : 0);
	return fclose(fp) == 0;
}

/* ------------------------------------------------------------------ */
/* Favorites                                                           */

static void favs_clear(void)
{
	int i;
	for (i = 0; i < g_nfavs; i++)
		free(g_favs[i]);
	g_nfavs = 0;
}

static bool favs_add(const char *s)
{
	char *d;
	if (g_nfavs == g_capfavs) {
		int cap = g_capfavs ? g_capfavs * 2 : 32;
		char **nf = realloc(g_favs, cap * sizeof(char *));
		if (!nf)
			return false;
		g_favs = nf;
		g_capfavs = cap;
	}
	d = br_strdup(s);
	if (!d)
		return false;
	g_favs[g_nfavs++] = d;
	return true;
}

static void favs_load(void)
{
	char path[BR_PATHBUF];
	char *buf, *p, *next;

	favs_clear();
	br_path(path, sizeof(path), "favorites.txt");
	buf = br_read_file(path, 64 * 1024 * 1024, NULL);
	if (!buf)
		return;
	for (p = buf; p && *p; p = next) {
		size_t l;
		next = strchr(p, '\n');
		if (next)
			*next++ = 0;
		l = strlen(p);
		while (l > 0 && p[l - 1] == '\r')
			p[--l] = 0;
		if (l > 0)
			favs_add(p);
	}
	free(buf);
}

static bool favs_save(void)
{
	char path[BR_PATHBUF];
	FILE *fp;
	int i;

	br_mkdirs(cfg_dir());
	br_path(path, sizeof(path), "favorites.txt");
	fp = fopen(path, "w");
	if (!fp)
		return false;
	for (i = 0; i < g_nfavs; i++)
		fprintf(fp, "%s\n", g_favs[i]);
	return fclose(fp) == 0;
}

int BrData_FavCount(void)
{
	return g_nfavs;
}

const char *BrData_FavGet(int i)
{
	return (i >= 0 && i < g_nfavs) ? g_favs[i] : NULL;
}

bool BrData_IsFav(const char *rel)
{
	int i;
	for (i = 0; rel && i < g_nfavs; i++)
		if (strcmp(g_favs[i], rel) == 0)
			return true;
	return false;
}

bool BrData_ToggleFav(const char *rel)
{
	int i;
	bool now;

	if (!rel || !rel[0])
		return false;
	for (i = 0; i < g_nfavs; i++) {
		if (strcmp(g_favs[i], rel) == 0) {
			free(g_favs[i]);
			memmove(&g_favs[i], &g_favs[i + 1], (g_nfavs - i - 1) * sizeof(char *));
			g_nfavs--;
			favs_save();
			return false;
		}
	}
	now = favs_add(rel);
	favs_save();
	return now;
}

BrNode *BrData_FavRoot(void)
{
	BrNode *root;
	int i;

	chain_free(&g_fav_chain);
	root = node_new(&g_fav_chain);
	if (!root)
		return NULL;
	root->flat = true;
	root->own_labels = true;
	root->own_paths = true;
	for (i = 0; i < g_nfavs; i++) {
		const char *base = strrchr(g_favs[i], '/');
		char *lab, *path;
		base = base ? base + 1 : g_favs[i];
		lab = label_dup(base, strlen(base));
		path = br_strdup(g_favs[i]);
		if (!lab || !path || !node_addfile(root, lab, path)) {
			free(lab);
			free(path);
		}
	}
	return root;
}

/* ------------------------------------------------------------------ */
/* categories.json (mini parser: {"KEY": ["str", ...], ...})           */

typedef struct {
	char   key[16];
	char **v;
	int    n, cap;
} CatList;

typedef struct {
	CatList list[16];
	int     n;
} Cats;

static void cats_free(Cats *c)
{
	int i, j;
	for (i = 0; i < c->n; i++) {
		for (j = 0; j < c->list[i].n; j++)
			free(c->list[i].v[j]);
		free(c->list[i].v);
	}
	c->n = 0;
}

static const char *json_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

static int hex4(const char *p)
{
	int v = 0, i;
	for (i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= c - '0';
		else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
		else return -1;
	}
	return v;
}

static size_t put_utf8(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/** Parse a JSON string at *pp (which points at the opening quote). Returns malloc()ed UTF-8. */
static char *json_string(const char **pp)
{
	const char *p = *pp;
	size_t cap = 64, len = 0;
	char *out;

	if (*p != '"')
		return NULL;
	p++;
	out = malloc(cap);
	if (!out)
		return NULL;
	while (*p && *p != '"') {
		char tmp[4];
		size_t n;
		if (len + 8 > cap) {
			char *no;
			cap *= 2;
			no = realloc(out, cap);
			if (!no) {
				free(out);
				return NULL;
			}
			out = no;
		}
		if (*p == '\\') {
			p++;
			switch (*p) {
			case 'n': out[len++] = '\n'; p++; break;
			case 't': out[len++] = '\t'; p++; break;
			case 'r': out[len++] = '\r'; p++; break;
			case 'b': out[len++] = '\b'; p++; break;
			case 'f': out[len++] = '\f'; p++; break;
			case 'u': {
				int cp = hex4(p + 1);
				if (cp < 0) {
					free(out);
					return NULL;
				}
				p += 5;
				if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
					int lo = hex4(p + 2);
					if (lo >= 0xDC00 && lo < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						p += 6;
					}
				}
				n = put_utf8((uint32_t)cp, tmp);
				memcpy(out + len, tmp, n);
				len += n;
				break;
			}
			case 0:
				free(out);
				return NULL;
			default:	/* \" \\ \/ and anything else: literal */
				out[len++] = *p++;
				break;
			}
		} else
			out[len++] = *p++;
	}
	if (*p != '"') {
		free(out);
		return NULL;
	}
	out[len] = 0;
	*pp = p + 1;
	return out;
}

static bool cats_add(CatList *cl, char *s)
{
	if (cl->n == cl->cap) {
		int cap = cl->cap ? cl->cap * 2 : 64;
		char **nv = realloc(cl->v, cap * sizeof(char *));
		if (!nv)
			return false;
		cl->v = nv;
		cl->cap = cap;
	}
	cl->v[cl->n++] = s;
	return true;
}

static bool cats_parse(const char *p, Cats *c)
{
	c->n = 0;
	p = json_ws(p);
	if (*p != '{')
		return false;
	p = json_ws(p + 1);
	while (*p && *p != '}') {
		char *key = json_string(&p);
		CatList *cl;
		if (!key)
			return false;
		p = json_ws(p);
		if (*p != ':') {
			free(key);
			return false;
		}
		p = json_ws(p + 1);
		if (*p != '[' || c->n >= 16) {
			free(key);
			return false;
		}
		cl = &c->list[c->n++];
		memset(cl, 0, sizeof(*cl));
		snprintf(cl->key, sizeof(cl->key), "%s", key);
		free(key);
		p = json_ws(p + 1);
		while (*p && *p != ']') {
			char *s = json_string(&p);
			if (!s)
				return false;
			if (!cats_add(cl, s))
				free(s);
			p = json_ws(p);
			if (*p == ',')
				p = json_ws(p + 1);
			else if (*p != ']')
				return false;
		}
		if (*p != ']')
			return false;
		p = json_ws(p + 1);
		if (*p == ',')
			p = json_ws(p + 1);
		else if (*p != '}')
			return false;
	}
	return *p == '}';
}

/* ------------------------------------------------------------------ */
/* Archive index and tree                                              */

static const struct { const char *code, *name; } category_names[] = {
	{ "PC", "Point And Click Adventures" },
	{ "FD", "Fire And Destroy" },
	{ "ST", "Strategy" },
	{ "JR", "Jump And Run" },
	{ "TA", "Text Adventure" }
};

static const char *const skip_dirs[] = {
	"Extras", "Screenshots", "Emulators", "Music", "Gfx", "Help", "Online Magazines",
	"GamebaseST v3.1d packed", "Installers", "Splash", "Sounds", "Scripts", "Shortcuts",
	"TOS", "HD", "UTILS", "MODULES", "web", "Sources", "ULSV3", NULL
};

static const char *const disk_exts[] = { ".st", ".msa", ".stx", ".dim", ".zip", NULL };

static bool has_disk_ext(const char *name)
{
	const char *dot = strrchr(name, '.');
	int i;
	if (!dot)
		return false;
	for (i = 0; disk_exts[i]; i++)
		if (strcasecmp(dot, disk_exts[i]) == 0)
			return true;
	return false;
}

bool BrData_HaveIndex(void)
{
	char path[BR_PATHBUF];
	struct stat st;
	br_path(path, sizeof(path), "index.txt");
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static void arch_invalidate(void)
{
	chain_free(&g_arch_chain);
	g_arch_root = NULL;
	free(g_arch_buf);
	g_arch_buf = NULL;
	g_arch_idx_size = g_arch_cat_size = -1;
}

typedef struct {
	FILE *fp;
	long  found;
	bool  (*progress)(int);
	bool  aborted;
} ScanCtx;

static void scan_dir(ScanCtx *ctx, const char *abs, const char *rel, int depth)
{
	DIR *d;
	struct dirent *de;
	char **files = NULL, **dirs = NULL;
	int nf = 0, nd = 0, capf = 0, capd = 0, i;

	if (depth > BR_MAX_DEPTH)
		return;
	d = opendir(abs);
	if (!d)
		return;
	while ((de = readdir(d)) != NULL) {
		char full[BR_PATHBUF];
		struct stat st;
		bool isdir;
		if (de->d_name[0] == '.' && (de->d_name[1] == 0 ||
		    (de->d_name[1] == '.' && de->d_name[2] == 0)))
			continue;
		snprintf(full, sizeof(full), "%s/%s", abs, de->d_name);
		if (lstat(full, &st) != 0)
			continue;
		isdir = S_ISDIR(st.st_mode);
		if (isdir) {
			int k;
			bool skip = false;
			for (k = 0; skip_dirs[k]; k++)
				if (strcmp(skip_dirs[k], de->d_name) == 0)
					skip = true;
			if (skip)
				continue;
			if (nd == capd) {
				char **t = realloc(dirs, (capd ? capd * 2 : 16) * sizeof(char *));
				if (!t)
					continue;
				dirs = t;
				capd = capd ? capd * 2 : 16;
			}
			dirs[nd++] = br_strdup(de->d_name);
		} else if (has_disk_ext(de->d_name)) {
			if (nf == capf) {
				char **t = realloc(files, (capf ? capf * 2 : 16) * sizeof(char *));
				if (!t)
					continue;
				files = t;
				capf = capf ? capf * 2 : 16;
			}
			files[nf++] = br_strdup(de->d_name);
		}
	}
	closedir(d);
	if (nf > 1)
		qsort(files, nf, sizeof(char *), cmp_str_ptr);
	if (nd > 1)
		qsort(dirs, nd, sizeof(char *), cmp_str_ptr);
	for (i = 0; i < nf; i++) {
		if (rel[0])
			fprintf(ctx->fp, "%s/%s\n", rel, files[i]);
		else
			fprintf(ctx->fp, "%s\n", files[i]);
		ctx->found++;
		free(files[i]);
	}
	free(files);
	if (ctx->progress && !ctx->progress((int)ctx->found))
		ctx->aborted = true;
	for (i = 0; i < nd; i++) {
		if (!ctx->aborted) {
			char sabs[BR_PATHBUF], srel[BR_PATHBUF];
			snprintf(sabs, sizeof(sabs), "%s/%s", abs, dirs[i]);
			if (rel[0])
				snprintf(srel, sizeof(srel), "%s/%s", rel, dirs[i]);
			else
				snprintf(srel, sizeof(srel), "%s", dirs[i]);
			scan_dir(ctx, sabs, srel, depth + 1);
		}
		free(dirs[i]);
	}
	free(dirs);
}

bool BrData_Rescan(bool (*progress)(int found))
{
	char idx[BR_PATHBUF], tmp[BR_PATHBUF];
	ScanCtx ctx;
	bool ok;

	if (!br_is_dir(g_cfg.archive_root) || !br_mkdirs(cfg_dir()))
		return false;
	br_path(idx, sizeof(idx), "index.txt");
	snprintf(tmp, sizeof(tmp), "%.4000s.tmp", idx);
	ctx.fp = fopen(tmp, "w");
	if (!ctx.fp)
		return false;
	ctx.found = 0;
	ctx.progress = progress;
	ctx.aborted = false;
	if (progress)
		progress(0);
	scan_dir(&ctx, g_cfg.archive_root, "", 0);
	ok = (fclose(ctx.fp) == 0) && !ctx.aborted;
	if (ok)
		ok = (rename(tmp, idx) == 0);
	if (!ok) {
		unlink(tmp);
		return false;
	}
	arch_invalidate();
	return true;
}

static bool stat_info(const char *path, long *size, long *mtime)
{
	struct stat st;
	if (stat(path, &st) != 0) {
		*size = -1;
		*mtime = 0;
		return false;
	}
	*size = (long)st.st_size;
	*mtime = (long)st.st_mtime;
	return true;
}

/** Open-addressing set of labels of one node (indexes into node->files), tab size is 2^k. */
static unsigned label_hash(const char *s)
{
	unsigned h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static bool label_present(const BrNode *node, const int *tab, unsigned mask, const char *label)
{
	unsigned h = label_hash(label) & mask;
	while (tab[h] >= 0) {
		if (strcmp(node->files[tab[h]].label, label) == 0)
			return true;
		h = (h + 1) & mask;
	}
	return false;
}

static void label_insert(const BrNode *node, int *tab, unsigned mask, int fileidx)
{
	unsigned h = label_hash(node->files[fileidx].label) & mask;
	while (tab[h] >= 0)
		h = (h + 1) & mask;
	tab[h] = fileidx;
}

/** "label  (fmt)" where fmt is the 3rd path segment from the end ("?" if none). */
static char *dup_label_with_format(const char *label, const char *rel)
{
	const char *starts[3] = { NULL, NULL, NULL };
	const char *s = rel, *e, *seg = NULL;
	int idx = 0;
	char *fmt, *out;
	size_t l;

	while (*s) {
		starts[idx % 3] = s;
		idx++;
		e = strchr(s, '/');
		if (!e)
			break;
		s = e + 1;
	}
	if (idx >= 3)
		seg = starts[(idx - 3) % 3];
	if (seg) {
		e = strchr(seg, '/');
		fmt = label_dup(seg, e ? (size_t)(e - seg) : strlen(seg));
	} else
		fmt = br_strdup("?");
	if (!fmt)
		return NULL;
	l = strlen(label) + strlen(fmt) + 8;
	out = malloc(l);
	if (out)
		snprintf(out, l, "%s  (%s)", label, fmt);
	free(fmt);
	return out;
}

static void arch_build_categories(BrNode *root, const char *const *sorted, int nsorted)
{
	char path[BR_PATHBUF];
	char *json;
	Cats cats;
	BrNode *catroot = NULL;
	size_t c;

	br_path(path, sizeof(path), "categories.json");
	json = br_read_file(path, 64 * 1024 * 1024, NULL);
	if (!json)
		return;
	memset(&cats, 0, sizeof(cats));
	cats_parse(json, &cats);      /* keep whatever was parsed */
	free(json);

	for (c = 0; c < sizeof(category_names) / sizeof(category_names[0]); c++) {
		CatList *cl = NULL;
		const char **found;
		int i, nfound = 0;
		BrNode *node = NULL;
		int *tab;
		unsigned mask = 1;

		for (i = 0; i < cats.n; i++)
			if (strcmp(cats.list[i].key, category_names[c].code) == 0)
				cl = &cats.list[i];
		if (!cl || cl->n == 0)
			continue;
		found = malloc(cl->n * sizeof(char *));
		if (!found)
			continue;
		for (i = 0; i < cl->n; i++) {
			const char *const *hit = bsearch(&cl->v[i], sorted, nsorted,
			                                 sizeof(char *), cmp_str_ptr);
			if (hit)
				found[nfound++] = *hit;
		}
		if (nfound == 0) {
			free(found);
			continue;
		}
		qsort(found, nfound, sizeof(char *), cmp_str_ptr);
		if (!catroot) {
			catroot = node_subdir(root, BR_CAT_ROOT, strlen(BR_CAT_ROOT));
			if (!catroot) {
				free(found);
				break;
			}
		}
		node = node_subdir(catroot, category_names[c].name, strlen(category_names[c].name));
		while (mask < (unsigned)nfound * 2)
			mask <<= 1;
		tab = node ? malloc(mask * sizeof(int)) : NULL;
		if (!tab) {
			free(found);
			continue;
		}
		memset(tab, 0xff, mask * sizeof(int));
		mask--;
		for (i = 0; i < nfound; i++) {
			const char *r = found[i], *base = strrchr(r, '/');
			char *label;
			size_t blen;
			bool dup;
			base = base ? base + 1 : r;
			blen = strlen(base);
			label = label_dup(base, blen);
			if (!label)
				continue;
			dup = label_present(node, tab, mask, label);
			if (dup) {
				/* same name in ST/STX/HD: add the format folder (3rd from the end) */
				char *nl = dup_label_with_format(label, r);
				free(label);
				label = nl;
				if (!label)
					continue;
				if (label_present(node, tab, mask, label)) {
					free(label);
					continue;
				}
			}
			if (!node_addfile(node, label, r))
				free(label);
			else
				label_insert(node, tab, mask, node->nfiles - 1);
		}
		free(tab);
		free(found);
	}
	cats_free(&cats);
}

static BrNode *arch_build(void)
{
	char path[BR_PATHBUF];
	size_t len;
	char *buf, *p, *next;
	const char **lines = NULL;
	int nlines = 0, cap = 0, i;
	BrNode *root;

	br_path(path, sizeof(path), "index.txt");
	buf = br_read_file(path, (size_t)1 << 30, &len);
	if (!buf)
		return NULL;
	root = node_new(&g_arch_chain);
	if (!root) {
		free(buf);
		return NULL;
	}
	root->own_labels = true;

	for (p = buf; p && *p; p = next) {
		size_t l;
		next = strchr(p, '\n');
		if (next)
			*next++ = 0;
		l = strlen(p);
		while (l > 0 && p[l - 1] == '\r')
			p[--l] = 0;
		if (l == 0)
			continue;
		if (nlines == cap) {
			const char **t = realloc(lines, (cap ? cap * 2 : 1024) * sizeof(char *));
			if (!t)
				break;
			lines = t;
			cap = cap ? cap * 2 : 1024;
		}
		lines[nlines++] = p;
	}
	if (nlines == 0) {
		free(lines);
		free(buf);
		chain_free(&g_arch_chain);
		return NULL;
	}

	for (i = 0; i < nlines; i++) {
		const char *s = lines[i], *e;
		BrNode *node = root;
		for (;;) {
			e = strchr(s, '/');
			if (!e)
				break;
			if (e > s) {
				node = node_subdir(node, s, (size_t)(e - s));
				if (!node)
					break;
			}
			s = e + 1;
		}
		if (node && *s) {
			char *label = label_dup(s, strlen(s));
			if (label && !node_addfile(node, label, lines[i]))
				free(label);
		}
	}
	/* sorted pointers for category lookup */
	qsort(lines, nlines, sizeof(char *), cmp_str_ptr);
	arch_build_categories(root, lines, nlines);
	free(lines);
	g_arch_buf = buf;
	return root;
}

BrNode *BrData_ArchiveRoot(void)
{
	char ipath[BR_PATHBUF], cpath[BR_PATHBUF];
	long isize, imtime, csize, cmtime;

	br_path(ipath, sizeof(ipath), "index.txt");
	br_path(cpath, sizeof(cpath), "categories.json");
	stat_info(ipath, &isize, &imtime);
	stat_info(cpath, &csize, &cmtime);
	if (g_arch_root && isize == g_arch_idx_size && imtime == g_arch_idx_mtime &&
	    csize == g_arch_cat_size && cmtime == g_arch_cat_mtime)
		return g_arch_root;
	arch_invalidate();
	if (isize <= 0)
		return NULL;
	g_arch_root = arch_build();
	g_arch_idx_size = isize;
	g_arch_idx_mtime = imtime;
	g_arch_cat_size = csize;
	g_arch_cat_mtime = cmtime;
	return g_arch_root;
}

/* ------------------------------------------------------------------ */
/* Local tree                                                          */

static BrNode *local_walk(const char *dir, int depth)
{
	BrNode *node = node_new(&g_local_chain);
	DIR *d;
	struct dirent *de;

	if (!node)
		return NULL;
	node->own_labels = true;
	node->own_paths = true;
	node->absdir = br_strdup(dir);
	if (depth > 32 || !(d = opendir(dir)))
		return node;
	while ((de = readdir(d)) != NULL) {
		char full[BR_PATHBUF];
		struct stat st;
		if (de->d_name[0] == '.')
			continue;
		snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
		if (lstat(full, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			BrNode *sub = local_walk(full, depth + 1);
			if (sub) {
				if (node->ndirs == node->capdirs) {
					int cap = node->capdirs ? node->capdirs * 2 : 4;
					BrDirEnt *nd = realloc(node->dirs, cap * sizeof(*nd));
					if (!nd)
						continue;
					node->dirs = nd;
					node->capdirs = cap;
				}
				node->dirs[node->ndirs].name = br_strdup(de->d_name);
				node->dirs[node->ndirs].node = sub;
				node->ndirs++;
			}
		} else if (S_ISREG(st.st_mode) ||
		           (S_ISLNK(st.st_mode) && stat(full, &st) == 0 && S_ISREG(st.st_mode))) {
			char *lab = label_dup(de->d_name, strlen(de->d_name));
			char *path = br_strdup(full);
			if (!lab || !path || !node_addfile(node, lab, path)) {
				free(lab);
				free(path);
			}
		}
	}
	closedir(d);
	return node;
}

BrNode *BrData_LocalRoot(void)
{
	chain_free(&g_local_chain);
	return local_walk(g_cfg.local_dir, 0);
}

/* ------------------------------------------------------------------ */
/* Descriptions and text files                                         */

/** Convert raw text to ASCII; NULL for binary data. */
static char *text_from_raw(const unsigned char *raw, size_t n, bool truncated)
{
	size_t i, ctrl = 0;
	char *out;

	for (i = 0; i < n; i++) {
		unsigned char c = raw[i];
		if (c == 0)
			return NULL;
		if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != 0x1A)
			ctrl++;
	}
	if (n > 0 && ctrl * 20 > n)
		return NULL;
	out = malloc(4 * n + 8);
	if (!out)
		return NULL;
	text_to_ascii(raw, n, out, true, truncated);
	return out;
}

char *BrData_ReadTextFile(const char *abs_path)
{
	size_t len = 0;
	char *raw, *out;

	if (!abs_path)
		return NULL;
	raw = br_read_file(abs_path, BR_TEXT_MAX, &len);
	if (!raw)
		return NULL;
	out = text_from_raw((unsigned char *)raw, len, len >= BR_TEXT_MAX);
	free(raw);
	return out;
}

/** "Name (v1) [x]" -> "Name": drop all (...) and [...] groups with their leading blanks. */
static void strip_brackets(const char *in, char *out, size_t outsize)
{
	size_t o = 0;
	const char *p = in;

	while (*p && o + 1 < outsize) {
		if (*p == '(' || *p == '[') {
			const char *q = p + 1;
			while (*q && *q != ')' && *q != ']')
				q++;
			if (*q) {
				while (o > 0 && isspace((unsigned char)out[o - 1]))
					o--;
				p = q + 1;
				continue;
			}
		}
		out[o++] = *p++;
	}
	out[o] = 0;
	/* strip */
	while (o > 0 && isspace((unsigned char)out[o - 1]))
		out[--o] = 0;
	for (p = out; *p && isspace((unsigned char)*p); p++)
		;
	if (p != out)
		memmove(out, p, strlen(p) + 1);
}

char *BrData_Description(const char *path)
{
	static const char *const exts[] = { ".txt", ".nfo", ".diz", ".TXT", NULL };
	char full[BR_PATHBUF], dir[BR_PATHBUF], stem[BR_PATHBUF], cand[BR_PATHBUF];
	const char *slash, *dot;
	size_t stemlen;
	int i;

	if (!path || !path[0])
		return NULL;
	BrData_AbsPath(path, full, sizeof(full));
	slash = strrchr(full, '/');
	if (!slash)
		return NULL;
	snprintf(dir, sizeof(dir), "%.*s", (int)(slash - full), full);
	snprintf(stem, sizeof(stem), "%s", slash + 1);
	dot = strrchr(stem, '.');
	stemlen = dot && dot != stem ? (size_t)(dot - stem) : strlen(stem);
	stem[stemlen] = 0;

	for (i = 0; exts[i]; i++) {
		snprintf(cand, sizeof(cand), "%.2000s/%.1500s%s", dir, stem, exts[i]);
		if (br_file_exists(cand)) {
			char *t = BrData_ReadTextFile(cand);
			if (t)
				return t;
		}
	}

	/* GameBase: <base>/Games/<fmt>/<letter>/x.zip -> <base>/Extras/Instructions/<letter>/x.txt */
	if (path[0] != '/') {
		char rel[BR_PATHBUF];
		const char *segs[64];
		size_t seglen[64];
		int nseg = 0, gi = -1;
		const char *s = path;

		snprintf(rel, sizeof(rel), "%s", path);
		while (*s && nseg < 64) {
			const char *e = strchr(s, '/');
			segs[nseg] = s;
			seglen[nseg] = e ? (size_t)(e - s) : strlen(s);
			nseg++;
			if (!e)
				break;
			s = e + 1;
		}
		for (i = nseg - 1; i >= 0; i--)
			if (seglen[i] == 5 && strncmp(segs[i], "Games", 5) == 0) {
				gi = i;
				break;
			}
		if (gi >= 0 && nseg >= gi + 4) {
			char base[BR_PATHBUF], variant[2][BR_PATHBUF];
			int v;
			size_t used = 0;

			snprintf(base, sizeof(base), "%s", g_cfg.archive_root);
			used = strlen(base);
			for (i = 0; i < gi; i++)
				used += snprintf(base + used, sizeof(base) - used < 1 ? 1 : sizeof(base) - used,
				                 "/%.*s", (int)seglen[i], segs[i]);
			snprintf(variant[0], sizeof(variant[0]), "%s", stem);
			strip_brackets(stem, variant[1], sizeof(variant[1]));
			for (v = 0; v < 2; v++) {
				if (v == 1 && (variant[1][0] == 0 || strcmp(variant[0], variant[1]) == 0))
					break;
				snprintf(cand, sizeof(cand), "%.2000s/Extras/Instructions/%.*s/%.1500s.txt",
				         base, (int)seglen[gi + 2], segs[gi + 2], variant[v]);
				if (br_file_exists(cand)) {
					char *t = BrData_ReadTextFile(cand);
					if (t)
						return t;
				}
			}
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* State                                                               */

void BrData_SaveState(const char *const *labels, int nlabels, const char *selected)
{
	char path[BR_PATHBUF];
	FILE *fp;
	int i;

	if (!br_mkdirs(cfg_dir()))
		return;
	br_path(path, sizeof(path), "state.txt");
	fp = fopen(path, "w");
	if (!fp)
		return;
	for (i = 0; i < nlabels; i++)
		if (labels[i])
			fprintf(fp, "D %s\n", labels[i]);
	if (selected)
		fprintf(fp, "S %s\n", selected);
	fclose(fp);
}

int BrData_LoadState(char labels[][200], int maxlabels, char *selected, size_t selsize)
{
	char path[BR_PATHBUF], line[1024];
	FILE *fp;
	int n = 0;

	if (selected && selsize)
		selected[0] = 0;
	br_path(path, sizeof(path), "state.txt");
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		size_t l = strlen(line);
		while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
			line[--l] = 0;
		if (l < 2 || line[1] != ' ')
			continue;
		if (line[0] == 'D' && n < maxlabels)
			snprintf(labels[n++], 200, "%.199s", line + 2);
		else if (line[0] == 'S' && selected && selsize)
			snprintf(selected, selsize, "%s", line + 2);
	}
	fclose(fp);
	return n;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */

static void import_legacy(const char *name)
{
	const char *home = getenv("HOME");
	char dst[BR_PATHBUF], src[BR_PATHBUF];

	br_path(dst, sizeof(dst), name);
	if (br_file_exists(dst) || !home)
		return;
	snprintf(src, sizeof(src), "%s/.config/SeekexecuTe/%s", home, name);
	if (br_file_exists(src))
		br_copy_file(src, dst);
}

bool BrData_Init(void)
{
	if (!br_mkdirs(cfg_dir()))
		return false;
	if (!g_inited) {
		g_inited = true;
		settings_defaults();
		settings_load();
		import_legacy("favorites.txt");
		import_legacy("categories.json");
		favs_load();
	}
	br_mkdirs(g_cfg.local_dir);
	return true;
}
