#include "cinspectorpanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSet>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QWidget>

namespace {
QString indexValue(int index, const QString& fallback = QStringLiteral("根"))
{
    return index < 0 ? fallback : QString::number(index);
}
}

CInspectorPanel::CInspectorPanel(QWidget* parent)
    : QDockWidget(QStringLiteral("属性"), parent)
{
    setObjectName(QStringLiteral("inspectorPanel"));
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    m_emptyLabel = new QLabel(QStringLiteral("从模型结构中选择一个对象"), content);
    m_emptyLabel->setObjectName(QStringLiteral("inspectorEmpty"));
    m_emptyLabel->setWordWrap(true);
    layout->addWidget(m_emptyLabel);

    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form->setVerticalSpacing(9);
    form->setHorizontalSpacing(12);

    m_typeLabel = new QLabel(content);
    m_idLabel = new QLabel(content);
    m_parentLabel = new QLabel(content);
    m_deformerLabel = new QLabel(content);
    m_visibilityLabel = new QLabel(content);
    m_visibilityCheck = new QCheckBox(QStringLiteral("显示"), content);
    m_opacitySpin = new QDoubleSpinBox(content);
    m_opacitySpin->setRange(0.0, 1.0);
    m_opacitySpin->setDecimals(3);
    m_opacitySpin->setSingleStep(0.05);
    m_drawOrderSpin = new QSpinBox(content);
    m_drawOrderSpin->setRange(0, 1000);
    m_geometryLabel = new QLabel(content);
    m_textureLabel = new QLabel(content);
    m_drawOrderLabel = new QLabel(content);
    m_blendLabel = new QLabel(content);
    m_maskLabel = new QLabel(content);
    m_keyformLabel = new QLabel(content);
    m_keyformCombo = new QComboBox(content);
    m_keyformCombo->setObjectName(QStringLiteral("keyformSelector"));
    m_keyformCombo->setEnabled(false);
    m_vertexTable = new QTableWidget(content);
    m_vertexTable->setObjectName(QStringLiteral("keyformVertexTable"));
    m_vertexTable->setColumnCount(3);
    m_vertexTable->setHorizontalHeaderLabels({QStringLiteral("顶点"), QStringLiteral("X"), QStringLiteral("Y")});
    m_vertexTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_vertexTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_vertexTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_vertexTable->verticalHeader()->setVisible(false);
    m_vertexTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_vertexTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_vertexTable->setMinimumHeight(120);
    m_vertexTable->setMaximumHeight(280);
    auto* keyformTools = new QWidget(content);
    auto* keyformToolsLayout = new QHBoxLayout(keyformTools);
    keyformToolsLayout->setContentsMargins(0, 0, 0, 0);
    m_mirrorHorizontal = new QPushButton(QStringLiteral("水平镜像"), keyformTools);
    m_mirrorVertical = new QPushButton(QStringLiteral("垂直镜像"), keyformTools);
    m_interpolatePrevious = new QPushButton(QStringLiteral("与前一形插值"), keyformTools);
    m_copyToNext = new QPushButton(QStringLiteral("复制到后一形"), keyformTools);
    m_resetKeyform = new QPushButton(QStringLiteral("重置当前形"), keyformTools);
    m_mirrorHorizontal->setObjectName(QStringLiteral("mirrorHorizontal"));
    m_mirrorVertical->setObjectName(QStringLiteral("mirrorVertical"));
    m_interpolatePrevious->setObjectName(QStringLiteral("interpolatePreviousKeyform"));
    m_copyToNext->setObjectName(QStringLiteral("copyKeyformToNext"));
    m_resetKeyform->setObjectName(QStringLiteral("resetKeyform"));
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    keyformToolsLayout->addWidget(m_mirrorHorizontal);
    keyformToolsLayout->addWidget(m_mirrorVertical);
    keyformToolsLayout->addWidget(m_interpolatePrevious);
    keyformToolsLayout->addWidget(m_copyToNext);
    keyformToolsLayout->addWidget(m_resetKeyform);

    for (QLabel* value : {m_typeLabel, m_idLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
    }
    form->addRow(QStringLiteral("类型"), m_typeLabel);
    form->addRow(QStringLiteral("ID"), m_idLabel);
    form->addRow(QStringLiteral("所属 Part"), m_parentLabel);
    form->addRow(QStringLiteral("父变形器"), m_deformerLabel);
    form->addRow(QStringLiteral("可见 / 启用"), m_visibilityLabel);
    form->addRow(QStringLiteral("编辑可见性"), m_visibilityCheck);
    form->addRow(QStringLiteral("编辑透明度"), m_opacitySpin);
    form->addRow(QStringLiteral("编辑绘制顺序"), m_drawOrderSpin);
    form->addRow(QStringLiteral("几何"), m_geometryLabel);
    form->addRow(QStringLiteral("纹理"), m_textureLabel);
    form->addRow(QStringLiteral("绘制顺序"), m_drawOrderLabel);
    form->addRow(QStringLiteral("混合 / 剔除"), m_blendLabel);
    form->addRow(QStringLiteral("Clipping"), m_maskLabel);
    form->addRow(QStringLiteral("Keyform"), m_keyformLabel);
    form->addRow(QStringLiteral("关键形选择"), m_keyformCombo);
    form->addRow(QStringLiteral("关键形工具"), keyformTools);
    form->addRow(QStringLiteral("顶点位置"), m_vertexTable);
    layout->addLayout(form);
    layout->addStretch(1);
    setWidget(content);
    connect(m_keyformCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){
        if (!m_updatingKeyform) RebuildKeyformTable();
    });
    connect(m_vertexTable, &QTableWidget::cellChanged, this, [this](int row, int column){
        if (!m_updatingKeyform) ApplyKeyformCellEdit(row, column);
    });
    connect(m_mirrorHorizontal, &QPushButton::clicked, this, [this](){ MirrorKeyform(true); });
    connect(m_mirrorVertical, &QPushButton::clicked, this, [this](){ MirrorKeyform(false); });
    connect(m_interpolatePrevious, &QPushButton::clicked, this, [this](){ InterpolatePreviousKeyform(); });
    connect(m_copyToNext, &QPushButton::clicked, this, [this](){ CopyKeyformToNext(); });
    connect(m_resetKeyform, &QPushButton::clicked, this, [this](){ ResetCurrentKeyform(); });
    connect(m_visibilityCheck, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state){
        if (state == Qt::PartiallyChecked) return;
        const bool visible = state == Qt::Checked;
        if (m_updatingVisibility || !m_document) return;
        if (m_selectionNames.size() > 1) {
            for (int i = 0; i < m_selectionNames.size(); ++i) {
                const QString name = m_selectionNames[i];
                const QString type = m_selectionTypes.value(i);
                if (IsLocked(type, name)) continue;
                bool before = visible;
                if (type == QLatin1String("part")) {
                    for (const auto& part : m_document->Parts()) if (part.id == name) { before = part.visible; break; }
                } else if (type == QLatin1String("warp") || type == QLatin1String("rotation")) {
                    for (const auto& deformer : m_document->Deformers()) if (deformer.id == name) { before = deformer.visible; break; }
                } else if (type == QLatin1String("artmesh")) {
                    for (const auto& drawable : m_document->Drawables()) if (drawable.id == name) { before = drawable.visible; break; }
                } else {
                    continue;
                }
                if (before == visible) continue;
                const live2dstruct::CModelEdit command{name, type, live2dstruct::CModelEdit::Visibility, before, visible};
                if (m_document->ExecuteEdit(command)) {
                    emit VisibilityChanged(name, type, visible);
                }
            }
            return;
        }
        if (m_selectionName.isEmpty()) return;
        if (IsLocked(m_selectionType, m_selectionName)) return;
        bool before = visible;
        if (m_selectionType == QLatin1String("part")) {
            for (const auto& part : m_document->Parts()) if (part.id == m_selectionName) { before = part.visible; break; }
        } else if (m_selectionType == QLatin1String("warp") || m_selectionType == QLatin1String("rotation")) {
            for (const auto& deformer : m_document->Deformers()) if (deformer.id == m_selectionName) { before = deformer.visible; break; }
        } else if (m_selectionType == QLatin1String("artmesh")) {
            for (const auto& drawable : m_document->Drawables()) if (drawable.id == m_selectionName) { before = drawable.visible; break; }
        }
        if (before == visible) return;
        const live2dstruct::CModelEdit command{m_selectionName, m_selectionType,
                                               live2dstruct::CModelEdit::Visibility, before, visible};
        if (m_document->ExecuteEdit(command)) {
            emit VisibilityChanged(m_selectionName, m_selectionType, visible);
        }
    });
    connect(m_opacitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value){
        if (m_updatingVisibility || !m_document) return;
        if (m_selectionNames.size() > 1) {
            for (int i = 0; i < m_selectionNames.size(); ++i) {
                if (m_selectionTypes.value(i) != QLatin1String("artmesh")) continue;
                const QString name = m_selectionNames[i];
                if (IsLocked(QStringLiteral("artmesh"), name)) continue;
                float before = static_cast<float>(value);
                for (const auto& drawable : m_document->Drawables()) if (drawable.id == name) { before = m_document->DrawableOpacity(drawable); break; }
                const float after = static_cast<float>(value);
                if (qFuzzyCompare(before + 1.0f, after + 1.0f)) continue;
                const live2dstruct::CModelEdit command{name, QLatin1String("artmesh"),
                                                       live2dstruct::CModelEdit::Opacity, before, after};
                if (m_document->ExecuteEdit(command)) {
                    emit DrawableOpacityChanged(name, after);
                }
            }
            return;
        }
        if (m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
        if (IsLocked(m_selectionType, m_selectionName)) return;
        float before = static_cast<float>(value);
        for (const auto& drawable : m_document->Drawables()) if (drawable.id == m_selectionName) { before = m_document->DrawableOpacity(drawable); break; }
        if (qFuzzyCompare(before + 1.0f, static_cast<float>(value) + 1.0f)) return;
        const float after = static_cast<float>(value);
        const live2dstruct::CModelEdit command{m_selectionName, m_selectionType,
                                               live2dstruct::CModelEdit::Opacity, before, after};
        if (m_document->ExecuteEdit(command)) {
            emit DrawableOpacityChanged(m_selectionName, after);
        }
    });
    connect(m_drawOrderSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value){
        if (m_updatingVisibility || !m_document) return;
        if (m_selectionNames.size() > 1) {
            for (int i = 0; i < m_selectionNames.size(); ++i) {
                if (m_selectionTypes.value(i) != QLatin1String("artmesh")) continue;
                const QString name = m_selectionNames[i];
                if (IsLocked(QStringLiteral("artmesh"), name)) continue;
                int before = value;
                for (const auto& drawable : m_document->Drawables()) if (drawable.id == name) { before = qRound(m_document->DrawableDrawOrder(drawable)); break; }
                if (before == value) continue;
                const live2dstruct::CModelEdit command{name, QLatin1String("artmesh"),
                                                       live2dstruct::CModelEdit::DrawOrder, before, value};
                if (m_document->ExecuteEdit(command)) {
                    emit DrawableDrawOrderChanged(name, value);
                }
            }
            return;
        }
        if (m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
        if (IsLocked(m_selectionType, m_selectionName)) return;
        int before = value;
        for (const auto& drawable : m_document->Drawables()) if (drawable.id == m_selectionName) { before = qRound(m_document->DrawableDrawOrder(drawable)); break; }
        if (before == value) return;
        const live2dstruct::CModelEdit command{m_selectionName, m_selectionType,
                                               live2dstruct::CModelEdit::DrawOrder, before, value};
        if (m_document->ExecuteEdit(command)) {
            emit DrawableDrawOrderChanged(m_selectionName, value);
        }
    });
    ClearSelection();
}

