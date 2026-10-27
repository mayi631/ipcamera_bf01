cmake_minimum_required(VERSION 3.10)
if(GCC_CROSS_COMPILE)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_C_COMPILER "${GCC_CROSS_COMPILE}gcc")
endif()
project(sample_rtsp VERSION 0.0.1 LANGUAGES C)

set(RTSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "RTSP component")
set(COMMON_DIR "${RTSP_DIR}/../common" CACHE PATH "Common headers")
set(CVI_OSAL_DIR "${RTSP_DIR}/../cvi_osal" CACHE PATH "CVI OSAL component")
set(RINGBUFFER_DIR "${RTSP_DIR}/../ringbuffer" CACHE PATH "Ring buffer component")
option(RTSP_ENABLE_RECV "Build RTSP receive client sample" ON)
find_package(Threads REQUIRED)

set(SAMPLES sample_rtsp_server)
if(RTSP_ENABLE_RECV)
    list(APPEND SAMPLES sample_rtsp_client)
endif()
foreach(sample ${SAMPLES})
    add_executable(${sample} ${sample}.c)
    add_executable(${sample}_static ${sample}.c)
    foreach(target ${sample} ${sample}_static)
        target_include_directories(${target} PRIVATE "${COMMON_DIR}" "${RTSP_DIR}/include"
            "${CVI_OSAL_DIR}/include" "${RINGBUFFER_DIR}/include")
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    endforeach()
    target_link_libraries(${sample} PRIVATE
        "${RTSP_DIR}/lib/libcomp_rtsp.so"
        "${RINGBUFFER_DIR}/lib/libcvi_comp_ringbuffer.so"
        "${CVI_OSAL_DIR}/lib/libcvi_comp_osal.so" Threads::Threads)
    target_link_libraries(${sample}_static PRIVATE
        "${RTSP_DIR}/lib/libcomp_rtsp.a"
        "${RINGBUFFER_DIR}/lib/libcvi_comp_ringbuffer.a"
        "${CVI_OSAL_DIR}/lib/libcvi_comp_osal.a" Threads::Threads)
    set_target_properties(${sample} PROPERTIES OUTPUT_NAME ${sample}.bin)
    set_target_properties(${sample}_static PROPERTIES OUTPUT_NAME ${sample}.bin.static)
    install(TARGETS ${sample} ${sample}_static DESTINATION bin)
endforeach()
