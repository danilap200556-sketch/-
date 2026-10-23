#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include "xlsx.h"

// Документы остатков: каждое списание, приход, перемещение, инвентаризация, импорт и отмена - отдельный документ
// с номером, видом, автором и записями внутри (одной или многими). Документ можно открыть и отменить целиком.
namespace Documents {

struct Info {
    int id = 0;            // номер документа
    QString key;           // stock_documents.doc_key = stock_movements.doc_id
    QString kind;          // receipt | writeoff | transfer | inventory | cancel | other
    QDateTime createdAt;
    QString username;
    QString comment;
    int lines = 0;         // записей журнала
    int positions = 0;     // разных товаров
    qint64 units = 0;      // штук (у перемещения - сколько переехало)
    int reverted = 0;      // сколько записей уже отменено
    QString warehouses;    // «Склад А», «Склад А → Склад Б» или «несколько складов»
    QString reverts;       // у отмены: номера отменённых документов
};

struct Line {
    int id = 0;            // номер записи журнала
    int productId = 0;
    QString sku;
    QString name;
    QString warehouse;
    QString type;          // receipt | writeoff | ...
    int delta = 0;
    QString comment;
    int revertedBy = 0;    // номер записи-отмены, 0 - не отменена
};

QString kindTitle(const QString &kind);
// «Списание №15»
QString title(const Info &d);
// «действует», «отменён», «отменён частично (2 из 5)», «отмена документа №12»
QString statusText(const Info &d);
bool isCancelled(const Info &d);

// Свежие документы (не больше limit), новые сверху. kind пустой - любые; search - номер, комментарий, автор, артикул или название.
QList<Info> list(const QString &kind, const QString &search, int limit = 500, QString *error = nullptr);
// Документ по номеру (id == 0 в результате - не найден).
Info get(int id);
QList<Line> lines(const QString &docKey);
// Записи документа, которые ещё можно отменить (не отмены и не отменённые ранее).
QList<int> cancellableIds(const QString &docKey);
// Номер последнего документа, у которого есть что отменять (отмены не считаются); 0 - таких нет.
int lastCancellable();

// Документ как таблица Excel: шапка и все записи.
Xlsx::Sheet toSheet(const Info &d, const QList<Line> &lines);

} // namespace Documents
