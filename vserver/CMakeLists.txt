cmake_minimum_required(VERSION 3.25)
project(vserver LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(FFMPEG_ROOT "" CACHE PATH "Cartella di una build shared LGPL di FFmpeg")
# Runtime C++ statico: gli eseguibili partono anche su una macchina senza Visual C++ installato.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Codice di terze parti: va creato prima delle opzioni severe del progetto.
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=2 SQLITE_DQS=0 SQLITE_OMIT_DEPRECATED)

if(MSVC)
  add_compile_options(/W4 /WX /utf-8)
endif()

add_library(vserver_core INTERFACE)
target_include_directories(vserver_core INTERFACE src)

add_library(vserver_catalog STATIC src/catalog/catalog.cpp)
target_link_libraries(vserver_catalog PUBLIC vserver_core PRIVATE sqlite3)

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE vserver_core vserver_catalog)
add_test(NAME core_tests COMMAND core_tests)

add_executable(catalog_crash tests/catalog_crash.cpp)
target_link_libraries(catalog_crash PRIVATE vserver_catalog)

if(FFMPEG_ROOT)
  add_library(vserver_record STATIC src/record/mxf_writer.cpp src/play/mxf_reader.cpp)
  target_link_libraries(vserver_record PUBLIC vserver_core)
  target_include_directories(vserver_record PRIVATE "${FFMPEG_ROOT}/include")
  target_link_libraries(vserver_record PUBLIC
    "${FFMPEG_ROOT}/lib/avformat.lib" "${FFMPEG_ROOT}/lib/avcodec.lib" "${FFMPEG_ROOT}/lib/avutil.lib")

  file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
  foreach(tool record_smoke play_smoke flow_smoke make_test_clip)
    add_executable(${tool} tests/${tool}.cpp)
    target_link_libraries(${tool} PRIVATE vserver_record vserver_catalog)
    add_custom_command(TARGET ${tool} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:${tool}>)
  endforeach()
endif()
