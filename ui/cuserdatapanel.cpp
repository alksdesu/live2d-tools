#include "cuserdatapanel.h"
#include "ctableeditdelegate.h"

#include "std/cmodeljson.h"
#include "std/cmodeljsonwriter.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <exception>

CUserDataPanel::CUserDataPanel(QWidget* parent)
    : QDockWidget(QStringLiteral("网格标签 · UserData"), parent)
{
    setObjectName(QStringLiteral("userDataPanel"));
    auto* wrapper = new QWidget(this);
    auto* layout = new QVBoxLayout(wrapper);
    m_status = new QLabel(wrapper);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto* toolbar = new QHBoxLayout();
    m_addButton = new QPushButton(QStringLiteral("添加标签"), wrapper);
    m_addButton->setObjectName(QStringLiteral("addUserData"));
    m_removeButton = new QPushButton(QStringLiteral("删除"), wrapper);
    m_removeButton->setObjectName(QStringLiteral("removeUserData"));
    m_saveButton = new QPushButton(QStringLiteral("保存"), wrapper);
    m_saveButton->setObjectName(QStringLiteral("primaryBtn"));
    toolbar->addWidget(m_addButton);
    toolbar->addWidget(m_removeButton);
    toolbar->addStretch(1);
    toolbar->addWidget(m_saveButton);
    layout->addLayout(toolbar);

    m_table = new QTableWidget(wrapper);
    m_table->setObjectName(QStringLiteral("userDataTable"));
    m_table->setColumnCount(2);
    m_valueDelegate = new CTableEditDelegate(m_table);
    m_table->setItemDelegateForColumn(1, m_valueDelegate);
    m_table->setHorizontalHeaderLabels({QStringLiteral("ArtMesh ID"), QStringLiteral("标签内容")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setAccessibleName(QStringLiteral("ArtMesh 用户数据"));
    m_table->setToolTip(QStringLiteral("编辑网格标签；双击 ID 可在画布中定位"));
    layout->addWidget(m_table);
    setWidget(wrapper);

    connect(m_addButton, &QPushButton::clicked, this, &CUserDataPanel::AddRow);
    connect(m_removeButton, &QPushButton::clicked, this, &CUserDataPanel::RemoveRows);
    connect(m_saveButton, &QPushButton::clicked, this, [this](){
        QString error;
        if (!SaveUserData(&error)) QMessageBox::warning(this, QStringLiteral("保存 UserData 失败"), error);
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](){
        if (!m_updating) SetDirty(true);
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &CUserDataPanel::RefreshState);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int column){
        if (column != 0 || !m_table->item(row, 0)) return;
        const QString id = m_table->item(row, 0)->text();
        if (!m_hasModelIds || m_validArtMeshIds.contains(id)) emit ArtMeshSelected(id);
    });
    RefreshState();
}

void CUserDataPanel::SetValidArtMeshIds(const QSet<QString>& ids)
{
    m_validArtMeshIds = ids;
    m_hasModelIds = true;
    RefreshState();
}

bool CUserDataPanel::LoadModel(const QString& modelJsonPath, QString* error)
{
    CModelJson model;
    model.Parse(modelJsonPath);
    if (!model.IsOpen()) {
        if (error) *error = QStringLiteral("无法读取模型的 UserData 引用");
        return false;
    }
    const QString path = model.m_UserData.isEmpty() ? QString() : QDir(model.m_parentPath).filePath(model.m_UserData);
    if (!LoadUserData(path, error)) return false;
    m_modelJsonPath = QDir::cleanPath(QFileInfo(modelJsonPath).absoluteFilePath());
    m_modelSourceDigest = model.SourceDigest();
    RefreshState();
    return true;
}

bool CUserDataPanel::LoadUserData(const QString& path, QString* error)
{
    live2dstruct::CModelUserData candidate;
    QByteArray digest;
    const auto sourcePath = path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!sourcePath.isEmpty() && !candidate.Load(sourcePath, error, &digest)) return false;
    m_data = candidate;
    m_path = sourcePath;
    m_sourceDigest.swap(digest);
    m_modelJsonPath.clear();
    m_modelSourceDigest.clear();
    RebuildTable();
    SetDirty(false);
    RefreshState();
    return true;
}

void CUserDataPanel::RebuildTable()
{
    m_updating = true;
    m_table->setRowCount(0);
    for (const auto& entry : m_data.Entries()) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto* id = new QTableWidgetItem(entry.id);
        id->setData(Qt::UserRole, entry.fields);
        id->setFlags(id->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 0, id);
        m_table->setItem(row, 1, new QTableWidgetItem(entry.value));
    }
    m_updating = false;
}

