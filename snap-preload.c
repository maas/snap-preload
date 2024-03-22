#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/limits.h>
#include <string.h>
#include <semaphore.h>

#define MAX_SNAP_NAME_SIZE 40
#define MAX_SEM_NAME_SIZE (NAME_MAX - MAX_SNAP_NAME_SIZE)
#define SHM_DIR "/dev/shm"

// constants set at library init time
static char SNAP_INSTANCE_NAME[MAX_SNAP_NAME_SIZE + 1] = {'\0'};
static int DEBUG = 0;

// original functions
static int (*orig_setgroups)(size_t size, const gid_t *list);
static int (*orig_shm_open)(const char *name, int oflag, mode_t mode);
static int (*orig_shm_unlink)(const char *name);
static sem_t *(*orig_sem_open)(const char *name, int oflag, ...);
static int (*orig_sem_unlink)(const char *name);

#define SAVE_ORIGINAL_SYMBOL(SYM) orig_##SYM = dlsym(RTLD_NEXT, #SYM)

#define log(FORMAT, ...)                                          \
  if (DEBUG)                                                      \
  {                                                               \
    fprintf(stderr, "snap-preload: " FORMAT "\n", ##__VA_ARGS__); \
  }

// Return the snap instance name from the application cgroup.
// The line for the `0` entry ends with a scope name in the format:
//
// 0::/system.slice/snap.maas.pebble-0a51aac5-56bd-4dce-82ec-ce4b29658cb0.scope
//
int snap_instance_name(char *snap_name, size_t len)
{
  FILE *fp;

  if (!(fp = fopen("/proc/self/cgroup", "r")))
    return -1;

  while (!feof(fp))
  {
    char name[MAX_SNAP_NAME_SIZE + 1] = {'\0'};
    if (fscanf(fp, "0:%*[^/]/%*[^/]/snap.%40[^.].%*[^.].scope", name) == 1)
    {
      strncpy(snap_name, name, len);
      fclose(fp);
      return 0;
    }
    fscanf(fp, "%*[^\n]");
    fscanf(fp, "%*[\n]");
  }

  fclose(fp);
  return -1;
}

// Snapd only allows applications to access shared memory paths that match the
// snap.$SNAP_INSTANCE_NAME.* format. This rewrites paths so that applications
// don't need changes to conform
int adjust_path(char *new_path, const char *orig_path, size_t len)
{
  if (SNAP_INSTANCE_NAME[0] == '\0')
  {
    strncpy(new_path, orig_path, len);
  }
  else
  {
    if (orig_path[0] == '/')
      orig_path++;
    snprintf(new_path, len, "/snap.%s.%s", SNAP_INSTANCE_NAME, orig_path);
  }
  return 0;
}

// overrides

int setgroups(size_t size, const gid_t *list)
{
  return orig_setgroups(0, NULL);
}

// This is only needed until there's proper support in snapd for initgroups()
// see https://forum.snapcraft.io/t/seccomp-filtering-for-setgroups/2109 for
// more info.
int initgroups(const char *user, gid_t group)
{
  return setgroups(0, NULL);
}

int shm_open(const char *name, int oflag, mode_t mode)
{
  char new_path[NAME_MAX];
  adjust_path(new_path, name, sizeof(new_path));
  return orig_shm_open(new_path, oflag, mode);
}

int shm_unlink(const char *name)
{
  char new_path[NAME_MAX];
  adjust_path(new_path, name, sizeof(new_path));
  return orig_shm_unlink(new_path);
}

sem_t *sem_open(const char *name, int oflag, ...)
{
  va_list args;
  mode_t mode;
  unsigned int value;
  int fd;
  int existed = 0;
  sem_t initial_sem;
  sem_t *final_sem;
  char new_name[MAX_SEM_NAME_SIZE];
  char sem_path[NAME_MAX];
  char tmp_path[NAME_MAX];

  adjust_path(new_name, name, sizeof(new_name));
  log("new_name %s", new_name);

  if (!(oflag & O_CREAT))
    return orig_sem_open(new_name, oflag);

  va_start(args, oflag);
  mode = va_arg(args, mode_t);
  value = va_arg(args, unsigned int);
  va_end(args);
  if (value > SEM_VALUE_MAX)
  {
    errno = EINVAL;
    return SEM_FAILED;
  }

  // glibc creates a tempfile, initializes it, hardlink to the requested name
  // and finally unlinks the tempfile. Due to snap confinement rules, the first
  // step fails in the original implementation.

  // create a temporary file
  snprintf(tmp_path, sizeof(tmp_path), "%s%s.XXXXXX", SHM_DIR, new_name);
  if ((fd = mkstemp(tmp_path)) < 0)
    return SEM_FAILED;

  if (fchmod(fd, mode) < 0)
    goto clean_tmp;
  sem_init(&initial_sem, 1, value);
  if (write(fd, &initial_sem, sizeof(sem_t)) < 0)
    goto clean_tmp;
  close(fd);

  // new_name is an absolute path, skip the initial '/'
  snprintf(sem_path, sizeof(sem_path), "%s/sem.%s", SHM_DIR, &(new_name[1]));
  log("sem_path %s", sem_path);
  if ((existed = link(tmp_path, sem_path)) < 0)
  {
    if (oflag & O_EXCL || errno != EEXIST)
    {
      unlink(tmp_path);
      return SEM_FAILED;
    }
  }
  unlink(tmp_path);

  final_sem = orig_sem_open(new_name, oflag & ~(O_CREAT | O_EXCL));
  if (final_sem == SEM_FAILED && !existed)
    unlink(sem_path);
  return final_sem;

clean_tmp:
  close(fd);
  unlink(tmp_path);
  return SEM_FAILED;
}

int sem_unlink(const char *name)
{
  char new_path[NAME_MAX];

  adjust_path(new_path, name, sizeof(new_path));
  return orig_sem_unlink(new_path);
}

// library init
static __attribute__((constructor)) void init(void)
{
  if (secure_getenv("SNAP_PRELOAD_DEBUG"))
  {
    DEBUG = 1;
  }

  if (snap_instance_name(SNAP_INSTANCE_NAME, sizeof(SNAP_INSTANCE_NAME)) != 0)
  {
    log("snap instance name not identified from cgroup");
  }

  SAVE_ORIGINAL_SYMBOL(setgroups);
  SAVE_ORIGINAL_SYMBOL(shm_open);
  SAVE_ORIGINAL_SYMBOL(shm_unlink);
  SAVE_ORIGINAL_SYMBOL(sem_open);
  SAVE_ORIGINAL_SYMBOL(sem_unlink);
}
