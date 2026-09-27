#include "cmotionkeymodel.h"
#include "theme.h"
#include <QColor>
#include <QPalette>
#include <QPointer>
#include <QScopeGuard>
#include <QWidget>
#include <exception>

namespace {
QString segmentName(int type)
{
    if (type == 1) return QStringLiteral("Bézier");
    if (type == 2) return QStringLiteral("阶梯");
    if (type == 3) return QStringLiteral("反阶梯");
    return QStringLiteral("线性");
}
}

CMotionKeyModel::CMotionKeyModel(QObject* parent) : QAbstractTableModel(parent) {}

void CMotionKeyModel::SetRows(QVector<Row> rows)
{
    QHash<CMotionPointRef, int> indices;
    indices.reserve(rows.size());
    for (int row = 0; row < rows.size(); ++row) indices.insert(rows[row].ref, row);
    const QPointer<CMotionKeyModel> alive(this);
    beginResetModel();
    if (!alive) return;
    m_rows.swap(rows);
    m_indices.swap(indices);
    const bool discarded = m_draft.has_value();
    m_draft.reset();
    endResetModel();
    if (!alive) return;
    if (discarded) emit DraftChanged();
}

void CMotionKeyModel::SetEditHandler(EditHandler handler)
{
    if (!m_editing) m_edit = std::move(handler);
}

const CMotionKeyModel::Row* CMotionKeyModel::RowAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? &m_rows[row] : nullptr;
}

int CMotionKeyModel::RowFor(const CMotionPointRef& ref) const { return m_indices.value(ref, -1); }
QString CMotionKeyModel::DraftError() const { return m_draft ? m_draft->error : QString(); }
int CMotionKeyModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : m_rows.size(); }
int CMotionKeyModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : 4; }

QVariant CMotionKeyModel::data(const QModelIndex& index, int role) const
{
    const auto* row = RowAt(index.row());
    if (!index.isValid() || !row || index.column() < 0 || index.column() >= 4) return {};
    if (role == Qt::UserRole) return QVariant::fromValue(row->ref);
    if (m_draft && m_draft->ref == row->ref && m_draft->column == index.column()) {
        if (role == Qt::DisplayRole || role == Qt::EditRole) return m_draft->text;
        if (role == Qt::ToolTipRole) return QStringLiteral("<qt>%1；按 Esc 放弃这次输入。</qt>").arg(m_draft->error.toHtmlEscaped());
        if (role == Qt::ForegroundRole) {
            const auto* widget = qobject_cast<QWidget*>(parent());
            const bool dark = widget && widget->palette().color(QPalette::Base).lightness() < 128;
            return QColor(Theme::tokens(dark).value(QStringLiteral("@danger@")));
        }
    }
    if (role == Qt::ToolTipRole && index.column() == 3 && row->ref.index > 0)
        return QStringLiteral("此关键点之前的曲线段：0 线性，1 Bézier，2 阶梯，3 反阶梯");
    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
    const int precision = role == Qt::EditRole ? 17 : 9;
    if (index.column() == 0) return QString::number(row->point.x(), 'g', precision);
    if (index.column() == 1) return row->ref.track.TargetName() + QStringLiteral(" · ") + row->ref.track.id;
    if (index.column() == 2) return QString::number(row->point.y(), 'g', precision);
    return row->ref.index == 0 ? QStringLiteral("起点") : segmentName(row->segmentType);
}

QVariant CMotionKeyModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    const QStringList labels{QStringLiteral("时间"), QStringLiteral("曲线"), QStringLiteral("值"), QStringLiteral("插值")};
    return section >= 0 && section < labels.size() ? labels[section] : QVariant();
}

Qt::ItemFlags CMotionKeyModel::flags(const QModelIndex& index) const
{
    const auto* row = RowAt(index.row());
    if (!index.isValid() || !row) return Qt::NoItemFlags;
    auto result = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() != 1 && (index.column() != 3 || row->ref.index > 0)) result |= Qt::ItemIsEditable;
    return result;
}

bool CMotionKeyModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::EditRole || !(flags(index) & Qt::ItemIsEditable) || !m_edit || m_editing) return false;
    const auto ref = m_rows[index.row()].ref;
    const int column = index.column();
    if (m_draft && (m_draft->ref != ref || m_draft->column != column)) {
        emit EditRejected(QStringLiteral("请先修正或取消尚未接受的输入"));
        return false;
    }
    const auto text = value.toString();
    const QPointer<CMotionKeyModel> alive(this);
    m_editing = true;
    const auto done = qScopeGuard([alive] { if (alive) alive->m_editing = false; });
    const auto edit = m_edit;
    QString error;
    bool accepted = false;
    try { accepted = edit(ref, column, text, &error); }
    catch (const std::exception&) { error = QStringLiteral("无法应用关键点修改，原动画保持不变"); }
    if (!alive) return false;
    if (accepted) { DiscardDraft(); return true; }
    if (error.isEmpty()) error = QStringLiteral("关键点输入未被接受");
    const int row = RowFor(ref);
    if (row >= 0) {
        m_draft = Draft{ref, column, text, error};
        const auto cell = this->index(row, column);
        emit dataChanged(cell, cell, {Qt::DisplayRole, Qt::EditRole, Qt::ForegroundRole, Qt::ToolTipRole});
        if (!alive) return false;
        emit DraftChanged();
        if (!alive) return false;
    }
    emit EditRejected(error);
    return false;
}

void CMotionKeyModel::DiscardDraft()
{
    if (!m_draft) return;
    const int row = RowFor(m_draft->ref), column = m_draft->column;
    m_draft.reset();
    const QPointer<CMotionKeyModel> alive(this);
    if (row >= 0) emit dataChanged(index(row, column), index(row, column),
                                 {Qt::DisplayRole, Qt::EditRole, Qt::ForegroundRole, Qt::ToolTipRole});
    if (alive) emit DraftChanged();
}
