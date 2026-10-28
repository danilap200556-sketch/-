#pragma once

#include "photofetch.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QLabel;
class QListWidget;
class QTableWidget;

// Просмотр найденных фото перед сохранением: у каждого товара - снимки с подписью, откуда они и подтверждён ли артикул.
// Проверенные первые perProduct снимков отмечены заранее, непроверенные - нет. Двойной щелчок - крупный просмотр.
class PhotoReviewDialog : public QDialog
{
    Q_OBJECT
public:
    PhotoReviewDialog(const QList<PhotoFetch::Proposal> &proposals, int perProduct, bool replaceExisting, QWidget *parent = nullptr);

    // Отмеченное, по товарам (товары без отмеченных снимков не попадают).
    QList<PhotoFetch::Choice> choices() const;
    // «Заменить прежние фото» отмечено.
    bool replaceExisting() const;
    int selectedCount() const;

private:
    void updateSummary();
    void markVerified();
    void clearMarks();
    void showFull(QListWidget *list, int index);

    QList<PhotoFetch::Proposal> m_proposals;
    int m_perProduct;
    QList<QListWidget *> m_lists; // по одному на товар, в порядке m_proposals
    QTableWidget *m_table;
    QCheckBox *m_replace;
    QLabel *m_summary;
};
