#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/limits.h>
#include <string.h>

// original functions
static int (*orig_setgroups)(size_t size, const gid_t *list);
static int (*orig_shm_open)(const char *name, int oflag, mode_t mode);
static int (*orig_shm_unlink)(const char *name);

// contants set at library init time
static char *SNAP_INSTANCE_NAME;
static int DEBUG = 0;


#define SAVE_ORIGINAL_SYMBOL(SYM) orig_##SYM = dlsym(RTLD_NEXT, #SYM)

#define log(FORMAT, ...) if (DEBUG) {fprintf(stderr, "snap-preload: " FORMAT "\n", __VA_ARGS__);}

// Snapd only allows applications to access shared memory paths that match the
// snap.$SNAP_INSTANCE_NAME.* format. This rewrites paths so that applications
// don't need changes to conform
static char *adjust_shm_path(const char *orig_path) {
  int path_len = strlen(orig_path) + strlen(SNAP_INSTANCE_NAME) + strlen("/snap..") + 1;
  char *new_path = malloc(path_len);
  assert(new_path != NULL);
  if (SNAP_INSTANCE_NAME) {
    const char *path = (orig_path[0] == '/') ? &(orig_path[1]) : orig_path;
    snprintf(new_path, path_len , "/snap.%s.%s", SNAP_INSTANCE_NAME, path);
    log("shm path rewritten: %s -> %s", orig_path, new_path);
  } else {
    new_path = strncpy(new_path, orig_path, path_len);
  }
  return new_path;
}

// overrides

int setgroups(size_t size, const gid_t *list) {
  return orig_setgroups(0, NULL);
}

// This is only needed until there's proper support in snapd for initgroups()
// see https://forum.snapcraft.io/t/seccomp-filtering-for-setgroups/2109 for
// more info.
int initgroups(const char *user, gid_t group) {
  return setgroups(0, NULL);
}


int shm_open(const char *name, int oflag, mode_t mode) {
  char *new_path = adjust_shm_path(name);
  int res = orig_shm_open(new_path, oflag, mode);
  free(new_path);
  return res;
}


int shm_unlink(const char *name) {
  char *new_path = adjust_shm_path(name);
  int res = orig_shm_unlink(new_path);
  free(new_path);
  return res;
}


// library init
static void __attribute__ ((constructor)) init(void) {
  SNAP_INSTANCE_NAME = secure_getenv("SNAP_INSTANCE_NAME");
  if (secure_getenv("SNAP_PRELOAD_DEBUG")) {
    DEBUG = 1;
  }

  SAVE_ORIGINAL_SYMBOL(setgroups);
  SAVE_ORIGINAL_SYMBOL(shm_open);
  SAVE_ORIGINAL_SYMBOL(shm_unlink);
}
