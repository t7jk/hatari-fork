/*
  Standalone test of src/gui-sdl/browser_data.c (no SDL, no Hatari build).

  Build and run (from hatari-src):
    gcc -Wall -Wextra -std=gnu99 -Isrc/includes tests/browser/test_data.c \
        src/gui-sdl/browser_data.c -o test_data
    ./test_data <scratch dir> [index.txt] [categories.json]

  Defaults for the real-data input: ~/.cache/SeekexecuTe/index.txt and
  ~/.config/SeekexecuTe/categories.json. HOME is redirected to <scratch dir>/home.
*/
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <malloc.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "browser.h"
#include "paths.h"

static char g_home[512], g_hatari[600];

const char *Paths_GetHatariHome(void) { return g_hatari; }

static int g_fail, g_ok;
#define CHECK(c) do { if (c) g_ok++; else { g_fail++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void sh(const char *fmt, ...)
{
	char cmd[8192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	if (system(cmd) != 0)
		printf("(command failed: %s)\n", cmd);
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void write_file(const char *path, const void *data, size_t n)
{
	FILE *fp = fopen(path, "wb");
	if (fp) {
		fwrite(data, 1, n, fp);
		fclose(fp);
	}
}

/* Recursively count file items, check sorting/grouping invariants. */
static long walk_files(BrNode *n, int depth, long *dirs, long *virtgroups)
{
	int cnt, i, nfiles = 0, ndirs_seen = 0;
	const BrItem *it = BrNode_Items(n, &cnt);
	long total = 0;
	int last_dir = -1, last_file = -1;
	char path[4096];

	for (i = 0; i < cnt; i++) {
		if (it[i].kind == BR_DIR) {
			CHECK(last_file < 0);		/* dirs first */
			CHECK(it[i].sub != NULL);
			if (it[i].virt)
				(*virtgroups)++;
			(*dirs)++;
			ndirs_seen++;
			if (last_dir >= 0 && depth > 0 && !it[last_dir].virt && !it[i].virt &&
			    !(depth == 1))
				CHECK(strcasecmp(it[last_dir].label, it[i].label) <= 0);
			last_dir = i;
			CHECK(!BrNode_ItemPath(n, &it[i], path, sizeof(path)));
			total += walk_files(it[i].sub, depth + 1, dirs, virtgroups);
		} else {
			nfiles++;
			if (last_file >= 0)
				CHECK(strcasecmp(it[last_file].label, it[i].label) <= 0);
			last_file = i;
			CHECK(BrNode_ItemPath(n, &it[i], path, sizeof(path)));
			total++;
		}
	}
	if (nfiles > 30) {
		/* only acceptable when the names cannot be split by a prefix of <= 8 chars */
		char first[9], k8[9];
		int k;
		bool same = true;
		for (k = 0; k < cnt; k++) {
			if (it[k].kind != BR_FILE)
				continue;
			if (!isalpha((unsigned char)it[k].label[0]))
				strcpy(k8, "0-9");
			else
				snprintf(k8, sizeof(k8), "%.8s", it[k].label);
			for (int j = 0; k8[j] && k8[j] != '-'; j++)
				k8[j] = (char)toupper((unsigned char)k8[j]);
			if (k == nfiles * 0 + (cnt - nfiles))
				strcpy(first, k8);
			else if (strcmp(first, k8) != 0)
				same = false;
		}
		if (!same)
			printf("unsplit folder with %d files, first item '%s'\n", nfiles, it[cnt - nfiles].label);
		CHECK(same);
	}
	(void)ndirs_seen;
	return total;
}

static long count_lines(const char *path)
{
	FILE *fp = fopen(path, "r");
	char line[8192];
	long n = 0;
	if (!fp)
		return -1;
	while (fgets(line, sizeof(line), fp))
		if (line[0] != '\n')
			n++;
	fclose(fp);
	return n;
}

static bool g_abort_after;
static int g_progress_calls, g_last_found;
static bool progress_cb(int found)
{
	g_progress_calls++;
	g_last_found = found;
	return !(g_abort_after && found >= 3);
}

int main(int argc, char **argv)
{
	char scratch[400], idx_src[1024], cat_src[1024], p[4096], buf[4096];
	const char *env_home = getenv("HOME");
	BrNode *root, *n2;
	const BrItem *it;
	int cnt, i;
	double t0;
	long total, dirs, virt, lines;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <scratch dir> [index.txt] [categories.json]\n", argv[0]);
		return 2;
	}
	snprintf(scratch, sizeof(scratch), "%.399s", argv[1]);
	snprintf(idx_src, sizeof(idx_src), "%.1000s", argc > 2 ? argv[2] : "");
	snprintf(cat_src, sizeof(cat_src), "%.1000s", argc > 3 ? argv[3] : "");
	if (!idx_src[0])
		snprintf(idx_src, sizeof(idx_src), "%s/.cache/SeekexecuTe/index.txt", env_home);
	if (!cat_src[0])
		snprintf(cat_src, sizeof(cat_src), "%s/.config/SeekexecuTe/categories.json", env_home);

	sh("rm -rf '%s/home' '%s/arch' && mkdir -p '%s/home/.config/SeekexecuTe' "
	   "'%s/home/.config/hatari/browser'", scratch, scratch, scratch, scratch);
	snprintf(g_home, sizeof(g_home), "%s/home", scratch);
	snprintf(g_hatari, sizeof(g_hatari), "%s/home/.config/hatari", scratch);
	setenv("HOME", g_home, 1);
	sh("cp '%s' '%s/browser/index.txt'", idx_src, g_hatari);
	sh("cp '%s' '%s/.config/SeekexecuTe/categories.json'", cat_src, g_home);
	sh("printf 'GameBase/x/Fav One.zip\\nD-BUG - Menus/Menus/Dbug199b.msa\\n' > "
	   "'%s/.config/SeekexecuTe/favorites.txt'", g_home);

	/* ---- Init / settings / legacy import */
	CHECK(BrData_Init());
	CHECK(BrData_Init());		/* idempotent */
	snprintf(p, sizeof(p), "%s/browser/favorites.txt", g_hatari);
	CHECK(access(p, R_OK) == 0);
	snprintf(p, sizeof(p), "%s/browser/categories.json", g_hatari);
	CHECK(access(p, R_OK) == 0);
	CHECK(BrData_FavCount() == 2);
	CHECK(BrData_IsFav("D-BUG - Menus/Menus/Dbug199b.msa"));
	CHECK(BrData_Settings()->copy_local);
	snprintf(p, sizeof(p), "%s/browser/local", g_hatari);
	CHECK(strcmp(BrData_Settings()->local_dir, p) == 0);
	CHECK(access(p, R_OK) == 0);
	snprintf(BrData_Settings()->archive_root, 1024, "%s/arch", scratch);
	BrData_Settings()->copy_local = false;
	CHECK(BrData_SaveSettings());
	snprintf(p, sizeof(p), "%s/browser/browser.cfg", g_hatari);
	{
		char *t = BrData_ReadTextFile(p);
		CHECK(t && strstr(t, "copy_local=0") && strstr(t, "/arch"));
		free(t);
	}

	/* ---- Real index: tree */
	CHECK(BrData_HaveIndex());
	snprintf(p, sizeof(p), "%s/browser/index.txt", g_hatari);
	lines = count_lines(p);
	t0 = now();
	root = BrData_ArchiveRoot();
	printf("tree build: %.3f s for %ld lines\n", now() - t0, lines);
	CHECK(root != NULL);
	CHECK(now() - t0 < 1.0);
	t0 = now();
	CHECK(BrData_ArchiveRoot() == root);
	CHECK(now() - t0 < 0.01);
	it = BrNode_Items(root, &cnt);
	CHECK(cnt > 0);
	CHECK(it[0].kind == BR_DIR && strcmp(it[0].label, "BY CATEGORY") == 0);
	printf("root items: %d, first '%s' (%d)\n", cnt, it[0].label, it[0].count);
	for (i = 0; i < cnt; i++)
		printf("  %-50s %6d\n", it[i].label, it[i].kind == BR_DIR ? it[i].count : 0);

	/* categories */
	{
		static const char *want[] = { "Fire And Destroy", "Jump And Run",
			"Point And Click Adventures", "Strategy", "Text Adventure" };
		int ci, ccnt;
		const BrItem *cit = BrNode_Items(it[0].sub, &ccnt);
		printf("categories: %d\n", ccnt);
		CHECK(ccnt == 5);
		for (ci = 0; ci < ccnt && ci < 5; ci++) {
			CHECK(strcmp(cit[ci].label, want[ci]) == 0);
			printf("  %-30s %5d\n", cit[ci].label, cit[ci].count);
			walk_files(cit[ci].sub, 1, &dirs, &virt);
		}
		/* a file in a category resolves to a path present in the index */
		{
			int fc;
			const BrItem *fi = BrNode_Items(cit[0].sub, &fc);
			int k = 0;
			while (k < fc && fi[k].kind == BR_DIR)
				k++;
			if (k < fc) {
				CHECK(BrNode_ItemPath(cit[0].sub, &fi[k], buf, sizeof(buf)));
				printf("  sample category entry: '%s' -> %s\n", fi[k].label, buf);
				CHECK(strchr(buf, '/') != NULL);
			}
		}
	}

	/* the rest of the tree: all files accounted for, invariants hold */
	dirs = virt = total = 0;
	for (i = 1; i < cnt; i++) {
		if (it[i].kind == BR_FILE)
			total++;
		else
			total += walk_files(it[i].sub, 1, &dirs, &virt);
	}
	printf("files in tree (without categories): %ld of %ld, dir nodes %ld, letter groups %ld\n",
	       total, lines, dirs, virt);
	CHECK(total == lines);

	/* a big folder should be split into letter groups */
	{
		const BrItem *g = it[1].kind == BR_DIR ? &it[1] : NULL;
		int depth = 0;
		BrNode *cur = root;
		while (g && depth < 12) {
			int gc, k;
			const BrItem *gi = BrNode_Items(g->sub, &gc);
			bool any_virt = false;
			for (k = 0; k < gc; k++)
				if (gi[k].virt)
					any_virt = true;
			if (any_virt) {
				printf("letter groups at depth %d under '%s':", depth, g->label);
				for (k = 0; k < gc && k < 40; k++)
					printf(" %s(%d)", gi[k].label, gi[k].count);
				printf("\n");
				CHECK(gc >= 2);
				/* group item resolves a file with a path */
				{
					int fc;
					const BrItem *fi = BrNode_Items(gi[0].sub, &fc);
					CHECK(fc > 0);
					if (fc > 0 && fi[fc - 1].kind == BR_FILE)
						CHECK(BrNode_ItemPath(gi[0].sub, &fi[fc - 1], buf, sizeof(buf)));
				}
				break;
			}
			/* descend into the largest dir */
			{
				int best = -1;
				for (k = 0; k < gc; k++)
					if (gi[k].kind == BR_DIR && (best < 0 || gi[k].count > gi[best].count))
						best = k;
				if (best < 0)
					break;
				cur = g->sub;
				g = &gi[best];
			}
			depth++;
		}
		CHECK(g != NULL && depth < 12);
		(void)cur;
	}

	/* ---- repeated calls (leak check is done with ASan/LSan externally) */
	for (i = 0; i < 100; i++) {
		BrNode *l = BrData_LocalRoot();
		BrNode *f = BrData_FavRoot();
		int k;
		CHECK(l && f);
		BrNode_Items(f, &k);
		CHECK(k == 2);
	}

	/* ---- favorites */
	CHECK(BrData_ToggleFav("A/b.st") == true);
	CHECK(BrData_FavCount() == 3 && BrData_IsFav("A/b.st"));
	CHECK(strcmp(BrData_FavGet(2), "A/b.st") == 0);
	CHECK(BrData_ToggleFav("A/b.st") == false);
	CHECK(BrData_FavCount() == 2 && !BrData_IsFav("A/b.st"));
	CHECK(BrData_ToggleFav("GameBase/x/Fav One.zip") == false);
	CHECK(BrData_FavCount() == 1);
	{
		char fp[4096];
		char *t;
		snprintf(fp, sizeof(fp), "%s/browser/favorites.txt", g_hatari);
		t = BrData_ReadTextFile(fp);
		CHECK(t && strcmp(t, "D-BUG - Menus/Menus/Dbug199b.msa\n") == 0);
		free(t);
	}
	n2 = BrData_FavRoot();
	it = BrNode_Items(n2, &cnt);
	CHECK(cnt == 1 && it[0].kind == BR_FILE && strcmp(it[0].label, "Dbug199b.msa") == 0);
	CHECK(BrNode_ItemPath(n2, &it[0], buf, sizeof(buf)) &&
	      strcmp(buf, "D-BUG - Menus/Menus/Dbug199b.msa") == 0);
	BrData_AbsPath(buf, p, sizeof(p));
	CHECK(strstr(p, "/arch/D-BUG - Menus") != NULL);
	BrData_AbsPath("/abs/x.st", p, sizeof(p));
	CHECK(strcmp(p, "/abs/x.st") == 0);

	/* ---- state */
	{
		const char *labs[] = { "GameBase/GamebaseST 3.1d", "Games", "A" };
		char out[8][200], sel[200];
		CHECK(BrData_LoadState(out, 8, sel, sizeof(sel)) == 0 && sel[0] == 0);
		BrData_SaveState(labs, 3, "Air Support.zip");
		CHECK(BrData_LoadState(out, 8, sel, sizeof(sel)) == 3);
		CHECK(strcmp(out[0], labs[0]) == 0 && strcmp(out[2], "A") == 0);
		CHECK(strcmp(sel, "Air Support.zip") == 0);
		CHECK(BrData_LoadState(out, 2, sel, sizeof(sel)) == 2);
	}

	/* ---- fake archive: rescan, descriptions */
	sh("cd '%s' && mkdir -p 'arch/Games/GBST/Games/ST/A' 'arch/Games/GBST/Extras/Instructions/A' "
	   "'arch/Games/GBST/Extras/Boxscans' 'arch/Music/X' 'arch/Top' 'arch/Top/Sub/UTILS' "
	   "'arch/Top/Sub/Deep' && "
	   "touch 'arch/Games/GBST/Games/ST/A/Aliens (v1)(De).zip' 'arch/Games/GBST/Games/ST/A/Zork.ST' "
	   "'arch/Games/GBST/Extras/Boxscans/skip.st' 'arch/Music/X/skip.st' 'arch/Top/a.MSA' "
	   "'arch/Top/b.stx' 'arch/Top/c.dim' 'arch/Top/d.txt' 'arch/Top/Sub/UTILS/skip.st' "
	   "'arch/Top/Sub/Deep/e.st'", scratch);
	{
		/* description: cp1250 text, UTF-8 text, binary */
		static const unsigned char cp1250[] = "Gra \xb9\xe6\xea\xb3\xf1\xf3\x9c\x9f\xbf tab\there\r\nline2\r\n";
		static const unsigned char binary[] = { 'a', 0, 'b', 1, 2, 3 };
		static const char utf8[] = "Za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 \xc3\xa9 \xe2\x80\x94 \xe2\x80\xa6\n";
		char f[4096];
		char *t;

		snprintf(f, sizeof(f), "%s/arch/Games/GBST/Extras/Instructions/A/Aliens.txt", scratch);
		write_file(f, cp1250, sizeof(cp1250) - 1);
		snprintf(f, sizeof(f), "%s/arch/Games/GBST/Games/ST/A/Zork.txt", scratch);
		write_file(f, utf8, sizeof(utf8) - 1);
		snprintf(f, sizeof(f), "%s/arch/Top/a.nfo", scratch);
		write_file(f, binary, sizeof(binary));

		t = BrData_Description("Games/GBST/Games/ST/A/Aliens (v1)(De).zip");
		CHECK(t != NULL);
		if (t) {
			printf("cp1250 desc: [%s]\n", t);
			CHECK(strstr(t, "Gra acelnoszz") != NULL || strstr(t, "Gra a") != NULL);
			CHECK(strstr(t, "acelnoszz") != NULL);
			CHECK(strchr(t, '\r') == NULL && strchr(t, '\t') == NULL);
			CHECK(strstr(t, "tab    here") != NULL);
			free(t);
		}
		t = BrData_Description("Games/GBST/Games/ST/A/Zork.ST");
		CHECK(t != NULL);
		if (t) {
			printf("utf8 desc: [%s]\n", t);
			CHECK(strncmp(t, "Zazolc e - ...", 14) == 0);
			free(t);
		}
		CHECK(BrData_Description("Top/a.MSA") == NULL);	/* binary .nfo */
		CHECK(BrData_Description("Top/e.st") == NULL);
		snprintf(f, sizeof(f), "%s/arch/Top/d.txt", scratch);
		write_file(f, "plain\r\n", 7);
		t = BrData_ReadTextFile(f);
		CHECK(t && strcmp(t, "plain\n") == 0);
		free(t);
		CHECK(BrData_ReadTextFile("/nonexistent/file") == NULL);
	}

	/* rescan */
	g_progress_calls = 0;
	CHECK(BrData_Rescan(progress_cb));
	CHECK(g_progress_calls > 0);
	snprintf(p, sizeof(p), "%s/browser/index.txt", g_hatari);
	{
		char *t = BrData_ReadTextFile(p);
		printf("rescan index:\n%s", t ? t : "(null)\n");
		CHECK(t != NULL);
		if (t) {
			CHECK(strstr(t, "Games/GBST/Games/ST/A/Aliens (v1)(De).zip\n"));
			CHECK(strstr(t, "Games/GBST/Games/ST/A/Zork.ST\n"));
			CHECK(strstr(t, "Top/a.MSA\n") && strstr(t, "Top/b.stx\n") &&
			      strstr(t, "Top/c.dim\n") && strstr(t, "Top/Sub/Deep/e.st\n"));
			CHECK(!strstr(t, "skip") && !strstr(t, "d.txt"));
			CHECK(count_lines(p) == 6);
			free(t);
		}
	}
	root = BrData_ArchiveRoot();	/* cache invalidated by rescan, categories irrelevant */
	CHECK(root != NULL);
	it = BrNode_Items(root, &cnt);
	/* Games/GBST/Games/ST/A collapses: no files in the chain until A */
	printf("fake root:");
	for (i = 0; i < cnt; i++)
		printf(" '%s'(%d)", it[i].label, it[i].count);
	printf("\n");
	CHECK(cnt >= 2);
	CHECK(BrData_ToggleFav("Top/a.MSA"));		/* leave state for abort test */
	CHECK(!BrData_ToggleFav("Top/a.MSA"));
	g_abort_after = true;
	snprintf(p, sizeof(p), "%s/browser/index.txt", g_hatari);
	{
		long before = count_lines(p);
		CHECK(!BrData_Rescan(progress_cb));	/* aborted: old index kept */
		CHECK(count_lines(p) == before);
	}
	g_abort_after = false;
	snprintf(BrData_Settings()->archive_root, 1024, "%s/doesnotexist", scratch);
	CHECK(!BrData_Rescan(NULL));
	snprintf(BrData_Settings()->archive_root, 1024, "%s/arch", scratch);

	/* ---- JSON escapes and unicode paths */
	{
		const char *idx = "Gry/Za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87.st\nGry/Other.st\nGry/Other2.st\r\n";
		const char *js = "{ \"ST\" : [ \"Gry\\/Za\\u017c\\u00f3\\u0142\\u0107.st\", "
		                 "\"Gry/Other.st\", \"no/such.st\" ],\n \"XX\": [\"Gry/Other2.st\"],"
		                 " \"PC\": [\"Gry\\/Other2.st\", \"q\\\"q\\\\\"] }";
		char f[4096];
		BrNode *r2;
		const BrItem *ri, *si;
		int rc, sc;

		snprintf(f, sizeof(f), "%s/browser/index.txt", g_hatari);
		write_file(f, idx, strlen(idx));
		snprintf(f, sizeof(f), "%s/browser/categories.json", g_hatari);
		write_file(f, js, strlen(js));
		r2 = BrData_ArchiveRoot();
		CHECK(r2 != NULL);
		ri = BrNode_Items(r2, &rc);
		CHECK(rc == 2 && strcmp(ri[0].label, "BY CATEGORY") == 0);
		CHECK(strcmp(ri[1].label, "Gry") == 0 && ri[1].count == 3);
		si = BrNode_Items(ri[0].sub, &sc);
		CHECK(sc == 2 && strcmp(si[0].label, "Point And Click Adventures") == 0 &&
		      strcmp(si[1].label, "Strategy") == 0);
		CHECK(si[0].count == 1 && si[1].count == 2);
		if (sc == 2) {
			int fc;
			const BrItem *fi = BrNode_Items(si[1].sub, &fc);
			CHECK(fc == 2 && strcmp(fi[0].label, "Other.st") == 0 &&
			      strcmp(fi[1].label, "Zazolc.st") == 0);
			CHECK(BrNode_ItemPath(si[1].sub, &fi[1], buf, sizeof(buf)) &&
			      strcmp(buf, "Gry/Za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87.st") == 0);
		}
		/* broken JSON: tree still builds, no categories (or partial) */
		write_file(f, "{ \"ST\": [ \"Gry/Other.st\" ", 25);
		r2 = BrData_ArchiveRoot();
		CHECK(r2 != NULL);
		ri = BrNode_Items(r2, &rc);
		CHECK(rc >= 1);
		/* no categories file */
		unlink(f);
		r2 = BrData_ArchiveRoot();
		ri = BrNode_Items(r2, &rc);
		CHECK(rc == 1 && strcmp(ri[0].label, "Gry") == 0);
		/* empty index => NULL */
		snprintf(f, sizeof(f), "%s/browser/index.txt", g_hatari);
		write_file(f, "", 0);
		CHECK(BrData_ArchiveRoot() == NULL);
		CHECK(!BrData_HaveIndex());
	}

	/* ---- >30 files => letter groups with deeper split */
	{
		char f[4096];
		FILE *fp;
		BrNode *r3;
		const BrItem *ri, *gi;
		int gc;
		snprintf(f, sizeof(f), "%s/browser/index.txt", g_hatari);
		fp = fopen(f, "w");
		for (i = 0; i < 40; i++)
			fprintf(fp, "G/%c File%d.st\n", 'a' + (i % 3), i);		/* a, b, c */
		for (i = 0; i < 40; i++)
			fprintf(fp, "G/Same%02d.st\n", i);		/* prefix S, then Sa, ... */
		for (i = 0; i < 3; i++)
			fprintf(fp, "G/%d num.st\n", i);
		fclose(fp);
		r3 = BrData_ArchiveRoot();
		CHECK(r3 != NULL);
		ri = BrNode_Items(r3, &cnt);
		CHECK(cnt == 1 && strcmp(ri[0].label, "G") == 0 && ri[0].count == 83);
		gi = BrNode_Items(ri[0].sub, &gc);
		printf("groups:");
		for (i = 0; i < gc; i++)
			printf(" %s(%d)", gi[i].label, gi[i].count);
		printf("\n");
		CHECK(gc == 5 && strcmp(gi[0].label, "0-9") == 0 && gi[0].count == 3 &&
		      strcmp(gi[1].label, "A") == 0 && strcmp(gi[4].label, "S") == 0);
		CHECK(gi[1].virt && gi[4].count == 40 && gi[1].count == 14);
		{
			int sc2;
			const BrItem *s2 = BrNode_Items(gi[4].sub, &sc2);
			/* 40 files with common prefix Same -> grouped on longer prefix; S+a.. */
			CHECK(sc2 > 0);
			printf("S group has %d items, first '%s'%s\n", sc2, s2[0].label,
			       s2[0].kind == BR_DIR ? " (dir)" : "");
		}
	}

	/* ---- local tree */
	{
		char dir[4096], f[4096];
		BrNode *l;
		const BrItem *li, *sub;
		int lc, sc;
		snprintf(dir, sizeof(dir), "%s", BrData_Settings()->local_dir);
		sh("mkdir -p '%s/Game One' '%s/Game Two/disks' '%s/.hidden' && "
		   "touch '%s/Game One/a.st' '%s/Game One/readme.txt' '%s/Game Two/disks/b.msa' "
		   "'%s/.hidden/x' && ln -s /etc/passwd '%s/Game One/link'", dir, dir, dir, dir, dir, dir, dir, dir);
		l = BrData_LocalRoot();
		li = BrNode_Items(l, &lc);
		CHECK(lc == 2 && strcmp(li[0].label, "Game One") == 0 && li[0].count == 3);
		CHECK(strcmp(li[1].label, "Game Two/disks") == 0);	/* collapsed */
		CHECK(BrNode_ItemDir(l, &li[1], f, sizeof(f)));
		snprintf(p, sizeof(p), "%.3000s/Game Two/disks", dir);
		CHECK(strcmp(f, p) == 0);
		sub = BrNode_Items(li[0].sub, &sc);
		CHECK(sc == 3);
		CHECK(BrNode_ItemPath(li[0].sub, &sub[0], f, sizeof(f)));
		printf("local first file: %s (%s)\n", f, sub[0].label);
		CHECK(f[0] == '/');
		/* regenerated on every call */
		sh("touch '%s/Game One/new.st'", dir);
		l = BrData_LocalRoot();
		li = BrNode_Items(l, &lc);
		BrNode_Items(li[0].sub, &sc);
		CHECK(sc == 4);
		/* empty / missing local dir */
		snprintf(BrData_Settings()->local_dir, 1024, "%s/nolocal", scratch);
		l = BrData_LocalRoot();
		CHECK(l != NULL);
		BrNode_Items(l, &lc);
		CHECK(lc == 0);
	}

	/* ---- heap growth over repeated cycles (no valgrind/ASan needed) */
	{
		char f[4096];
		size_t base = 0, after;
		int round;
		for (round = 0; round < 60; round++) {
			FILE *fp;
			BrNode *r4, *l4, *f4;
			int k, jc;
			const BrItem *ji;
			if (round == 10)
				base = mallinfo2().uordblks;
			snprintf(f, sizeof(f), "%s/browser/index.txt", g_hatari);
			fp = fopen(f, "w");
			for (k = 0; k < 100 + round; k++)
				fprintf(fp, "Dir%d/Sub/File %d.st\n", k % 7, k);
			fclose(fp);
			snprintf(f, sizeof(f), "%s/browser/categories.json", g_hatari);
			fp = fopen(f, "w");
			fprintf(fp, "{\"ST\":[\"Dir0/Sub/File 0.st\",\"Dir0/Sub/File 7.st\"]}");
			fclose(fp);
			r4 = BrData_ArchiveRoot();
			CHECK(r4 != NULL);
			walk_files(r4, 0, &dirs, &virt);
			l4 = BrData_LocalRoot();
			BrNode_Items(l4, &jc);
			f4 = BrData_FavRoot();
			ji = BrNode_Items(f4, &jc);
			(void)ji;
			BrData_ToggleFav("tmp/x.st");
			BrData_ToggleFav("tmp/x.st");
			{
				char *t = BrData_Description("Games/GBST/Games/ST/A/Zork.ST");
				free(t);
				t = BrData_ReadTextFile(f);
				free(t);
			}
			BrData_Rescan(NULL);
		}
		after = mallinfo2().uordblks;
		printf("heap in use: %zu -> %zu bytes after 50 more cycles\n", base, after);
		CHECK(after <= base + 4096);
	}

	/* ---- NULL safety */
	BrNode_Items(NULL, &cnt);
	CHECK(cnt == 0);
	CHECK(!BrNode_ItemPath(NULL, NULL, buf, sizeof(buf)));
	CHECK(!BrData_IsFav(NULL));
	CHECK(BrData_Description(NULL) == NULL);

	printf("\n%d checks passed, %d failed\n", g_ok, g_fail);
	return g_fail ? 1 : 0;
}
