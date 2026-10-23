#pragma once

#include "xlsx.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

// Обновление данных товаров из Excel/CSV: название, описание, цена, артикул на Маркете, свой код, штрихкоды и параметры
// для карточки Яндекс Маркета (бренд, страна, габариты и вес, ТН ВЭД, теги, 18+).
// Остатки не затрагиваются. Те же правила и тексты, что у страницы «Обновление товаров из Excel» на сайте
// (web/src/productsync.js). Шаги: plan() - что изменится (ничего не пишет), apply() - записать одной транзакцией.
namespace ProductSync {

enum Role {
    RoleIgnore = 0, RoleKey, RoleBarcode, RoleName, RoleDescription, RolePrice, RoleMarketSku, RoleCustomCode,
    // параметры для карточки Яндекс Маркета
    RoleVendor, RoleCountries, RoleLength, RoleWidth, RoleHeight, RoleWeight, RoleCommodity, RoleTags, RoleAdult,
    RoleCount
};
constexpr int kMaxRows = 20'000;

QStringList roleLabels();
int guessRole(const QString &header);
// Роли для колонок строки заголовков; одну роль получает только первая подходящая колонка.
QList<int> guessRoles(const QStringList &header, int width = -1);
int guessHeaderRow(const QVector<QStringList> &rows);
// «12 990,5», «12990.5», «1,234.50», «500 руб.» -> число с двумя знаками; иначе пусто.
std::optional<double> parsePrice(const QString &raw);

struct Options {
    bool createMissing = false; // создавать товары, которых нет в базе
    bool clearEmpty = false;    // пустая ячейка очищает описание, артикул на Маркете, свой код
};

struct Change {
    QString field;  // name, description, price, market_sku, custom_code, barcodes, ym_vendor, ym_countries, ym_length, ...
    QString label;
    QString from;
    QString to;
};

struct Item {
    int n = 0;                  // номер строки файла (с единицы)
    QString status;             // update, same, create, missing, error
    int id = 0;
    QString sku;
    QString name;
    QList<Change> changes;
    QStringList newCodes;       // штрихкоды, которые будут привязаны
    bool byCode = false;        // товар найден по штрихкоду
    QString message;            // для missing/error
    // для create
    QString createName, createDescription, createMarketSku, createCustomCode;
    double createPrice = 0;
    QMap<QString, QString> createYm; // параметры для Маркета нового товара: поле -> значение (как в Change::to)
};

struct Problem {
    int n = 0;
    QString text;
};

struct Plan {
    QString fatal;              // общая ошибка (нет колонки артикула и т.п.)
    QList<Item> items;
    int rows = 0, update = 0, same = 0, create = 0, missing = 0, error = 0;
    QList<Problem> problems;
};

Plan plan(const QVector<QStringList> &rows, int headerIdx, const QList<int> &roles, const Options &options);

struct Result {
    Plan plan;
    int updated = 0;
    int created = 0;
    QList<Problem> problems;    // ошибки файла и то, что не удалось записать
    Xlsx::Sheet backup;         // «как было» для обновлённых товаров (формат выгрузки товаров); пуст, если обновлений нет
};
// Записывает изменения одной транзакцией. false - ничего не записано, причина в *error.
bool apply(const QVector<QStringList> &rows, int headerIdx, const QList<int> &roles, const Options &options,
           Result *result, QString *error);

} // namespace ProductSync
