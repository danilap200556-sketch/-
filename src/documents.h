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
    QDateTime editedAt;    // когда документ правили в разделе «Документы» (пусто - не правили)
    QString editedBy;
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

// --- Правка документа. Править можно приход, списание и перемещение (одним складом или парой складов), пока документ не
// отменён ни целиком, ни частично и не создан списанием заказа. Правка заменяет состав документа: остатки меняются на разницу
// (старые записи убираются, новые записываются) одной транзакцией, номер, дата и автор документа остаются, ставится отметка
// «изменён». Если из-за правки остаток где-то уйдёт в минус, не меняется ничего.
struct Item {
    int productId = 0;
    int qty = 0;           // всегда положительное
};
struct Draft {
    QString comment;
    int warehouseId = 0;   // приход и списание: склад; перемещение: откуда
    int warehouseToId = 0; // перемещение: куда
    QList<Item> items;
};
// Текущее содержимое документа для правки. false - править нельзя, причина в *why.
bool draftOf(const Info &d, Draft *out, QString *why = nullptr);
bool canEdit(const Info &d, QString *why = nullptr);
bool update(int docId, const Draft &draft, QStringList *problems, QString *error = nullptr);
// Только комментарий (у любого документа): и в документе, и в его записях журнала.
bool setComment(int docId, const QString &comment, QString *error = nullptr);

// --- Удаление документа целиком. Из журнала исчезают все записи документа и отмены его записей (отменённый документ уходит
// вместе с отменой: остатки от этого не меняются). restoreStock - вернуть остатки, как будто документа не было (приход
// убирается с остатка, списание возвращается); без этого остатки не меняются, а документ просто пропадает из истории.
// Документ-отмену удалять можно только с возвратом остатков (отменённые документы снова станут действующими).
struct DeleteRow {
    int productId = 0;
    QString sku;
    QString name;
    int warehouseId = 0;
    QString warehouse;
    qint64 current = 0;    // остаток сейчас
    qint64 after = 0;      // остаток после удаления
};
struct DeletePlan {
    QList<DeleteRow> rows;     // что изменится в остатках (пусто, если остатки не трогаем или итог нулевой)
    QStringList problems;      // почему удалить нельзя
    int movements = 0;         // записей журнала исчезнет
    int cancelDocuments = 0;   // документов-отмен исчезнет вместе с этим
    bool ok() const { return problems.isEmpty(); }
};
DeletePlan planDelete(int docId, bool restoreStock);
bool remove(int docId, bool restoreStock, QStringList *problems, QString *error = nullptr);

} // namespace Documents
