# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.17

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:


#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:


# Disable VCS-based implicit rules.
% : %,v


# Disable VCS-based implicit rules.
% : RCS/%


# Disable VCS-based implicit rules.
% : RCS/%,v


# Disable VCS-based implicit rules.
% : SCCS/s.%


# Disable VCS-based implicit rules.
% : s.%


.SUFFIXES: .hpux_make_needs_suffix_list


# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

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
CMAKE_COMMAND = /Applications/CMake.app/Contents/bin/cmake

# The command to remove a file.
RM = /Applications/CMake.app/Contents/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release

# Include any dependencies generated for this target.
include CMakeFiles/parameters.dir/depend.make

# Include the progress variables for this target.
include CMakeFiles/parameters.dir/progress.make

# Include the compile flags for this target's objects.
include CMakeFiles/parameters.dir/flags.make

CMakeFiles/parameters.dir/codegen/output/parameters.cpp.o: CMakeFiles/parameters.dir/flags.make
CMakeFiles/parameters.dir/codegen/output/parameters.cpp.o: ../codegen/output/parameters.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CXX object CMakeFiles/parameters.dir/codegen/output/parameters.cpp.o"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/parameters.dir/codegen/output/parameters.cpp.o -c /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/codegen/output/parameters.cpp

CMakeFiles/parameters.dir/codegen/output/parameters.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/parameters.dir/codegen/output/parameters.cpp.i"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/codegen/output/parameters.cpp > CMakeFiles/parameters.dir/codegen/output/parameters.cpp.i

CMakeFiles/parameters.dir/codegen/output/parameters.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/parameters.dir/codegen/output/parameters.cpp.s"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/codegen/output/parameters.cpp -o CMakeFiles/parameters.dir/codegen/output/parameters.cpp.s

CMakeFiles/parameters.dir/src/util/util.cpp.o: CMakeFiles/parameters.dir/flags.make
CMakeFiles/parameters.dir/src/util/util.cpp.o: ../src/util/util.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building CXX object CMakeFiles/parameters.dir/src/util/util.cpp.o"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/parameters.dir/src/util/util.cpp.o -c /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/util.cpp

CMakeFiles/parameters.dir/src/util/util.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/parameters.dir/src/util/util.cpp.i"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/util.cpp > CMakeFiles/parameters.dir/src/util/util.cpp.i

CMakeFiles/parameters.dir/src/util/util.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/parameters.dir/src/util/util.cpp.s"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/util.cpp -o CMakeFiles/parameters.dir/src/util/util.cpp.s

CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.o: CMakeFiles/parameters.dir/flags.make
CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.o: ../src/util/parameter_parser.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Building CXX object CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.o"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.o -c /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/parameter_parser.cpp

CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.i"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/parameter_parser.cpp > CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.i

CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.s"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/parameter_parser.cpp -o CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.s

CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.o: CMakeFiles/parameters.dir/flags.make
CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.o: ../codegen/output/cmd_parameters.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release/CMakeFiles --progress-num=$(CMAKE_PROGRESS_4) "Building CXX object CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.o"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.o -c /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/codegen/output/cmd_parameters.cpp

CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.i"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/codegen/output/cmd_parameters.cpp > CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.i

CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.s"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/codegen/output/cmd_parameters.cpp -o CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.s

CMakeFiles/parameters.dir/src/util/timer.cpp.o: CMakeFiles/parameters.dir/flags.make
CMakeFiles/parameters.dir/src/util/timer.cpp.o: ../src/util/timer.cpp
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --progress-dir=/Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release/CMakeFiles --progress-num=$(CMAKE_PROGRESS_5) "Building CXX object CMakeFiles/parameters.dir/src/util/timer.cpp.o"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++  $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -o CMakeFiles/parameters.dir/src/util/timer.cpp.o -c /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/timer.cpp

CMakeFiles/parameters.dir/src/util/timer.cpp.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Preprocessing CXX source to CMakeFiles/parameters.dir/src/util/timer.cpp.i"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -E /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/timer.cpp > CMakeFiles/parameters.dir/src/util/timer.cpp.i

CMakeFiles/parameters.dir/src/util/timer.cpp.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green "Compiling CXX source to assembly CMakeFiles/parameters.dir/src/util/timer.cpp.s"
	/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/c++ $(CXX_DEFINES) $(CXX_INCLUDES) $(CXX_FLAGS) -S /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/src/util/timer.cpp -o CMakeFiles/parameters.dir/src/util/timer.cpp.s

# Object files for target parameters
parameters_OBJECTS = \
"CMakeFiles/parameters.dir/codegen/output/parameters.cpp.o" \
"CMakeFiles/parameters.dir/src/util/util.cpp.o" \
"CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.o" \
"CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.o" \
"CMakeFiles/parameters.dir/src/util/timer.cpp.o"

# External object files for target parameters
parameters_EXTERNAL_OBJECTS =

libparameters.a: CMakeFiles/parameters.dir/codegen/output/parameters.cpp.o
libparameters.a: CMakeFiles/parameters.dir/src/util/util.cpp.o
libparameters.a: CMakeFiles/parameters.dir/src/util/parameter_parser.cpp.o
libparameters.a: CMakeFiles/parameters.dir/codegen/output/cmd_parameters.cpp.o
libparameters.a: CMakeFiles/parameters.dir/src/util/timer.cpp.o
libparameters.a: CMakeFiles/parameters.dir/build.make
libparameters.a: CMakeFiles/parameters.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color --switch=$(COLOR) --green --bold --progress-dir=/Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release/CMakeFiles --progress-num=$(CMAKE_PROGRESS_6) "Linking CXX static library libparameters.a"
	$(CMAKE_COMMAND) -P CMakeFiles/parameters.dir/cmake_clean_target.cmake
	$(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/parameters.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
CMakeFiles/parameters.dir/build: libparameters.a

.PHONY : CMakeFiles/parameters.dir/build

CMakeFiles/parameters.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/parameters.dir/cmake_clean.cmake
.PHONY : CMakeFiles/parameters.dir/clean

CMakeFiles/parameters.dir/depend:
	cd /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release /Users/robotics_qi/project_comment/VISUAL_WITH_IMU/HybVIO/cmake-build-release/CMakeFiles/parameters.dir/DependInfo.cmake --color=$(COLOR)
.PHONY : CMakeFiles/parameters.dir/depend

