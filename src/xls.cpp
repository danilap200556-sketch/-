#include "xls.h"

#include <QHash>
#include <QPair>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVector>

#include <cstring>

namespace Xls {

namespace {

constexpr quint32 kEndOfChain = 0xFFFFFFFE;
constexpr quint32 kFatSector = 0xFFFFFFFD;
constexpr quint32 kFree = 0xFFFFFFFF;
constexpr int kSector = 512;
constexpr int kMaxRecordBody = 8224;  // больше в одну запись BIFF8 не помещается: остаток идёт в CONTINUE
constexpr int kMaxCellChars = 4000;   // чтобы строка гарантированно уместилась в одну запись общей таблицы строк
constexpr int kDefaultXf = 15;        // первый «ячеечный» формат (после 15 стилевых) - обычная ячейка без украшений

// Шрифты, форматы, XF и стили книги: ровно те записи, которые пишет LibreOffice (Excel открывает такие файлы без замечаний).
// 5 шрифтов, формат General, 21 XF (15 стилевых + ячеечные), 6 встроенных стилей.
const char *const kStylesHex =
    "31001a00c8000000ff7f9001000000020000050141007200690061006c0031001a00c8000000ff7f9001000000000000"
    "050141007200690061006c0031001a00c8000000ff7f9001000000000000050141007200690061006c0031001a00c800"
    "0000ff7f9001000000000000050141007200690061006c0031002e00c8000000ff7f90010000000100000f0154006900"
    "6d006500730020004e0065007700200052006f006d0061006e001e040c00a40007000047656e6572616ce00014000000"
    "a400f5ff200000000000000000000000c020e000140001000000f5ff200000f40000000000000000c020e00014000100"
    "0000f5ff200000f40000000000000000c020e000140002000000f5ff200000f40000000000000000c020e00014000200"
    "0000f5ff200000f40000000000000000c020e000140000000000f5ff200000f40000000000000000c020e00014000000"
    "0000f5ff200000f40000000000000000c020e000140000000000f5ff200000f40000000000000000c020e00014000000"
    "0000f5ff200000f40000000000000000c020e000140000000000f5ff200000f40000000000000000c020e00014000000"
    "0000f5ff200000f40000000000000000c020e000140000000000f5ff200000f40000000000000000c020e00014000000"
    "0000f5ff200000f40000000000000000c020e000140000000000f5ff200000f40000000000000000c020e00014000000"
    "0000f5ff200000f40000000000000000c020e00014000000a4000100200000000000000000000000c020e00014000100"
    "2b00f5ff200000f00000000000000000c020e000140001002900f5ff200000f00000000000000000c020e00014000100"
    "2c00f5ff200000f00000000000000000c020e000140001002a00f5ff200000f00000000000000000c020e00014000100"
    "0900f5ff200000f00000000000000000c020e00014000500a4000100280000180000000000000000c020930204000080"
    "00ff93020400108003ff93020400118006ff93020400128004ff93020400138007ff93020400148005ff";

struct Buf
{
    QByteArray b;
    Buf &u8(int v) { b.append(char(v & 0xFF)); return *this; }
    Buf &u16(int v) { u8(v); u8(v >> 8); return *this; }
    Buf &u32(quint32 v) { u16(int(v & 0xFFFF)); u16(int(v >> 16)); return *this; }
    Buf &raw(const QByteArray &d) { b.append(d); return *this; }
    Buf &f64(double d)
    {
        quint64 bits = 0;
        std::memcpy(&bits, &d, sizeof bits);
        for (int i = 0; i < 8; ++i)
            u8(int((bits >> (8 * i)) & 0xFF));
        return *this;
    }
};

void record(QByteArray &out, int type, const QByteArray &body = {})
{
    Buf h;
    h.u16(type).u16(int(body.size()));
    out.append(h.b);
    out.append(body);
}

QByteArray hexBytes(const char *hex) { return QByteArray::fromHex(QByteArray(hex)); }

// Строка без заголовка: для общей таблицы строк (SST). Латиница и цифры - по байту на знак, остальное - UTF-16.
QByteArray sstString(const QString &s)
{
    bool wide = false;
    for (QChar c : s) {
        if (c.unicode() > 0xFF) {
            wide = true;
            break;
        }
    }
    Buf b;
    b.u16(int(s.size())).u8(wide ? 1 : 0);
    for (QChar c : s) {
        if (wide)
            b.u16(c.unicode());
        else
            b.u8(c.unicode());
    }
    return b.b;
}

// Короткая строка: имя листа (длина в один байт).
QByteArray shortString(const QString &s)
{
    Buf b;
    b.u8(int(s.size())).u8(1);
    for (QChar c : s)
        b.u16(c.unicode());
    return b.b;
}

void bof(QByteArray &out, int type)
{
    Buf b;
    b.u16(0x0600).u16(type).u16(0x0DBB).u16(0x07CC).u32(0).u32(6);
    record(out, 0x0809, b.b);
}

// Контейнер OLE2 (сложный документ): заголовок, поток Workbook, каталог, таблица размещения.
QByteArray container(QByteArray workbook)
{
    // Поток короче 4096 байт хранился бы в «мини-потоке»; чтобы не усложнять, дополняем его нулями (так делают и другие писатели .xls).
    if (workbook.size() < 4096)
        workbook.append(QByteArray(4096 - workbook.size(), '\0'));
    if (workbook.size() % kSector)
        workbook.append(QByteArray(kSector - workbook.size() % kSector, '\0'));
    const int dataSectors = int(workbook.size() / kSector);
    int fatSectors = 1;
    while (dataSectors + 1 + fatSectors > 128 * fatSectors)
        ++fatSectors;
    if (fatSectors > 109)
        return {};
    const int dirSector = dataSectors;
    const int firstFat = dataSectors + 1;

    QVector<quint32> fat(128 * fatSectors, kFree);
    for (int i = 0; i < dataSectors - 1; ++i)
        fat[i] = quint32(i + 1);
    fat[dataSectors - 1] = kEndOfChain;
    fat[dirSector] = kEndOfChain;
    for (int i = 0; i < fatSectors; ++i)
        fat[firstFat + i] = kFatSector;

    Buf h;
    h.raw(hexBytes("D0CF11E0A1B11AE1"));
    h.raw(QByteArray(16, '\0'));
    h.u16(0x003E).u16(3).u16(0xFFFE).u16(9).u16(6);
    h.raw(QByteArray(6, '\0'));
    h.u32(0);                     // число секторов каталога (для версии 3 - 0)
    h.u32(quint32(fatSectors));
    h.u32(quint32(dirSector));
    h.u32(0);
    h.u32(4096);                  // порог мини-потока
    h.u32(kEndOfChain).u32(0);    // мини-FAT нет
    h.u32(kEndOfChain).u32(0);    // дополнительных секторов DIFAT нет
    for (int i = 0; i < 109; ++i)
        h.u32(i < fatSectors ? quint32(firstFat + i) : kFree);

    auto entry = [](const QString &name, int type, quint32 child, quint32 start, quint64 size, bool excelClsid) {
        Buf e;
        QByteArray nameBytes;
        for (QChar c : name) {
            nameBytes.append(char(c.unicode() & 0xFF));
            nameBytes.append(char(c.unicode() >> 8));
        }
        nameBytes.append(QByteArray(64 - nameBytes.size(), '\0'));
        e.raw(nameBytes);
        e.u16(name.isEmpty() ? 0 : int((name.size() + 1) * 2));
        e.u8(type).u8(1);
        e.u32(kFree).u32(kFree).u32(child);
        e.raw(excelClsid ? hexBytes("2008020000000000C000000000000046") : QByteArray(16, '\0'));
        e.u32(0).u32(0).u32(0).u32(0).u32(0);   // биты состояния, время создания и изменения
        e.u32(start).u32(quint32(size & 0xFFFFFFFF)).u32(quint32(size >> 32));
        return e.b;
    };
    QByteArray dir;
    dir += entry(QStringLiteral("Root Entry"), 5, 1, kEndOfChain, 0, true);
    dir += entry(QStringLiteral("Workbook"), 2, kFree, 0, quint64(workbook.size()), false);
    dir += entry(QString(), 0, kFree, 0, 0, false);
    dir += entry(QString(), 0, kFree, 0, 0, false);
    // у свободных записей «нет потока»: начальный сектор - 0, размер - 0 (уже так)

    QByteArray out = h.b;
    out += workbook;
    out += dir;
    for (quint32 v : fat) {
        Buf b;
        b.u32(v);
        out += b.b;
    }
    return out;
}

} // namespace

QByteArray buildColumn(const QString &sheetName, const QStringList &values, QString *error)
{
    auto fail = [error](const QString &text) {
        if (error)
            *error = text;
        return QByteArray();
    };
    QString name = sheetName.trimmed();
    if (name.isEmpty() || name.size() > kMaxSheetName
        || name.contains(QRegularExpression(QStringLiteral("[\\[\\]:*?/\\\\]"))) || name.startsWith(QLatin1Char('\''))
        || name.endsWith(QLatin1Char('\'')))
        return fail(QStringLiteral("недопустимое имя листа"));
    if (values.size() > kMaxRows)
        return fail(QStringLiteral("в файле .xls не больше %1 строк").arg(kMaxRows));
    int maxChars = 0;
    for (const QString &v : values) {
        if (v.size() > kMaxCellChars)
            return fail(QStringLiteral("ячейка длиннее %1 знаков").arg(kMaxCellChars));
        maxChars = qMax(maxChars, int(v.size()));
    }

    // Общая таблица строк: каждое значение один раз, в ячейках - номера.
    QHash<QString, int> index;
    QStringList unique;
    QVector<int> cellString(values.size(), -1);
    int stringCells = 0;
    for (int i = 0; i < values.size(); ++i) {
        if (values[i].isEmpty())
            continue;
        ++stringCells;
        auto it = index.constFind(values[i]);
        if (it == index.constEnd()) {
            it = index.insert(values[i], int(unique.size()));
            unique << values[i];
        }
        cellString[i] = it.value();
    }

    // ---- начало книги (до таблицы строк)
    QByteArray head;
    bof(head, 0x0005);
    record(head, 0x00E1, Buf().u16(0x04B0).b);   // INTERFACEHDR: кодовая страница
    record(head, 0x00C1, Buf().u16(0).b);        // MMS
    record(head, 0x00E2);                        // INTERFACEEND
    {
        // WRITEACCESS: «кем записан» - строка, добитая пробелами до 112 байт
        const QString who = QStringLiteral("Inventory Manager");
        Buf w;
        w.u16(int(who.size())).u8(1);
        for (QChar c : who)
            w.u16(c.unicode());
        w.raw(QByteArray(112 - w.b.size(), ' '));
        record(head, 0x005C, w.b);
    }
    record(head, 0x0042, Buf().u16(0x04B0).b);   // CODEPAGE: UTF-16
    record(head, 0x0161, Buf().u16(0).b);        // DSF
    record(head, 0x013D, Buf().u16(1).b);        // TABID
    record(head, 0x009C, Buf().u16(0x0E).b);     // FNGROUPCOUNT
    record(head, 0x01AF, Buf().u16(0).b);        // PROT4REV
    record(head, 0x01BC, Buf().u16(0).b);        // PROT4REVPASS
    record(head, 0x003D, hexBytes("00000000004000203800000000000100F401")); // WINDOW1
    record(head, 0x0040, Buf().u16(0).b);        // BACKUP
    record(head, 0x008D, Buf().u16(0).b);        // HIDEOBJ
    record(head, 0x0022, Buf().u16(0).b);        // DATEMODE
    record(head, 0x000E, Buf().u16(1).b);        // PRECISION
    record(head, 0x01B7, Buf().u16(0).b);        // REFRESHALL
    record(head, 0x00DA, Buf().u16(0).b);        // BOOKBOOL
    head += hexBytes(kStylesHex);                // шрифты, формат, XF, стили
    record(head, 0x0160, Buf().u16(0).b);        // USESELFS
    const int sheetPosOffset = int(head.size()) + 4;   // где в потоке лежит «адрес листа» из BOUNDSHEET
    record(head, 0x0085, Buf().u32(0).u8(0).u8(0).raw(shortString(name)).b);
    record(head, 0x008C, Buf().u16(1).u16(1).b); // COUNTRY

    // ---- таблица строк (SST) с продолжениями, не разрывая строки, и её указатель EXTSST
    struct Chunk { int type; QByteArray body; };
    QVector<Chunk> chunks;
    QVector<QPair<int, int>> where;   // у каждой строки: номер куска и смещение в его теле
    {
        Chunk cur{0x00FC, Buf().u32(quint32(stringCells)).u32(quint32(unique.size())).b};
        for (const QString &s : unique) {
            const QByteArray bytes = sstString(s);
            if (cur.body.size() + bytes.size() > kMaxRecordBody) {
                chunks << cur;
                cur = Chunk{0x003C, QByteArray()};
            }
            where << qMakePair(int(chunks.size()), int(cur.body.size()));
            cur.body += bytes;
        }
        chunks << cur;
    }
    const int sstStart = int(head.size());
    QVector<int> chunkStart;
    {
        int pos = sstStart;
        for (const Chunk &c : chunks) {
            chunkStart << pos;
            pos += 4 + int(c.body.size());
        }
    }
    QByteArray sst;
    for (const Chunk &c : chunks)
        record(sst, c.type, c.body);

    Buf ext;
    {
        const int perBucket = qMax(8, (int(unique.size()) + 127) / 128);
        ext.u16(perBucket);
        for (int i = 0; i < unique.size(); i += perBucket) {
            ext.u32(quint32(chunkStart[where[i].first] + 4 + where[i].second));
            ext.u16(4 + where[i].second);
            ext.u16(0);
        }
    }
    QByteArray extsst;
    record(extsst, 0x00FF, ext.b);
    QByteArray eofRecord;
    record(eofRecord, 0x000A);

    const int sheetPos = int(head.size() + sst.size() + extsst.size() + eofRecord.size());
    for (int i = 0; i < 4; ++i)
        head[sheetPosOffset + i] = char((sheetPos >> (8 * i)) & 0xFF);

    // ---- лист
    QByteArray sheet;
    bof(sheet, 0x0010);
    record(sheet, 0x000C, Buf().u16(100).b);                   // CALCCOUNT
    record(sheet, 0x000F, Buf().u16(1).b);                     // REFMODE
    record(sheet, 0x0011, Buf().u16(0).b);                     // ITERATION
    record(sheet, 0x0010, Buf().f64(0.001).b);                 // DELTA
    record(sheet, 0x005F, Buf().u16(1).b);                     // SAVERECALC
    record(sheet, 0x0080, Buf().u16(0).u16(0).u16(0).u16(0).b); // GUTS
    record(sheet, 0x0225, Buf().u16(0).u16(0x0100).b);         // DEFAULTROWHEIGHT
    record(sheet, 0x0081, Buf().u16(0x04C1).b);                // WSBOOL
    record(sheet, 0x002A, Buf().u16(0).b);                     // PRINTHEADERS
    record(sheet, 0x002B, Buf().u16(0).b);                     // PRINTGRIDLINES
    record(sheet, 0x0082, Buf().u16(1).b);                     // GRIDSET
    record(sheet, 0x0083, Buf().u16(0).b);                     // HCENTER
    record(sheet, 0x0084, Buf().u16(0).b);                     // VCENTER
    record(sheet, 0x0026, Buf().f64(0.7).b);                   // поля, дюймы
    record(sheet, 0x0027, Buf().f64(0.7).b);
    record(sheet, 0x0028, Buf().f64(1.0).b);
    record(sheet, 0x0029, Buf().f64(1.0).b);
    record(sheet, 0x00A1, hexBytes("0900640001000100010082002C012C01333333333333E93F333333333333E93F0100")); // SETUP: A4
    record(sheet, 0x0055, Buf().u16(8).b);                     // DEFCOLWIDTH
    const int width = qBound(12, maxChars + 3, 60);            // ширина столбца A в знаках: под самый длинный код
    record(sheet, 0x007D, Buf().u16(0).u16(0).u16(width * 256).u16(kDefaultXf).u16(0).u16(0).b); // COLINFO
    const int rows = int(values.size());
    record(sheet, 0x0200, Buf().u32(0).u32(quint32(rows)).u16(0).u16(1).u16(0).b);               // DIMENSIONS
    for (int block = 0; block < rows; block += 32) {
        const int last = qMin(block + 32, rows);
        for (int r = block; r < last; ++r)
            record(sheet, 0x0208, Buf().u16(r).u16(0).u16(1).u16(0x0100).u16(0).u16(0).u16(0x0100).u16(kDefaultXf).b); // ROW
        for (int r = block; r < last; ++r) {
            if (cellString[r] >= 0)
                record(sheet, 0x00FD, Buf().u16(r).u16(0).u16(kDefaultXf).u32(quint32(cellString[r])).b); // LABELSST
        }
    }
    record(sheet, 0x023E, hexBytes("B60600000000400000000000000000000000"));    // WINDOW2
    record(sheet, 0x001D, Buf().u8(3).u16(0).u16(0).u16(0).u16(1).u16(0).u16(0).u8(0).u8(0).b); // SELECTION: ячейка A1
    record(sheet, 0x000A);

    const QByteArray out = container(head + sst + extsst + eofRecord + sheet);
    if (out.isEmpty())
        return fail(QStringLiteral("слишком большой файл"));
    return out;
}

bool writeColumn(const QString &path, const QString &sheetName, const QStringList &values, QString *error)
{
    const QByteArray data = buildColumn(sheetName, values, error);
    if (data.isEmpty())
        return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("не удалось создать файл (возможно, он открыт в Excel)");
        return false;
    }
    f.write(data);
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("ошибка записи файла");
        return false;
    }
    return true;
}

} // namespace Xls
