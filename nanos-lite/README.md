# Nanos-lite

Nanos-lite is the simplified version of Nanos (http://cslab.nju.edu.cn/opsystem).
It is ported to the [AM project](https://github.com/NJU-ProjectN/abstract-machine.git).
It is a two-tasking operating system with the following features
* ramdisk device drivers
* ELF program loader
* memory management with paging
* a simple file system
  * with fix number and size of files
  * without directory
  * some device files
* 9 system calls
  * open, read, write, lseek, close, gettimeofday, brk, exit, execve
* scheduler with two tasks

## Running ONScripter

ONScripter is under `navy-apps/apps`, so build it through `NAVY_APPS` instead of
the default `NAVY_TESTS` path:

```sh
make -C nanos-lite ARCH=x86-nemu NANOS_INIT=onscripter NAVY_APPS=onscripter NAVY_TESTS="" FS_MODE=fat32 update run
```

`proc.c` passes the fixed `-r /share/games/ons` argv when booting
`/bin/onscripter`.

The default Navy image is kept in sync with the default process list in
`proc.c`: `/bin/pal`, `/bin/onscripter`, and `/bin/bird`.
