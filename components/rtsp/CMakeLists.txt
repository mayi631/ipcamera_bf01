cmake_minimum_required(VERSION 3.10)

# Also accept the compiler prefix used by the original RTSP component.
if(GCC_CROSS_COMPILE)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_C_COMPILER "${GCC_CROSS_COMPILE}gcc")
endif()
project(rtsp VERSION 0.0.1 LANGUAGES C)

set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common" CACHE PATH "Common headers")
set(CVI_OSAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cvi_osal" CACHE PATH "CVI OSAL component")
set(RINGBUFFER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ringbuffer" CACHE PATH "Ring buffer component")
option(RTSP_ENABLE_RECV "Enable RTSP receive client" ON)
find_package(Threads REQUIRED)

set(SOURCE_CODE_FILES src/rtsp.c src/network.c src/rtp.c)
if(RTSP_ENABLE_RECV)
    list(APPEND SOURCE_CODE_FILES src/rtsp_client.c)
else()
    list(APPEND SOURCE_CODE_FILES src/rtsp_client_stub.c)
endif()

add_library(comp_rtsp SHARED ${SOURCE_CODE_FILES})
add_library(comp_rtsp_static STATIC ${SOURCE_CODE_FILES})
foreach(target comp_rtsp comp_rtsp_static)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
        PRIVATE "${COMMON_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src/inc"
        "${CVI_OSAL_DIR}/include" "${RINGBUFFER_DIR}/include")
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    if(RTSP_ENABLE_RECV)
        target_compile_definitions(${target} PRIVATE RTSP_ENABLE_RECV=1)
    endif()
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME comp_rtsp)
endforeach()
target_link_libraries(comp_rtsp PRIVATE
    "${RINGBUFFER_DIR}/lib/libcvi_comp_ringbuffer.so"
    "${CVI_OSAL_DIR}/lib/libcvi_comp_osal.so" Threads::Threads)
set_target_properties(comp_rtsp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

install(TARGETS comp_rtsp comp_rtsp_static DESTINATION lib)
install(FILES include/rtsp.h DESTINATION include)
