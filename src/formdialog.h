#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QTextEdit;

// Небольшой универсальный диалог с набором текстовых полей.
// Используется для простых сущностей (склад, место хранения), где не нужна
// отдельная форма как для товара (там есть выбор файла-фото и т.п.).
class FormDialog : public QDialog
{
    Q_OBJECT
public:
    struct Field {
        QString label;
        QString initialValue;
        bool multiline = false;
    };

    FormDialog(const QString &title, const QList<Field> &fields, QWidget *parent = nullptr);

    QStringList values() const;

private:
    QList<QLineEdit *> m_lineEdits;
    QList<QTextEdit *> m_textEdits;
    QList<bool> m_isMultiline;
};
