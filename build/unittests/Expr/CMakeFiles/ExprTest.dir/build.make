# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.10

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Remove some rules from gmake that .SUFFIXES does not remove.
SUFFIXES =

.SUFFIXES: .hpux_make_needs_suffix_list


# Suppress display of executed commands.
$(VERBOSE).SILENT:


# A target that is always out of date.
cmake_force:

.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E remove -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/beverly/Documents/klee-n-aeg

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/beverly/Documents/klee-n-aeg/build

# Include any dependencies generated for this target.
include unittests/Expr/CMakeFiles/ExprTest.dir/depend.make

# Include the progress variables for this target.
include unittests/Expr/CMakeFiles/ExprTest.dir/progress.make

# Include the compile flags for this target's objects.
include unittests/Expr/CMakeFiles/ExprTest.dir/flags.make

unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o: unittests/Expr/CMakeFiles/ExprTest.dir/flags.make
unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o: ../unittests/Expr/ExprTest.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/beverly/Documents/klee-n-aeg/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o"
	cd /home/beverly/Documents/klee-n-aeg/build/unittests/Expr && /usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/ExprTest.dir/ExprTest.cpp.o -c /home/beverly/Documents/klee-n-aeg/unittests/Expr/ExprTest.cpp

unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/ExprTest.dir/ExprTest.cpp.i"
	cd /home/beverly/Documents/klee-n-aeg/build/unittests/Expr && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/beverly/Documents/klee-n-aeg/unittests/Expr/ExprTest.cpp > CMakeFiles/ExprTest.dir/ExprTest.cpp.i

unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/ExprTest.dir/ExprTest.cpp.s"
	cd /home/beverly/Documents/klee-n-aeg/build/unittests/Expr && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/beverly/Documents/klee-n-aeg/unittests/Expr/ExprTest.cpp -o CMakeFiles/ExprTest.dir/ExprTest.cpp.s

unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o.requires:

.PHONY : unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o.requires

unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o.provides: unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o.requires
	$(MAKE) -f unittests/Expr/CMakeFiles/ExprTest.dir/build.make unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o.provides.build
.PHONY : unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o.provides

unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o.provides.build: unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o


unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o: unittests/Expr/CMakeFiles/ExprTest.dir/flags.make
unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o: ../unittests/Expr/ArrayExprTest.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/beverly/Documents/klee-n-aeg/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building CXX object unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o"
	cd /home/beverly/Documents/klee-n-aeg/build/unittests/Expr && /usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o -c /home/beverly/Documents/klee-n-aeg/unittests/Expr/ArrayExprTest.cpp

unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.i"
	cd /home/beverly/Documents/klee-n-aeg/build/unittests/Expr && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/beverly/Documents/klee-n-aeg/unittests/Expr/ArrayExprTest.cpp > CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.i

unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.s"
	cd /home/beverly/Documents/klee-n-aeg/build/unittests/Expr && /usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/beverly/Documents/klee-n-aeg/unittests/Expr/ArrayExprTest.cpp -o CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.s

unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o.requires:

.PHONY : unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o.requires

unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o.provides: unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o.requires
	$(MAKE) -f unittests/Expr/CMakeFiles/ExprTest.dir/build.make unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o.provides.build
.PHONY : unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o.provides

unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o.provides.build: unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o


# Object files for target ExprTest
ExprTest_OBJECTS = \
"CMakeFiles/ExprTest.dir/ExprTest.cpp.o" \
"CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o"

# External object files for target ExprTest
ExprTest_EXTERNAL_OBJECTS =

unittests/ExprTest: unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o
unittests/ExprTest: unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o
unittests/ExprTest: unittests/Expr/CMakeFiles/ExprTest.dir/build.make
unittests/ExprTest: lib/libgtest_main.a
unittests/ExprTest: lib/libkleaverExpr.a
unittests/ExprTest: lib/libkleeSupport.a
unittests/ExprTest: lib/libkleaverSolver.a
unittests/ExprTest: lib/libgtest.a
unittests/ExprTest: lib/libkleaverExpr.a
unittests/ExprTest: lib/libkleeSupport.a
unittests/ExprTest: lib/libkleeBasic.a
unittests/ExprTest: /usr/lib/x86_64-linux-gnu/libz.so
unittests/ExprTest: /usr/lib/x86_64-linux-gnu/libtcmalloc.so
unittests/ExprTest: /usr/lib/llvm-9/lib/libLLVM-9.so
unittests/ExprTest: /home/beverly/Documents/tracerX/stp/build/lib/libstp.a
unittests/ExprTest: /usr/lib/x86_64-linux-gnu/libboost_program_options.a
unittests/ExprTest: /home/beverly/Documents/tracerX/minisat/build/release/lib/libminisat.a
unittests/ExprTest: /usr/lib/libz3.so
unittests/ExprTest: unittests/Expr/CMakeFiles/ExprTest.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/home/beverly/Documents/klee-n-aeg/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Linking CXX executable ../ExprTest"
	cd /home/beverly/Documents/klee-n-aeg/build/unittests/Expr && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/ExprTest.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
unittests/Expr/CMakeFiles/ExprTest.dir/build: unittests/ExprTest

.PHONY : unittests/Expr/CMakeFiles/ExprTest.dir/build

unittests/Expr/CMakeFiles/ExprTest.dir/requires: unittests/Expr/CMakeFiles/ExprTest.dir/ExprTest.cpp.o.requires
unittests/Expr/CMakeFiles/ExprTest.dir/requires: unittests/Expr/CMakeFiles/ExprTest.dir/ArrayExprTest.cpp.o.requires

.PHONY : unittests/Expr/CMakeFiles/ExprTest.dir/requires

unittests/Expr/CMakeFiles/ExprTest.dir/clean:
	cd /home/beverly/Documents/klee-n-aeg/build/unittests/Expr && $(CMAKE_COMMAND) -P CMakeFiles/ExprTest.dir/cmake_clean.cmake
.PHONY : unittests/Expr/CMakeFiles/ExprTest.dir/clean

unittests/Expr/CMakeFiles/ExprTest.dir/depend:
	cd /home/beverly/Documents/klee-n-aeg/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/beverly/Documents/klee-n-aeg /home/beverly/Documents/klee-n-aeg/unittests/Expr /home/beverly/Documents/klee-n-aeg/build /home/beverly/Documents/klee-n-aeg/build/unittests/Expr /home/beverly/Documents/klee-n-aeg/build/unittests/Expr/CMakeFiles/ExprTest.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : unittests/Expr/CMakeFiles/ExprTest.dir/depend

