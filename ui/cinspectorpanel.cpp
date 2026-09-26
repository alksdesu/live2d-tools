#include "cinspectorpanel.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmeshtopology.h"
#include <QFormLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QPointer>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QListView>
#include <QStyledItemDelegate>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QTimer>
#include <QSpinBox>
#include <QSet>
#include <QTableWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QWidget>
#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <cmath>
#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace {
class VisibilityCheckBox : public QCheckBox
{
public:
    using QCheckBox::QCheckBox;
protected:
    void nextCheckState() override
    {
        setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }
};

class InspectorComboBox : public QComboBox
{
public:
    explicit InspectorComboBox(QWidget* parent) : QComboBox(parent)
    {
        setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        setMinimumContentsLength(12);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* choices = new QListView(this);
        setView(choices);
        setItemDelegate(new QStyledItemDelegate(choices));
        choices->setTextElideMode(Qt::ElideNone);
        choices->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        connect(this, &QComboBox::currentTextChanged, this, [this](const QString& text) {
            setToolTip(QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()));
        });
    }
};

void setRowVisible(QWidget* field, bool visible)
{
    auto* form = field->parentWidget()->findChild<QFormLayout*>(QStringLiteral("inspectorForm"));
    int row = -1;
    QFormLayout::ItemRole role;
    if (form) form->getWidgetPosition(field, &row, &role);
    if (row >= 0) form->setRowVisible(row, visible);
    else field->setVisible(visible);
}

