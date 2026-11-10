#pragma once

#include <QByteArray>
#include <QDate>
#include <QList>
#include <QString>
#include <QStringList>

// Файлы для загрузки в личный кабинет «Честного знака» (без электронной подписи в программе: файл загружается в кабинете, а подписывает
// и отправляет его сам кабинет).
//
//  * XML заказа кодов маркировки (urn:oms.order) для обуви (shoes) и лёгкой промышленности (lp) - по XSD-схеме заказа версии 5.90
//    (docs/xsd/oms-order-5.90.xsd): «Заказы → Активные → Импортировать». В файле не больше 10 товарных позиций и до 150 000 кодов на позицию.
//  * «Ввод в оборот» - список кодов идентификации для кабинета: Excel по шаблону кабинета (один столбец, по коду в строке; .xls как в шаблоне
//    или .xlsx) либо XML (vvod_individuals, action_id 5.3, version 3) по образцу из кабинета: ИНН участника, дата, список кодов.
//    XSD этого XML у нас нет, поэтому он повторяет только присланный образец; в Excel-файле ИНН и дату вводят уже в кабинете.
namespace CabinetXml {

constexpr int kProductsPerOrderFile = 10;
constexpr int kMaxQuantity = 150000;
constexpr int kSerialLength = 12;           // для обуви и лёгкой промышленности: последние 12 знаков серийного номера, 13-й добавляет система
constexpr int kCodesPerIntroFile = 5000;    // чтобы файл ввода в оборот не получился слишком большим (и XML, и Excel)

struct OrderOptions {
    QString group = QStringLiteral("shoes");             // shoes (обувь) или lp (лёгкая промышленность)
    QString contactPerson;                                // необязательно, до 128 знаков
    QString releaseMethod = QStringLiteral("PRODUCTION"); // способ выпуска в оборот
    QString createMethod = QStringLiteral("SELF_MADE");   // способ изготовления
    bool ownSerials = false;                              // false - серийные номера присваивает система (OPERATOR), true - свои (SELF_MADE)
    int templateId = 0;                                   // шаблон КМ; 0 - по умолчанию для группы
    QString producerInn;                                  // ИНН владельца карточки товара (необязательно)
    QString productionOrderId;                            // идентификатор производственного заказа (необязательно)
    QString cisType = QStringLiteral("UNIT");             // только lp: UNIT, BUNDLE, SET
};

struct OrderItem {
    QString gtin;     // 14 цифр
    int quantity = 0; // 1..150000
};

// Что допускает схема для группы.
QStringList groups();                                  // shoes, lp
QString groupTitle(const QString &group);
QStringList releaseMethods(const QString &group);
QString releaseMethodTitle(const QString &code);
QStringList createMethods();
QString createMethodTitle(const QString &code);
QList<int> templateIds(const QString &group);          // shoes: 1, 35; lp: 10, 35
int defaultTemplateId(const QString &group);

// Пустая строка - всё в порядке, иначе причина по-русски (в том числе по позициям).
QString checkOrder(const QList<OrderItem> &items, const OrderOptions &options);
// XML заказа; позиции с одним GTIN складываются, в файле не больше kProductsPerOrderFile позиций, остальное - в следующих файлах.
// Пусто и *error - если проверка не пройдена.
QList<QByteArray> orderFiles(const QList<OrderItem> &items, const OrderOptions &options, QString *error = nullptr);
// Случайный серийный номер из kSerialLength знаков (цифры и латинские буквы): для заказа со своими серийными номерами.
QString randomSerial();

struct IntroOptions {
    QString inn;                 // ИНН участника оборота: 10 или 12 цифр
    QDate date = QDate::currentDate(); // дата ввода товара в оборот
};
// Код идентификации (КИ, «01…21…») из кода маркировки: без криптохвоста (всё до первого разделителя групп). Пусто - это не код GS1.
QString identificationCode(const QString &code);
QString checkIntro(const QStringList &codes, const IntroOptions &options);
// XML «Ввод в оборот» по КИ (повторы выбрасываются, не больше kCodesPerIntroFile в файле). *skipped - сколько строк не похожи на КИ.
QList<QByteArray> introFiles(const QStringList &codes, const IntroOptions &options, QString *error = nullptr, int *skipped = nullptr);

// Коды идентификации из кодов маркировки: без криптохвоста, повторы выброшены, порядок сохранён. *skipped - сколько строк не похожи на КИ.
QStringList introCodes(const QStringList &codes, int *skipped = nullptr);
// КИ со скобками, как в читаемом виде GS1: «(01)GTIN(21)серийный номер». Пусто - это не КИ.
QString introCodeWithBrackets(const QString &ki);

enum class IntroFormat {
    Xls,   // Excel 97-2003, как в шаблоне кабинета: один лист, столбец A, по коду в строке, без шапки
    Xlsx,  // то же самое в новом формате Excel
    Xml    // vvod_individuals (action_id 5.3)
};
QString introFormatTitle(IntroFormat format);
QString introFormatExtension(IntroFormat format);   // «xls», «xlsx», «xml»

// Excel-файлы «Ввод в оборот» (не больше kCodesPerIntroFile кодов в файле) записываются в папку как <prefix>[-<номер>].xls/.xlsx; пути - в *paths.
// brackets - писать коды со скобками. false и причина в *error.
bool writeIntroTables(const QString &directory, const QString &prefix, const QStringList &codes, IntroFormat format, bool brackets,
                      QStringList *paths, QString *error = nullptr, int *skipped = nullptr);

// Записывает файлы в папку как <prefix>[-<номер>].<extension>; пути записанных - в *paths. false и причина в *error.
bool writeFiles(const QString &directory, const QString &prefix, const QList<QByteArray> &files, QStringList *paths, QString *error = nullptr,
                const QString &extension = QStringLiteral("xml"));

} // namespace CabinetXml
