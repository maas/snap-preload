# snap-preload

Hepler with system functions overrides inside a snap

Currently the library overrides the following functions:

- `initgroups` and `setgroups` to no-op
- `shm_open` and `shm_unlink` to prefix the name with
  `snap.$SNAP_INSTANCE_NAME.`, as required by snaps


## Compile

To build and install the library run

```sh
$ make
$ make install DESTDIR=/some/path
```

## Run applications with the preload

The `snap-preload.so` library is intended to be used under `LD_PRELOAD`, e.g.

```
$ LD_PRELOAD=/usr/lib/snap-preload.so /usr/bin/myapp ...
```
