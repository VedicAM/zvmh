cmake_minimum_required(VERSION 3.10.0)
project(zvmh VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

find_package(cpr REQUIRED)

add_subdirectory(vendor/json)

add_executable(zvmh src/main.cpp
    src/agent.cpp
    src/agent.h
    src/auth/ClaudeAuth.cpp
    src/auth/ClaudeAuth.h
    src/auth/CodexAuth.cpp
    src/auth/CodexAuth.h
    src/message/message.h
    src/provider/Provider.h
    src/provider/OpenRouter.cpp
    src/provider/OpenRouter.h
    src/provider/Claude.cpp
    src/provider/Claude.h
    src/provider/Codex.cpp
    src/provider/Codex.h
    src/tool/tool.h
    src/tool/registry.h
    src/tool/bash.h
    src/tool/edit.h
    src/tool/glob.h
    src/tool/grep.h
    src/tool/ls.h
    src/tool/read.h
    src/tool/write.h)

target_link_libraries(zvmh PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(zvmh PRIVATE cpr::cpr)