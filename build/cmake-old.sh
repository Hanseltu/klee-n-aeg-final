cmake -DENABLE_SOLVER_STP=ON -DENABLE_POSIX_RUNTIME=ON -DENABLE_KLEE_UCLIBC=ON -DKLEE_UCLIBC_PATH=/home/jqhong/klee-nme/klee-nme-aeg/klee-uclibc/ -DENABLE_UNIT_TESTS=OFF -DGTEST_SRC_DIR=/home/jqhong/klee-nme/klee-nme-aeg/googletest-release-1.7.0/ -DLLVM_CONFIG_BINARY=/usr/bin/llvm-config-9 -DLLVMCC=/usr/bin/clang-9 -DLLVMCXX=/usr/bin/clang++-9 ../

#lit patckage is required to enable testing. pip requies sudo privilege, and do remember to add -H for sudo. sudo -h pip/pip3 install lit 
