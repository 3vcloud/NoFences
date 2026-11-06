cmake_minimum_required(VERSION 3.20)
project(NoFences CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Each platform builds into its own folder under bin/, so the Windows and Linux executables never mix.
if(WIN32)
    set(NOFENCES_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin/windows")
else()
    set(NOFENCES_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin/linux")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${NOFENCES_BIN_DIR}")

# The window layer is the WebViewWindow submodule. Its example isn't built here.
set(WEBVIEW_WINDOW_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/WebViewWindow)

# The fence page is compiled in from frontend/index.html.
set(NOFENCES_PAGE "${CMAKE_CURRENT_SOURCE_DIR}/frontend/index.html")
set(NOFENCES_PAGE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedPage.cpp")
add_custom_command(OUTPUT "${NOFENCES_PAGE_SOURCE}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${NOFENCES_PAGE} -DOUTPUT=${NOFENCES_PAGE_SOURCE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS "${NOFENCES_PAGE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake"
    COMMENT "Embedding the fence page"
    VERBATIM)

add_executable(NoFences
    src/main.cpp
    src/Entries.cpp
    src/Fence.cpp
    src/FenceApp.cpp
    src/FenceWindow.cpp
    src/Text.cpp
    src/Xml.cpp
    "${NOFENCES_PAGE_SOURCE}")
target_include_directories(NoFences PRIVATE include)
target_compile_options(NoFences PRIVATE -Wall -Wextra)
target_link_libraries(NoFences PRIVATE webview_window)

if(WIN32)
    target_sources(NoFences PRIVATE platform/windows/Desktop.cpp platform/windows/DirectoryWatch.cpp)
    target_link_libraries(NoFences PRIVATE user32 gdi32 ole32 shell32 advapi32 gdiplus uuid)
    target_link_options(NoFences PRIVATE -static -mwindows)
    # WebView2 is loaded at runtime from WebView2Loader.dll, which is copied beside the executable.
    add_custom_command(TARGET NoFences POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/WebViewWindow/third_party/webview2/bin/x64/WebView2Loader.dll"
                "$<TARGET_FILE_DIR:NoFences>"
        VERBATIM)
else()
    # GTK and WebKitGTK are opened at runtime, so only their headers are needed to build, and the window layer brings those.
    find_package(Threads REQUIRED)
    target_sources(NoFences PRIVATE platform/linux/Desktop.cpp platform/linux/DirectoryWatch.cpp)
    target_link_libraries(NoFences PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

    # A Linux executable can't carry an icon, so a desktop entry beside it lets the file manager and autostart start NoFences.
    file(MAKE_DIRECTORY "${NOFENCES_BIN_DIR}")
    file(WRITE "${NOFENCES_BIN_DIR}/NoFences.desktop"
         "[Desktop Entry]\nType=Application\nName=NoFences\nComment=Desktop fences for files, folders and links\n"
         "Exec=${NOFENCES_BIN_DIR}/NoFences\nTerminal=false\nCategories=Utility;\n")
endif()
