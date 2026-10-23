#include "productsuggest.h"

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QScreen>

namespace {
constexpr int kIdRole = Qt::UserRole;
constexpr int kSkuRole = Qt::UserRole + 1;
constexpr int kVisibleRows = 8;
} // namespace

ProductSuggest::ProductSuggest(QLineEdit *edit, QObject *parent)
    : QObject(parent ? parent : edit), m_edit(edit)
{
    // Список - отдельное окно без рамки, которое не забирает фокус у строки ввода: печатать можно дальше.
    m_list = new QListWidget(edit->window());
    m_list->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    m_list->setAttribute(Qt::WA_ShowWithoutActivating);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->hide();

    m_timer.setSingleShot(true);
    m_timer.setInterval(180);
    connect(&m_timer, &QTimer::timeout, this, &ProductSuggest::searchNow);
    connect(m_edit, &QLineEdit::textEdited, this, [this]() { m_timer.start(); });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { choose(m_list->row(item)); });
    m_edit->installEventFilter(this);
    if (QWidget *w = m_edit->window())
        w->installEventFilter(this); // окно двигают или сворачивают - список прячем
}

bool ProductSuggest::isOpen() const { return m_list->isVisible(); }
int ProductSuggest::itemCount() const { return m_list->count(); }

QStringList ProductSuggest::labels() const
{
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i)
        out << m_list->item(i)->text();
    return out;
}

void ProductSuggest::hidePopup()
{
    m_timer.stop();
    m_list->hide();
}

void ProductSuggest::searchNow()
{
    m_timer.stop();
    if (!m_edit->hasFocus() && !m_edit->isVisible()) {
        hidePopup();
        return;
    }
    QString q = m_query ? m_query(m_edit->text()) : m_edit->text();
    q = q.simplified();
    static const QRegularExpression digits(QStringLiteral("^[0-9 ]{8,}$"));
    if (q.size() < m_minChars || digits.match(q).hasMatch()) {
        hidePopup();
        return;
    }
    const QList<ProductFinder::Suggestion> found = ProductFinder::suggest(q, 30);
    if (found.isEmpty()) {
        hidePopup();
        return;
    }
    m_list->clear();
    for (const ProductFinder::Suggestion &s : found) {
        auto *item = new QListWidgetItem(s.label() + (s.marketSku.isEmpty() ? QString() : QStringLiteral("   [%1]").arg(s.marketSku)), m_list);
        item->setData(kIdRole, s.id);
        item->setData(kSkuRole, s.sku);
    }
    const int rows = qMin<int>(m_list->count(), kVisibleRows);
    const int rowH = m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0) : 28;
    const int width = qMax(m_edit->width(), 380);
    m_list->setFixedSize(width, rows * rowH + 2 * m_list->frameWidth() + 2);
    QPoint pos = m_edit->mapToGlobal(QPoint(0, m_edit->height() + 2));
    if (const QScreen *screen = m_edit->screen()) {
        const QRect avail = screen->availableGeometry();
        if (pos.y() + m_list->height() > avail.bottom()) // не помещается снизу - над строкой
            pos.setY(m_edit->mapToGlobal(QPoint(0, 0)).y() - m_list->height() - 2);
        pos.setX(qBound(avail.left(), pos.x(), qMax(avail.left(), avail.right() - width)));
    }
    m_list->move(pos);
    m_list->setCurrentRow(-1); // пока ничего не выбрано, Enter работает как раньше
    m_list->show();
}

void ProductSuggest::choose(int row)
{
    if (row < 0 || row >= m_list->count())
        return;
    QListWidgetItem *item = m_list->item(row);
    const int id = item->data(kIdRole).toInt();
    const QString sku = item->data(kSkuRole).toString();
    const QString typed = m_edit->text();
    hidePopup();
    emit chosen(id, sku, typed);
}

void ProductSuggest::move(int delta)
{
    const int n = m_list->count();
    if (n == 0)
        return;
    int row = m_list->currentRow();
    row = row < 0 ? (delta > 0 ? 0 : n - 1) : qBound(0, row + delta, n - 1);
    m_list->setCurrentRow(row);
}

bool ProductSuggest::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_edit) {
        switch (event->type()) {
        case QEvent::KeyPress: {
            if (!m_list->isVisible())
                break;
            const auto *ke = static_cast<QKeyEvent *>(event);
            switch (ke->key()) {
            case Qt::Key_Down: move(1); return true;
            case Qt::Key_Up: move(-1); return true;
            case Qt::Key_PageDown: move(kVisibleRows); return true;
            case Qt::Key_PageUp: move(-kVisibleRows); return true;
            case Qt::Key_Escape: hidePopup(); return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (m_list->currentRow() >= 0) {
                    choose(m_list->currentRow());
                    return true;
                }
                hidePopup(); // обычный Enter: строка обработает его как раньше
                break;
            default: break;
            }
            break;
        }
        case QEvent::FocusOut:
            // Щелчок по самому списку фокус не забирает, так что FocusOut - это уход в другое поле.
            hidePopup();
            break;
        case QEvent::Hide:
            hidePopup();
            break;
        default: break;
        }
    } else if (event->type() == QEvent::Move || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide) {
        if (m_list->isVisible())
            hidePopup();
    }
    return QObject::eventFilter(obj, event);
}
