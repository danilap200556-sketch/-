#pragma once

#include <QHash>
#include <QList>
#include <QString>

// ИП (юридические лица), на которых числятся коды маркировки «Честного знака». У каждого ИП свой кабинет, свои коды и свои настройки СУЗ
// (см. ChestnyZnak::loadSettings(ownerId)). Склады к ИП не привязаны: код хранится и с ИП, и (отдельно) со складом, под остаток которого заказан.
// Идентификатор 0 везде означает «без ИП» (так лежат коды, добавленные до появления ИП).
namespace MarkingOwner {

struct Owner {
    int id = 0;
    QString name;
    QString inn;   // 10 или 12 цифр; может быть пустым
};

QList<Owner> list();
Owner get(int id);
QString nameOf(int id);   // пусто для 0 и несуществующего

// Пустая строка - ИНН подходит (или пуст); иначе причина по-русски.
QString innProblem(const QString &inn);

// Добавляет ИП; возвращает его номер, 0 - ошибка (причина в *error: пустое название, повтор названия, неверный ИНН).
int add(const QString &name, const QString &inn, QString *error = nullptr);
bool update(int id, const QString &name, const QString &inn, QString *error = nullptr);
// Удаляет ИП: его коды и склады остаются, но уже без ИП.
bool remove(int id, QString *error = nullptr);

// ИП кабинета Маркета (0 - не задан): коды к заказам кабинета берутся у этого ИП.
int accountOwner(int accountId);
bool setAccountOwner(int accountId, int ownerId, QString *error = nullptr);

// ИП -> сколько кодов (ключ 0 - без ИП).
QHash<int, int> codeCounts();

} // namespace MarkingOwner
