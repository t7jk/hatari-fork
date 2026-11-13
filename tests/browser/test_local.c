/* Standalone test for browser_local.c.
 * Build: gcc -Wall -Wextra -DHAVE_LIBZ=1 -I../../hatari-src/src/includes test_local.c \
 *        ../../hatari-src/src/gui-sdl/browser_local.c -lz -o test_local
 * Run:   ./test_local <scratch-dir>   (needs python3 to create test zips) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "browser.h"

static BrSettings cfg;
static char ins_a[1024], ins_b[1024];
BrSettings *BrData_Settings(void) { return &cfg; }
void BrData_AbsPath(const char *p, char *out, size_t sz)
{
	if (p[0] == '/') snprintf(out, sz, "%s", p);
	else snprintf(out, sz, "%s/%s", cfg.archive_root, p);
}
const char *Floppy_SetDiskFileName(int d, const char *f, const char *z)
{
	(void)z;
	snprintf(d == 0 ? ins_a : ins_b, 1024, "%s", f);
	return NULL;
}
const char *Floppy_SetDiskFileNameNone(int d) { (void)d; return NULL; }

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static void wr(const char *path, const char *txt)
{
	FILE *f = fopen(path, "wb");
	if (f) { fputs(txt, f); fclose(f); }
}
static bool ex(const char *path) { struct stat st; return lstat(path, &st) == 0; }
static bool progress_abort(int d, int t) { (void)d; (void)t; return false; }

static void name_test(const char *in, const char *want)
{
	char out[256];
	BrLocal_GameName(in, out, sizeof(out));
	checks++;
	if (strcmp(out, want)) { fails++; printf("FAIL name '%s' -> '%s' (want '%s')\n", in, out, want); }
}

int main(int argc, char **argv)
{
	char sc[512], cmd[8192], p[4096], p2[4096], err[256], dir[2048], dir2[2048];
	const char *rels[3];
	char **files = NULL;
	int nf = 0, i;

	if (argc < 2) { fprintf(stderr, "usage: %s scratchdir\n", argv[0]); return 2; }
	snprintf(sc, sizeof(sc), "%s", argv[1]);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/arc/sub' '%s/outside'", sc, sc, sc);
	CHECK(system(cmd) == 0);
	snprintf(cfg.archive_root, sizeof(cfg.archive_root), "%s/arc", sc);
	snprintf(cfg.local_dir, sizeof(cfg.local_dir), "%s/local", sc);

	/* IsDisk */
	CHECK(BrLocal_IsDisk("a/B.ST") && BrLocal_IsDisk("x.msa") && BrLocal_IsDisk("x.Zip")
	      && BrLocal_IsDisk("x.stx") && BrLocal_IsDisk("x.dim"));
	CHECK(!BrLocal_IsDisk("x.txt") && !BrLocal_IsDisk("st") && !BrLocal_IsDisk(NULL));

	/* GameName */
	name_test("dir/Foo (Disk 1 of 2).st", "Foo");
	name_test("Foo (Disk 1).st", "Foo");
	name_test("Foo - Disk 2.msa", "Foo");
	name_test("Foo_Disc A.st", "Foo");
	name_test("Foo [Side B] [h].st", "Foo [Side B] [h]");	/* same as the Python regex: no match */
	name_test("Foo [Side B].st", "Foo");
	name_test("Foo (Side 1)(a).st", "Foo (Side 1)(a)");
	name_test("Foo Disk1.st", "Foo");
	name_test("Plain Game.st", "Plain Game");
	name_test("Diskette Fun.st", "Diskette Fun");
	name_test("Foo: Bar?.zip", "Foo_ Bar_");
	name_test(".st", "st");	/* splitext(".st") keeps the name */
	name_test("...", "game");
	name_test("(Disk 1).st", "game");

	/* sources */
	snprintf(p, sizeof(p), "%s/arc/sub/Game (Disk 1 of 2).st", sc); wr(p, "disk1");
	snprintf(p, sizeof(p), "%s/arc/sub/Game (Disk 2 of 2).st", sc); wr(p, "disk2");
	snprintf(p, sizeof(p), "%s/arc/Plain.st", sc); wr(p, "plain");
	snprintf(cmd, sizeof(cmd),
		"python3 -I - '%s' <<'PY'\n"
		"import zipfile,sys\n"
		"sc=sys.argv[1]\n"
		"with zipfile.ZipFile(sc+'/arc/Zipped.zip','w',zipfile.ZIP_DEFLATED) as z:\n"
		"    z.writestr('Zipped/b.st','B'*5000)\n"
		"    z.writestr('a.st','A'*3000)\n"
		"    z.writestr('docs/readme.txt','hello')\n"
		"    z.writestr('other/a.st','A2')\n"
		"    z.writestr('emptydir/','')\n"
		"with zipfile.ZipFile(sc+'/arc/Slip.zip','w') as z:\n"
		"    z.writestr('../../evil.st','EVIL')\n"
		"    z.writestr('/abs/evil2.st','EVIL2')\n"
		"    z.writestr('..\\\\evil3.st','EVIL3')\n"
		"    z.writestr('ok.st','OK')\n"
		"open(sc+'/arc/Bad.zip','wb').write(b'not a zip at all, really not a zip file')\n"
		"PY", sc);
	CHECK(system(cmd) == 0);

	/* copy: 2 plain disks */
	rels[0] = "sub/Game (Disk 1 of 2).st"; rels[1] = "sub/Game (Disk 2 of 2).st";
	CHECK(BrLocal_Copy(rels, 2, dir, sizeof(dir), &files, &nf, err, sizeof(err), NULL));
	CHECK(nf == 2 && files && strstr(dir, "/local/Game") && !strstr(dir, "("));
	if (nf == 2) CHECK(strstr(files[0], "Disk 1") && strstr(files[1], "Disk 2") && ex(files[0]) && ex(files[1]));
	BrLocal_FreeList(files, nf);

	/* second copy -> "Game (2)" */
	CHECK(BrLocal_Copy(rels, 1, dir2, sizeof(dir2), &files, &nf, err, sizeof(err), NULL));
	CHECK(strstr(dir2, "/local/Game (2)") && nf == 1 && ex(dir));
	BrLocal_FreeList(files, nf);

	/* same basename collision within one folder -> N_ prefix */
	rels[0] = "Plain.st"; rels[1] = "Plain.st";
	CHECK(BrLocal_Copy(rels, 2, dir, sizeof(dir), &files, &nf, err, sizeof(err), NULL));
	CHECK(nf == 2 && strstr(files[1], "/2_Plain.st"));
	BrLocal_FreeList(files, nf);

	/* zip extraction */
	rels[0] = "Zipped.zip";
	CHECK(BrLocal_Copy(rels, 1, dir, sizeof(dir), &files, &nf, err, sizeof(err), NULL));
	CHECK(nf == 4);
	if (nf == 4)
	{
		/* sorted case-insens by entry name: Zipped/b.st, a.st, docs/readme.txt, other/a.st
		 * -> sort: a.st, docs/readme.txt, other/a.st, Zipped/b.st */
		CHECK(strstr(files[0], "/a.st") && strstr(files[1], "/readme.txt")
		      && strstr(files[2], "/2_a.st") && strstr(files[3], "/b.st"));
		struct stat st;
		CHECK(stat(files[3], &st) == 0 && st.st_size == 5000);
		CHECK(stat(files[0], &st) == 0 && st.st_size == 3000);
	}
	snprintf(p, sizeof(p), "%s/Zipped.zip", dir);
	CHECK(!ex(p));
	BrLocal_FreeList(files, nf);

	/* zip-slip */
	rels[0] = "Slip.zip";
	CHECK(BrLocal_Copy(rels, 1, dir, sizeof(dir), &files, &nf, err, sizeof(err), NULL));
	CHECK(nf == 4);
	for (i = 0; i < nf; i++)
	{
		char *d = strdup(files[i]);
		*strrchr(d, '/') = '\0';
		CHECK(strcmp(d, dir) == 0);
		free(d);
	}
	snprintf(p, sizeof(p), "%s/evil.st", sc); CHECK(!ex(p));
	snprintf(p, sizeof(p), "%s/../evil.st", cfg.local_dir); CHECK(!ex(p));
	snprintf(p, sizeof(p), "%s/arc/evil.st", sc); CHECK(!ex(p));
	CHECK(ex("/abs") == false);
	BrLocal_FreeList(files, nf);

	/* bad zip, missing file, abort: fail, no leftover folder */
	rels[0] = "Bad.zip";
	CHECK(!BrLocal_Copy(rels, 1, dir, sizeof(dir), &files, &nf, err, sizeof(err), NULL) && err[0]);
	snprintf(p, sizeof(p), "%s/Bad", cfg.local_dir); CHECK(!ex(p));
	rels[0] = "nonexistent.st";
	CHECK(!BrLocal_Copy(rels, 1, dir, sizeof(dir), &files, &nf, err, sizeof(err), NULL));
	rels[0] = "Plain.st";
	CHECK(!BrLocal_Copy(rels, 1, dir, sizeof(dir), &files, &nf, err, sizeof(err), progress_abort));
	CHECK(!strcmp(err, "Aborted"));

	/* delete */
	snprintf(p, sizeof(p), "%s/nested/deeper/x.st", cfg.local_dir);
	snprintf(cmd, sizeof(cmd), "mkdir -p '%s/nested/deeper'", cfg.local_dir); CHECK(system(cmd) == 0);
	wr(p, "x");
	CHECK(BrLocal_Delete(p));
	CHECK(!ex(p));
	snprintf(p2, sizeof(p2), "%s/nested", cfg.local_dir);
	CHECK(!ex(p2));			/* empty parents cleaned */
	CHECK(ex(cfg.local_dir));		/* local_dir itself kept */
	/* parents with other content are kept */
	snprintf(cmd, sizeof(cmd), "mkdir -p '%s/n2/a' '%s/n2/b'", cfg.local_dir, cfg.local_dir); CHECK(system(cmd) == 0);
	snprintf(p, sizeof(p), "%s/n2/a/x.st", cfg.local_dir); wr(p, "x");
	snprintf(p2, sizeof(p2), "%s/n2/b/y.st", cfg.local_dir); wr(p2, "y");
	CHECK(BrLocal_Delete(p));
	snprintf(p, sizeof(p), "%s/n2/a", cfg.local_dir); CHECK(!ex(p));
	CHECK(ex(p2));
	/* recursive dir delete */
	snprintf(p, sizeof(p), "%s/n2", cfg.local_dir);
	CHECK(BrLocal_Delete(p));
	CHECK(!ex(p));
	/* refuse: local_dir itself, outside, traversal, symlink to outside, missing */
	CHECK(!BrLocal_Delete(cfg.local_dir) && ex(cfg.local_dir));
	snprintf(p, sizeof(p), "%s/outside/keep.st", sc); wr(p, "keep");
	CHECK(!BrLocal_Delete(p) && ex(p));
	snprintf(p2, sizeof(p2), "%s/outside", sc);
	CHECK(!BrLocal_Delete(p2) && ex(p));
	snprintf(p2, sizeof(p2), "%s/../outside/keep.st", cfg.local_dir);
	CHECK(!BrLocal_Delete(p2) && ex(p));
	snprintf(p2, sizeof(p2), "%s/lnk", cfg.local_dir);
	CHECK(symlink("../outside", p2) == 0);
	CHECK(!BrLocal_Delete(p2) && ex(p));
	snprintf(p2, sizeof(p2), "%s/lnk/keep.st", cfg.local_dir);
	CHECK(!BrLocal_Delete(p2) && ex(p));
	snprintf(p2, sizeof(p2), "%s/lnkf", cfg.local_dir);
	CHECK(symlink(p, p2) == 0);
	CHECK(!BrLocal_Delete(p2) && ex(p) && ex(p2));
	CHECK(!BrLocal_Delete("") && !BrLocal_Delete(NULL));
	snprintf(p2, sizeof(p2), "%s/nope.st", cfg.local_dir);
	CHECK(!BrLocal_Delete(p2));

	/* insert */
	{
		char *one[] = { "/x/a.st" };
		char *two[] = { "/x/a.st", "/x/b.msa" };
		char *three[] = { "/x/a.st", "/x/b.msa", "/x/c.zip" };
		char *mix[] = { "/x/readme.txt", "/x/b.msa" };
		char *mix2[] = { "/x/readme.txt", "/x/a.st", "/x/b.msa" };
		ins_a[0] = ins_b[0] = 0;
		CHECK(BrLocal_Insert(one, 1) == 1 && !strcmp(ins_a, "/x/a.st") && !ins_b[0]);
		ins_a[0] = ins_b[0] = 0;
		CHECK(BrLocal_Insert(two, 2) == 2 && !strcmp(ins_a, "/x/a.st") && !strcmp(ins_b, "/x/b.msa"));
		ins_a[0] = ins_b[0] = 0;
		CHECK(BrLocal_Insert(three, 3) == 1 && !strcmp(ins_a, "/x/a.st") && !ins_b[0]);
		ins_a[0] = ins_b[0] = 0;
		CHECK(BrLocal_Insert(mix, 2) == 1 && !strcmp(ins_a, "/x/b.msa") && !ins_b[0]);
		ins_a[0] = ins_b[0] = 0;
		CHECK(BrLocal_Insert(mix2, 3) == 2 && !strcmp(ins_a, "/x/a.st") && !strcmp(ins_b, "/x/b.msa"));
		CHECK(BrLocal_Insert(NULL, 0) == 0);
	}

	printf("%d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
