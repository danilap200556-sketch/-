#pragma once

#include "productfinder.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>

class QLineEdit;
class QListWidget;

// Подсказки под строкой ввода: человек печатает часть артикула, названия или штрихкода - под строкой появляется
// список подходящих товаров («артикул — название»); стрелки вверх/вниз, Enter или щелчок выбирают пункт.
// Пока пункт не выбран, Enter работает как раньше (штрихкод от сканера, артикул целиком и т. п.).
// Сканер печатает цифры штрихкода очень быстро и заканчивает Enter'ом - для строк из одних цифр (8 и больше) подсказки
// не показываются, чтобы они не мешали.
class ProductSuggest : public QObject
{
    Q_OBJECT
public:
    explicit ProductSuggest(QLineEdit *edit, QObject *parent = nullptr);

    // Что искать по тексту строки. По умолчанию - весь текст; для «артикул [количество]» подставляют функцию,
    // отрезающую количество.
    void setQueryFunction(std::function<QString(const QString &)> f) { m_query = std::move(f); }
    // Показывать подсказки не раньше, чем набрано столько знаков.
    void setMinChars(int n) { m_minChars = n; }

    bool isOpen() const;
    int itemCount() const;
    QStringList labels() const;

    // Для тестов: найти сейчас, не дожидаясь паузы после ввода.
    void searchNow();

signals:
    // Выбран пункт; typed - весь текст строки в этот момент (в нём может быть и количество).
    void chosen(int productId, const QString &sku, const QString &typed);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void hidePopup();
    void choose(int row);
    void move(int delta);

    QLineEdit *m_edit;
    QListWidget *m_list;
    QTimer m_timer;
    std::function<QString(const QString &)> m_query;
    int m_minChars = 2;
};