void updateObjectRows(CInspectorPanel* panel, const QString& type)
{
    auto* form = panel->findChild<QFormLayout*>(QStringLiteral("inspectorForm"));
    if (!form) return;
    for (int row = 0; row < form->rowCount(); ++row) {
        auto* item = form->itemAt(row, QFormLayout::FieldRole);
        if (!item) item = form->itemAt(row, QFormLayout::SpanningRole);
        auto* field = item ? item->widget() : nullptr;
        if (!field) continue;
        const auto types = field->property("inspectorTypes").toStringList();
        bool visible = types.isEmpty() ? !type.isEmpty() && type != QLatin1String("multiple") : types.contains(type);
        if (auto* table = qobject_cast<QTableWidget*>(field)) visible = visible && table->rowCount() > 0;
        if (auto* combo = qobject_cast<QComboBox*>(field)) visible = visible && combo->count() > 0;
        form->setRowVisible(row, visible);
    }
}

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
    form->setObjectName(QStringLiteral("inspectorForm"));
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setVerticalSpacing(9);
    form->setHorizontalSpacing(12);

    m_typeLabel = new QLabel(content);
    m_idLabel = new QLabel(content);
    m_parentLabel = new QLabel(content);
    m_deformerLabel = new QLabel(content);
    m_deformerParentCombo = new InspectorComboBox(content);
    m_deformerParentCombo->setObjectName(QStringLiteral("deformerParentSelector"));
    m_deformerParentCombo->setEnabled(false);
    m_visibilityLabel = new QLabel(content);
    m_visibilityCheck = new VisibilityCheckBox(QStringLiteral("显示"), content);
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
    m_maskLabel->setObjectName(QStringLiteral("maskCountLabel"));
    m_editMasks = new QPushButton(QStringLiteral("编辑蒙版"), content);
    m_editMasks->setObjectName(QStringLiteral("editMasks"));
    m_editMasks->setEnabled(false);
    m_maskInvertedCheck = new QCheckBox(QStringLiteral("反向蒙版"), content);
    m_maskInvertedCheck->setObjectName(QStringLiteral("maskInverted"));
    m_maskInvertedCheck->setEnabled(false);
    m_keyformLabel = new QLabel(content);
    m_keyformCombo = new InspectorComboBox(content);
    m_keyformCombo->setObjectName(QStringLiteral("keyformSelector"));
    m_keyformCombo->setEnabled(false);
    m_vertexTable = new QTableWidget(content);
    m_vertexTable->setObjectName(QStringLiteral("keyformVertexTable"));
    m_vertexTable->setColumnCount(3);
    m_vertexTable->setHorizontalHeaderLabels({QStringLiteral("顶点"), QStringLiteral("X"), QStringLiteral("Y")});
    m_vertexTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_vertexTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_vertexTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_vertexTable->verticalHeader()->setVisible(false);
    m_vertexTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    m_vertexTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                   QAbstractItemView::SelectedClicked);
    m_vertexTable->setMinimumHeight(120);
    m_vertexTable->setMaximumHeight(280);
    auto* keyformTools = new QWidget(content);
    auto* keyformToolsLayout = new QGridLayout(keyformTools);
    keyformToolsLayout->setContentsMargins(0, 0, 0, 0);
    keyformToolsLayout->setHorizontalSpacing(6);
    keyformToolsLayout->setVerticalSpacing(6);
    keyformToolsLayout->setContentsMargins(0, 0, 0, 0);
    m_mirrorHorizontal = new QPushButton(QStringLiteral("水平镜像"), keyformTools);
    m_mirrorVertical = new QPushButton(QStringLiteral("垂直镜像"), keyformTools);
    m_interpolatePrevious = new QPushButton(QStringLiteral("与前一形插值"), keyformTools);
    m_copyToNext = new QPushButton(QStringLiteral("复制到后一形"), keyformTools);
    m_resetKeyform = new QPushButton(QStringLiteral("重置当前形"), keyformTools);
    m_copyKeyform = new QPushButton(QStringLiteral("复制关键形"), keyformTools);
    m_pasteKeyform = new QPushButton(QStringLiteral("粘贴关键形"), keyformTools);
    m_autoTriangulate = new QPushButton(QStringLiteral("自动三角剖分"), keyformTools);
    m_copyTopology = new QPushButton(QStringLiteral("复制拓扑"), keyformTools);
    m_pasteTopology = new QPushButton(QStringLiteral("粘贴拓扑"), keyformTools);
    m_mirrorHorizontal->setObjectName(QStringLiteral("mirrorHorizontal"));
    m_mirrorVertical->setObjectName(QStringLiteral("mirrorVertical"));
    m_interpolatePrevious->setObjectName(QStringLiteral("interpolatePreviousKeyform"));
    m_copyToNext->setObjectName(QStringLiteral("copyKeyformToNext"));
    m_resetKeyform->setObjectName(QStringLiteral("resetKeyform"));
    m_copyKeyform->setObjectName(QStringLiteral("copyKeyform"));
    m_pasteKeyform->setObjectName(QStringLiteral("pasteKeyform"));
    m_autoTriangulate->setObjectName(QStringLiteral("autoTriangulate"));
    m_copyTopology->setObjectName(QStringLiteral("copyTopology"));
    m_pasteTopology->setObjectName(QStringLiteral("pasteTopology"));
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    m_copyKeyform->setEnabled(false);
    m_pasteKeyform->setEnabled(false);
    m_autoTriangulate->setEnabled(false);
    m_copyTopology->setEnabled(false);
    m_pasteTopology->setEnabled(false);
    ResetMaskControls();
    const QList<QPushButton*> keyformButtons = {
        m_mirrorHorizontal, m_mirrorVertical, m_interpolatePrevious, m_copyToNext,
        m_resetKeyform, m_copyKeyform, m_pasteKeyform, m_autoTriangulate,
        m_copyTopology, m_pasteTopology
    };
    for (int i = 0; i < keyformButtons.size(); ++i)
        keyformToolsLayout->addWidget(keyformButtons[i], i / 2, i % 2);

    for (QLabel* value : {m_typeLabel, m_idLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        value->setMinimumWidth(0);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    form->addRow(QStringLiteral("类型"), m_typeLabel);
    form->addRow(QStringLiteral("ID"), m_idLabel);
    form->addRow(QStringLiteral("所属 Part"), m_parentLabel);
    form->addRow(QStringLiteral("父变形器"), m_deformerLabel);
    form->addRow(QStringLiteral("编辑父变形器"), m_deformerParentCombo);
    form->addRow(QStringLiteral("可见 / 启用"), m_visibilityLabel);
    form->addRow(QStringLiteral("编辑可见性"), m_visibilityCheck);
    form->addRow(QStringLiteral("编辑透明度"), m_opacitySpin);
    form->addRow(QStringLiteral("编辑绘制顺序"), m_drawOrderSpin);
    form->addRow(QStringLiteral("几何"), m_geometryLabel);
    form->addRow(QStringLiteral("纹理"), m_textureLabel);
    form->addRow(QStringLiteral("绘制顺序"), m_drawOrderLabel);
    form->addRow(QStringLiteral("混合 / 剔除"), m_blendLabel);
    form->addRow(QStringLiteral("Clipping"), m_maskLabel);
    form->addRow(QStringLiteral("蒙版关系"), m_editMasks);
    form->addRow(QStringLiteral("蒙版模式"), m_maskInvertedCheck);
    form->addRow(QStringLiteral("Keyform"), m_keyformLabel);
    form->addRow(QStringLiteral("关键形选择"), m_keyformCombo);
    form->addRow(QStringLiteral("关键形工具"), keyformTools);
    auto* vertexTransformTools = new QWidget(content);
    auto* vertexTransformLayout = new QFormLayout(vertexTransformTools);
    vertexTransformLayout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    vertexTransformLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    vertexTransformLayout->setContentsMargins(0, 0, 0, 0);
    m_vertexDeltaX = new QDoubleSpinBox(vertexTransformTools);
    m_vertexDeltaX->setObjectName(QStringLiteral("vertexDeltaX"));
    m_vertexDeltaX->setRange(-10000.0, 10000.0);
    m_vertexDeltaX->setDecimals(4);
    m_vertexDeltaX->setSingleStep(0.01);
    vertexTransformLayout->addRow(QStringLiteral("ΔX"), m_vertexDeltaX);
    m_vertexDeltaY = new QDoubleSpinBox(vertexTransformTools);
    m_vertexDeltaY->setObjectName(QStringLiteral("vertexDeltaY"));
    m_vertexDeltaY->setRange(-10000.0, 10000.0);
    m_vertexDeltaY->setDecimals(4);
    m_vertexDeltaY->setSingleStep(0.01);
    vertexTransformLayout->addRow(QStringLiteral("ΔY"), m_vertexDeltaY);
    m_moveVertices = new QPushButton(QStringLiteral("移动选中顶点"), vertexTransformTools);
    m_moveVertices->setObjectName(QStringLiteral("moveSelectedVertices"));
    m_moveVertices->setEnabled(false);
    vertexTransformLayout->addRow(m_moveVertices);
    m_mergeVertices = new QPushButton(QStringLiteral("合并拓扑顶点"), vertexTransformTools);
    m_mergeVertices->setObjectName(QStringLiteral("mergeSelectedVertices"));
    m_mergeVertices->setEnabled(false);
    vertexTransformLayout->addRow(m_mergeVertices);
    m_vertexScale = new QDoubleSpinBox(vertexTransformTools);
    m_vertexScale->setObjectName(QStringLiteral("vertexScale"));
    m_vertexScale->setRange(0.01, 100.0);
    m_vertexScale->setDecimals(3);
    m_vertexScale->setValue(1.0);
    vertexTransformLayout->addRow(QStringLiteral("缩放"), m_vertexScale);
    m_vertexRotation = new QDoubleSpinBox(vertexTransformTools);
    m_vertexRotation->setObjectName(QStringLiteral("vertexRotation"));
    m_vertexRotation->setRange(-3600.0, 3600.0);
    m_vertexRotation->setDecimals(2);
    vertexTransformLayout->addRow(QStringLiteral("旋转°"), m_vertexRotation);
    m_transformVertices = new QPushButton(QStringLiteral("应用变换"), vertexTransformTools);
    m_transformVertices->setObjectName(QStringLiteral("transformSelectedVertices"));
    m_transformVertices->setEnabled(false);
    vertexTransformLayout->addRow(m_transformVertices);
    form->addRow(QStringLiteral("顶点变换"), vertexTransformTools);
    form->addRow(QStringLiteral("顶点位置"), m_vertexTable);
    m_rotationKeyformTable = new QTableWidget(content);
    m_rotationKeyformTable->setObjectName(QStringLiteral("rotationKeyformTable"));
    m_rotationKeyformTable->setColumnCount(7);
    m_rotationKeyformTable->setHorizontalHeaderLabels({QStringLiteral("形"), QStringLiteral("角度"),
                                                        QStringLiteral("原点 X"), QStringLiteral("原点 Y"),
                                                        QStringLiteral("缩放"), QStringLiteral("反射 X"),
                                                        QStringLiteral("反射 Y")});
    m_rotationKeyformTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int column = 1; column < 7; ++column)
        m_rotationKeyformTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    m_rotationKeyformTable->verticalHeader()->setVisible(false);
    m_rotationKeyformTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                             QAbstractItemView::SelectedClicked);
    setRowVisible(m_rotationKeyformTable, false);
    form->addRow(QStringLiteral("旋转关键形"), m_rotationKeyformTable);
    m_warpKeyformCombo = new InspectorComboBox(content);
    m_warpKeyformCombo->setObjectName(QStringLiteral("warpKeyformSelector"));
    m_warpKeyformCombo->setEnabled(false);
    m_warpKeyformTable = new QTableWidget(content);
    m_warpKeyformTable->setObjectName(QStringLiteral("warpKeyformTable"));
    m_warpKeyformTable->setColumnCount(3);
    m_warpKeyformTable->setHorizontalHeaderLabels({QStringLiteral("网格点"), QStringLiteral("X"), QStringLiteral("Y")});
    m_warpKeyformTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_warpKeyformTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_warpKeyformTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_warpKeyformTable->verticalHeader()->setVisible(false);
    m_warpKeyformTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                                        QAbstractItemView::SelectedClicked);
    setRowVisible(m_warpKeyformTable, false);
    form->addRow(QStringLiteral("Warp 关键形"), m_warpKeyformCombo);
    form->addRow(QStringLiteral("Warp 网格坐标"), m_warpKeyformTable);
    for (QWidget* field : QList<QWidget*>{m_keyformCombo, keyformTools, vertexTransformTools, m_vertexTable,
                                         m_textureLabel, m_drawOrderLabel, m_blendLabel, m_maskLabel, m_editMasks, m_maskInvertedCheck})
        field->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh")});
    for (QWidget* field : QList<QWidget*>{m_opacitySpin, m_drawOrderSpin})
        field->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh"), QStringLiteral("multiple")});
    m_visibilityCheck->setProperty("inspectorTypes", QStringList{QStringLiteral("part"), QStringLiteral("artmesh"),
        QStringLiteral("warp"), QStringLiteral("rotation"), QStringLiteral("multiple")});
    m_deformerParentCombo->setProperty("inspectorTypes", QStringList{QStringLiteral("warp"), QStringLiteral("rotation")});
    m_geometryLabel->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh"), QStringLiteral("warp"), QStringLiteral("rotation")});
    m_keyformLabel->setProperty("inspectorTypes", QStringList{QStringLiteral("artmesh")});
    m_rotationKeyformTable->setProperty("inspectorTypes", QStringList{QStringLiteral("rotation")});
    for (QWidget* field : QList<QWidget*>{m_warpKeyformCombo, m_warpKeyformTable})
        field->setProperty("inspectorTypes", QStringList{QStringLiteral("warp")});
    for (QTableWidget* table : {m_vertexTable, m_rotationKeyformTable, m_warpKeyformTable}) {
        table->setMinimumHeight(120);
        table->setMaximumHeight(280);
        table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    const int headerMargin = m_rotationKeyformTable->style()->pixelMetric(QStyle::PM_HeaderMargin);
    m_rotationKeyformTable->horizontalHeader()->setMinimumSectionSize(
        m_rotationKeyformTable->fontMetrics().horizontalAdvance(QStringLiteral("原点 X")) + 2 * headerMargin);
    layout->addLayout(form);
    layout->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("inspectorScrollArea"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    setWidget(scroll);
    connect(m_keyformCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index){
        if (m_updatingKeyform) return;
        if (m_keyformHandler) {
            // activated 在 currentIndexChanged 之后发出，不能提前回滚用户选择。
            QTimer::singleShot(0, this, [this] {
                if (!m_keyformHandler || m_updatingKeyform) return;
                const QSignalBlocker blocker(m_keyformCombo);
                m_keyformCombo->setCurrentIndex(m_activeKeyformIndex);
            });
            return;
        }
        m_activeKeyformIndex = index;
        RebuildKeyformTable();
        if (index >= 0 && !m_selectionName.isEmpty()) {
            emit DrawableKeyformActivated(m_selectionName, index);
            emit DrawableKeyformSelected(m_selectionName, index);
        }
    });
    connect(m_keyformCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index){
        if (!m_keyformHandler || m_updatingKeyform || index == m_activeKeyformIndex ||
            index < 0 || index >= m_keyformCombo->count() || m_selectionType != QLatin1String("artmesh")) return;
        if (!RequestKeyform(m_selectionName, index)) return;
        m_activeKeyformIndex = index;
        {
            const QSignalBlocker blocker(m_keyformCombo);
            m_keyformCombo->setCurrentIndex(index);
        }
        RebuildKeyformTable();
        emit DrawableKeyformActivated(m_selectionName, index);
        emit DrawableKeyformSelected(m_selectionName, index);
    });
    connect(m_vertexTable, &QTableWidget::cellChanged, this, [this](int row, int column){
        if (!m_updatingKeyform) ApplyKeyformCellEdit(row, column);
    });
    connect(m_rotationKeyformTable, &QTableWidget::cellChanged, this, [this](int row, int column){
        if (!m_updatingRotationKeyform) ApplyRotationKeyformCellEdit(row, column);
    });
    connect(m_rotationKeyformTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) {
        if (m_updatingRotationKeyform || row < 0 || m_selectionType != QLatin1String("rotation")) return;
        if (!RequestDeformerKeyform(row)) return;
        m_activeDeformerKeyform = row;
    });
    connect(m_warpKeyformCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index){
        if (m_updatingWarpKeyform) return;
        if (m_deformerKeyformHandler) {
            QTimer::singleShot(0, this, [this] {
                if (!m_deformerKeyformHandler || m_updatingWarpKeyform) return;
                const QSignalBlocker blocker(m_warpKeyformCombo);
                m_warpKeyformCombo->setCurrentIndex(m_activeDeformerKeyform);
            });
            return;
        }
        m_activeDeformerKeyform = index;
        RebuildWarpKeyformTable();
    });
    connect(m_warpKeyformCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int index){
        if (!m_deformerKeyformHandler || m_updatingWarpKeyform || index < 0 || index >= m_warpKeyformCombo->count() ||
            m_selectionType != QLatin1String("warp")) return;
        if (!RequestDeformerKeyform(index)) return;
        m_activeDeformerKeyform = index;
        {
            const QSignalBlocker blocker(m_warpKeyformCombo);
            m_warpKeyformCombo->setCurrentIndex(index);
        }
        RebuildWarpKeyformTable();
    });
    connect(m_warpKeyformTable, &QTableWidget::cellChanged, this, [this](int row, int column){
        if (!m_updatingWarpKeyform) ApplyWarpKeyformCellEdit(row, column);
    });
    connect(m_mirrorHorizontal, &QPushButton::clicked, this, [this](){ MirrorKeyform(true); });
    connect(m_mirrorVertical, &QPushButton::clicked, this, [this](){ MirrorKeyform(false); });
    connect(m_interpolatePrevious, &QPushButton::clicked, this, [this](){ InterpolatePreviousKeyform(); });
    connect(m_copyToNext, &QPushButton::clicked, this, [this](){ CopyKeyformToNext(); });
    connect(m_resetKeyform, &QPushButton::clicked, this, [this](){ ResetCurrentKeyform(); });
    connect(m_copyKeyform, &QPushButton::clicked, this, [this](){ CopyCurrentKeyform(); });
    connect(m_pasteKeyform, &QPushButton::clicked, this, [this](){ PasteCurrentKeyform(); });
    connect(m_autoTriangulate, &QPushButton::clicked, this, [this](){ AutoTriangulate(); });
    connect(m_copyTopology, &QPushButton::clicked, this, [this](){ CopyTopology(); });
    connect(m_pasteTopology, &QPushButton::clicked, this, [this](){ PasteTopology(); });
    connect(m_editMasks, &QPushButton::clicked, this, [this](){ EditMasks(); });
    connect(m_moveVertices, &QPushButton::clicked, this, [this](){ TranslateSelectedVertices(); });
    connect(m_mergeVertices, &QPushButton::clicked, this, [this](){ MergeSelectedVertices(); });
    connect(m_transformVertices, &QPushButton::clicked, this, [this](){ TransformSelectedVertices(); });
    connect(m_vertexTable, &QTableWidget::itemSelectionChanged, this, [this](){
        const bool hasSelection = m_vertexTable->selectedItems().size() > 0 && m_vertexTable->isEnabled();
        QSet<int> selectedRows;
        for (const QTableWidgetSelectionRange& range : m_vertexTable->selectedRanges())
            for (int row = range.topRow(); row <= range.bottomRow(); ++row) selectedRows.insert(row);
        m_moveVertices->setEnabled(hasSelection &&
                                   m_vertexTable->isEnabled() && m_vertexDeltaX && m_vertexDeltaY);
        m_mergeVertices->setEnabled(selectedRows.size() >= 2);
        m_transformVertices->setEnabled(!selectedRows.isEmpty());
    });
    connect(m_maskInvertedCheck, &QCheckBox::toggled, this, [this](bool inverted){
        if (m_updatingVisibility || !m_document || m_selectionType != QLatin1String("artmesh") ||
            m_selectionName.isEmpty() || IsLocked(m_selectionType, m_selectionName)) return;
        const bool before = m_document->DrawableMaskInverted(m_selectionName);
        if (before == inverted) return;
        const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                               live2dstruct::CModelEdit::MaskInverted, before, inverted};
        if (!SubmitEdit(command)) return;
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
        showModelObject(m_selectionName, m_selectionType);
    });
    connect(m_deformerParentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index){
        if (m_updatingDeformerParent || !m_document || m_selectionName.isEmpty() ||
            (m_selectionType != QLatin1String("warp") && m_selectionType != QLatin1String("rotation")) ||
            IsLocked(m_selectionType, m_selectionName) || index < 0) return;
        const int before = m_document->DeformerParentIndex(m_selectionName);
        const int after = m_deformerParentCombo->itemData(index).toInt();
        if (before == after) return;
        const live2dstruct::CModelEdit command{m_selectionName, m_selectionType,
                                               live2dstruct::CModelEdit::DeformerParent,
                                               before, after};
        if (!SubmitEdit(command)) return;
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
        showModelObject(m_selectionName, m_selectionType);
    });
    connect(m_visibilityCheck, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state){
        if (state == Qt::PartiallyChecked) return;
        const bool visible = state == Qt::Checked;
        if (m_updatingVisibility || !m_document) return;
        if (m_selectionNames.size() > 1) {
            const auto current = m_document->VisibilityState();
            QMap<QString, bool> before, after;
            for (int i = 0; i < m_selectionNames.size(); ++i) {
                const QString name = m_selectionNames[i];
                const QString type = m_selectionTypes.value(i);
                if (IsLocked(type, name)) continue;
                const QString key = type + QLatin1Char('|') + name;
                const auto value = current.constFind(key);
                if (value == current.cend() || value.value() == visible) continue;
                before.insert(key, value.value());
                after.insert(key, visible);
            }
            if (after.isEmpty()) { RefreshSelection(); return; }
            const live2dstruct::CModelEdit command{QStringLiteral("selection"), QStringLiteral("objects"),
                live2dstruct::CModelEdit::VisibilityValues, QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
            if (!SubmitEdit(command)) return;
            emit VisibilityBatchChanged(after);
            emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
            RefreshSelection();
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
        if (!SubmitEdit(command)) return;
        emit VisibilityChanged(m_selectionName, m_selectionType, visible);
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
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
                if (!SubmitEdit(command)) return;
                emit DrawableOpacityChanged(name, after);
                emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
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
        if (!SubmitEdit(command)) return;
        emit DrawableOpacityChanged(m_selectionName, after);
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
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
                if (!SubmitEdit(command)) return;
                emit DrawableDrawOrderChanged(name, value);
                emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
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
        if (!SubmitEdit(command)) return;
        emit DrawableDrawOrderChanged(m_selectionName, value);
        emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    });
    ClearSelection();
}

