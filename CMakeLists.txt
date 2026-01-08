cmake_minimum_required(VERSION 3.28)
project(vibecut LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(LIBAV REQUIRED libavformat libavcodec libswscale libavutil)
find_package(OpenGL REQUIRED)

# ImGui
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC 
    ${IMGUI_DIR} 
    ${IMGUI_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
)
target_link_libraries(imgui PUBLIC ${GLFW_LIBRARIES} OpenGL::GL)

# Font embedding tool
add_executable(binary_to_compressed_c EXCLUDE_FROM_ALL
    ${IMGUI_DIR}/misc/fonts/binary_to_compressed_c.cpp
)

set(FONT_INPUT ${IMGUI_DIR}/misc/fonts/Roboto-Medium.ttf)
set(FONT_OUTPUT ${CMAKE_BINARY_DIR}/generated/embedded_font.h)

add_custom_command(
    OUTPUT ${FONT_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND binary_to_compressed_c -base85 ${FONT_INPUT} RobotoMedium > ${FONT_OUTPUT}
    DEPENDS binary_to_compressed_c ${FONT_INPUT}
    COMMENT "Generating embedded font header"
)
add_custom_target(embedded_font DEPENDS ${FONT_OUTPUT})

# Main executable
add_executable(vibecut src/main.cpp ${FONT_OUTPUT})
add_dependencies(vibecut embedded_font)
target_include_directories(vibecut PRIVATE ${GLFW_INCLUDE_DIRS} ${LIBAV_INCLUDE_DIRS} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(vibecut PRIVATE imgui ${GLFW_LIBRARIES} ${LIBAV_LIBRARIES} OpenGL::GL)
