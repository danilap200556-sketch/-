#pragma once

#include <QLayout>
#include <QList>
#include <QStyle>

// Панель кнопок, которая переносится на следующую строку, когда не хватает ширины окна (как текст в абзаце).
// Обычный QHBoxLayout требует места для всех кнопок сразу - из-за этого окно с десятью кнопками нельзя было сузить
// меньше двух тысяч пикселей. Минимальная ширина здесь - самая широкая кнопка.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int spacing = 8);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

private:
    int doLayout(const QRect &rect, bool apply) const;

    QList<QLayoutItem *> m_items;
    int m_spacing;
};
