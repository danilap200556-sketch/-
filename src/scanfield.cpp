#include "scanfield.h"
#include "productfinder.h"

ScanField::ScanField(QWidget *parent)
    : QLineEdit(parent)
{
    setPlaceholderText(tr("Сканер: отсканируйте штрихкод или введите артикул и нажмите Enter"));
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
