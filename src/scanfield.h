#pragma once

#include <QLineEdit>

class QKeyEvent;

// Строка "отсканируйте штрихкод или введите артикул / часть названия": на Enter (сканер заканчивает код
// Enter'ом) ищет товар через ProductFinder и сообщает, что нашла.
class ScanField : public QLineEdit
{
    Q_OBJECT
public:
    explicit ScanField(QWidget *parent = nullptr);

protected:
    // Enter в строке сканера обрабатывает сама строка: диалог не должен ещё и нажать свою кнопку по умолчанию
    // (иначе после каждого скана срабатывало бы «OK»).
    void keyPressEvent(QKeyEvent *event) override;

signals:
    // Первый из найденных товаров и сколько их всего (при точном совпадении - 1).
    void found(int productId, int matches);
    void notFound(const QString &text);
};