void CInspectorPanel::SetDocument(live2dstruct::CModelDocument* document)
{
    m_document = document;
    if (m_selectionName.isEmpty()) ClearSelection();
    else showModelObject(m_selectionName, m_selectionType);
}

void CInspectorPanel::SetSelection(const QString& name, const QString& type)
{
    m_selectionName = name;
    m_selectionType = type;
    m_selectionNames = {name};
    m_selectionTypes = {type};
    m_visibilityCheck->setTristate(false);
    showModelObject(name, type);
}

void CInspectorPanel::SetSelections(const QStringList& names, const QStringList& types)
{
    if (names.size() <= 1) {
        if (names.isEmpty()) ClearSelection();
        else SetSelection(names.first(), types.value(0));
        return;
    }
    m_selectionName.clear();
    m_selectionType.clear();
    m_selectionNames = names;
    m_selectionTypes = types;
    m_emptyLabel->setVisible(true);
    m_updatingKeyform = true;
    m_keyformCombo->clear();
    m_keyformCombo->setEnabled(false);
    m_vertexTable->setRowCount(0);
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    m_updatingKeyform = false;
    m_updatingVisibility = true;
    m_visibilityCheck->setTristate(true);
    bool anyVisible = false;
    bool anyHidden = false;
    bool allVisibleEditable = true;
    bool allArtMeshes = true;
    bool allUnlocked = true;
    if (!m_document) {
        allVisibleEditable = false;
        allArtMeshes = false;
    }
    for (int i = 0; i < names.size(); ++i) {
        if (!m_document) break;
        if (i >= types.size()) {
            allVisibleEditable = false;
            allArtMeshes = false;
            continue;
        }
        const QString type = types.value(i);
        const QString name = names[i];
        allUnlocked = allUnlocked && !IsLocked(type, name);
        bool found = false;
        if (type == QLatin1String("part")) {
            for (const auto& part : m_document->Parts()) if (part.id == name) { anyVisible |= part.visible; anyHidden |= !part.visible; found = true; break; }
        } else if (type == QLatin1String("warp") || type == QLatin1String("rotation")) {
            for (const auto& deformer : m_document->Deformers()) if (deformer.id == name) { anyVisible |= deformer.visible; anyHidden |= !deformer.visible; found = true; break; }
        } else if (type == QLatin1String("artmesh")) {
            for (const auto& drawable : m_document->Drawables()) if (drawable.id == name) { anyVisible |= drawable.visible; anyHidden |= !drawable.visible; found = true; break; }
        } else {
            allVisibleEditable = false;
        }
        if (!found) allVisibleEditable = false;
        allArtMeshes = allArtMeshes && type == QLatin1String("artmesh");
        if (!found) allArtMeshes = false;
    }
    if (anyVisible && anyHidden) m_visibilityCheck->setCheckState(Qt::PartiallyChecked);
    else m_visibilityCheck->setChecked(anyVisible);
    m_visibilityCheck->setEnabled(allVisibleEditable && allUnlocked);
    m_opacitySpin->setEnabled(allArtMeshes && allUnlocked);
    m_drawOrderSpin->setEnabled(allArtMeshes && allUnlocked);
    if (allArtMeshes && !names.isEmpty()) {
        for (const auto& drawable : m_document->Drawables()) {
            if (drawable.id == names.first()) {
                m_opacitySpin->setValue(m_document->DrawableOpacity(drawable));
                m_drawOrderSpin->setValue(qRound(m_document->DrawableDrawOrder(drawable)));
                break;
            }
        }
    } else {
        m_opacitySpin->setValue(1.0);
        m_drawOrderSpin->setValue(0);
    }
    m_updatingVisibility = false;
    m_emptyLabel->setText(QStringLiteral("已选择 %1 个对象，可在结构树中继续调整范围").arg(names.size()));
    for (QLabel* label : {m_typeLabel, m_idLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        label->clear();
    }
    QSet<QString> uniqueTypes;
    for (const QString& type : types) uniqueTypes.insert(type);
    m_typeLabel->setText(uniqueTypes.values().join(QStringLiteral("、")));
    m_idLabel->setText(QStringLiteral("%1 个对象").arg(names.size()));
}

void CInspectorPanel::RefreshSelection()
{
    if (m_selectionNames.size() > 1) SetSelections(m_selectionNames, m_selectionTypes);
    else if (!m_selectionName.isEmpty()) showModelObject(m_selectionName, m_selectionType);
}

void CInspectorPanel::SetLockedState(const QSet<QString>& lockedKeys)
{
    m_lockedKeys = lockedKeys;
    RefreshSelection();
}

bool CInspectorPanel::IsLocked(const QString& type, const QString& name) const
{
    return m_lockedKeys.contains(type + QLatin1Char('|') + name);
}

void CInspectorPanel::RebuildKeyformTable()
{
    m_updatingKeyform = true;
    m_vertexTable->setRowCount(0);
    const int keyformIndex = m_keyformCombo->currentIndex();
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        keyformIndex < 0 || IsLocked(m_selectionType, m_selectionName)) {
        m_vertexTable->setEnabled(false);
        m_updatingKeyform = false;
        return;
    }
    const QVector<float> positions = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (positions.isEmpty() || positions.size() % 2 != 0) {
        m_vertexTable->setEnabled(false);
        m_updatingKeyform = false;
        return;
    }
    m_vertexTable->setRowCount(positions.size() / 2);
    for (int row = 0; row < m_vertexTable->rowCount(); ++row) {
        auto* indexItem = new QTableWidgetItem(QString::number(row));
        indexItem->setFlags(indexItem->flags() & ~Qt::ItemIsEditable);
        m_vertexTable->setItem(row, 0, indexItem);
        m_vertexTable->setItem(row, 1, new QTableWidgetItem(QString::number(positions[row * 2], 'f', 6)));
        m_vertexTable->setItem(row, 2, new QTableWidgetItem(QString::number(positions[row * 2 + 1], 'f', 6)));
    }
    m_vertexTable->setEnabled(true);
    m_updatingKeyform = false;
}

