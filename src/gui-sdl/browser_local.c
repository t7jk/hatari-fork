/*
  Hatari - browser_local.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Disk-image Browser: local copies of disks (copy, zip extraction, delete)
  and inserting disks into the running emulator. No SDL dialogs here.
*/
const char BrowserLocal_fileid[] = "Hatari browser_local.c";

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#if HAVE_LIBZ
#include <zlib.h>
#endif

#include "browser.h"

/* Declared here (same as floppy.h) so that this file needs no Hatari core headers */
extern const char *Floppy_SetDiskFileName(int Drive, const char *pszFileName, const char *pszZipPath);

#define BRL_CHUNK 65536

/* ------------------------------------------------------------------ */

static void set_err(char *err, size_t errsize, const char *fmt, const char *arg)
{
	if (err && errsize)
		snprintf(err, errsize, fmt, arg ? arg : "");
}

static const char *base_name(const char *path)
{
	const char *p = strrchr(path, '/');
	const char *q = strrchr(path, '\\');
	if (q && (!p || q > p))
		p = q;
	return p ? p + 1 : path;
}

static bool has_ext_ci(const char *path, const char *ext)
{
	size_t l = strlen(path), e = strlen(ext);
	return l >= e && strcasecmp(path + l - e, ext) == 0;
}

/** Is the file a disk image we can insert (.st .msa .stx .dim .zip) */
bool BrLocal_IsDisk(const char *path)
{
	static const char *exts[] = { ".st", ".msa", ".stx", ".dim", ".zip" };
	size_t i;

	if (!path)
		return false;
	for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
	{
		if (has_ext_ci(path, exts[i]))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Game name, equivalent of the regex
 *  [\s_-]*[\(\[]?\s*(disk|disc|side)\s*[0-9a-z]\b[^\)\]]*[\)\]]?\s*$  (case-insens.) */

