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
CMAKE_SOURCE_DIR = /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build

# Include any dependencies generated for this target.
include elf-parser/CMakeFiles/elf-parser.dir/depend.make

# Include the progress variables for this target.
include elf-parser/CMakeFiles/elf-parser.dir/progress.make

# Include the compile flags for this target's objects.
include elf-parser/CMakeFiles/elf-parser.dir/flags.make

elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o: elf-parser/CMakeFiles/elf-parser.dir/flags.make
elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o: ../elf-parser/symbols.cc
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o"
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser && g++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/elf-parser.dir/symbols.cc.o -c /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/elf-parser/symbols.cc

elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/elf-parser.dir/symbols.cc.i"
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser && g++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/elf-parser/symbols.cc > CMakeFiles/elf-parser.dir/symbols.cc.i

elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/elf-parser.dir/symbols.cc.s"
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser && g++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/elf-parser/symbols.cc -o CMakeFiles/elf-parser.dir/symbols.cc.s

elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o.requires:

.PHONY : elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o.requires

elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o.provides: elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o.requires
	$(MAKE) -f elf-parser/CMakeFiles/elf-parser.dir/build.make elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o.provides.build
.PHONY : elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o.provides

elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o.provides.build: elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o


elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o: elf-parser/CMakeFiles/elf-parser.dir/flags.make
elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o: ../elf-parser/elf_parser.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building CXX object elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o"
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser && g++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/elf-parser.dir/elf_parser.cpp.o -c /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/elf-parser/elf_parser.cpp

elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/elf-parser.dir/elf_parser.cpp.i"
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser && g++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/elf-parser/elf_parser.cpp > CMakeFiles/elf-parser.dir/elf_parser.cpp.i

elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/elf-parser.dir/elf_parser.cpp.s"
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser && g++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/elf-parser/elf_parser.cpp -o CMakeFiles/elf-parser.dir/elf_parser.cpp.s

elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o.requires:

.PHONY : elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o.requires

elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o.provides: elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o.requires
	$(MAKE) -f elf-parser/CMakeFiles/elf-parser.dir/build.make elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o.provides.build
.PHONY : elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o.provides

elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o.provides.build: elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o


# Object files for target elf-parser
elf__parser_OBJECTS = \
"CMakeFiles/elf-parser.dir/symbols.cc.o" \
"CMakeFiles/elf-parser.dir/elf_parser.cpp.o"

# External object files for target elf-parser
elf__parser_EXTERNAL_OBJECTS =

lib/libelf-parser.so: elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o
lib/libelf-parser.so: elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o
lib/libelf-parser.so: elf-parser/CMakeFiles/elf-parser.dir/build.make
lib/libelf-parser.so: elf-parser/CMakeFiles/elf-parser.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Linking CXX shared library ../lib/libelf-parser.so"
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser && $(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/elf-parser.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
elf-parser/CMakeFiles/elf-parser.dir/build: lib/libelf-parser.so

.PHONY : elf-parser/CMakeFiles/elf-parser.dir/build

elf-parser/CMakeFiles/elf-parser.dir/requires: elf-parser/CMakeFiles/elf-parser.dir/symbols.cc.o.requires
elf-parser/CMakeFiles/elf-parser.dir/requires: elf-parser/CMakeFiles/elf-parser.dir/elf_parser.cpp.o.requires

.PHONY : elf-parser/CMakeFiles/elf-parser.dir/requires

elf-parser/CMakeFiles/elf-parser.dir/clean:
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser && $(CMAKE_COMMAND) -P CMakeFiles/elf-parser.dir/cmake_clean.cmake
.PHONY : elf-parser/CMakeFiles/elf-parser.dir/clean

elf-parser/CMakeFiles/elf-parser.dir/depend:
	cd /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/elf-parser /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser /home/jqhong/klee-nme/klee-nme-aeg/klee-n-aeg/build/elf-parser/CMakeFiles/elf-parser.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : elf-parser/CMakeFiles/elf-parser.dir/depend

