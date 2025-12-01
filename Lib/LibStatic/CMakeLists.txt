cmake_minimum_required(VERSION 3.7)
project(RakNetLibStatic)

FILE(GLOB ALL_HEADER_SRCS ${RakNet_SOURCE_DIR}/Source/*.h)
FILE(GLOB ALL_CPP_SRCS ${RakNet_SOURCE_DIR}/Source/*.cpp)

include_directories( ${RAKNET_INTERNAL_INCLUDE_DIRS} ) 

option(RAKNET_ENABLE_RDMA "Enable RDMA support" OFF)

# ==================== libfabric integration for RDMA support ====================
IF(RAKNET_ENABLE_RDMA)
	IF(WIN32 AND NOT UNIX)
		# Use LIBFABRIC_ROOT environment variable (set to C:\SDKs\libfabric-win)
		IF(NOT DEFINED ENV{LIBFABRIC_ROOT} OR "$ENV{LIBFABRIC_ROOT}" STREQUAL "")
			MESSAGE(FATAL_ERROR "RAKNET_ENABLE_RDMA=ON requires LIBFABRIC_ROOT environment variable. Set it to your libfabric SDK path (e.g., C:/SDKs/libfabric-win).")
		ENDIF()
		
		SET(_LF_ROOT "$ENV{LIBFABRIC_ROOT}")
		
		# Find headers
		FIND_PATH(_LF_INCLUDE rdma/fabric.h HINTS "${_LF_ROOT}/include" REQUIRED)
		IF(NOT _LF_INCLUDE)
			MESSAGE(FATAL_ERROR "libfabric headers not found in ${_LF_ROOT}/include")
		ENDIF()
		
		# Find library (libfabric_new.lib for monolithic build)
		FIND_LIBRARY(_LF_LIB NAMES libfabric_new libfabric HINTS "${_LF_ROOT}/lib" REQUIRED)
		IF(NOT _LF_LIB)
			MESSAGE(FATAL_ERROR "libfabric library not found in ${_LF_ROOT}/lib")
		ENDIF()
		
		MESSAGE(STATUS "RDMA enabled: libfabric headers at ${_LF_INCLUDE}, library at ${_LF_LIB}")
		
		# Add include directories
		INCLUDE_DIRECTORIES("${_LF_INCLUDE}")
		
		# Check for Windows-specific headers
		IF(EXISTS "${_LF_ROOT}/include/windows")
			INCLUDE_DIRECTORIES("${_LF_ROOT}/include/windows")
		ENDIF()
		
		# Store for linking later
		SET(LIBFABRIC_LIBRARY "${_LF_LIB}" CACHE INTERNAL "")
	ELSE()
		# Linux/Unix: try pkg-config or manual search
		FIND_PACKAGE(PkgConfig QUIET)
		IF(PkgConfig_FOUND)
			PKG_CHECK_MODULES(LIBFABRIC QUIET libfabric)
			IF(LIBFABRIC_FOUND)
				INCLUDE_DIRECTORIES(${LIBFABRIC_INCLUDE_DIRS})
				SET(LIBFABRIC_LIBRARY "${LIBFABRIC_LIBRARIES}" CACHE INTERNAL "")
			ENDIF()
		ENDIF()
		
		IF(NOT LIBFABRIC_FOUND)
			FIND_PATH(LIBFABRIC_INCLUDE_DIR rdma/fabric.h HINTS "$ENV{LIBFABRIC_ROOT}/include" "/usr/include" "/usr/local/include")
			FIND_LIBRARY(LIBFABRIC_LIBRARY NAMES fabric libfabric HINTS "$ENV{LIBFABRIC_ROOT}/lib" "/usr/lib" "/usr/local/lib")
			
			IF(LIBFABRIC_INCLUDE_DIR AND LIBFABRIC_LIBRARY)
				INCLUDE_DIRECTORIES("${LIBFABRIC_INCLUDE_DIR}")
				MESSAGE(STATUS "RDMA enabled: libfabric found at ${LIBFABRIC_INCLUDE_DIR}, ${LIBFABRIC_LIBRARY}")
			ELSE()
				MESSAGE(FATAL_ERROR "RAKNET_ENABLE_RDMA=ON but libfabric not found. Install libfabric or set LIBFABRIC_ROOT.")
			ENDIF()
		ENDIF()
	ENDIF()
ENDIF()
# ================================================================================

add_library(RakNetLibStatic STATIC ${ALL_CPP_SRCS} ${ALL_HEADER_SRCS} readme.txt)

IF(WIN32 AND NOT UNIX)
	SET( CMAKE_CXX_FLAGS "/D WIN32 /D _RAKNET_LIB /D _CRT_NONSTDC_NO_DEPRECATE /D _CRT_SECURE_NO_DEPRECATE /GS- /GR- ")
	
	# Add RDMA-specific definitions to the target
	IF(RAKNET_ENABLE_RDMA)
		target_compile_definitions(RakNetLibStatic PRIVATE _RAKNET_SUPPORT_RDMAInterface=1 _RAKNET_SUPPORT_PacketizedRDMA=1)
		MESSAGE(STATUS "Adding RDMA compile definitions to RakNetLibStatic")
	ENDIF()
ENDIF(WIN32 AND NOT UNIX)

IF(WIN32 AND NOT UNIX)
	target_link_libraries (RakNetLibStatic ${RAKNET_LIBRARY_LIBS})
	
	# Link libfabric if RDMA is enabled
	IF(RAKNET_ENABLE_RDMA AND LIBFABRIC_LIBRARY)
		target_link_libraries (RakNetLibStatic ${LIBFABRIC_LIBRARY})
		MESSAGE(STATUS "Linking RakNetLibStatic with libfabric: ${LIBFABRIC_LIBRARY}")
	ENDIF()

	IF(NOT ${CMAKE_GENERATOR} STREQUAL "MSYS Makefiles")

		IF( MSVC_VERSION GREATER_EQUAL 1600 )
			set_target_properties(RakNetLibStatic PROPERTIES STATIC_LIBRARY_FLAGS "/NODEFAULTLIB:\"LIBCD.lib LIBCMTD.lib MSVCRT.lib\"" )
		ELSE()
			set_target_properties(RakNetLibStatic PROPERTIES STATIC_LIBRARY_FLAGS "/NODEFAULTLIB:&quot;LIBCD.lib LIBCMTD.lib MSVCRT.lib&quot;" )
		ENDIF()
				
	ENDIF(NOT ${CMAKE_GENERATOR} STREQUAL "MSYS Makefiles")

ELSE(WIN32 AND NOT UNIX)
	target_link_libraries (RakNetLibStatic ${RAKNET_LIBRARY_LIBS})
	
	# Link libfabric if RDMA is enabled
	IF(RAKNET_ENABLE_RDMA AND LIBFABRIC_LIBRARY)
		target_link_libraries (RakNetLibStatic ${LIBFABRIC_LIBRARY})
		MESSAGE(STATUS "Linking RakNetLibStatic with libfabric: ${LIBFABRIC_LIBRARY}")
	ENDIF()
	
	INSTALL(TARGETS RakNetLibStatic DESTINATION ${RakNet_SOURCE_DIR}/Lib/RakNetLibStatic)
	INSTALL(FILES ${ALL_HEADER_SRCS} DESTINATION ${RakNet_SOURCE_DIR}/include/raknet)
ENDIF(WIN32 AND NOT UNIX)


