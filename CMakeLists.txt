cmake_minimum_required (VERSION 3.15)
project ("poorftpd")
add_executable (poorftpd "poorftpd.c"  "poorsock.h"  "poorsock.c")
set_target_properties(poorftpd PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")

if(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
	target_link_libraries(poorftpd network)
endif()
