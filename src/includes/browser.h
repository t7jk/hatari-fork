/*
  Hatari - browser.h

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Disk-image Browser: shared contract between
    gui-sdl/browser_data.c   (archive index, tree, favorites, config)   [data layer, no SDL]
    gui-sdl/browser_local.c  (local copies, zip extraction, launching)  [no SDL dialogs]
    gui-sdl/dlgBrowser.c     (the SDL GUI dialog)
*/
#ifndef HATARI_BROWSER_H
#define HATARI_BROWSER_H

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Entry point (dlgBrowser.c). Returns true if a disk was inserted and
 * the machine must be reset (caller leaves the main GUI with reset).   */
extern bool DlgBrowser_Main(void);

/* ------------------------------------------------------------------ */
/* Settings, stored in <hatari config dir>/browser/browser.cfg (key=value).
 * All browser files live in <hatari config dir>/browser/ :
 *   browser.cfg, index.txt, favorites.txt, categories.json, state.txt, local/ */
typedef struct {
	char archive_root[1024];  /* folder with the disk archive (already mounted) */
	char local_dir[1024];     /* where local copies live (default <cfgdir>/browser/local) */
	bool copy_local;          /* true: multi-disk/zip launches go through a local copy */
} BrSettings;

extern bool BrData_Init(void);                 /* create dir, load cfg/favorites/state; import legacy files, see plan */
extern BrSettings *BrData_Settings(void);      /* never NULL */
extern bool BrData_SaveSettings(void);

/* ------------------------------------------------------------------ */
/* Tree of disks. A node is a folder (real, category or virtual letter group). */
typedef struct BrNode BrNode;

typedef enum { BR_DIR, BR_FILE } BrKind;

typedef struct {
	BrKind  kind;
	char    label[200];   /* text shown in the list (dirs: collapsed "A/B/C" names) */
	BrNode *sub;          /* BR_DIR only */
	bool    virt;         /* BR_DIR only: virtual letter group (A, B, 0-9 ...) */
	int     count;        /* BR_DIR only: number of disks below, else 0 */
} BrItem;

/* Archive tree built from index.txt (+categories.json => top folder "BY CATEGORY").
 * Returns NULL if there is no index yet (caller should offer rescan).         */
extern BrNode *BrData_ArchiveRoot(void);
/* Local repository tree, rebuilt from disk on EVERY call (old tree is freed). */
extern BrNode *BrData_LocalRoot(void);
/* Sorted items of a node (dirs first, >30 files => virtual letter groups). Array owned by the node. */
extern const BrItem *BrNode_Items(BrNode *node, int *count);
/* Path of a file item: relative to archive_root for archive nodes (use BrData_AbsPath),
 * absolute for local nodes. Returns false for dirs. */
extern bool BrNode_ItemPath(BrNode *node, const BrItem *item, char *out, size_t outsize);
/* Absolute path for a path returned above (rel -> archive_root/rel; absolute stays). */
extern void BrData_AbsPath(const char *path, char *out, size_t outsize);
/* For local nodes: real directory of the folder item (for delete); false otherwise. */
extern bool BrNode_ItemDir(BrNode *node, const BrItem *item, char *out, size_t outsize);

/* Index: scan archive_root for .st .msa .stx .dim .zip (skip junk dirs), write index.txt.
 * progress(n) called now and then, may be NULL; return false from progress = abort. */
extern bool BrData_Rescan(bool (*progress)(int found));
extern bool BrData_HaveIndex(void);

/* Favorites (relative archive paths, order preserved) */
extern int         BrData_FavCount(void);
extern const char *BrData_FavGet(int i);
extern bool        BrData_IsFav(const char *rel);
extern bool        BrData_ToggleFav(const char *rel);   /* returns new state (true = now favorite) */
extern BrNode     *BrData_FavRoot(void);                /* flat node of favorites, rebuilt each call */

/* Description text for the top-right/bottom-right panes.
 * Looks next to the disk (<name>.txt/.nfo/.diz) and GameBase Extras/Instructions.
 * Returns malloc()ed NUL-terminated text (CR removed, non-ASCII mapped to '?' or latin1 safe), or NULL. Caller free()s. */
extern char *BrData_Description(const char *path);
/* Text of any plain text file (first 100 KB), same conventions. NULL if binary/unreadable. */
extern char *BrData_ReadTextFile(const char *abs_path);

/* Last browsing position (list of folder labels from root + selected label) */
extern void BrData_SaveState(const char *const *labels, int nlabels, const char *selected);
extern int  BrData_LoadState(char labels[][200], int maxlabels, char *selected, size_t selsize);

/* ------------------------------------------------------------------ */
/* browser_local.c */

/* Copy files (archive-relative paths) into <local_dir>/<game name>/ ; zips are extracted
 * (files flattened, zip-slip safe, zip then removed). out_dir: resulting folder.
 * out_files: malloc()ed array of malloc()ed absolute paths of disk images, in input order;
 * free with BrLocal_FreeList. Returns false (and sets err) on failure; progress may be NULL. */
extern bool BrLocal_Copy(const char *const *rels, int n, char *out_dir, size_t dirsize,
                         char ***out_files, int *out_nfiles, char *err, size_t errsize,
                         bool (*progress)(int done, int total));
extern void BrLocal_FreeList(char **files, int n);
/* Delete a file/folder, ONLY if it is inside local_dir (realpath check); removes empty parents up to local_dir. */
extern bool BrLocal_Delete(const char *abs_path);
/* Insert disks (absolute paths) into the running emulator: 1 -> A:, 2 -> A: and B:,
 * >2 -> A: only (first). Non-disk files are ignored. Does NOT reset. Returns number inserted. */
extern int  BrLocal_Insert(char *const *abs_paths, int n);
/* Is the file a disk image we can insert (.st .msa .stx .dim .zip, case-insens.) */
extern bool BrLocal_IsDisk(const char *path);
/* Name of the game folder derived from the first disk name ("Foo (Disk 1 of 2)" -> "Foo") */
extern void BrLocal_GameName(const char *rel, char *out, size_t outsize);

#endif