CInspectorPanel::~CInspectorPanel()
{
    // 浮动窗口销毁时会提交编辑，须在派生成员释放前断开。
    for (QObject* child : findChildren<QObject*>()) QObject::disconnect(child, nullptr, this, nullptr);
}

void CInspectorPanel::SetDocument(live2dstruct::CModelDocument* document)
{
    const bool sameDocument = m_document == document;
    m_document = document;
    if (m_selectionName.isEmpty()) ClearSelection();
    else showModelObject(m_selectionName, m_selectionType, sameDocument);
}

void CInspectorPanel::SetEditHandler(std::function<bool(const live2dstruct::CModelEdit&, QString*)> handler)
{
    m_editHandler = std::move(handler);
}

void CInspectorPanel::SetHistoryHandler(std::function<bool(bool, live2dstruct::CModelEdit&, QString*)> handler)
{
    m_historyHandler = std::move(handler);
}

void CInspectorPanel::SetKeyformHandler(std::function<bool(const QString&, int, QString*)> handler)
{
    m_keyformHandler = std::move(handler);
}

void CInspectorPanel::SetDeformerKeyformHandler(std::function<bool(const QString&, const QString&, int, QString*)> handler)
{
    m_deformerKeyformHandler = std::move(handler);
}

bool CInspectorPanel::RequestDeformerKeyform(int index)
{
    if (!m_deformerKeyformHandler) return true;
    if (m_submitting) return false;
    const QScopedValueRollback<bool> guard(m_submitting, true);
    QString error;
    try {
        const auto handler = m_deformerKeyformHandler;
        if (handler(m_selectionName, m_selectionType, index, &error)) return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("变形器关键形选择失败：%1").arg(QString::fromUtf8(exception.what()));
    }
    RejectEdit(error);
    return false;
}

void CInspectorPanel::RejectEdit(const QString& error)
{
    {
        const QSignalBlocker blocker(this);
        RefreshSelection();
    }
    emit EditRejected(error.isEmpty() ? QStringLiteral("无法提交当前编辑，已保留原状态") : error);
}

