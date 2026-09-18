#include "formdialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QTextEdit>
#include <QVBoxLayout>

FormDialog::FormDialog(const QString &title, const QList<Field> &fields, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(title);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout();
    layout->addLayout(form);

    for (const Field &f : fields) {
        if (f.multiline) {
            auto *edit = new QTextEdit(this);
            edit->setPlainText(f.initialValue);
            edit->setFixedHeight(80);
            form->addRow(f.label, edit);
            m_textEdits.append(edit);
            m_lineEdits.append(nullptr);
            m_isMultiline.append(true);
        } else {
            auto *edit = new QLineEdit(f.initialValue, this);
            form->addRow(f.label, edit);
            m_lineEdits.append(edit);
            m_textEdits.append(nullptr);
            m_isMultiline.append(false);
        }
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QStringList FormDialog::values() const
{
    QStringList result;
    for (int i = 0; i < m_isMultiline.size(); ++i) {
        if (m_isMultiline[i])
            result << m_textEdits[i]->toPlainText();
        else
            result << m_lineEdits[i]->text();
    }
    return result;
}