static bool is_ws(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static bool is_word(int c) { return isalnum((unsigned char)c) || c == '_'; }

/** Try to match the "disk N" suffix pattern starting exactly at s[i]; true if it runs to the end */
static bool match_disk_suffix(const char *s, size_t i)
{
	size_t k = i;
	size_t j;
	static const char *kw[] = { "disk", "disc", "side" };
	bool found = false;

	while (s[k] && (is_ws(s[k]) || s[k] == '_' || s[k] == '-'))
		k++;
	if (s[k] == '(' || s[k] == '[')
		k++;
	while (s[k] && is_ws(s[k]))
		k++;
	for (j = 0; j < 3; j++)
	{
		if (strncasecmp(s + k, kw[j], 4) == 0)
		{
			found = true;
			break;
		}
	}
	if (!found)
		return false;
	k += 4;
	while (s[k] && is_ws(s[k]))
		k++;
	if (!isalnum((unsigned char)s[k]))
		return false;
	k++;
	if (is_word(s[k]))		/* \b */
		return false;
	/* [^\)\]]* then optional closing bracket then \s*$ */
	while (s[k] && s[k] != ')' && s[k] != ']')
		k++;
	if (s[k] == ')' || s[k] == ']')
		k++;
	while (s[k] && is_ws(s[k]))
		k++;
	return s[k] == '\0';
}

/** Name of the game folder derived from the first disk name */
void BrLocal_GameName(const char *rel, char *out, size_t outsize)
{
	char stem[512];
	const char *b;
	char *dot;
	size_t i, len, start;

	if (!out || !outsize)
		return;
	b = base_name(rel ? rel : "");
	snprintf(stem, sizeof(stem), "%s", b);
	dot = strrchr(stem, '.');
	if (dot && dot != stem)			/* os.path.splitext ignores leading dots */
	{
		char *q = stem;
		while (*q == '.')
			q++;
		if (dot >= q)
			*dot = '\0';
	}
	for (i = 0; stem[i]; i++)
	{
		if (match_disk_suffix(stem, i))
		{
			stem[i] = '\0';
			break;
		}
	}
	for (i = 0; stem[i]; i++)
	{
		if (strchr("/\\:*?\"<>|", stem[i]))
			stem[i] = '_';
	}
	/* strip() whitespace, then strip(" .") */
	start = 0;
	len = strlen(stem);
	while (len > 0 && is_ws(stem[len - 1]))
		len--;
	while (start < len && is_ws(stem[start]))
		start++;
	while (len > start && (stem[len - 1] == ' ' || stem[len - 1] == '.'))
		len--;
	while (start < len && (stem[start] == ' ' || stem[start] == '.'))
		start++;
	stem[len] = '\0';
	if (start >= len)
		snprintf(out, outsize, "game");
	else
		snprintf(out, outsize, "%s", stem + start);
}

/* ------------------------------------------------------------------ */
/* File helpers */

static bool exists_any(const char *path)
{
	struct stat st;
	return lstat(path, &st) == 0;
}

static bool mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	char *p;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return false;
	for (p = tmp + 1; *p; p++)
	{
		if (*p == '/')
		{
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return false;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return false;
	return true;
}

/** dst = gdir/name, or gdir/N_name (N = 2, 3, ...) if it already exists */
static void unique_dst(const char *gdir, const char *name, char *dst, size_t dstsize)
{
	int n = 1;
	snprintf(dst, dstsize, "%s/%s", gdir, name);
	while (exists_any(dst))
	{
		n++;
		snprintf(dst, dstsize, "%s/%d_%s", gdir, n, name);
	}
}

static bool copy_file(const char *src, const char *dst)
{
	FILE *fi, *fo;
	char *buf;
	size_t n;
	bool ok = true;

	fi = fopen(src, "rb");
	if (!fi)
		return false;
	fo = fopen(dst, "wb");
	if (!fo)
	{
		fclose(fi);
		return false;
	}
	buf = malloc(BRL_CHUNK);
	if (!buf)
	{
		fclose(fi);
		fclose(fo);
		return false;
	}
	while ((n = fread(buf, 1, BRL_CHUNK, fi)) > 0)
	{
		if (fwrite(buf, 1, n, fo) != n)
		{
			ok = false;
			break;
		}
	}
	if (ferror(fi))
		ok = false;
	free(buf);
	fclose(fi);
	if (fclose(fo) != 0)
		ok = false;
	if (!ok)
		remove(dst);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Minimal zip reader (stored + deflate) on top of zlib */

#if HAVE_LIBZ

typedef struct {
	char     *name;
	uint32_t  csize, usize, crc, lho;
	uint16_t  method, flags;
} ZipEnt;

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int zipent_cmp(const void *a, const void *b)
{
	return strcasecmp(((const ZipEnt *)a)->name, ((const ZipEnt *)b)->name);
}

/** Make a safe flat file name from a zip entry name; false if nothing usable */
static bool safe_name(const char *entry, char *out, size_t outsize)
{
	const char *b = base_name(entry);
	size_t i;

	if (!*b || strcmp(b, ".") == 0 || strcmp(b, "..") == 0)
		return false;
	snprintf(out, outsize, "%s", b);
	for (i = 0; out[i]; i++)
	{
		if ((unsigned char)out[i] < 32)
			out[i] = '_';
	}
	return true;
}

/** Inflate/copy one entry to dst. */
static bool zip_extract_entry(FILE *fp, const ZipEnt *e, const char *dst)
{
	unsigned char lh[30];
	unsigned char *in, *out = NULL;
	FILE *fo;
	z_stream zs;
	long off;
	int rc;
	bool ok = false;
	uint32_t crc;

	if (fseek(fp, (long)e->lho, SEEK_SET) != 0 || fread(lh, 1, 30, fp) != 30 || rd32(lh) != 0x04034b50)
		return false;
	off = (long)e->lho + 30 + rd16(lh + 26) + rd16(lh + 28);
	if (fseek(fp, off, SEEK_SET) != 0)
		return false;
	in = malloc(e->csize ? e->csize : 1);
	if (!in)
		return false;
	if (e->csize && fread(in, 1, e->csize, fp) != e->csize)
	{
		free(in);
		return false;
	}
	out = malloc(e->usize ? e->usize : 1);
	if (!out)
	{
		free(in);
		return false;
	}
	if (e->method == 0)
	{
		if (e->csize != e->usize)
			goto done;
		memcpy(out, in, e->usize);
	}
	else if (e->method == 8)
	{
		memset(&zs, 0, sizeof(zs));
		if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
			goto done;
		zs.next_in = in;
		zs.avail_in = e->csize;
		zs.next_out = out;
		zs.avail_out = e->usize;
		rc = inflate(&zs, Z_FINISH);
		inflateEnd(&zs);
		if (rc != Z_STREAM_END || zs.total_out != e->usize)
			goto done;
	}
	else
		goto done;
	crc = (uint32_t)crc32(0L, out, e->usize);
	if (crc != e->crc)
		goto done;
	fo = fopen(dst, "wb");
	if (!fo)
		goto done;
	ok = fwrite(out, 1, e->usize, fo) == e->usize;
	if (fclose(fo) != 0)
		ok = false;
	if (!ok)
		remove(dst);
done:
	free(in);
	free(out);
	return ok;
}

/** Extract all files of a zip (flattened) into gdir; appends paths to *files. */
static bool zip_extract(const char *zipfile, const char *gdir, char ***files, int *nfiles,
                        char *err, size_t errsize)
{
	FILE *fp = fopen(zipfile, "rb");
	unsigned char *tail = NULL, *cd = NULL;
	long fsize, tlen, i;
	long eocd = -1;
	uint32_t cdsize, cdoff;
	uint16_t count;
	ZipEnt *ents = NULL;
	int nents = 0, k;
	size_t pos;
	bool ok = false;

	if (!fp)
	{
		set_err(err, errsize, "Cannot open %s", zipfile);
		return false;
	}
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	tlen = fsize < 70000 ? fsize : 70000;
	tail = malloc(tlen > 0 ? tlen : 1);
	if (!tail || fsize < 22 || fseek(fp, fsize - tlen, SEEK_SET) != 0
	    || fread(tail, 1, tlen, fp) != (size_t)tlen)
		goto bad;
	for (i = tlen - 22; i >= 0; i--)
	{
		if (rd32(tail + i) == 0x06054b50)
		{
			eocd = i;
			break;
		}
	}
	if (eocd < 0)
		goto bad;
	count = rd16(tail + eocd + 10);
	cdsize = rd32(tail + eocd + 12);
	cdoff = rd32(tail + eocd + 16);
	if (count == 0xFFFF || cdsize == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu
	    || (long)cdoff + (long)cdsize > fsize)
		goto bad;
	cd = malloc(cdsize ? cdsize : 1);
	ents = calloc(count ? count : 1, sizeof(ZipEnt));
	if (!cd || !ents || fseek(fp, (long)cdoff, SEEK_SET) != 0
	    || fread(cd, 1, cdsize, fp) != cdsize)
		goto bad;
	pos = 0;
	for (k = 0; k < count; k++)
	{
		uint16_t nl, xl, cl;
		char tmp[512];
		ZipEnt *e;

		if (pos + 46 > cdsize || rd32(cd + pos) != 0x02014b50)
			goto bad;
		nl = rd16(cd + pos + 28);
		xl = rd16(cd + pos + 30);
		cl = rd16(cd + pos + 32);
		if (pos + 46 + nl + xl + cl > cdsize)
			goto bad;
		/* skip directories and unusable names */
		if (nl > 0 && cd[pos + 46 + nl - 1] != '/' && nl < sizeof(tmp))
		{
			memcpy(tmp, cd + pos + 46, nl);
			tmp[nl] = '\0';
			e = &ents[nents];
			e->name = strdup(tmp);
			e->flags = rd16(cd + pos + 8);
			e->method = rd16(cd + pos + 10);
			e->crc = rd32(cd + pos + 16);
			e->csize = rd32(cd + pos + 20);
			e->usize = rd32(cd + pos + 24);
			e->lho = rd32(cd + pos + 42);
			if (!e->name)
				goto bad;
			nents++;
		}
		pos += 46 + nl + xl + cl;
	}
	qsort(ents, nents, sizeof(ZipEnt), zipent_cmp);
	for (k = 0; k < nents; k++)
	{
		char name[512], dst[PATH_MAX], **nf;

		if (!safe_name(ents[k].name, name, sizeof(name)))
			continue;
		if (ents[k].flags & 1)
		{
			set_err(err, errsize, "Encrypted zip entry: %s", ents[k].name);
			goto fail;
		}
		unique_dst(gdir, name, dst, sizeof(dst));
		if (!zip_extract_entry(fp, &ents[k], dst))
		{
			set_err(err, errsize, "Cannot extract %s", ents[k].name);
			goto fail;
		}
		nf = realloc(*files, (*nfiles + 1) * sizeof(char *));
		if (!nf)
			goto fail;
		*files = nf;
		(*files)[*nfiles] = strdup(dst);
		if (!(*files)[*nfiles])
			goto fail;
		(*nfiles)++;
	}
	ok = true;
	goto fail;
bad:
	set_err(err, errsize, "Bad or unsupported zip file: %s", zipfile);
fail:
	for (k = 0; k < nents; k++)
		free(ents[k].name);
	free(ents);
	free(cd);
	free(tail);
	fclose(fp);
	return ok;
}

#else  /* !HAVE_LIBZ */

static bool zip_extract(const char *zipfile, const char *gdir, char ***files, int *nfiles,
                        char *err, size_t errsize)
{
	(void)gdir; (void)files; (void)nfiles;
	set_err(err, errsize, "No zlib support, cannot unpack %s", zipfile);
	return false;
}

#endif

/* ------------------------------------------------------------------ */

/** Recursively remove path; symlinks are removed, never followed */
static void rm_tree(const char *path)
{
	struct stat st;

	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode))
	{
		DIR *d = opendir(path);
		struct dirent *de;
		if (d)
		{
			while ((de = readdir(d)) != NULL)
			{
				char sub[PATH_MAX];
				if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
					continue;
				if (snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name) >= (int)sizeof(sub))
					continue;
				rm_tree(sub);
			}
			closedir(d);
		}
		rmdir(path);
	}
	else
		remove(path);
}

