#include "cinspectorpanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QFormLayout>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QSignalBlocker>
#include <QSet>
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
    m_geometryLabel = new QLabel(content);
    m_textureLabel = new QLabel(content);
    m_drawOrderLabel = new QLabel(content);
    m_blendLabel = new QLabel(content);
    m_maskLabel = new QLabel(content);
    m_keyformLabel = new QLabel(content);

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
    form->addRow(QStringLiteral("几何"), m_geometryLabel);
    form->addRow(QStringLiteral("纹理"), m_textureLabel);
    form->addRow(QStringLiteral("绘制顺序"), m_drawOrderLabel);
    form->addRow(QStringLiteral("混合 / 剔除"), m_blendLabel);
    form->addRow(QStringLiteral("Clipping"), m_maskLabel);
    form->addRow(QStringLiteral("Keyform"), m_keyformLabel);
    layout->addLayout(form);
    layout->addStretch(1);
    setWidget(content);
    connect(m_visibilityCheck, &QCheckBox::toggled, this, [this](bool visible){
        if (m_updatingVisibility || !m_document || m_selectionName.isEmpty()) return;
        bool changed = false;
        if (m_selectionType == QLatin1String("part")) changed = m_document->SetPartVisible(m_selectionName, visible);
        else if (m_selectionType == QLatin1String("warp") || m_selectionType == QLatin1String("rotation")) changed = m_document->SetDeformerVisible(m_selectionName, visible);
        else if (m_selectionType == QLatin1String("artmesh")) changed = m_document->SetDrawableVisible(m_selectionName, visible);
        if (changed) emit VisibilityChanged(m_selectionName, m_selectionType, visible);
    });
    connect(m_opacitySpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value){
        if (m_updatingVisibility || !m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
        if (m_document->SetDrawableOpacity(m_selectionName, static_cast<float>(value)))
            emit DrawableOpacityChanged(m_selectionName, static_cast<float>(value));
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
    showModelObject(name, type);
}

void CInspectorPanel::SetSelections(const QStringList& names, const QStringList& types)
{
    if (names.size() <= 1) {
        if (!names.isEmpty()) SetSelection(names.first(), types.value(0));
        return;
    }
    m_selectionName.clear();
    m_selectionType.clear();
    m_emptyLabel->setVisible(true);
    m_updatingVisibility = true;
    m_visibilityCheck->setChecked(false);
    m_visibilityCheck->setEnabled(false);
    m_opacitySpin->setValue(1.0);
    m_opacitySpin->setEnabled(false);
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

void CInspectorPanel::ClearSelection()
{
    m_selectionName.clear();
    m_selectionType.clear();
    m_emptyLabel->setText(QStringLiteral("从模型结构中选择一个对象"));
    m_emptyLabel->setVisible(true);
    m_updatingVisibility = true;
    m_visibilityCheck->setChecked(false);
    m_opacitySpin->setValue(1.0);
    m_visibilityCheck->setEnabled(false);
    m_opacitySpin->setEnabled(false);
    m_updatingVisibility = false;
    for (QLabel* label : {m_typeLabel, m_idLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        label->clear();
    }
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
    m_visibilityCheck->setEnabled(true);
    m_opacitySpin->setEnabled(false);

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
            m_opacitySpin->setEnabled(true);
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
