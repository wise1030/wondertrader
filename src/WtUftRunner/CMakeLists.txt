#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

#2. 确定工程名
PROJECT(WtUftRunner LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/WtUftRunner)

#7. 添加源码
file(GLOB SRCS *.cpp)
file(GLOB HDRS *.h)

IF(MSVC)
	list (APPEND SRCS ../Common/mdump.cpp)
ENDIF()

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})

SET(LIBS
	WtUftCore
	WTSTools
    WTSUtils
	)

IF (MSVC)
	LIST(APPEND LIBS ws2_32)
ELSE(GNUCC)
	LIST(APPEND LIBS
		dl
		pthread
		boost_filesystem
		boost_thread)
	IF(WIN32)
		LIST(APPEND LIBS
			ws2_32 iconv)
	ENDIF()
ENDIF()

ADD_EXECUTABLE(WtUftRunner ${SRCS} ${HDRS})
TARGET_LINK_LIBRARIES(WtUftRunner ${LIBS})

IF (MSVC)
ELSE (GNUCC)
	SET_TARGET_PROPERTIES(WtUftRunner PROPERTIES
        LINK_FLAGS_RELEASE -s
        # 部署自包含: 优先 $ORIGIN(同目录 libWtUftCore.so), 保留 /home/mydeps/lib
        # 兜底本机构建树运行解析 boost; 不写构建机绝对路径(远程不存在导致裸启动失败,
        # 见 WtFutuCore/AGENTS.md §20.2/§20.3)
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "\${ORIGIN};/home/mydeps/lib")
ENDIF ()

file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/parsers/)
file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/traders/)

add_custom_command(TARGET WtUftRunner POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E
	copy ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/${PREFIX}TraderCTP${SUFFIX} ${EXECUTABLE_OUTPUT_PATH}/traders/
    
    COMMAND ${CMAKE_COMMAND} -E
	copy ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin/${PREFIX}ParserCTP${SUFFIX} ${EXECUTABLE_OUTPUT_PATH}/parsers/

	COMMENT "depends modules copied")