void CInspectorPanel::ApplyKeyformCellEdit(int row, int column)
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName) ||
        (column != 1 && column != 2) || row < 0 || row >= m_vertexTable->rowCount()) return;
    const int keyformIndex = m_keyformCombo->currentIndex();
    QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.size() != m_vertexTable->rowCount() * 2) return;
    bool ok = false;
    const double parsed = m_vertexTable->item(row, column)->text().toDouble(&ok);
    if (!ok || !std::isfinite(parsed)) {
        m_updatingKeyform = true;
        m_vertexTable->item(row, column)->setText(QString::number(before[row * 2 + column - 1], 'f', 6));
        m_updatingKeyform = false;
        return;
    }
    QVector<float> after = before;
    after[row * 2 + column - 1] = static_cast<float>(parsed);
    if (qFuzzyCompare(before[row * 2 + column - 1] + 1.0f, after[row * 2 + column - 1] + 1.0f)) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex};
    if (!m_document->ExecuteEdit(command)) {
        m_updatingKeyform = true;
        m_vertexTable->item(row, column)->setText(QString::number(before[row * 2 + column - 1], 'f', 6));
        m_updatingKeyform = false;
        return;
    }
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::MirrorKeyform(bool horizontal)
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_keyformCombo->currentIndex();
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty() || before.size() % 2 != 0) return;
    float minimum = before[horizontal ? 0 : 1];
    float maximum = minimum;
    for (int i = horizontal ? 0 : 1; i < before.size(); i += 2) {
        minimum = qMin(minimum, before[i]);
        maximum = qMax(maximum, before[i]);
    }
    const float center = (minimum + maximum) * 0.5f;
    QVector<float> after = before;
    for (int i = horizontal ? 0 : 1; i < after.size(); i += 2) after[i] = 2.0f * center - after[i];
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!m_document->ExecuteEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::InterpolatePreviousKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_keyformCombo->currentIndex();
    if (keyformIndex <= 0) return;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    const QVector<float> previous = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex - 1);
    if (before.isEmpty() || before.size() != previous.size()) return;
    QVector<float> after;
    after.resize(before.size());
    for (int i = 0; i < before.size(); ++i) after[i] = (before[i] + previous[i]) * 0.5f;
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!m_document->ExecuteEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::CopyKeyformToNext()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_keyformCombo->currentIndex();
    const QVector<float> source = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex + 1);
    if (keyformIndex < 0 || source.isEmpty() || source.size() != before.size()) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(source), keyformIndex + 1, false};
    if (!m_document->ExecuteEdit(command)) return;
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex + 1, source);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::ResetCurrentKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_keyformCombo->currentIndex();
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    QVector<float> original;
    for (const auto& drawable : m_document->Drawables()) {
        if (drawable.id == m_selectionName && keyformIndex >= 0 && keyformIndex < drawable.keyforms.size()) {
            original = drawable.keyforms.at(keyformIndex).positions;
            break;
        }
    }
    if (before.isEmpty() || original.isEmpty() || before.size() != original.size() || before == original) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(original), keyformIndex, false};
    if (!m_document->ExecuteEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, original);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::ClearSelection()
{
    m_selectionName.clear();
    m_selectionType.clear();
    m_selectionNames.clear();
    m_selectionTypes.clear();
    m_emptyLabel->setText(QStringLiteral("从模型结构中选择一个对象"));
    m_emptyLabel->setVisible(true);
    m_updatingVisibility = true;
    m_visibilityCheck->setTristate(false);
    m_visibilityCheck->setChecked(false);
    m_opacitySpin->setValue(1.0);
    m_visibilityCheck->setEnabled(false);
    m_opacitySpin->setEnabled(false);
    m_drawOrderSpin->setValue(0);
    m_drawOrderSpin->setEnabled(false);
    m_updatingVisibility = false;
    m_updatingKeyform = true;
    m_keyformCombo->clear();
    m_keyformCombo->setEnabled(false);
    m_vertexTable->setRowCount(0);
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    m_updatingKeyform = false;
    for (QLabel* label : {m_typeLabel, m_idLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        label->clear();
    }
}

void CInspectorPanel::EmitEdit(const live2dstruct::CModelEdit& edit, bool forward)
{
    const QVariant& value = forward ? edit.after : edit.before;
    if (edit.field == live2dstruct::CModelEdit::Visibility) {
        emit VisibilityChanged(edit.name, edit.type, value.toBool());
    } else if (edit.field == live2dstruct::CModelEdit::Opacity) {
        emit DrawableOpacityChanged(edit.name, value.toFloat());
    } else if (edit.field == live2dstruct::CModelEdit::DrawOrder) {
        emit DrawableDrawOrderChanged(edit.name, value.toInt());
    } else if (edit.field == live2dstruct::CModelEdit::KeyformPositions) {
        emit DrawableKeyformPositionsChanged(edit.name, edit.index, value.value<QVector<float>>());
    }
    if (!m_selectionName.isEmpty()) showModelObject(m_selectionName, m_selectionType);
}

void CInspectorPanel::ClearHistory()
{
    if (m_document) m_document->ClearEditHistory();
    emit HistoryChanged(false, false);
}

void CInspectorPanel::SyncHistoryState()
{
    if (!m_document) {
        emit HistoryChanged(false, false);
        return;
    }
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::Undo()
{
    if (!m_document) return;
    live2dstruct::CModelEdit pending;
    if (!m_document->PeekUndoEdit(&pending) || pending.field == live2dstruct::CModelEdit::ParameterValue) return;
    live2dstruct::CModelEdit edit;
    if (!m_document->UndoEdit(&edit)) return;
    EmitEdit(edit, false);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::Redo()
{
    if (!m_document) return;
    live2dstruct::CModelEdit pending;
    if (!m_document->PeekRedoEdit(&pending) || pending.field == live2dstruct::CModelEdit::ParameterValue) return;
    live2dstruct::CModelEdit edit;
    if (!m_document->RedoEdit(&edit)) return;
    EmitEdit(edit, true);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::showModelObject(const QString& name, const QString& type)
{
    if (!m_document || name.isEmpty()) {
        ClearSelection();
        return;
    }
    m_emptyLabel->setVisible(false);
    m_typeLabel->setText(type == QLatin1String("artmesh") ? QStringLiteral("ArtMesh")
                       : type == QLatin1String("part") ? QStringLiteral("Part")
                       : type == QLatin1String("warp") ? QStringLiteral("Warp Deformer")
                       : type == QLatin1String("rotation") ? QStringLiteral("Rotation Deformer")
                       : type);
    m_idLabel->setText(name);
    m_parentLabel->setText(QStringLiteral("—"));
    m_deformerLabel->setText(QStringLiteral("—"));
    m_visibilityLabel->setText(QStringLiteral("—"));
    m_geometryLabel->setText(QStringLiteral("—"));
    m_textureLabel->setText(QStringLiteral("—"));
    m_drawOrderLabel->setText(QStringLiteral("—"));
    m_blendLabel->setText(QStringLiteral("—"));
    m_maskLabel->setText(QStringLiteral("—"));
    m_keyformLabel->setText(QStringLiteral("—"));
    const bool unlocked = !IsLocked(type, name);
    m_visibilityCheck->setEnabled(unlocked);
    m_opacitySpin->setEnabled(false);
    m_drawOrderSpin->setEnabled(false);
    m_updatingKeyform = true;
    m_keyformCombo->clear();
    m_keyformCombo->setEnabled(false);
    m_vertexTable->setRowCount(0);
    m_vertexTable->setEnabled(false);
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_updatingKeyform = false;

    if (type == QLatin1String("artmesh")) {
        for (const auto& drawable : m_document->Drawables()) {
            if (drawable.id != name) continue;
            m_parentLabel->setText(indexValue(drawable.parentPartIndex));
            m_deformerLabel->setText(indexValue(drawable.parentDeformerIndex, QStringLiteral("无")));
            m_visibilityLabel->setText(QStringLiteral("%1 / %2").arg(drawable.visible ? QStringLiteral("是") : QStringLiteral("否"),
                                                                    drawable.enabled ? QStringLiteral("是") : QStringLiteral("否")));
            m_updatingVisibility = true;
            m_visibilityCheck->setChecked(drawable.visible);
            m_updatingVisibility = false;
            m_updatingVisibility = true;
            m_opacitySpin->setValue(m_document->DrawableOpacity(drawable));
            m_opacitySpin->setEnabled(unlocked);
            m_drawOrderSpin->setValue(qRound(m_document->DrawableDrawOrder(drawable)));
            m_drawOrderSpin->setEnabled(unlocked);
            m_updatingVisibility = false;
            m_geometryLabel->setText(QStringLiteral("%1 顶点，%2 索引").arg(drawable.vertexCount).arg(drawable.indices.size()));
            m_textureLabel->setText(drawable.textureIndex < 0 ? QStringLiteral("无") : QString::number(drawable.textureIndex));
            const QString blend = drawable.blendMode == 1 ? QStringLiteral("ADD")
                               : drawable.blendMode == 2 ? QStringLiteral("MULTIPLY")
                                                          : QStringLiteral("NORMAL");
            m_drawOrderLabel->setText(QStringLiteral("%1（默认透明度 %2）")
                                      .arg(QString::number(drawable.drawOrder, 'f', 0))
                                      .arg(QString::number(drawable.opacity, 'f', 3)));
            m_blendLabel->setText(QStringLiteral("%1 / %2").arg(blend, drawable.culling ? QStringLiteral("启用") : QStringLiteral("关闭")));
            m_maskLabel->setText(drawable.maskIndices.isEmpty() ? QStringLiteral("无") : QString::number(drawable.maskIndices.size()));
            m_keyformLabel->setText(QString::number(drawable.keyforms.size()));
            m_updatingKeyform = true;
            for (int keyformIndex = 0; keyformIndex < drawable.keyforms.size(); ++keyformIndex) {
                m_keyformCombo->addItem(QStringLiteral("Keyform %1").arg(keyformIndex), keyformIndex);
            }
            m_keyformCombo->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_mirrorHorizontal->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_mirrorVertical->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_interpolatePrevious->setEnabled(unlocked && drawable.keyforms.size() > 1);
            m_copyToNext->setEnabled(unlocked && drawable.keyforms.size() > 1);
            m_resetKeyform->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            if (!drawable.keyforms.isEmpty()) m_keyformCombo->setCurrentIndex(0);
            m_updatingKeyform = false;
            RebuildKeyformTable();
            return;
        }
    } else if (type == QLatin1String("part")) {
        for (const auto& part : m_document->Parts()) {
            if (part.id != name) continue;
            m_parentLabel->setText(indexValue(part.parentIndex));
            m_visibilityLabel->setText(QStringLiteral("%1 / %2").arg(part.visible ? QStringLiteral("是") : QStringLiteral("否"),
                                                                    part.enabled ? QStringLiteral("是") : QStringLiteral("否")));
            m_updatingVisibility = true;
            m_visibilityCheck->setChecked(part.visible);
            m_updatingVisibility = false;
            return;
        }
    } else {
        for (const auto& deformer : m_document->Deformers()) {
            if (deformer.id != name) continue;
            m_parentLabel->setText(indexValue(deformer.parentPartIndex));
            m_deformerLabel->setText(indexValue(deformer.parentDeformerIndex, QStringLiteral("无")));
            m_visibilityLabel->setText(QStringLiteral("%1 / %2").arg(deformer.visible ? QStringLiteral("是") : QStringLiteral("否"),
                                                                    deformer.enabled ? QStringLiteral("是") : QStringLiteral("否")));
            m_updatingVisibility = true;
            m_visibilityCheck->setChecked(deformer.visible);
            m_updatingVisibility = false;
            if (deformer.type == live2dstruct::CModelDeformer::Warp) {
                m_geometryLabel->setText(QStringLiteral("Warp %1 × %2").arg(deformer.columns).arg(deformer.rows));
            } else {
                m_geometryLabel->setText(QStringLiteral("Rotation Deformer"));
            }
            return;
        }
    }
    ClearSelection();
}