bool CInspectorPanel::SubmitEdit(const live2dstruct::CModelEdit& edit)
{
    if (!m_document || m_submitting) return false;
    const QScopedValueRollback<bool> guard(m_submitting, true);
    QString error;
    try {
        const auto handler = m_editHandler;
        if (handler ? handler(edit, &error) : m_document->ExecuteEdit(edit)) return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("编辑提交失败：%1").arg(QString::fromUtf8(exception.what()));
    }
    RejectEdit(error);
    return false;
}

bool CInspectorPanel::SubmitHistory(bool undo, live2dstruct::CModelEdit& edit)
{
    if (!m_document || m_submitting) return false;
    const QScopedValueRollback<bool> guard(m_submitting, true);
    QString error;
    try {
        const auto handler = m_historyHandler;
        const bool accepted = handler ? handler(undo, edit, &error)
                                      : (undo ? m_document->UndoEdit(&edit) : m_document->RedoEdit(&edit));
        if (accepted) return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("历史操作失败：%1").arg(QString::fromUtf8(exception.what()));
    }
    RejectEdit(error);
    return false;
}

bool CInspectorPanel::RequestKeyform(const QString& name, int index)
{
    if (!m_keyformHandler) return true;
    if (m_submitting) return false;
    const QScopedValueRollback<bool> guard(m_submitting, true);
    QString error;
    try {
        const auto handler = m_keyformHandler;
        if (handler(name, index, &error)) return true;
    } catch (const std::exception& exception) {
        error = QStringLiteral("关键形选择失败：%1").arg(QString::fromUtf8(exception.what()));
    }
    RejectEdit(error);
    return false;
}

void CInspectorPanel::SetSelection(const QString& name, const QString& type)
{
    const bool sameObject = m_selectionName == name && m_selectionType == type;
    m_selectionName = name;
    m_selectionType = type;
    m_selectionNames = {name};
    m_selectionTypes = {type};
    m_visibilityCheck->setTristate(false);
    showModelObject(name, type, sameObject);
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
    m_activeKeyformIndex = -1;
    m_activeDeformerKeyform = -1;
    m_selectionNames = names;
    m_selectionTypes = types;
    m_emptyLabel->setVisible(true);
    m_updatingKeyform = true;
    m_keyformCombo->clear();
    m_keyformCombo->setEnabled(false);
    m_vertexTable->setRowCount(0);
    m_rotationKeyformTable->setRowCount(0);
    setRowVisible(m_rotationKeyformTable, false);
    m_rotationKeyformTable->setEnabled(false);
    m_warpKeyformCombo->clear();
    m_warpKeyformCombo->setEnabled(false);
    m_warpKeyformTable->setRowCount(0);
    setRowVisible(m_warpKeyformTable, false);
    m_warpKeyformTable->setEnabled(false);
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    m_copyKeyform->setEnabled(false);
    m_pasteKeyform->setEnabled(false);
    m_autoTriangulate->setEnabled(false);
    m_copyTopology->setEnabled(false);
    m_pasteTopology->setEnabled(false);
    ResetMaskControls();
    m_deformerParentCombo->setEnabled(false);
    m_moveVertices->setEnabled(false);
    m_mergeVertices->setEnabled(false);
    m_transformVertices->setEnabled(false);
    m_updatingKeyform = false;
    m_updatingVisibility = true;
    m_visibilityCheck->setTristate(true);
    bool anyVisible = false;
    bool anyHidden = false;
    bool allVisibleEditable = true;
    bool allArtMeshes = true;
    bool allUnlocked = true;
    bool anyUnlocked = false;
    const auto visibility = m_document ? m_document->VisibilityState() : QMap<QString, bool>();
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
        const bool unlocked = !IsLocked(type, name);
        allUnlocked = allUnlocked && unlocked;
        anyUnlocked = anyUnlocked || unlocked;
        const auto value = visibility.constFind(type + QLatin1Char('|') + name);
        const bool found = value != visibility.cend();
        if (found) {
            anyVisible = anyVisible || value.value();
            anyHidden = anyHidden || !value.value();
        }
        if (!found) allVisibleEditable = false;
        allArtMeshes = allArtMeshes && type == QLatin1String("artmesh");
        if (!found) allArtMeshes = false;
    }
    if (anyVisible && anyHidden) m_visibilityCheck->setCheckState(Qt::PartiallyChecked);
    else m_visibilityCheck->setChecked(anyVisible);
    m_visibilityCheck->setEnabled(allVisibleEditable && anyUnlocked);
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
    m_emptyLabel->setText((allUnlocked ? QStringLiteral("已选择 %1 个对象，可在结构树中继续调整范围") :
        QStringLiteral("已选择 %1 个对象，锁定对象不受批量修改影响")).arg(names.size()));
    for (QLabel* label : {m_typeLabel, m_idLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        label->clear();
    }
    QSet<QString> uniqueTypes;
    for (const QString& type : types) uniqueTypes.insert(type);
    m_typeLabel->setText(uniqueTypes.values().join(QStringLiteral("、")));
    m_idLabel->setText(QStringLiteral("%1 个对象").arg(names.size()));
    updateObjectRows(this, QStringLiteral("multiple"));
}

void CInspectorPanel::RefreshSelection()
{
    if (m_selectionNames.size() > 1) SetSelections(m_selectionNames, m_selectionTypes);
    else if (!m_selectionName.isEmpty()) showModelObject(m_selectionName, m_selectionType);
}

bool CInspectorPanel::SyncActiveKeyform(const QString& id, const QString& type, int keyform)
{
    if (!m_document || keyform < 0) return false;
    int count = 0;
    if (type == QLatin1String("artmesh")) {
        for (const auto& mesh : m_document->Drawables()) if (mesh.id == id) { count = mesh.keyforms.size(); break; }
    } else if (type == QLatin1String("warp")) count = m_document->DeformerWarpKeyforms(id).size();
    else if (type == QLatin1String("rotation")) count = m_document->DeformerRotationKeyforms(id).size();
    if (keyform >= count) return false;
    const QSignalBlocker blocker(this), visibilityBlocker(m_visibilityCheck);
    m_selectionName = id;
    m_selectionType = type;
    m_selectionNames = {id};
    m_selectionTypes = {type};
    m_visibilityCheck->setTristate(false);
    m_activeKeyformIndex = type == QLatin1String("artmesh") ? keyform : -1;
    m_activeDeformerKeyform = type == QLatin1String("artmesh") ? -1 : keyform;
    showModelObject(id, type, true);
    return true;
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
    const int keyformIndex = m_activeKeyformIndex;
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
        m_vertexTable->setItem(row, 1, new QTableWidgetItem(QString::number(positions[row * 2], 'g', 9)));
        m_vertexTable->setItem(row, 2, new QTableWidgetItem(QString::number(positions[row * 2 + 1], 'g', 9)));
    }
    m_vertexTable->setEnabled(true);
    m_updatingKeyform = false;
}

void CInspectorPanel::RebuildRotationKeyformTable()
{
    m_updatingRotationKeyform = true;
    m_rotationKeyformTable->setRowCount(0);
    if (!m_document || m_selectionType != QLatin1String("rotation") || m_selectionName.isEmpty()) {
        m_rotationKeyformTable->setEnabled(false);
        m_updatingRotationKeyform = false;
        return;
    }
    const QVector<live2dstruct::CModelRotationKeyform> keyforms =
        m_document->DeformerRotationKeyforms(m_selectionName);
    const bool unlocked = !IsLocked(m_selectionType, m_selectionName);
    m_rotationKeyformTable->setRowCount(keyforms.size());
    for (int row = 0; row < keyforms.size(); ++row) {
        const auto& keyform = keyforms[row];
        auto* index = new QTableWidgetItem(QString::number(row));
        index->setFlags(index->flags() & ~Qt::ItemIsEditable);
        m_rotationKeyformTable->setItem(row, 0, index);
        m_rotationKeyformTable->setItem(row, 1, new QTableWidgetItem(QString::number(keyform.angle, 'g', 9)));
        m_rotationKeyformTable->setItem(row, 2, new QTableWidgetItem(QString::number(keyform.originX, 'g', 9)));
        m_rotationKeyformTable->setItem(row, 3, new QTableWidgetItem(QString::number(keyform.originY, 'g', 9)));
        m_rotationKeyformTable->setItem(row, 4, new QTableWidgetItem(QString::number(keyform.scale, 'g', 9)));
        m_rotationKeyformTable->setItem(row, 5, new QTableWidgetItem(keyform.reflectX ? QStringLiteral("是") : QStringLiteral("否")));
        m_rotationKeyformTable->setItem(row, 6, new QTableWidgetItem(keyform.reflectY ? QStringLiteral("是") : QStringLiteral("否")));
    }
    m_rotationKeyformTable->setEnabled(unlocked);
    if (m_activeDeformerKeyform >= 0 && m_activeDeformerKeyform < keyforms.size())
        m_rotationKeyformTable->setCurrentCell(m_activeDeformerKeyform, 0);
    m_updatingRotationKeyform = false;
}

