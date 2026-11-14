#include "markingowner.h"

#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace MarkingOwner {

namespace {
QString trimmedName(const QString &s) { return s.simplified(); }
} // namespace

QString innProblem(const QString &inn)
{
    const QString t = inn.trimmed();
    if (t.isEmpty())
        return {};
    static const QRegularExpression re(QStringLiteral("^(\\d{10}|\\d{12})$"));
    return re.match(t).hasMatch() ? QString() : QStringLiteral("ИНН - 10 цифр (организация) или 12 (ИП)");
}

QList<Owner> list()
{
    QList<Owner> out;
    QSqlQuery q(QStringLiteral("SELECT id, name, inn FROM marking_owners ORDER BY lower(name), id"));
    while (q.next())
        out.append({q.value(0).toInt(), q.value(1).toString(), q.value(2).toString()});
    return out;
}

Owner get(int id)
{
    if (id <= 0)
        return {};
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT id, name, inn FROM marking_owners WHERE id = ?"));
    q.addBindValue(id);
    if (q.exec() && q.next())
        return {q.value(0).toInt(), q.value(1).toString(), q.value(2).toString()};
    return {};
}

QString nameOf(int id) { return get(id).name; }

int add(const QString &name, const QString &inn, QString *error)
{
    const QString n = trimmedName(name);
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return 0;
    };
    if (n.isEmpty())
        return fail(QStringLiteral("название ИП обязательно"));
    const QString problem = innProblem(inn);
    if (!problem.isEmpty())
        return fail(problem);
    QSqlQuery q;
    q.prepare(QStringLiteral("INSERT INTO marking_owners (name, inn) VALUES (?, ?) ON CONFLICT (name) DO NOTHING RETURNING id"));
    q.addBindValue(n);
    q.addBindValue(inn.trimmed());
    if (!q.exec())
        return fail(q.lastError().text());
    if (!q.next())
        return fail(QStringLiteral("ИП «%1» уже есть").arg(n));
    return q.value(0).toInt();
}

bool update(int id, const QString &name, const QString &inn, QString *error)
{
    const QString n = trimmedName(name);
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (id <= 0)
        return fail(QStringLiteral("ИП не выбран"));
    if (n.isEmpty())
        return fail(QStringLiteral("название ИП обязательно"));
    const QString problem = innProblem(inn);
    if (!problem.isEmpty())
        return fail(problem);
    QSqlQuery same;
    same.prepare(QStringLiteral("SELECT 1 FROM marking_owners WHERE name = ? AND id <> ?"));
    same.addBindValue(n);
    same.addBindValue(id);
    if (same.exec() && same.next())
        return fail(QStringLiteral("ИП «%1» уже есть").arg(n));
    QSqlQuery q;
    q.prepare(QStringLiteral("UPDATE marking_owners SET name = ?, inn = ? WHERE id = ?"));
    q.addBindValue(n);
    q.addBindValue(inn.trimmed());
    q.addBindValue(id);
    if (!q.exec())
        return fail(q.lastError().text());
    return true;
}

bool remove(int id, QString *error)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("DELETE FROM marking_owners WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

int accountOwner(int accountId)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT owner_id FROM marking_account_owner WHERE account_id = ?"));
    q.addBindValue(accountId);
    return q.exec() && q.next() ? q.value(0).toInt() : 0;
}

bool setAccountOwner(int accountId, int ownerId, QString *error)
{
    QSqlQuery q;
    if (ownerId > 0) {
        q.prepare(QStringLiteral("INSERT INTO marking_account_owner (account_id, owner_id) VALUES (?, ?) "
                                 "ON CONFLICT (account_id) DO UPDATE SET owner_id = EXCLUDED.owner_id"));
        q.addBindValue(accountId);
        q.addBindValue(ownerId);
    } else {
        q.prepare(QStringLiteral("DELETE FROM marking_account_owner WHERE account_id = ?"));
        q.addBindValue(accountId);
    }
    if (!q.exec()) {
        if (error)
            *error = q.lastError().text();
        return false;
    }
    return true;
}

QHash<int, int> codeCounts()
{
    QHash<int, int> out;
    QSqlQuery q(QStringLiteral("SELECT COALESCE(owner_id, 0), count(*) FROM marking_codes GROUP BY 1"));
    while (q.next())
        out.insert(q.value(0).toInt(), q.value(1).toInt());
    return out;
}

} // namespace MarkingOwner
