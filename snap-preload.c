#define _GNU_SOURCE
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
static char *SNAP_NAME;
static int DEBUG = 0;


#define SAVE_ORIGINAL_SYMBOL(SYM) orig_##SYM = dlsym(RTLD_NEXT, #SYM)

#define log(FORMAT, ...) if (DEBUG) {fprintf(stderr, "snap-preload: " FORMAT "\n", __VA_ARGS__);}

static void exit_error(char *message) {
  fprintf(stderr, "error: %s\n", message);
  exit(-1);
}

static char *adjust_shm_path(const char *orig_path) {
  const char *path = (orig_path[0] == '/') ? &(orig_path[1]) : orig_path;
  char *new_path = malloc(PATH_MAX);
  snprintf(new_path, PATH_MAX, "/snap.%s.%s", SNAP_NAME, path);
  log("shm path rewritten: %s -> %s", orig_path, new_path);
  return new_path;
}

// overrides

int setgroups(size_t size, const gid_t *list) {
  return orig_setgroups(0, NULL);
}

// this is only needed until there's proper support in snapd for initgroups()
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
  SNAP_NAME = secure_getenv("SNAP_INSTANCE_NAME");
  if (! SNAP_NAME) {
    exit_error("SNAP_INSTANCE_NAME not defined");
  }
  if (secure_getenv("SNAP_PRELOAD_DEBUG")) {
    DEBUG = 1;
  }

  SAVE_ORIGINAL_SYMBOL(setgroups);
  SAVE_ORIGINAL_SYMBOL(shm_open);
  SAVE_ORIGINAL_SYMBOL(shm_unlink);
}
