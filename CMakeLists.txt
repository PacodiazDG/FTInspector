cmake_minimum_required(VERSION 3.10)
project(MiProyecto C)

set(CMAKE_C_STANDARD 17) # O la versión de C que prefieras
include_directories(${PROJECT_SOURCE_DIR}/include/ftinspector)
file(GLOB SRC_FILES ${PROJECT_SOURCE_DIR}/src/*.c)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -pie -Wl,-z,relro,-z,now")

add_executable(MiEjecutable ${SRC_FILES})