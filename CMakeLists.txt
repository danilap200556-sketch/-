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
# До Qt 6.9 цели Qt6::SqlPrivate/Qt6::CorePrivate создаются вместе с
# публичными модулями, отдельных пакетов *Private нет; с 6.9 их нужно
# запрашивать явно. Поэтому проверяем наличие целей, а не пакетов.
find_package(Qt6 QUIET OPTIONAL_COMPONENTS SqlPrivate CorePrivate)
find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
    find_path(PG_CONFIG_H_DIR pg_config.h HINTS ${PostgreSQL_INCLUDE_DIRS})
endif()

option(REQUIRE_QPSQL_EMBEDDED
       "Падать на этапе конфигурации, если встроенный драйвер QPSQL собрать нельзя" OFF)

# src/thirdparty/qpsql - исходник из Qt 6.7.3, он опирается на приватный API
# Qt, который меняется между минорными версиями (напр. в 6.4 другие сигнатуры
# QSqlResultPrivate), поэтому с другой версией Qt его не собрать.
set(_qpsql_qt_ok FALSE)
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.7 AND Qt6_VERSION VERSION_LESS 6.8)
    set(_qpsql_qt_ok TRUE)
endif()

if(_qpsql_qt_ok AND TARGET Qt6::SqlPrivate AND TARGET Qt6::CorePrivate
   AND PostgreSQL_FOUND AND PG_CONFIG_H_DIR)
    set(QPSQL_EMBEDDED_AVAILABLE TRUE)
    message(STATUS "QPSQL embedded driver: доступен")
else()
    set(QPSQL_EMBEDDED_AVAILABLE FALSE)
    set(_qpsql_why "Qt ${Qt6_VERSION} (нужна 6.7.x), Qt6::SqlPrivate=")
    if(TARGET Qt6::SqlPrivate)
        string(APPEND _qpsql_why "есть")
    else()
        string(APPEND _qpsql_why "НЕТ")
    endif()
    string(APPEND _qpsql_why ", Qt6::CorePrivate=")
    if(TARGET Qt6::CorePrivate)
        string(APPEND _qpsql_why "есть")
    else()
        string(APPEND _qpsql_why "НЕТ")
    endif()
    string(APPEND _qpsql_why ", PostgreSQL_FOUND=${PostgreSQL_FOUND}, pg_config.h=${PG_CONFIG_H_DIR}")
    if(REQUIRE_QPSQL_EMBEDDED)
        message(FATAL_ERROR "QPSQL embedded driver недоступен (${_qpsql_why}) - без него подключение к Neon не работает")
    endif()
    message(STATUS "QPSQL embedded driver: недоступен (${_qpsql_why}), используется обычный QSqlDatabase::addDatabase(\"QPSQL\")")
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
    target_include_directories(inventory_manager PRIVATE ${PG_CONFIG_H_DIR})
    target_link_libraries(inventory_manager PRIVATE
        Qt6::SqlPrivate
        Qt6::CorePrivate
        PostgreSQL::PostgreSQL
    )
    target_compile_definitions(inventory_manager PRIVATE QPSQL_EMBEDDED_AVAILABLE)
    # QT_PLUGIN здесь не означает "мы собираем плагин" - оно лишь отключает
    # dllimport/dllexport-атрибуты (Q_EXPORT_SQLDRIVER_PSQL) на классе
    # QPSQLDriver, чтобы он стал обычным символом внутри нашего exe, а не
    # ожидал импорта из отдельной qsqlpsql.dll. Нужно в КАЖДОМ файле, который
    # видит объявление класса, иначе MSVC будет искать __imp_-символ.
    set_source_files_properties(
        src/thirdparty/qpsql/qsql_psql.cpp
        src/database.cpp
        PROPERTIES COMPILE_DEFINITIONS QT_PLUGIN)
endif()

# Диагностический консольный инструмент: тот же QSqlDatabase/QSqlQuery путь,
# что и в Database::open(), но отдельно и с подробным выводом - для отладки
# проблем подключения к серверу без запуска всего приложения.
add_executable(qtpgtest tools/qtpgtest.cpp)
target_link_libraries(qtpgtest PRIVATE Qt6::Core Qt6::Sql)