bool CUserDataPanel::ReadTable(live2dstruct::CModelUserData* data, QString* error) const
{
    if (!data) return false;
    QVector<live2dstruct::CModelUserDataEntry> entries;
    entries.reserve(m_table->rowCount());
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto* idItem = m_table->item(row, 0);
        const QString id = idItem ? idItem->text() : QString();
        const auto* valueItem = m_table->item(row, 1);
        if (m_hasModelIds && !m_validArtMeshIds.contains(id)) {
            if (error) *error = QStringLiteral("第 %1 行的 ArtMesh 不存在：%2").arg(row + 1).arg(id);
            return false;
        }
        entries.append({QStringLiteral("ArtMesh"), id, valueItem ? valueItem->text() : QString(),
                        idItem ? idItem->data(Qt::UserRole).value<QJsonObject>() : QJsonObject{}});
    }
    *data = m_data;
    data->SetEntries(entries);
    return data->Validate(error);
}

bool CUserDataPanel::CaptureUserDataBytes(QByteArray& bytes, QString* error) const
{
    if (error) error->clear();
    try {
        if (m_updating || m_valueDelegate->HasPendingChanges()) {
            if (error) *error = QStringLiteral("请先确认或取消正在编辑的网格标签");
            return false;
        }
        live2dstruct::CModelUserData data;
        if (!ReadTable(&data, error)) return false;
        QByteArray candidate = data.ToJson();
        live2dstruct::CModelUserData validated;
        if (!validated.FromJson(candidate, error)) return false;
        bytes.swap(candidate);
        return true;
    } catch (const std::exception&) {
        if (error) *error = QStringLiteral("无法生成网格标签快照");
        return false;
    }
}

bool CUserDataPanel::SaveUserData(QString* error)
{
    m_valueDelegate->CommitEditor();
    live2dstruct::CModelUserData data;
    if (!ReadTable(&data, error)) return false;
    QByteArray digest;
    QString savedPath = m_path;
    if (m_path.isEmpty()) {
        if (m_modelJsonPath.isEmpty() || m_modelSourceDigest.size() != 32) {
            if (error) *error = QStringLiteral("请先打开模型");
            return false;
        }
        digest = QCryptographicHash::hash(data.ToJson(), QCryptographicHash::Sha256);
        if (!CModelJsonWriter::CreateUserData(m_modelJsonPath, data, &savedPath, error, m_modelSourceDigest)) return false;
    } else if (!data.Save(m_path, error, &digest)) {
        return false;
    }
    m_data = data;
    m_path = QDir::cleanPath(QFileInfo(savedPath).absoluteFilePath());
    m_sourceDigest.swap(digest);
    SetDirty(false);
    RefreshState();
    emit UserDataSaved(m_path);
    return true;
}

bool CUserDataPanel::HasUnsavedChanges() const
{
    return m_dirty || m_valueDelegate->HasPendingChanges();
}

void CUserDataPanel::AddRow()
{
    QStringList ids = m_validArtMeshIds.values();
    for (int row = 0; row < m_table->rowCount(); ++row)
        if (m_table->item(row, 0)) ids.removeAll(m_table->item(row, 0)->text());
    ids.sort();
    if (ids.isEmpty()) return;
    bool accepted = false;
    const QString id = QInputDialog::getItem(this, QStringLiteral("添加网格标签"),
                                             QStringLiteral("ArtMesh"), ids, 0, false, &accepted);
    if (!accepted || id.isEmpty()) return;
    const int row = m_table->rowCount();
    m_updating = true;
    m_table->insertRow(row);
    auto* idItem = new QTableWidgetItem(id);
    idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
    m_table->setItem(row, 0, idItem);
    m_table->setItem(row, 1, new QTableWidgetItem());
    m_updating = false;
    m_table->setCurrentCell(row, 1);
    m_table->editItem(m_table->item(row, 1));
    SetDirty(true);
}

void CUserDataPanel::RemoveRows()
{
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    QList<int> indices;
    for (const QModelIndex& index : rows) indices.append(index.row());
    std::sort(indices.rbegin(), indices.rend());
    m_updating = true;
    for (int row : indices) m_table->removeRow(row);
    m_updating = false;
    SetDirty(true);
}

void CUserDataPanel::SetDirty(bool dirty)
{
    if (m_dirty != dirty) {
        m_dirty = dirty;
        emit DirtyChanged(dirty);
    }
    RefreshState();
}

void CUserDataPanel::RefreshState()
{
    const bool editable = !m_path.isEmpty() || !m_modelJsonPath.isEmpty();
    QSet<QString> available = m_validArtMeshIds;
    for (int row = 0; row < m_table->rowCount(); ++row)
        if (m_table->item(row, 0)) available.remove(m_table->item(row, 0)->text());
    m_addButton->setEnabled(editable && !available.isEmpty());
    m_removeButton->setEnabled(editable && !m_table->selectionModel()->selectedRows().isEmpty());
    m_saveButton->setEnabled(editable);
    m_table->setEnabled(editable);
    if (!editable) m_status->setText(QStringLiteral("打开模型后可为网格添加标签。"));
    else if (m_path.isEmpty()) m_status->setText(QStringLiteral("此模型暂无网格标签。首次保存会创建 UserData 文件并关联模型。"));
    else m_status->setText(QStringLiteral("%1 个网格标签 · %2").arg(m_table->rowCount())
                               .arg(m_dirty ? QStringLiteral("未保存") : QStringLiteral("已保存")));
    m_status->setToolTip(m_path);
}
