#pragma once

#include <QHash>
#include <QList>
#include <QString>

// ИП (юридические лица), на которых числятся коды маркировки «Честного знака». У каждого ИП свой кабинет, свои коды и свои настройки СУЗ
// (см. ChestnyZnak::loadSettings(ownerId)). Склад принадлежит одному ИП: товары на нём - этого ИП, и коды под них нужны от него же.
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

// Склад -> ИП (0 - не задан).
int warehouseOwner(int warehouseId);
QHash<int, int> warehouseOwners();
bool setWarehouseOwner(int warehouseId, int ownerId, QString *error = nullptr);

// ИП -> сколько кодов (ключ 0 - без ИП).
QHash<int, int> codeCounts();

} // namespace MarkingOwner
