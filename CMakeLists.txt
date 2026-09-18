cmake_minimum_required(VERSION 3.16)
project(InventoryManager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Sql PrintSupport)
find_package(PkgConfig REQUIRED)
pkg_check_modules(QRENCODE REQUIRED libqrencode)

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
)

target_include_directories(inventory_manager PRIVATE ${QRENCODE_INCLUDE_DIRS})
target_link_directories(inventory_manager PRIVATE ${QRENCODE_LIBRARY_DIRS})
target_link_libraries(inventory_manager PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::PrintSupport
    ${QRENCODE_LIBRARIES}
)
