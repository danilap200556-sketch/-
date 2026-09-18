cmake_minimum_required(VERSION 3.16)
project(InventoryManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    # Qt 6.5's qvarlengtharray.h still references the deprecated MSVC STL
    # stdext::checked_array_iterator helpers, which newer MSVC toolsets
    # removed from <iterator> by default. This restores them.
    add_compile_definitions(_HAS_DEPRECATED_ITERATOR_HELPERS=1)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql PrintSupport)

# libqrencode ships pkg-config files on most platforms (Linux packages,
# many vcpkg ports), but not reliably everywhere - fall back to a plain
# header/library search so the build also works where no .pc file exists.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(QRENCODE libqrencode)
endif()

if(NOT QRENCODE_FOUND)
    find_path(QRENCODE_INCLUDE_DIRS qrencode.h)
    find_library(QRENCODE_LIBRARIES NAMES qrencode libqrencode)
    if(NOT QRENCODE_INCLUDE_DIRS OR NOT QRENCODE_LIBRARIES)
        message(FATAL_ERROR "libqrencode not found - install it (e.g. libqrencode-dev, or vcpkg install libqrencode) "
                             "or point CMAKE_PREFIX_PATH at it")
    endif()
endif()

add_executable(inventory_manager
    src/main.cpp
    src/database.cpp
    src/authservice.cpp
    src/logindialog.cpp
    src/mainwindow.cpp
    src/productstab.cpp
    src/productdialog.cpp
    src/warehousestab.cpp
    src/stocktab.cpp
    src/movementdialog.cpp
    src/labelstab.cpp
    src/qrlabel.cpp
    src/formdialog.cpp
    src/csvimport.cpp
    src/importtab.cpp
)

target_include_directories(inventory_manager PRIVATE ${QRENCODE_INCLUDE_DIRS})
target_link_directories(inventory_manager PRIVATE ${QRENCODE_LIBRARY_DIRS})
target_link_libraries(inventory_manager PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::PrintSupport
    ${QRENCODE_LIBRARIES}
)
