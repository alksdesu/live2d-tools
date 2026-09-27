#ifndef CMOTIONKEYMODEL_H
#define CMOTIONKEYMODEL_H

#include "live2dstruct/cmotiontimeline.h"
#include <QAbstractTableModel>
#include <QHash>
#include <functional>
#include <optional>

class CMotionKeyModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    struct Row { CMotionPointRef ref; QPointF point; int segmentType = 0; };
    using EditHandler = std::function<bool(const CMotionPointRef&, int column, const QString& text, QString* error)>;
    explicit CMotionKeyModel(QObject* parent = nullptr);
    void SetRows(QVector<Row> rows);
    void SetEditHandler(EditHandler handler);
    const Row* RowAt(int row) const;
    int RowFor(const CMotionPointRef& ref) const;
    bool HasRejectedDraft() const { return m_draft.has_value(); }
    QString DraftError() const;
    void DiscardDraft();
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

signals:
    void DraftChanged();
    void EditRejected(const QString& error);

private:
    struct Draft { CMotionPointRef ref; int column = 0; QString text, error; };
    QVector<Row> m_rows;
    QHash<CMotionPointRef, int> m_indices;
    EditHandler m_edit;
    std::optional<Draft> m_draft;
    bool m_editing = false;
};

#endif