void BrLocal_FreeList(char **files, int n)
{
	int i;
	if (!files)
		return;
	for (i = 0; i < n; i++)
		free(files[i]);
	free(files);
}

/**
 * Copy files (archive-relative paths) into <local_dir>/<game name>/ ; zips are extracted.
 */
bool BrLocal_Copy(const char *const *rels, int n, char *out_dir, size_t dirsize,
                  char ***out_files, int *out_nfiles, char *err, size_t errsize,
                  bool (*progress)(int done, int total))
{
	char base[256], gdir[PATH_MAX], src[PATH_MAX], dst[PATH_MAX];
	const char *local = BrData_Settings()->local_dir;
	char **files = NULL;
	int nfiles = 0, i, cnt = 2;

	if (out_files)
		*out_files = NULL;
	if (out_nfiles)
		*out_nfiles = 0;
	if (n <= 0 || !rels || !local[0])
	{
		set_err(err, errsize, "Nothing to copy%s", "");
		return false;
	}
	BrLocal_GameName(rels[0], base, sizeof(base));
	if (!mkdir_p(local))
	{
		set_err(err, errsize, "Cannot create %s", local);
		return false;
	}
	snprintf(gdir, sizeof(gdir), "%s/%s", local, base);
	while (exists_any(gdir))	/* earlier copies stay untouched */
		snprintf(gdir, sizeof(gdir), "%s/%s (%d)", local, base, cnt++);
	if (mkdir(gdir, 0755) != 0)
	{
		set_err(err, errsize, "Cannot create %s", gdir);
		return false;
	}
	for (i = 0; i < n; i++)
	{
		BrData_AbsPath(rels[i], src, sizeof(src));
		if (has_ext_ci(src, ".zip"))
		{
			if (!zip_extract(src, gdir, &files, &nfiles, err, errsize))
				goto fail;
		}
		else
		{
			char **nf;
			unique_dst(gdir, base_name(src), dst, sizeof(dst));
			if (!copy_file(src, dst))
			{
				set_err(err, errsize, "Cannot copy %s", src);
				goto fail;
			}
			nf = realloc(files, (nfiles + 1) * sizeof(char *));
			if (!nf)
				goto fail;
			files = nf;
			files[nfiles] = strdup(dst);
			if (!files[nfiles])
				goto fail;
			nfiles++;
		}
		if (progress && !progress(i + 1, n))
		{
			set_err(err, errsize, "Aborted%s", "");
			goto fail;
		}
	}
	if (out_dir && dirsize)
		snprintf(out_dir, dirsize, "%s", gdir);
	if (out_files)
		*out_files = files;
	else
		BrLocal_FreeList(files, nfiles);
	if (out_nfiles)
		*out_nfiles = nfiles;
	return true;

fail:
	BrLocal_FreeList(files, nfiles);
	rm_tree(gdir);
	return false;
}

