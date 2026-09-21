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

# Neon (и, видимо, некоторые другие serverless/pooled Postgres) обрывает
# соединение после пятого запроса подряд без паузы - экспериментально
# подтверждено отдельными диагностическими сборками (tools/tlsdiag.c),
# причём это не зависит от содержимого запросов. QPSQLDriver::open() в
# самом Qt всегда шлёт 5 служебных запросов сразу после подключения
# (SELECT version(), проверка экранирования, SET CLIENT_ENCODING,
# SET DATESTYLE, SET bytea_output) - и это ломает вообще любое подключение
# к такому серверу, ещё до того, как наш код получит управление. Обхода
# через публичный API Qt нет. Поэтому при наличии приватных заголовков Qt
# (SqlPrivate/CorePrivate) и libpq мы компилируем исходник qt-плагина QPSQL
# (src/thirdparty/qpsql/) прямо в приложение и используем его конструктор
# QPSQLDriver(PGconn*), который принимает уже открытое НАМИ соединение и
# выполняет только 2 из этих пяти запросов - подробности в database.cpp.
find_package(Qt6 QUIET COMPONENTS SqlPrivate CorePrivate)
find_package(PostgreSQL QUIET)
if(Qt6SqlPrivate_FOUND AND Qt6CorePrivate_FOUND AND PostgreSQL_FOUND)
    set(QPSQL_EMBEDDED_AVAILABLE TRUE)
    message(STATUS "QPSQL embedded driver: доступен (SqlPrivate + CorePrivate + libpq найдены)")
else()
    set(QPSQL_EMBEDDED_AVAILABLE FALSE)
    message(STATUS "QPSQL embedded driver: недоступен, используется обычный QSqlDatabase::addDatabase(\"QPSQL\")")
endif()

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
    src/serverconfig.cpp
    src/connectiondialog.cpp
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

if(QPSQL_EMBEDDED_AVAILABLE)
    target_sources(inventory_manager PRIVATE src/thirdparty/qpsql/qsql_psql.cpp)
    target_include_directories(inventory_manager PRIVATE ${PostgreSQL_INCLUDE_DIRS})
    target_link_libraries(inventory_manager PRIVATE
        Qt6::SqlPrivate
        Qt6::CorePrivate
        ${PostgreSQL_LIBRARIES}
    )
    target_compile_definitions(inventory_manager PRIVATE QPSQL_EMBEDDED_AVAILABLE)
    # QT_PLUGIN здесь не означает "мы собираем плагин" - оно лишь отключает
    # dllimport/dllexport-атрибуты (Q_EXPORT_SQLDRIVER_PSQL) на классе
    # QPSQLDriver, чтобы он стал обычным символом внутри нашего exe, а не
    # ожидал импорта из отдельной qsqlpsql.dll.
    set_source_files_properties(src/thirdparty/qpsql/qsql_psql.cpp PROPERTIES
        COMPILE_DEFINITIONS QT_PLUGIN)
endif()

# Диагностический консольный инструмент: тот же QSqlDatabase/QSqlQuery путь,
# что и в Database::open(), но отдельно и с подробным выводом - для отладки
# проблем подключения к серверу без запуска всего приложения.
add_executable(qtpgtest tools/qtpgtest.cpp)
target_link_libraries(qtpgtest PRIVATE Qt6::Core Qt6::Sql)
