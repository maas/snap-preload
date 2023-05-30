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

// contants set at library init time
static char *SNAP_INSTANCE_NAME = NULL;
static int DEBUG = 0;

// original functions
static int (*orig_setgroups)(size_t size, const gid_t *list);
static int (*orig_shm_open)(const char *name, int oflag, mode_t mode);
static int (*orig_shm_unlink)(const char *name);

#define SAVE_ORIGINAL_SYMBOL(SYM) orig_##SYM = dlsym(RTLD_NEXT, #SYM)

#define log(FORMAT, ...) if (DEBUG) {fprintf(stderr, "snap-preload: " FORMAT "\n", ##__VA_ARGS__);}


// Return the snap instance name from the application cgroup.
// The line for the `0` entry ends with a scope name in the format:
//
//  /snap.<snap instance name>.<snap-command>.<uuid>.scope
//
char* snap_instance_name() {
  FILE* fp;
  char* line = NULL;
  char* sub = NULL;
  char* name = NULL;
  size_t len = 0;

  fp = fopen("/proc/self/cgroup", "r");
  if (!fp) {
    return NULL;
  }

  // Find cgroup 0
  while (getline(&line, &len, fp) > 0 && line[0] != '0') {};
  if (len < 1 || line[0] != '0') {
    return NULL;
  }

  // Look for the snap name prefix
  sub = strrchr(line, '/');
  if (sub && strncmp(sub, "/snap.", 6) == 0) {
    sub += 6;

    // Extract the snap instance name
    char* end = strchr(sub, '.');
    if (end) {
      *end = '\0';
      name = strdup(sub);
    }
  }

  fclose(fp);
  free(line);
  return name;
}


// Snapd only allows applications to access shared memory paths that match the
// snap.$SNAP_INSTANCE_NAME.* format. This rewrites paths so that applications
// don't need changes to conform
static char *adjust_shm_path(const char *orig_path) {
  if (!SNAP_INSTANCE_NAME) {
    return strdup(orig_path);
  }

  int path_len = strlen(orig_path) + strlen(SNAP_INSTANCE_NAME) + 9;
  char *new_path = malloc(path_len);
  assert(new_path);
  const char *path = (orig_path[0] == '/') ? &(orig_path[1]) : orig_path;
  snprintf(new_path, path_len , "/snap.%s.%s", SNAP_INSTANCE_NAME, path);
  log("shm path rewritten: %s -> %s", orig_path, new_path);
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
  if (secure_getenv("SNAP_PRELOAD_DEBUG")) {
    DEBUG = 1;
  }

  SNAP_INSTANCE_NAME = snap_instance_name();
  if (!SNAP_INSTANCE_NAME) {
    log("snap instance name not identified from cgroup");
  }

  SAVE_ORIGINAL_SYMBOL(setgroups);
  SAVE_ORIGINAL_SYMBOL(shm_open);
  SAVE_ORIGINAL_SYMBOL(shm_unlink);
}  
