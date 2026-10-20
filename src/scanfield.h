#pragma once

#include <QLineEdit>

// Строка "отсканируйте штрихкод или введите артикул": на Enter (сканер заканчивает код
// Enter'ом) ищет товар через ProductFinder и сообщает, что нашла.
class ScanField : public QLineEdit
{
    Q_OBJECT
public:
    explicit ScanField(QWidget *parent = nullptr);

signals:
    // Первый из найденных товаров и сколько их всего (при точном совпадении - 1).
    void found(int productId, int matches);
    void notFound(const QString &text);
};
