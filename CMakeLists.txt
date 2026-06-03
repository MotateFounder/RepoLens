cmake_minimum_required(VERSION 3.16)

project(
    RepoLens
    VERSION 0.1.0
    DESCRIPTION "Local repository context retrieval engine"
    LANGUAGES C CXX
)

option(REPOLENS_BUILD_STANDALONE "Build RepoLens as a standalone executable where practical." ON)
option(REPOLENS_ENABLE_AI "Enable optional AI enrichment commands." ON)
option(REPOLENS_ENABLE_API "Enable the local HTTP API server." ON)
option(REPOLENS_USE_SYSTEM_SQLITE "Use a system SQLite runtime instead of the bundled amalgamation." OFF)

add_executable(repolens
    src/apple_interpreter.cpp
    src/build_file_interpreter.cpp
    src/cpp_interpreter.cpp
    src/csharp_interpreter.cpp
    src/devops_interpreter.cpp
    src/file_scanner.cpp
    src/go_interpreter.cpp
    src/interpreter_registry.cpp
    src/jvm_interpreter.cpp
    src/main.cpp
    src/matlab_interpreter.cpp
    src/php_interpreter.cpp
    src/python_interpreter.cpp
    src/r_interpreter.cpp
    src/ruby_interpreter.cpp
    src/rust_interpreter.cpp
    src/shell_interpreter.cpp
    src/sql_interpreter.cpp
    src/sqlite_database.cpp
    src/web_interpreter.cpp
    src/xml_interpreter.cpp
)

if(NOT REPOLENS_USE_SYSTEM_SQLITE)
    target_sources(repolens PRIVATE third_party/sqlite/sqlite3.c)
    target_include_directories(repolens
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
    )
    target_compile_definitions(repolens PRIVATE REPOLENS_EMBEDDED_SQLITE=1)
endif()

target_compile_features(repolens PRIVATE cxx_std_20)

target_include_directories(repolens
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(repolens
    PRIVATE
        REPOLENS_VERSION="${PROJECT_VERSION}"
        REPOLENS_BUILD_STANDALONE=$<BOOL:${REPOLENS_BUILD_STANDALONE}>
        REPOLENS_ENABLE_AI=$<BOOL:${REPOLENS_ENABLE_AI}>
        REPOLENS_ENABLE_API=$<BOOL:${REPOLENS_ENABLE_API}>
)

if(REPOLENS_USE_SYSTEM_SQLITE AND UNIX AND NOT APPLE)
    target_link_libraries(repolens PRIVATE dl)
endif()

if((REPOLENS_ENABLE_AI OR REPOLENS_ENABLE_API) AND WIN32)
    target_link_libraries(repolens PRIVATE ws2_32)
endif()

if(REPOLENS_BUILD_STANDALONE AND MSVC)
    set_property(TARGET repolens PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
