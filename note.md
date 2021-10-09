### When restart system

go to `/home/jqhong/klee-nme/onsite/new_elf_mod`

$ make && sudo insmod ld.ko

### Check out the native address of global function pointer

* get pid by `printf ("pid: %d. \n", getpid());`

* run test binary, then `sudo less /proc/#pid/maps`
