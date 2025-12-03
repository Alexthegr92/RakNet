cmake_minimum_required(VERSION 3.7)
project(RDMAEchoSample)

# Include directories for RakNet headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../Source)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include/RakNet)

# ==================== libfabric for RDMA samples ====================
IF(WIN32 AND NOT UNIX)
	IF(NOT DEFINED ENV{LIBFABRIC_ROOT} OR "$ENV{LIBFABRIC_ROOT}" STREQUAL "")
		MESSAGE(FATAL_ERROR "RDMA samples require LIBFABRIC_ROOT environment variable.")
	ENDIF()
	
	SET(_LF_ROOT "$ENV{LIBFABRIC_ROOT}")
	FIND_PATH(_LF_INCLUDE rdma/fabric.h HINTS "${_LF_ROOT}/include" REQUIRED)
	FIND_LIBRARY(_LF_LIB NAMES libfabric_new libfabric HINTS "${_LF_ROOT}/lib" REQUIRED)
	
	INCLUDE_DIRECTORIES("${_LF_INCLUDE}")
	IF(EXISTS "${_LF_ROOT}/include/windows")
		INCLUDE_DIRECTORIES("${_LF_ROOT}/include/windows")
	ENDIF()
	
	SET(LIBFABRIC_LIBRARY "${_LF_LIB}")
	MESSAGE(STATUS "RDMA samples: using libfabric from ${_LF_ROOT}")
ENDIF()
# ====================================================================

add_executable(rdma_echo_server rdma_echo_server.cpp)
add_executable(rdma_echo_client rdma_echo_client.cpp)

target_link_libraries(rdma_echo_server RakNetLibStatic ${RAKNET_LIBRARY_LIBS})
target_link_libraries(rdma_echo_client RakNetLibStatic ${RAKNET_LIBRARY_LIBS})

# Link libfabric
IF(LIBFABRIC_LIBRARY)
	target_link_libraries(rdma_echo_server ${LIBFABRIC_LIBRARY})
	target_link_libraries(rdma_echo_client ${LIBFABRIC_LIBRARY})
ENDIF()

IF(WIN32 AND NOT UNIX)
	target_compile_definitions(rdma_echo_server PRIVATE WIN32 _RAKNET_LIB _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_DEPRECATE)
	target_compile_definitions(rdma_echo_client PRIVATE WIN32 _RAKNET_LIB _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_DEPRECATE)
	
	# Windows-specific definitions for libfabric
	target_compile_definitions(rdma_echo_server PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE _WINSOCKAPI_=)
	target_compile_definitions(rdma_echo_client PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE _WINSOCKAPI_=)
	
	# Link Windows libraries required by libfabric
	target_link_libraries(rdma_echo_server Iphlpapi Shlwapi Ws2_32)
	target_link_libraries(rdma_echo_client Iphlpapi Shlwapi Ws2_32)
	
	IF(RAKNET_ENABLE_RDMA)
		target_compile_definitions(rdma_echo_server PRIVATE _RAKNET_SUPPORT_RDMAInterface=1 _RAKNET_SUPPORT_PacketizedRDMA=1)
		target_compile_definitions(rdma_echo_client PRIVATE _RAKNET_SUPPORT_RDMAInterface=1 _RAKNET_SUPPORT_PacketizedRDMA=1)
	ENDIF()
	
	# Copy libfabric DLL to output directory after build
	# Use Release build from C:\repos\Libfabric-win if available, otherwise fallback to SDK
	IF(EXISTS "C:/repos/Libfabric-win/x64/Release/libfabric_new.dll")
		SET(_LF_DLL "C:/repos/Libfabric-win/x64/Release/libfabric_new.dll")
		MESSAGE(STATUS "Using Release libfabric_new.dll from build directory")
	ELSE()
		SET(_LF_DLL "${_LF_ROOT}/bin/libfabric_new.dll")
	ENDIF()
	
	IF(EXISTS "${_LF_DLL}")
		add_custom_command(TARGET rdma_echo_server POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${_LF_DLL}" $<TARGET_FILE_DIR:rdma_echo_server>
			COMMENT "Copying libfabric_new.dll to output directory")
		add_custom_command(TARGET rdma_echo_client POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${_LF_DLL}" $<TARGET_FILE_DIR:rdma_echo_client>
			COMMENT "Copying libfabric_new.dll to output directory")
	ELSE()
		MESSAGE(WARNING "libfabric_new.dll not found at ${_LF_DLL}")
	ENDIF()
ENDIF()
