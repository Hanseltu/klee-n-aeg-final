# ~/Documents/klee-n-aeg/build/bin/klee --libc=uclibc --posix-runtime ./aaw.bc
# ~/Documents/klee-n-aeg/build/bin/klee --libc=uclibc --posix-runtime ./aaw.bc -sym-arg 48
#/home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/bin/klee --libc=uclibc --posix-runtime ./aaw.bc 

rm -rf klee-*
clang -emit-llvm -c aaw.c
klee --libc=uclibc --posix-runtime ./aaw.bc
# ~/Documents/klee-n-aeg/build/bin/klee-test klee-last/test000001.ktest
