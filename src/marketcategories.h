#pragma once

#include "marketapi.h"

#include <QJsonObject>
#include <QList>
#include <QString>

// Характеристика категории Маркета (CategoryParameterDTO): что за поле, обязательно ли, какие бывают значения.
struct MarketCategoryParam
{
    struct Option {
        qint64 id = 0;
        QString value;
    };
    struct Unit {
        qint64 id = 0;
        QString name;
    };

    qint64 id = 0;
    QString name;
    QString type; // TEXT, ENUM, BOOLEAN, NUMERIC
    QString description;
    bool required = false;
    bool multivalue = false;
    bool allowCustomValues = false;
    bool distinctive = false;
    QList<Option> values; // только ENUM
    QList<Unit> units;
    qint64 defaultUnitId = 0;
    bool hasMin = false;
    bool hasMax = false;
    double minValue = 0;
    double maxValue = 0;
    int maxLength = 0;

    static MarketCategoryParam fromJson(const QJsonObject &o);
};

// Справочники категорий и характеристик Маркета. Они большие и меняются редко, поэтому кладутся в кэш
// на этом компьютере (папка данных программы) и обновляются раз в две недели или по просьбе.
namespace MarketCategories {

// Дерево категорий (корень с вложенными children).
bool loadTree(MarketApi &api, bool refresh, QJsonObject *root, QString *error);
// «Обувь › Кроссовки» - путь до категории; пусто, если такой нет.
QString pathOf(const QJsonObject &root, qint64 id);
// Листовая ли категория (в карточке можно указывать только листовые).
bool isLeaf(const QJsonObject &root, qint64 id);

// Характеристики листовой категории: обязательные идут первыми.
bool loadParams(MarketApi &api, qint64 businessId, qint64 categoryId, bool refresh, QList<MarketCategoryParam> *out,
                QString *error);
// То же из кэша, без запроса к Маркету (false - в кэше нет).
bool cachedParams(qint64 categoryId, QList<MarketCategoryParam> *out);

// Папка кэша (для тестов можно подменить переменной окружения INVENTORY_CACHE_DIR).
QString cacheDir();

} // namespace MarketCategories
