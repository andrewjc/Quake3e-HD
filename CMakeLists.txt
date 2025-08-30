CMAKE_MINIMUM_REQUIRED(VERSION 3.12)

PROJECT(quake3e)

SET(CNAME "quake3e")
SET(DNAME "quake3e.ded")

SET(RENDERER_PREFIX ${CNAME})
SET(RENDERER_DEFAULT vulkan) # valid options: vulkan

OPTION(USE_SDL "" ON)
OPTION(USE_CURL "" ON)
OPTION(USE_LOCAL_HEADERS "" ON)
OPTION(USE_VULKAN "" ON)
OPTION(USE_SYSTEM_JPEG "" OFF)
OPTION(USE_RENDERER_DLOPEN "" ON)

SET(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules)

# Common sources from various directories
file(GLOB QCOMMON_SRCS 
  src/engine/core/*.c
  src/engine/common/*.c
  src/engine/common/math/*.c
  src/engine/common/compression/*.c
  src/engine/common/crypto/*.c
  src/engine/filesystem/*.c
  src/engine/network/*.c
  src/engine/collision/*.c
  src/engine/vm/*.c
)
# exclude platform-dependent QVM bytecode compilers
list(FILTER QCOMMON_SRCS EXCLUDE REGEX ".*vm_[alx].*.c")
# add platform-dependent QVM bytecode compilers
IF(CMAKE_SYSTEM_PROCESSOR MATCHES AMD64|x86|i*86)
	IF(CMAKE_SYSTEM_PROCESSOR MATCHES AMD64)
		IF(WIN32)
			SET(BINEXT .x64)
		ELSE()
			SET(BINEXT .x86_64)
		ENDIF()
		SET(RENDEXT _x86_64)
	ELSE()
		#SET(BINEXT .x86)
		SET(RENDEXT _x86)
	ENDIF()
	list(APPEND QCOMMON_SRCS src/engine/vm/vm_x86.c)
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES aarch64|arm64)
	SET(BINEXT .aarch64)
	SET(RENDEXT _aarch64)
	list(APPEND QCOMMON_SRCS src/engine/vm/vm_aarch64.c)
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES arm*)
	SET(BINEXT .arm)
	SET(RENDEXT _arm)
	list(APPEND QCOMMON_SRCS src/engine/vm/vm_armv7l.c)
ELSE()
	TARGET_COMPILE_DEFINITIONS(qcommon PRIVATE -DNO_VM_COMPILED)
	TARGET_COMPILE_DEFINITIONS(qcommon_ded PRIVATE -DNO_VM_COMPILED)
ENDIF()

# botlib (AI)
file(GLOB BOTLIB_SRCS
  src/engine/ai/*.c
  src/engine/ai/navigation/*.c
  src/engine/ai/behavior/*.c
  src/engine/ai/util/*.c
)
ADD_LIBRARY(botlib OBJECT ${BOTLIB_SRCS})
TARGET_COMPILE_DEFINITIONS(botlib PRIVATE BOTLIB)

file(GLOB SERVER_SRCS
  src/game/server/*.c
  src/game/server/network/*.c
  src/game/server/world/*.c
)
list(REMOVE_ITEM SERVER_SRCS src/game/server/sv_rankings.c)

# universal binary
ADD_LIBRARY(qcommon OBJECT ${QCOMMON_SRCS} ${SERVER_SRCS})

# dedicated server
ADD_LIBRARY(qcommon_ded OBJECT ${QCOMMON_SRCS} ${SERVER_SRCS})
TARGET_COMPILE_DEFINITIONS(qcommon_ded PUBLIC DEDICATED)

# client + cURL
file(GLOB CLIENT_SRCS
  src/game/client/*.c
  src/game/client/input/*.c
  src/game/client/media/*.c
  src/game/client/network/*.c
  src/game/client/ui/*.c
  src/engine/audio/*.c
  src/engine/audio/codecs/*.c
)
IF(NOT USE_CURL)
	list(REMOVE_ITEM CLIENT_SRCS src/game/client/network/cl_curl.c)
ENDIF()

#ADD_LIBRARY(client OBJECT ${CLIENT_SRCS})

# jpeg
IF(USE_SYSTEM_JPEG)
	ADD_LIBRARY(client OBJECT ${CLIENT_SRCS})
	find_package(JPEG REQUIRED)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE -DUSE_SYSTEM_JPEG)
	TARGET_LINK_LIBRARIES(client ${JPEG_LIBRARIES})
ELSE()
	AUX_SOURCE_DIRECTORY(src/contrib/libjpeg JPEG_SRCS)
	#ADD_LIBRARY(jpeg ${JPEG_SRCS})
	#TARGET_LINK_LIBRARIES(client jpeg)
	ADD_LIBRARY(client OBJECT ${CLIENT_SRCS} ${JPEG_SRCS})
ENDIF()

# renderers
file(GLOB RENDERER_SRCS
  src/engine/renderer/*.c
  src/engine/renderer/effects/*.c
  src/engine/renderer/images/*.c
  src/engine/renderer/lighting/*.c
  src/engine/renderer/models/*.c
  src/engine/renderer/shading/*.c
  src/engine/renderer/text/*.c
  src/engine/renderer/vulkan/*.c
  src/engine/renderer/world/*.c
)

IF(USE_RENDERER_DLOPEN)
	SET(AUX_SRCS
		src/engine/common/q_shared.c
		src/engine/common/compression/puff.c
		src/engine/common/math/q_math.c
	)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_RENDERER_DLOPEN RENDERER_PREFIX="${RENDERER_PREFIX}" RENDERER_DEFAULT="${RENDERER_DEFAULT}")

	ADD_LIBRARY(${RENDERER_PREFIX}_vulkan${RENDEXT} SHARED ${RENDERER_SRCS} ${AUX_SRCS})
	TARGET_COMPILE_DEFINITIONS(${RENDERER_PREFIX}_vulkan${RENDEXT} PRIVATE USE_RENDERER_DLOPEN)
ELSE()
	ADD_LIBRARY(${RENDERER_PREFIX}_vulkan OBJECT ${RENDERER_SRCS})
ENDIF()

# common function, window system
IF(UNIX)
	SET (Q3_SRCS
		src/platform/unix/unix_main.c
		src/platform/unix/unix_shared.c
		src/platform/unix/linux_signals.c)

	IF(USE_SDL)
		AUX_SOURCE_DIRECTORY(src/platform/sdl Q3_UI_SRCS)
	ELSE()
		SET(Q3_UI_SRCS
			src/platform/unix/linux_glimp.c
			src/platform/unix/linux_qgl.c
			src/platform/unix/linux_snd.c
			src/platform/unix/x11_dga.c
			src/platform/unix/x11_randr.c
			src/platform/unix/x11_vidmode.c)
		IF(USE_VULKAN)
			list(APPEND Q3_UI_SRCS src/platform/unix/linux_qvk.c)
		ENDIF()
	ENDIF()

	IF(APPLE)
		SET(EXE_TYPE MACOSX_BUNDLE)
		list(APPEND Q3_SRCS src/platform/unix/quake3_flat.icns)
		set_source_files_properties(src/platform/unix/quake3_flat.icns PROPERTIES
			MACOSX_PACKAGE_LOCATION Resources)
		SET(MACOSX_BUNDLE_ICON_FILE quake3_flat.icns)
	ENDIF()
ELSEIF(WIN32)
	SET(EXE_TYPE WIN32)
	IF(MSVC)
		SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
		ADD_COMPILE_DEFINITIONS(_CRT_SECURE_NO_WARNINGS) # v3.12+
	ENDIF()

	SET(Q3_SRCS
		src/platform/windows/win_main.c
		src/platform/windows/win_shared.c
		src/platform/windows/win_syscon.c
		src/platform/windows/win_resource.rc)

	SET(Q3_UI_SRCS
		src/platform/windows/win_gamma.c
		src/platform/windows/win_glimp.c
		src/platform/windows/win_input.c
		src/platform/windows/win_minimize.c
		src/platform/windows/win_qgl.c
		src/platform/windows/win_snd.c
		src/platform/windows/win_wndproc.c)
	IF(USE_VULKAN)
		list(APPEND Q3_UI_SRCS src/platform/windows/win_qvk.c)
	ENDIF()
ENDIF(UNIX)

ADD_LIBRARY(q3ui OBJECT ${Q3_UI_SRCS})

IF(NOT MSVC)
	IF(USE_SDL)
		find_package(SDL2 REQUIRED)
		TARGET_LINK_LIBRARIES(q3ui SDL2::Main)
	ELSEIF(UNIX)
		find_package (X11 REQUIRED)
		TARGET_LINK_LIBRARIES(q3ui ${X11_LIBRARIES})
	ENDIF()
ENDIF()

IF(USE_VULKAN)
	TARGET_COMPILE_DEFINITIONS(q3ui PRIVATE USE_VULKAN_API)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_VULKAN_API)
ENDIF()

# client executable

ADD_EXECUTABLE(${CNAME}${BINEXT} ${EXE_TYPE} ${Q3_SRCS})
TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} qcommon botlib client q3ui)
#TARGET_LINK_OPTIONS(${CNAME} PRIVATE -fno-pie) # v3.13+

IF(NOT USE_RENDERER_DLOPEN)
	TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} ${RENDERER_PREFIX}_vulkan)
ENDIF()

# dedicated server executable

ADD_EXECUTABLE(${DNAME}${BINEXT} ${EXE_TYPE} ${Q3_SRCS})
TARGET_LINK_LIBRARIES(${DNAME}${BINEXT} qcommon_ded botlib)
#TARGET_LINK_OPTIONS(${DNAME} PRIVATE -fno-pie) # v3.13+

IF(WIN32)
	TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} winmm comctl32 ws2_32)
	TARGET_LINK_LIBRARIES(${DNAME}${BINEXT} winmm comctl32 ws2_32)
ELSE()
	TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} m ${CMAKE_DL_LIBS})
	TARGET_LINK_LIBRARIES(${DNAME}${BINEXT} m ${CMAKE_DL_LIBS})
ENDIF()
