#ifndef CTABLEEDITDELEGATE_H
#define CTABLEEDITDELEGATE_H

#include <QLineEdit>
#include <QPointer>
#include <QStyledItemDelegate>

class CTableEditDelegate : public QStyledItemDelegate
{
public:
    explicit CTableEditDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                          const QModelIndex& index) const override
    {
        QWidget* editor = QStyledItemDelegate::createEditor(parent, option, index);
        m_editor = qobject_cast<QLineEdit*>(editor);
        m_index = index;
        return editor;
    }

    void destroyEditor(QWidget* editor, const QModelIndex& index) const override
    {
        if (m_editor == editor) {
            m_editor.clear();
            m_index = QPersistentModelIndex();
        }
        QStyledItemDelegate::destroyEditor(editor, index);
    }

    bool HasPendingChanges() const
    {
        return m_editor && m_index.isValid() && m_editor->text() != m_index.data(Qt::EditRole).toString();
    }

    void CommitEditor()
    {
        if (!m_editor || !m_index.isValid()) return;
        QPointer<QLineEdit> editor = m_editor;
        emit commitData(editor);
        if (editor) emit closeEditor(editor, QAbstractItemDelegate::NoHint);
    }

private:
    mutable QPointer<QLineEdit> m_editor;
    mutable QPersistentModelIndex m_index;
};

#endif
