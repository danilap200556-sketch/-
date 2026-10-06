#pragma once

#include <QDialog>
#include <QList>
#include <QPixmap>

class QLabel;
class QPushButton;

// Просмотр фото товара крупно: стрелки влево/вправо листают фото.
class PhotoViewer : public QDialog
{
    Q_OBJECT
public:
    // Фото берутся из базы по id; title - подпись окна (артикул и название).
    PhotoViewer(const QList<int> &photoIds, const QString &title, QWidget *parent = nullptr);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void showPhoto(int index);
    void refit();

    QList<int> m_ids;
    int m_index = 0;
    QPixmap m_pixmap;
    QLabel *m_image;
    QLabel *m_counter;
    QPushButton *m_prev;
    QPushButton *m_next;
};
