#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// Коды маркировки «Честного знака» (КМ): GS1 DataMatrix вида 01<GTIN, 14 цифр>21<серийный номер> GS 91<ключ> GS 92<подпись>,
// где GS - разделитель групп (байт 0x1D). Коды хранятся в общей базе (таблица marking_codes), привязываются к товару по GTIN
// (GTIN = штрихкод EAN-13 товара с нулём впереди) и печатаются на термоэтикетках.
namespace MarkingCode {

constexpr QChar kGs = QChar(0x1D);

struct Parsed {
    bool ok = false;
    QString error;
    QString code;    // нормализованный КМ: группы разделены символом GS
    QString gtin;    // 14 цифр
    QString serial;  // серийный номер (AI 21)
};

// Разбирает код в любом виде, в котором он встречается: с настоящим GS, с «<GS>», «\x1d» или «^]», в скобках
// «(01)...(21)...», с префиксом сканера «]d2», а также слитно, без разделителей (серийный номер 13 знаков, ключ 4, подпись 44 или 88).
Parsed parse(const QString &raw);

// «(01)04600702084030(21)Zk9nB1kQEWm3x» - то, что печатается человеку под кодом (без ключа и подписи).
QString humanReadable(const QString &code);
// Для таблиц и текста: GS заменён на «␝», подпись сокращена.
QString shortText(const QString &code);

// GTIN-14 -> штрихкод товара: «04600702084030» -> «4600702084030» (EAN-13); «00012345678905» -> «012345678905» (UPC-A, 12 цифр).
QString barcodeFromGtin(const QString &gtin);
// GTIN-14 из штрихкода EAN-13 / UPC-A (дополняется нулями слева); пусто, если штрихкод не из цифр или неподходящей длины.
QString gtinFromBarcode(const QString &barcode);
// Контрольная цифра GS1 верна (для строки из 8, 12, 13 или 14 цифр).
bool gtinChecksumOk(const QString &digits);

// Товар по GTIN (через штрихкоды товаров); 0 - не найден.
int productIdForGtin(const QString &gtin);
// Привязывает GTIN к товару: добавляет ему штрихкод (EAN-13 из GTIN), если он ещё ничей. false и причина в *error, если занят другим товаром.
bool linkGtin(const QString &gtin, int productId, QString *error = nullptr);

struct AddResult {
    int added = 0;
    int duplicates = 0;      // такой код уже есть в базе
    int invalid = 0;
    int linked = 0;          // из добавленных - с найденным товаром
    int unlinked = 0;        // из добавленных - товар по GTIN не найден
    QStringList unknownGtins; // GTIN без товара (без повторов)
    QStringList problems;    // подробности по неразобранным (не больше 30)
};
// Добавляет коды (строки в любом виде, см. parse). forceProductId > 0 - все коды привязываются к этому товару, а его штрихкод
// становится GTIN кодов (если он свободен). source: «import», «scan», «suz». orderId - заказ в СУЗ.
AddResult add(const QStringList &rawCodes, const QString &source, const QString &orderId = QString(), int forceProductId = 0,
              const QString &status = QStringLiteral("new"));

// Есть ли у кода криптохвост (группа 91 / 92 или 93 после GS). Без него это КОД ИДЕНТИФИКАЦИИ (КИ): им подают сведения в Систему маркировки
// (ввод в оборот, вывод из оборота), но Data Matrix для нанесения на товар из него не получится - такой код печатать нельзя.
bool hasCryptoTail(const QString &code);

// Строка выгрузки «Реестр кодов» из личного кабинета «Честного знака» (CSV).
struct RegistryRow {
    QString code;        // КИ (чаще без криптохвоста)
    QString gtin;
    QString name;        // наименование товара
    QString brand;
    QString status;      // EMITTED, APPLIED, INTRODUCED...
    QString method;      // способ ввода в оборот: COMMISSION, PRODUCTION...
    QString orderId;     // номер заказа в СУЗ
    QString owner;       // ИНН владельца
    QString appliedAt, emittedAt;
};
// Читает такую выгрузку: первая строка - описание фильтра «Фильтр(...)», затем заголовок «Код, GTIN, ...» и коды. true - файл распознан как
// реестр (rows заполнены); false - это не реестр (*error пуст) или файл не прочитался (*error - причина).
bool readRegistryCsv(const QString &path, QList<RegistryRow> *rows, QString *error = nullptr);
// Добавляет строки реестра: источник «registry», статус по статусу кода в системе (EMITTED - новый, остальное: нанесён, в обороте... - напечатан).
AddResult addRegistry(const QList<RegistryRow> &rows);
// Достаёт коды из текста: по одному в строке, либо вставленные вплотную; пустые строки и заголовки пропускаются.
QStringList splitText(const QString &text);

// Коды без товара получают его, если GTIN теперь известен (после добавления штрихкодов). Возвращает, скольким привязан товар.
int relink();

struct Row {
    int id = 0;
    QString code, gtin, serial;
    int productId = 0;       // 0 - не привязан
    QString sku, name;
    QString status;          // new / printed
    int printCount = 0;
    QString source, orderId;
    QDateTime createdAt, printedAt;
};
struct Filter {
    QString status;          // пусто - все; new, printed
    bool onlyUnlinked = false;
    QString text;            // артикул, название, GTIN или часть кода
    int productId = 0;
    int limit = 2000;
};
QList<Row> list(const Filter &filter = Filter());
Row get(int id);
// Коды (как в базе, с разделителями групп) по номерам строк: пачками по одному запросу, в порядке номеров.
QStringList codesByIds(const QList<int> &ids);
struct Counts {
    int total = 0, fresh = 0, printed = 0, unlinked = 0;
};
Counts counts();

bool markPrinted(const QList<int> &ids, QString *error = nullptr);
// Удаляет коды. onlyNew - только ещё не печатавшиеся (напечатанные могут быть уже наклеены на товар). Возвращает число удалённых.
int remove(const QList<int> &ids, bool onlyNew = true);

} // namespace MarkingCode