/* ------------------------------------------------------------------ */

/** Is realpath'ed p strictly inside realpath'ed root */
static bool inside_root(const char *root_real, const char *p_real)
{
	size_t rl = strlen(root_real);
	if (rl > 1 && root_real[rl - 1] == '/')
		rl--;
	return strncmp(p_real, root_real, rl) == 0 && p_real[rl] == '/' && p_real[rl + 1] != '\0';
}

/**
 * Delete a file/folder, ONLY if it is inside local_dir; removes empty parents up to local_dir.
 */
bool BrLocal_Delete(const char *abs_path)
{
	char root[PATH_MAX], real[PATH_MAX], parent[PATH_MAX];
	struct stat st;
	char *slash;

	if (!abs_path || !*abs_path || !BrData_Settings()->local_dir[0])
		return false;
	if (!realpath(BrData_Settings()->local_dir, root))
		return false;
	if (!realpath(abs_path, real) || !inside_root(root, real))
		return false;
	if (lstat(abs_path, &st) != 0)
		return false;
	if (S_ISDIR(st.st_mode))
		rm_tree(real);
	else if (remove(abs_path) != 0)	/* file, or symlink pointing inside local_dir */
		return false;
	/* clean up empty parents (only empty ones, up to local_dir) */
	snprintf(parent, sizeof(parent), "%s", real);
	while ((slash = strrchr(parent, '/')) != NULL && slash != parent)
	{
		*slash = '\0';
		if (!inside_root(root, parent) || rmdir(parent) != 0)
			break;
	}
	return !exists_any(abs_path);
}

/**
 * Insert disks into the running emulator: 1 -> A:, 2 -> A: and B:, >2 -> A: only (first).
 */
int BrLocal_Insert(char *const *abs_paths, int n)
{
	const char *disks[3];
	int nd = 0, i, inserted = 0;

	if (!abs_paths)
		return 0;
	for (i = 0; i < n && nd < 3; i++)
	{
		if (abs_paths[i] && BrLocal_IsDisk(abs_paths[i]))
			disks[nd++] = abs_paths[i];
	}
	if (nd > 2)
		nd = 1;
	for (i = 0; i < nd; i++)
	{
		Floppy_SetDiskFileName(i, disks[i], NULL);
		inserted++;
	}
	return inserted;
}
