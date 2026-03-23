#ifndef CTREENODEDELEGATE_H
#define CTREENODEDELEGATE_H

#include <QStyledItemDelegate>
#include <QPainter>
#include <QTreeWidget>

// 按节点类型在左侧画色条
class CTreeNodeDelegate : public QStyledItemDelegate
{
public:
    explicit CTreeNodeDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);

        // 从 UserRole+1 获取节点类型
        QString type = index.data(Qt::UserRole + 1).toString();
        if(type.isEmpty()) return;

        QColor tagColor;
        if(type == "artmesh")       tagColor = QColor("#4A90D9");
        else if(type == "warp")     tagColor = QColor("#D9A04A");
        else if(type == "rotation") tagColor = QColor("#8B6BD9");
        else if(type == "part")     tagColor = QColor("#5BBF8A");
        else return;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(tagColor);

        // 左侧 3px 宽色条
        QRect tagRect = option.rect;
        tagRect.setWidth(3);
        tagRect.adjust(0, 2, 0, -2);
        painter->drawRoundedRect(tagRect, 1.5, 1.5);

        painter->restore();
    }
};

#endif // CTREENODEDELEGATE_H
