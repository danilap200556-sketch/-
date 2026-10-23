#include "scanfield.h"
#include "productfinder.h"

#include <QKeyEvent>

ScanField::ScanField(QWidget *parent)
    : QLineEdit(parent)
{
    setPlaceholderText(tr("Штрихкод, артикул или часть названия - подсказки появятся под строкой; Enter - найти"));
    setClearButtonEnabled(true);
    connect(this, &QLineEdit::returnPressed, this, [this]() {
        const QString text = this->text().trimmed();
        if (text.isEmpty())
            return;
        const ProductFinder::Result r = ProductFinder::find(text);
        if (r.ids.isEmpty()) {
            emit notFound(text);
        } else {
            emit found(r.ids.first(), int(r.ids.size()));
        }
        selectAll(); // следующий скан заменит предыдущий
    });
}

void ScanField::keyPressEvent(QKeyEvent *event)
{
    QLineEdit::keyPressEvent(event);
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        event->accept();
}