void CInspectorPanel::ApplyRotationKeyformCellEdit(int row, int column)
{
    if (!m_document || m_selectionType != QLatin1String("rotation") || m_selectionName.isEmpty() ||
        row < 0 || column < 1 || column > 6 || IsLocked(m_selectionType, m_selectionName)) return;
    QVector<live2dstruct::CModelRotationKeyform> before =
        m_document->DeformerRotationKeyforms(m_selectionName);
    if (row >= before.size() || !m_rotationKeyformTable->item(row, column)) return;
    QVector<live2dstruct::CModelRotationKeyform> after = before;
    const QString text = m_rotationKeyformTable->item(row, column)->text().trimmed().toLower();
    if (column == 5 || column == 6) {
        bool value = false;
        if (text == QStringLiteral("是") || text == QStringLiteral("true") || text == QStringLiteral("1")) value = true;
        else if (text != QStringLiteral("否") && text != QStringLiteral("false") && text != QStringLiteral("0")) {
            RebuildRotationKeyformTable();
            return;
        }
        if (column == 5) after[row].reflectX = value;
        else after[row].reflectY = value;
    } else {
        bool ok = false;
        const double number = m_rotationKeyformTable->item(row, column)->text().toDouble(&ok);
        if (!ok || !std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
            RebuildRotationKeyformTable();
            return;
        }
        const float value = static_cast<float>(number);
        if (column == 1) after[row].angle = value;
        else if (column == 2) after[row].originX = value;
        else if (column == 3) after[row].originY = value;
        else if (column == 4) after[row].scale = value;
    }
    if (!after[row].IsValid()) {
        RebuildRotationKeyformTable();
        return;
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("rotation"),
                                           live2dstruct::CModelEdit::RotationKeyforms,
                                           QVariant::fromValue(before), QVariant::fromValue(after), row};
    if (!SubmitEdit(command)) return;
    m_activeDeformerKeyform = row;
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    RebuildRotationKeyformTable();
}

void CInspectorPanel::RebuildWarpKeyformTable()
{
    m_updatingWarpKeyform = true;
    m_warpKeyformTable->setRowCount(0);
    if (!m_document || m_selectionType != QLatin1String("warp") || m_selectionName.isEmpty()) {
        m_warpKeyformTable->setEnabled(false);
        m_updatingWarpKeyform = false;
        return;
    }
    const int keyformIndex = m_activeDeformerKeyform;
    const QVector<live2dstruct::CModelWarpKeyform> keyforms = m_document->DeformerWarpKeyforms(m_selectionName);
    if (keyformIndex < 0 || keyformIndex >= keyforms.size()) {
        m_warpKeyformTable->setEnabled(false);
        m_updatingWarpKeyform = false;
        return;
    }
    const auto& keyform = keyforms[keyformIndex];
    const int pointCount = keyform.positions.size() / 2;
    const bool unlocked = !IsLocked(m_selectionType, m_selectionName);
    m_warpKeyformTable->setRowCount(pointCount);
    for (int row = 0; row < pointCount; ++row) {
        auto* index = new QTableWidgetItem(QString::number(row));
        index->setFlags(index->flags() & ~Qt::ItemIsEditable);
        m_warpKeyformTable->setItem(row, 0, index);
        m_warpKeyformTable->setItem(row, 1, new QTableWidgetItem(QString::number(keyform.positions[row * 2], 'g', 9)));
        m_warpKeyformTable->setItem(row, 2, new QTableWidgetItem(QString::number(keyform.positions[row * 2 + 1], 'g', 9)));
    }
    m_warpKeyformTable->setEnabled(unlocked);
    m_updatingWarpKeyform = false;
}

void CInspectorPanel::ApplyWarpKeyformCellEdit(int row, int column)
{
    if (!m_document || m_selectionType != QLatin1String("warp") || m_selectionName.isEmpty() ||
        row < 0 || (column != 1 && column != 2) || IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeDeformerKeyform;
    QVector<live2dstruct::CModelWarpKeyform> before = m_document->DeformerWarpKeyforms(m_selectionName);
    if (keyformIndex < 0 || keyformIndex >= before.size() || !m_warpKeyformTable->item(row, column)) return;
    QVector<live2dstruct::CModelWarpKeyform> after = before;
    bool ok = false;
    const double number = m_warpKeyformTable->item(row, column)->text().toDouble(&ok);
    if (!ok || !std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
        RebuildWarpKeyformTable();
        return;
    }
    after[keyformIndex].positions[row * 2 + (column - 1)] = float(number);
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("warp"),
                                           live2dstruct::CModelEdit::WarpKeyforms,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex};
    if (!SubmitEdit(command)) return;
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    RebuildWarpKeyformTable();
}

