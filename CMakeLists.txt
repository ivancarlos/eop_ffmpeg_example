cmake_minimum_required(VERSION 3.16)
project(get_frame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED libavformat>=58 libavcodec>=58 libavutil>=56
                  libswscale>=5)

add_executable(get_frame get_frame.cpp)
target_include_directories(get_frame PRIVATE ${LIBAV_INCLUDE_DIRS})
target_link_libraries(get_frame PRIVATE ${LIBAV_LIBRARIES})