void CInspectorPanel::ApplyKeyformCellEdit(int row, int column)
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName) ||
        (column != 1 && column != 2) || row < 0 || row >= m_vertexTable->rowCount()) return;
    const int keyformIndex = m_activeKeyformIndex;
    QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.size() != m_vertexTable->rowCount() * 2) return;
    bool ok = false;
    const double parsed = m_vertexTable->item(row, column)->text().toDouble(&ok);
    if (!ok || !std::isfinite(parsed) || std::abs(parsed) > std::numeric_limits<float>::max()) {
        m_updatingKeyform = true;
        m_vertexTable->item(row, column)->setText(QString::number(before[row * 2 + column - 1], 'g', 9));
        m_updatingKeyform = false;
        return;
    }
    QVector<float> after = before;
    after[row * 2 + column - 1] = static_cast<float>(parsed);
    if (before[row * 2 + column - 1] == after[row * 2 + column - 1]) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex};
    if (!SubmitEdit(command)) return;
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::MirrorKeyform(bool horizontal)
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty() || before.size() % 2 != 0) return;
    float minimum = before[horizontal ? 0 : 1];
    float maximum = minimum;
    for (int i = horizontal ? 0 : 1; i < before.size(); i += 2) {
        minimum = qMin(minimum, before[i]);
        maximum = qMax(maximum, before[i]);
    }
    const double sum = double(minimum) + maximum;
    QVector<float> after = before;
    for (int i = horizontal ? 0 : 1; i < after.size(); i += 2) after[i] = float(sum - after[i]);
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::InterpolatePreviousKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    if (keyformIndex <= 0) return;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    const QVector<float> previous = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex - 1);
    if (before.isEmpty() || before.size() != previous.size()) return;
    QVector<float> after;
    after.resize(before.size());
    for (int i = 0; i < before.size(); ++i) after[i] = float((double(before[i]) + previous[i]) * 0.5);
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::CopyKeyformToNext()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> source = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex + 1);
    if (keyformIndex < 0 || source.isEmpty() || source.size() != before.size()) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(source), keyformIndex + 1, false};
    if (!SubmitEdit(command)) return;
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex + 1, source);
    emit DrawableKeyformActivated(m_selectionName, keyformIndex);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::ResetCurrentKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
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
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, original);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::CopyCurrentKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> positions = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || positions.isEmpty() || positions.size() % 2 != 0) return;
    QJsonArray values;
    for (float value : positions) values.append(static_cast<double>(value));
    QJsonObject object;
    object.insert(QStringLiteral("format"), QStringLiteral("live2d2psd-keyform-v1"));
    object.insert(QStringLiteral("vertexCount"), positions.size() / 2);
    object.insert(QStringLiteral("positions"), values);
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void CInspectorPanel::AutoTriangulate()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> positions = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    const QVector<quint16> before = m_document->DrawableIndices(m_selectionName);
    if (keyformIndex < 0 || positions.size() < 6 || positions.size() % 2 != 0 || before.isEmpty()) return;
    const int pointCount = positions.size() / 2;
    if (pointCount > 32768) {
        QMessageBox::warning(this, QStringLiteral("自动三角剖分失败"), QStringLiteral("顶点数量超过 moc3 的索引范围"));
        return;
    }

    struct Point { double x; double y; };
    struct Triangle { int a; int b; int c; };
    QVector<Point> points;
    points.reserve(pointCount + 3);
    double minX = positions[0];
    double maxX = positions[0];
    double minY = positions[1];
    double maxY = positions[1];
    for (int i = 0; i < pointCount; ++i) {
        const double x = positions[i * 2];
        const double y = positions[i * 2 + 1];
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        points.append({x, y});
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    const double span = std::max({maxX - minX, maxY - minY, 1.0});
    const double centerX = (minX + maxX) * 0.5;
    const double centerY = (minY + maxY) * 0.5;
    points.append({centerX - 32.0 * span, centerY - span});
    points.append({centerX, centerY + 32.0 * span});
    points.append({centerX + 32.0 * span, centerY - span});

    const auto orientation = [](const Point& a, const Point& b, const Point& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    const auto containsCircumcircle = [&points, &orientation, span](const Triangle& triangle, const Point& point) {
        const Point& a = points[triangle.a];
        const Point& b = points[triangle.b];
        const Point& c = points[triangle.c];
        const double d = 2.0 * orientation(a, b, c);
        if (std::fabs(d) < 1e-12) return false;
        const double aa = a.x * a.x + a.y * a.y;
        const double bb = b.x * b.x + b.y * b.y;
        const double cc = c.x * c.x + c.y * c.y;
        const double ux = (aa * (b.y - c.y) + bb * (c.y - a.y) + cc * (a.y - b.y)) / d;
        const double uy = (aa * (c.x - b.x) + bb * (a.x - c.x) + cc * (b.x - a.x)) / d;
        const double dx = point.x - ux;
        const double dy = point.y - uy;
        const double radiusSquared = (a.x - ux) * (a.x - ux) + (a.y - uy) * (a.y - uy);
        return dx * dx + dy * dy <= radiusSquared + 1e-9 * span * span;
    };

    QVector<Triangle> triangles;
    triangles.append({pointCount, pointCount + 1, pointCount + 2});
    for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
        QSet<int> badIndices;
        for (int i = 0; i < triangles.size(); ++i) {
            if (containsCircumcircle(triangles[i], points[pointIndex])) badIndices.insert(i);
        }
        if (badIndices.isEmpty()) continue;
        QHash<QString, int> edgeCounts;
        QHash<QString, QPair<int, int>> edgePairs;
        const auto addEdge = [&edgeCounts, &edgePairs](int first, int second) {
            const int a = std::min(first, second);
            const int b = std::max(first, second);
            const QString key = QString::number(a) + QLatin1Char(':') + QString::number(b);
            edgeCounts[key] = edgeCounts.value(key, 0) + 1;
            edgePairs.insert(key, qMakePair(a, b));
        };
        for (int index : badIndices) {
            const Triangle& triangle = triangles[index];
            addEdge(triangle.a, triangle.b);
            addEdge(triangle.b, triangle.c);
            addEdge(triangle.c, triangle.a);
        }
        QVector<Triangle> kept;
        kept.reserve(triangles.size());
        for (int i = 0; i < triangles.size(); ++i) if (!badIndices.contains(i)) kept.append(triangles[i]);
        triangles = kept;
        for (auto edgeIt = edgeCounts.cbegin(); edgeIt != edgeCounts.cend(); ++edgeIt) {
            if (edgeIt.value() != 1) continue;
            const QPair<int, int> edge = edgePairs.value(edgeIt.key());
            Triangle triangle{edge.first, edge.second, pointIndex};
            if (orientation(points[triangle.a], points[triangle.b], points[triangle.c]) < 0.0)
                std::swap(triangle.a, triangle.b);
            if (std::fabs(orientation(points[triangle.a], points[triangle.b], points[triangle.c])) > 1e-12)
                triangles.append(triangle);
        }
    }

    QVector<quint16> generated;
    for (const Triangle& triangle : triangles) {
        if (triangle.a >= pointCount || triangle.b >= pointCount || triangle.c >= pointCount) continue;
        if (std::fabs(orientation(points[triangle.a], points[triangle.b], points[triangle.c])) < 1e-12) continue;
        generated.append(static_cast<quint16>(triangle.a));
        generated.append(static_cast<quint16>(triangle.b));
        generated.append(static_cast<quint16>(triangle.c));
    }
    QVector<quint16> after;
    if (!live2dstruct::CMeshTopology::CleanTriangles(generated, pointCount, &after)) {
        QMessageBox::warning(this, QStringLiteral("自动三角剖分失败"), QStringLiteral("顶点无法形成有效三角形"));
        return;
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::MeshIndices,
                                           QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (!SubmitEdit(command)) return;
    emit DrawableIndicesChanged(m_selectionName, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::CopyTopology()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty()) return;
    const QVector<quint16> indices = m_document->DrawableIndices(m_selectionName);
    if (indices.isEmpty() || indices.size() % 3 != 0) return;
    QJsonArray values;
    for (quint16 index : indices) values.append(static_cast<int>(index));
    int vertexCount = 0;
    for (const auto& drawable : m_document->Drawables()) {
        if (drawable.id == m_selectionName) { vertexCount = drawable.vertexCount; break; }
    }
    QJsonObject object;
    object.insert(QStringLiteral("format"), QStringLiteral("live2d2psd-topology-v1"));
    object.insert(QStringLiteral("vertexCount"), vertexCount);
    object.insert(QStringLiteral("indices"), values);
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void CInspectorPanel::PasteTopology()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const QVector<quint16> before = m_document->DrawableIndices(m_selectionName);
    int vertexCount = 0;
    for (const auto& drawable : m_document->Drawables()) {
        if (drawable.id == m_selectionName) { vertexCount = drawable.vertexCount; break; }
    }
    if (vertexCount <= 0) return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("剪贴板不是有效的拓扑数据"));
        return;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("live2d2psd-topology-v1")) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("剪贴板格式不受支持"));
        return;
    }
    if (object.value(QStringLiteral("vertexCount")).toInt() != vertexCount) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("目标网格的顶点数量不同"));
        return;
    }
    const QJsonArray values = object.value(QStringLiteral("indices")).toArray();
    if (values.isEmpty() || values.size() % 3 != 0 || values.size() > 3000000) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("三角形索引数量无效"));
        return;
    }
    QVector<quint16> after;
    after.reserve(values.size());
    for (const QJsonValue& value : values) {
        const double number = value.toDouble(-1.0);
        if (!value.isDouble() || std::floor(number) != number || number < 0.0 || number >= vertexCount || number > 65535.0) {
            QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("索引超出目标网格范围"));
            return;
        }
        after.append(static_cast<quint16>(number));
    }
    if (!live2dstruct::CMeshTopology::IsValid(after, vertexCount)) {
        QMessageBox::warning(this, QStringLiteral("粘贴拓扑失败"), QStringLiteral("索引超出目标网格范围"));
        return;
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::MeshIndices,
                                           QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (!SubmitEdit(command)) return;
    emit DrawableIndicesChanged(m_selectionName, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::PasteCurrentKeyform()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty()) return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        QMessageBox::warning(this, QStringLiteral("粘贴关键形失败"), QStringLiteral("剪贴板不是有效的关键形数据"));
        return;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("live2d2psd-keyform-v1")) {
        QMessageBox::warning(this, QStringLiteral("粘贴关键形失败"), QStringLiteral("剪贴板格式不受支持"));
        return;
    }
    const QJsonArray values = object.value(QStringLiteral("positions")).toArray();
    if (values.size() != before.size() || object.value(QStringLiteral("vertexCount")).toInt() != before.size() / 2) {
        QMessageBox::warning(this, QStringLiteral("粘贴关键形失败"), QStringLiteral("目标网格的顶点数量不同"));
        return;
    }
    QVector<float> after;
    after.reserve(values.size());
    for (const QJsonValue& value : values) {
        if (!value.isDouble() || !std::isfinite(value.toDouble())) {
            QMessageBox::warning(this, QStringLiteral("粘贴关键形失败"), QStringLiteral("关键形包含无效顶点坐标"));
            return;
        }
        after.append(static_cast<float>(value.toDouble()));
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::ClearSelection()
{
    m_activeKeyformIndex = -1;
    m_activeDeformerKeyform = -1;
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
    m_rotationKeyformTable->setRowCount(0);
    setRowVisible(m_rotationKeyformTable, false);
    m_rotationKeyformTable->setEnabled(false);
    m_warpKeyformCombo->clear();
    m_warpKeyformCombo->setEnabled(false);
    m_warpKeyformTable->setRowCount(0);
    setRowVisible(m_warpKeyformTable, false);
    m_warpKeyformTable->setEnabled(false);
    m_mirrorHorizontal->setEnabled(false);
    m_mirrorVertical->setEnabled(false);
    m_interpolatePrevious->setEnabled(false);
    m_copyToNext->setEnabled(false);
    m_resetKeyform->setEnabled(false);
    m_copyKeyform->setEnabled(false);
    m_pasteKeyform->setEnabled(false);
    m_autoTriangulate->setEnabled(false);
    m_copyTopology->setEnabled(false);
    m_pasteTopology->setEnabled(false);
    ResetMaskControls();
    m_deformerParentCombo->setEnabled(false);
    m_moveVertices->setEnabled(false);
    m_mergeVertices->setEnabled(false);
    m_transformVertices->setEnabled(false);
    m_updatingKeyform = false;
    for (QLabel* label : {m_typeLabel, m_idLabel, m_parentLabel, m_deformerLabel,
                          m_visibilityLabel, m_geometryLabel, m_textureLabel,
                          m_drawOrderLabel, m_blendLabel, m_maskLabel, m_keyformLabel}) {
        label->clear();
    }
    updateObjectRows(this, QString());
}

void CInspectorPanel::EmitEdit(const live2dstruct::CModelEdit& edit, bool forward)
{
    const QVariant& value = forward ? edit.after : edit.before;
    if (edit.field == live2dstruct::CModelEdit::Visibility) {
        emit VisibilityChanged(edit.name, edit.type, value.toBool());
    } else if (edit.field == live2dstruct::CModelEdit::VisibilityValues) {
        emit VisibilityBatchChanged(value.value<QMap<QString, bool>>());
    } else if (edit.field == live2dstruct::CModelEdit::Opacity) {
        emit DrawableOpacityChanged(edit.name, value.toFloat());
    } else if (edit.field == live2dstruct::CModelEdit::DrawOrder) {
        emit DrawableDrawOrderChanged(edit.name, value.toInt());
    } else if (edit.field == live2dstruct::CModelEdit::KeyformPositions) {
        emit DrawableKeyformPositionsChanged(edit.name, edit.index, value.value<QVector<float>>());
    } else if (edit.field == live2dstruct::CModelEdit::MeshIndices) {
        emit DrawableIndicesChanged(edit.name, value.value<QVector<quint16>>());
    } else if (edit.field == live2dstruct::CModelEdit::MaskIndices) {
        emit DrawableMaskIndicesChanged(edit.name, value.value<QVector<int>>());
    }
    RefreshSelection();
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

void CInspectorPanel::ResetMaskControls()
{
    const QSignalBlocker blocker(m_maskInvertedCheck);
    m_editMasks->setEnabled(false);
    m_maskInvertedCheck->setEnabled(false);
    m_maskInvertedCheck->setChecked(false);
}

void CInspectorPanel::EditMasks()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    const auto* document = m_document;
    const auto source = document->SourceDigest();
    const QString target = m_selectionName;
    const QVector<int> current = document->DrawableMaskIndices(target);
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("maskEditorDialog"));
    dialog.setWindowTitle(QStringLiteral("编辑蒙版 · %1").arg(target));
    dialog.resize(400, 480);
    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel(QStringLiteral("选择裁剪此网格的蒙版。多个蒙版会合并可见区域。"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* search = new QLineEdit(&dialog);
    search->setObjectName(QStringLiteral("maskSearch"));
    search->setPlaceholderText(QStringLiteral("搜索网格 ID"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto* selectedOnly = new QCheckBox(QStringLiteral("仅显示已选"), &dialog);
    selectedOnly->setObjectName(QStringLiteral("selectedMasksOnly"));
    layout->addWidget(selectedOnly);
    auto* list = new QListWidget(&dialog);
    list->setObjectName(QStringLiteral("maskList"));
    list->setAccessibleName(QStringLiteral("可用蒙版"));
    const QSet<int> selected(current.cbegin(), current.cend());
    const auto& drawables = document->Drawables();
    for (int index = 0; index < drawables.size(); ++index) {
        const bool self = drawables[index].id == target;
        if (self && !selected.contains(index)) continue;
        const QString label = self ? QStringLiteral("%1（自身 · 原始引用）").arg(drawables[index].id) : drawables[index].id;
        auto* item = new QListWidgetItem(label, list);
        item->setData(Qt::UserRole, index);
        item->setCheckState(selected.contains(index) ? Qt::Checked : Qt::Unchecked);
    }
    QSet<int> added;
    for (int index : current) {
        if ((index >= 0 && index < drawables.size()) || added.contains(index)) continue;
        added.insert(index);
        auto* item = new QListWidgetItem(QStringLiteral("无效索引 %1（原始引用）").arg(index), list);
        item->setData(Qt::UserRole, index);
        item->setCheckState(Qt::Checked);
    }
    layout->addWidget(list, 1);
    auto* footer = new QHBoxLayout();
    auto* count = new QLabel(&dialog);
    count->setObjectName(QStringLiteral("maskSelectionCount"));
    auto* clear = new QPushButton(QStringLiteral("清空选择"), &dialog);
    clear->setObjectName(QStringLiteral("clearMasks"));
    footer->addWidget(count, 1);
    footer->addWidget(clear);
    layout->addLayout(footer);
    const auto refresh = [list, search, selectedOnly, count, clear] {
        const QString query = search->text().trimmed();
        int selected = 0, visible = 0;
        for (int row = 0; row < list->count(); ++row) {
            auto* item = list->item(row);
            const bool checked = item->checkState() == Qt::Checked;
            if (checked) ++selected;
            const bool shown = item->text().contains(query, Qt::CaseInsensitive) && (!selectedOnly->isChecked() || checked);
            item->setHidden(!shown);
            if (shown) ++visible;
        }
        count->setText(QStringLiteral("已选 %1 个 · 显示 %2 / %3").arg(selected).arg(visible).arg(list->count()));
        clear->setEnabled(selected > 0);
    };
    connect(search, &QLineEdit::textChanged, &dialog, refresh);
    connect(selectedOnly, &QCheckBox::toggled, &dialog, refresh);
    connect(list, &QListWidget::itemChanged, &dialog, refresh);
    connect(clear, &QPushButton::clicked, &dialog, [list, refresh] {
        const QSignalBlocker blocker(list);
        for (int row = 0; row < list->count(); ++row) list->item(row)->setCheckState(Qt::Unchecked);
        refresh();
    });
    refresh();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    search->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;
    if (m_document != document || m_document->SourceDigest() != source ||
        m_document->DrawableMaskIndices(target) != current) {
        RejectEdit(QStringLiteral("模型或蒙版已改变，请重新打开蒙版编辑器"));
        return;
    }
    QSet<int> checked;
    for (int row = 0; row < list->count(); ++row)
        if (list->item(row)->checkState() == Qt::Checked) checked.insert(list->item(row)->data(Qt::UserRole).toInt());
    if (checked == selected) return;
    QVector<int> after;
    for (int index : current) if (checked.remove(index)) after.append(index);
    for (int row = 0; row < list->count(); ++row) {
        const int index = list->item(row)->data(Qt::UserRole).toInt();
        if (checked.remove(index)) after.append(index);
    }
    if (after == current) return;
    const live2dstruct::CModelEdit command{target, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::MaskIndices,
                                           QVariant::fromValue(current), QVariant::fromValue(after)};
    if (!SubmitEdit(command)) return;
    emit DrawableMaskIndicesChanged(target, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
    showModelObject(m_selectionName, m_selectionType);
}

void CInspectorPanel::TranslateSelectedVertices()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName) || !m_vertexDeltaX || !m_vertexDeltaY) return;
    const float deltaX = static_cast<float>(m_vertexDeltaX->value());
    const float deltaY = static_cast<float>(m_vertexDeltaY->value());
    if (!std::isfinite(deltaX) || !std::isfinite(deltaY) || (deltaX == 0.0f && deltaY == 0.0f)) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty() || before.size() % 2 != 0) return;
    QSet<int> rows;
    for (const QTableWidgetSelectionRange& range : m_vertexTable->selectedRanges()) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) rows.insert(row);
    }
    if (rows.isEmpty()) return;
    QVector<float> after = before;
    for (int row : rows) {
        if (row < 0 || row * 2 + 1 >= after.size()) continue;
        after[row * 2] += deltaX;
        after[row * 2 + 1] += deltaY;
    }
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::MergeSelectedVertices()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName)) return;
    QSet<int> selectedRows;
    for (const QTableWidgetSelectionRange& range : m_vertexTable->selectedRanges())
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) selectedRows.insert(row);
    if (selectedRows.size() < 2) return;
    const QVector<quint16> before = m_document->DrawableIndices(m_selectionName);
    int vertexCount = 0;
    for (const auto& drawable : m_document->Drawables())
        if (drawable.id == m_selectionName) { vertexCount = drawable.vertexCount; break; }
    QVector<quint16> after;
    if (!live2dstruct::CMeshTopology::MergeVertices(before, selectedRows, vertexCount, &after) || after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::MeshIndices,
                                           QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (!SubmitEdit(command)) return;
    emit DrawableIndicesChanged(m_selectionName, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::TransformSelectedVertices()
{
    if (!m_document || m_selectionType != QLatin1String("artmesh") || m_selectionName.isEmpty() ||
        IsLocked(m_selectionType, m_selectionName) || !m_vertexScale || !m_vertexRotation) return;
    const float scale = static_cast<float>(m_vertexScale->value());
    const float degrees = static_cast<float>(m_vertexRotation->value());
    if (!std::isfinite(scale) || !std::isfinite(degrees) || scale <= 0.0f ||
        (qFuzzyCompare(scale, 1.0f) && qFuzzyIsNull(degrees))) return;
    const int keyformIndex = m_activeKeyformIndex;
    const QVector<float> before = m_document->DrawableKeyformPositions(m_selectionName, keyformIndex);
    if (keyformIndex < 0 || before.isEmpty() || before.size() % 2 != 0) return;
    QSet<int> selectedRows;
    for (const QTableWidgetSelectionRange& range : m_vertexTable->selectedRanges())
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) selectedRows.insert(row);
    if (selectedRows.isEmpty()) return;
    double centerX = 0.0;
    double centerY = 0.0;
    int count = 0;
    for (int row : selectedRows) {
        if (row < 0 || row * 2 + 1 >= before.size()) continue;
        centerX += before[row * 2];
        centerY += before[row * 2 + 1];
        ++count;
    }
    if (count == 0) return;
    centerX /= count;
    centerY /= count;
    const double radians = double(degrees) * 0.017453292519943295;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    QVector<float> after = before;
    for (int row : selectedRows) {
        if (row < 0 || row * 2 + 1 >= after.size()) continue;
        const double x = double(before[row * 2]) - centerX;
        const double y = double(before[row * 2 + 1]) - centerY;
        const double changedX = centerX + (x * cosine - y * sine) * scale;
        const double changedY = centerY + (x * sine + y * cosine) * scale;
        if (!std::isfinite(changedX) || !std::isfinite(changedY) ||
            std::abs(changedX) > std::numeric_limits<float>::max() || std::abs(changedY) > std::numeric_limits<float>::max()) {
            RejectEdit(QStringLiteral("变换结果超出坐标范围"));
            return;
        }
        after[row * 2] = float(changedX);
        after[row * 2 + 1] = float(changedY);
    }
    if (after == before) return;
    const live2dstruct::CModelEdit command{m_selectionName, QStringLiteral("artmesh"),
                                           live2dstruct::CModelEdit::KeyformPositions,
                                           QVariant::fromValue(before), QVariant::fromValue(after), keyformIndex, false};
    if (!SubmitEdit(command)) return;
    RebuildKeyformTable();
    emit DrawableKeyformPositionsChanged(m_selectionName, keyformIndex, after);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::Undo()
{
    if (!m_document) return;
    live2dstruct::CModelEdit pending;
    if (!m_document->PeekUndoEdit(&pending) || pending.field == live2dstruct::CModelEdit::ParameterValue) return;
    live2dstruct::CModelEdit edit;
    if (!SubmitHistory(true, edit)) return;
    EmitEdit(edit, false);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::Redo()
{
    if (!m_document) return;
    live2dstruct::CModelEdit pending;
    if (!m_document->PeekRedoEdit(&pending) || pending.field == live2dstruct::CModelEdit::ParameterValue) return;
    live2dstruct::CModelEdit edit;
    if (!SubmitHistory(false, edit)) return;
    EmitEdit(edit, true);
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CInspectorPanel::showModelObject(const QString& name, const QString& type, bool preserveKeyforms)
{
    const QPointer<CInspectorPanel> owner(this);
    const auto rows = qScopeGuard([owner] { if (owner) updateObjectRows(owner, owner->GetSelectionType()); });

    if (!m_document || name.isEmpty()) {
        ClearSelection();
        return;
    }
    const int selectedKeyform = preserveKeyforms ? m_activeKeyformIndex : 0;
    const int selectedDeformerKeyform = preserveKeyforms ? m_activeDeformerKeyform : 0;
    m_activeKeyformIndex = -1;
    m_activeDeformerKeyform = -1;
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
    m_updatingDeformerParent = true;
    m_deformerParentCombo->clear();
    m_deformerParentCombo->setEnabled(false);
    m_updatingDeformerParent = false;
    ResetMaskControls();
    const bool unlocked = !IsLocked(type, name);
    m_visibilityCheck->setEnabled(unlocked);
    m_opacitySpin->setEnabled(false);
    m_drawOrderSpin->setEnabled(false);
    m_updatingKeyform = true;
    m_keyformCombo->clear();
    m_keyformCombo->setEnabled(false);
    m_vertexTable->setRowCount(0);
    m_vertexTable->setEnabled(false);
    m_rotationKeyformTable->setRowCount(0);
    setRowVisible(m_rotationKeyformTable, false);
    m_rotationKeyformTable->setEnabled(false);
    m_warpKeyformCombo->clear();
    m_warpKeyformCombo->setEnabled(false);
    m_warpKeyformTable->setRowCount(0);
    setRowVisible(m_warpKeyformTable, false);
    m_warpKeyformTable->setEnabled(false);
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
            const auto masks = m_document->DrawableMaskIndices(name);
            m_maskLabel->setText(masks.isEmpty() ? QStringLiteral("无") : QString::number(masks.size()));
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
            m_copyKeyform->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_pasteKeyform->setEnabled(unlocked && !drawable.keyforms.isEmpty());
            m_autoTriangulate->setEnabled(unlocked && drawable.vertexCount >= 3 && !drawable.keyforms.isEmpty());
            m_copyTopology->setEnabled(unlocked && !drawable.indices.isEmpty());
            m_pasteTopology->setEnabled(unlocked && drawable.vertexCount > 0);
            m_editMasks->setEnabled(unlocked);
            m_updatingVisibility = true;
            m_maskInvertedCheck->setChecked(m_document->DrawableMaskInverted(name));
            m_maskInvertedCheck->setEnabled(unlocked && !masks.isEmpty());
            m_updatingVisibility = false;
            if (!drawable.keyforms.isEmpty())
                m_keyformCombo->setCurrentIndex(selectedKeyform >= 0 && selectedKeyform < drawable.keyforms.size()
                                                   ? selectedKeyform : 0);
            m_activeKeyformIndex = m_keyformCombo->currentIndex();
            m_updatingKeyform = false;
            RebuildKeyformTable();
            if (m_keyformCombo->currentIndex() >= 0)
                emit DrawableKeyformActivated(name, m_keyformCombo->currentIndex());
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
            m_deformerLabel->setText(indexValue(m_document->DeformerParentIndex(name), QStringLiteral("无")));
            m_updatingDeformerParent = true;
            m_deformerParentCombo->clear();
            m_deformerParentCombo->addItem(QStringLiteral("无（根级）"), -1);
            for (int parentIndex = 0; parentIndex < m_document->Deformers().size(); ++parentIndex) {
                const auto& candidate = m_document->Deformers()[parentIndex];
                if (candidate.id == name || (candidate.parentPartIndex != deformer.parentPartIndex &&
                    parentIndex != deformer.parentDeformerIndex &&
                    parentIndex != m_document->SourceDeformerParentIndex(deformer.id))) continue;
                m_deformerParentCombo->addItem(candidate.id, parentIndex);
            }
            const int parentIndex = m_document->DeformerParentIndex(name);
            for (int comboIndex = 0; comboIndex < m_deformerParentCombo->count(); ++comboIndex) {
                if (m_deformerParentCombo->itemData(comboIndex).toInt() == parentIndex) {
                    m_deformerParentCombo->setCurrentIndex(comboIndex);
                    break;
                }
            }
            m_deformerParentCombo->setEnabled(unlocked);
            m_updatingDeformerParent = false;
            m_visibilityLabel->setText(QStringLiteral("%1 / %2").arg(deformer.visible ? QStringLiteral("是") : QStringLiteral("否"),
                                                                    deformer.enabled ? QStringLiteral("是") : QStringLiteral("否")));
            m_updatingVisibility = true;
            m_visibilityCheck->setChecked(deformer.visible);
            m_updatingVisibility = false;
            if (deformer.type == live2dstruct::CModelDeformer::Warp) {
                m_geometryLabel->setText(QStringLiteral("Warp %1 × %2").arg(deformer.columns).arg(deformer.rows));
                const auto keyforms = m_document->DeformerWarpKeyforms(name);
                m_updatingWarpKeyform = true;
                for (int keyformIndex = 0; keyformIndex < keyforms.size(); ++keyformIndex)
                    m_warpKeyformCombo->addItem(QStringLiteral("Keyform %1").arg(keyformIndex), keyformIndex);
                m_warpKeyformCombo->setEnabled(unlocked && !keyforms.isEmpty());
                setRowVisible(m_warpKeyformTable, !keyforms.isEmpty());
                m_warpKeyformTable->setEnabled(unlocked && !keyforms.isEmpty());
                if (!keyforms.isEmpty()) {
                    m_activeDeformerKeyform = selectedDeformerKeyform >= 0 && selectedDeformerKeyform < keyforms.size()
                        ? selectedDeformerKeyform : 0;
                    m_warpKeyformCombo->setCurrentIndex(m_activeDeformerKeyform);
                }
                m_updatingWarpKeyform = false;
                RebuildWarpKeyformTable();
            } else {
                m_geometryLabel->setText(QStringLiteral("Rotation Deformer"));
                setRowVisible(m_rotationKeyformTable, true);
                m_rotationKeyformTable->setEnabled(unlocked);
                m_activeDeformerKeyform = selectedDeformerKeyform >= 0 && selectedDeformerKeyform < deformer.rotationKeyforms.size()
                    ? selectedDeformerKeyform : 0;
                RebuildRotationKeyformTable();
            }
            return;
        }
    }
    ClearSelection();
}
