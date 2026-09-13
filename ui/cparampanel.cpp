#include "cparampanel.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "live2dstruct/cmodeldocument.h"
#include "ui/para/ccidjson.h"
#include "ui/cxyparameterpad.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QComboBox>
#include <QSignalBlocker>
#include <QToolButton>
#include <QApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QSaveFile>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QCheckBox>
#include <QSpinBox>
#include <algorithm>
#include <iterator>
#include <cmath>

using namespace Live2D::Cubism::Framework;

namespace {
bool parseParameterPreset(const QByteArray& data, QMap<QString, float>* values, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = parseError.errorString();
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("live2d2psd-parameters-v1")) {
        if (error) *error = QStringLiteral("参数预设格式不受支持");
        return false;
    }
    QMap<QString, float> parsed;
    for (const QString& key : object.keys()) {
        if (key == QStringLiteral("format")) continue;
        const QJsonValue value = object.value(key);
        if (!value.isDouble() || !std::isfinite(value.toDouble())) {
            if (error) *error = QStringLiteral("参数值无效：%1").arg(key);
            return false;
        }
        parsed.insert(key, static_cast<float>(value.toDouble()));
    }
    if (values) *values = parsed;
    return true;
}
}

CParamPanel::CParamPanel(QWidget *parent)
    : QDockWidget("参数控制", parent)
{
    setObjectName(QStringLiteral("paramPanel"));
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    auto* buttonRow = new QGridLayout();
    buttonRow->setHorizontalSpacing(6);
    buttonRow->setVerticalSpacing(6);
    QPushButton* resetBtn = new QPushButton("重置全部", wrapper);
    resetBtn->setObjectName("primaryBtn");
    connect(resetBtn, &QPushButton::clicked, this, &CParamPanel::ResetAll);
    buttonRow->addWidget(resetBtn, 0, 0);
    auto* copyBtn = new QPushButton(QStringLiteral("复制参数"), wrapper);
    auto* pasteBtn = new QPushButton(QStringLiteral("粘贴参数"), wrapper);
    auto* savePresetBtn = new QPushButton(QStringLiteral("保存预设"), wrapper);
    auto* loadPresetBtn = new QPushButton(QStringLiteral("载入预设"), wrapper);
    auto* metadataBtn = new QPushButton(QStringLiteral("编辑参数元数据"), wrapper);
    buttonRow->addWidget(copyBtn, 0, 1);
    buttonRow->addWidget(pasteBtn, 1, 0);
    buttonRow->addWidget(savePresetBtn, 1, 1);
    buttonRow->addWidget(loadPresetBtn, 2, 0, 1, 2);
    buttonRow->addWidget(metadataBtn, 3, 0, 1, 2);
    mainLayout->addLayout(buttonRow);
    connect(copyBtn, &QPushButton::clicked, this, &CParamPanel::CopyValues);
    connect(pasteBtn, &QPushButton::clicked, this, &CParamPanel::PasteValues);
    connect(savePresetBtn, &QPushButton::clicked, this, &CParamPanel::SavePreset);
    connect(loadPresetBtn, &QPushButton::clicked, this, &CParamPanel::LoadPreset);
    connect(metadataBtn, &QPushButton::clicked, this, &CParamPanel::EditParameterMetadata);

    m_search = new QLineEdit(wrapper);
    m_search->setPlaceholderText("搜索参数名称或 ID");
    m_search->setClearButtonEnabled(true);
    mainLayout->addWidget(m_search);

    m_xyBox = new QGroupBox(QStringLiteral("XY 参数控制"), wrapper);
    auto* xyLayout = new QGridLayout(m_xyBox);
    m_xyXSelector = new QComboBox(m_xyBox);
    m_xyYSelector = new QComboBox(m_xyBox);
    m_xyXSelector->setObjectName(QStringLiteral("xyXParameter"));
    m_xyYSelector->setObjectName(QStringLiteral("xyYParameter"));
    m_xyPad = new CXYParameterPad(m_xyBox);
    m_xyPad->setObjectName(QStringLiteral("xyParameterPad"));
    xyLayout->addWidget(new QLabel(QStringLiteral("X"), m_xyBox), 0, 0);
    xyLayout->addWidget(m_xyXSelector, 0, 1);
    xyLayout->addWidget(new QLabel(QStringLiteral("Y"), m_xyBox), 1, 0);
    xyLayout->addWidget(m_xyYSelector, 1, 1);
    xyLayout->addWidget(m_xyPad, 0, 2, 2, 1);
    m_xyBox->setEnabled(false);
    mainLayout->addWidget(m_xyBox);
    connect(m_xyXSelector, &QComboBox::currentTextChanged, this, [this](){ RefreshXYControls(); });
    connect(m_xyYSelector, &QComboBox::currentTextChanged, this, [this](){ RefreshXYControls(); });
    connect(m_xyPad, &CXYParameterPad::ValuesChanged, this, &CParamPanel::ApplyXYPad);

    m_scroll = new QScrollArea(wrapper);
    m_scroll->setWidgetResizable(true);
    m_container = new QWidget();
    m_scroll->setWidget(m_container);
    mainLayout->addWidget(m_scroll);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text){
        const QString query = text.trimmed().toLower();
        if (!query.isEmpty()) {
            for (QToolButton* groupButton : m_container->findChildren<QToolButton*>()) groupButton->setChecked(true);
        }
        for (const ParamRow& row : m_rows) {
            const QString searchText = row.widget ? row.widget->property("searchText").toString() : QString();
            row.widget->setVisible(query.isEmpty() || searchText.contains(query));
        }
    });

    setWidget(wrapper);
}

void CParamPanel::SetDocument(live2dstruct::CModelDocument* document)
{
    m_document = document;
}

void CParamPanel::ApplyExternalValue(const QString& paramId, float value, bool record, bool userEdit)
{
    auto rowIt = std::find_if(m_rows.cbegin(), m_rows.cend(), [&paramId](const ParamRow& row){ return row.paramId == paramId; });
    if (rowIt == m_rows.cend()) return;
    const int rowIndex = static_cast<int>(rowIt - m_rows.cbegin());
    ParamRow& row = m_rows[rowIndex];
    value = qBound(row.minVal, value, row.maxVal);
    const float before = m_lastValues.value(paramId, row.defaultVal);
    const int sliderValue = row.maxVal > row.minVal
        ? qBound(0, qRound((value - row.minVal) * 1000.0f / (row.maxVal - row.minVal)), 1000) : 0;
    QSignalBlocker sliderBlocker(row.slider);
    QSignalBlocker spinBlocker(row.spinBox);
    row.slider->setValue(sliderValue);
    row.spinBox->setValue(value);
    if (userEdit) m_userLocked.insert(rowIndex);
    m_lastValues.insert(paramId, value);
    if (m_document) m_document->SetParameterValue(paramId, value);
    emit ParamChanged(paramId, value, !userEdit);
    RefreshXYControls();
    if (record && !qFuzzyCompare(before + 1.0f, value + 1.0f)) PushEdit(paramId, before, value);
}

void CParamPanel::BuildParams(CCidJson* cid, const QSet<QString>* blendShapeParams)
{
    m_rows.clear();
    m_lastValues.clear();
    ClearHistory();
    {
        QSignalBlocker xBlocker(m_xyXSelector);
        QSignalBlocker yBlocker(m_xyYSelector);
        m_xyXSelector->clear();
        m_xyYSelector->clear();
        m_xyXSelector->parentWidget()->setEnabled(false);
    }

    // 删除旧 layout
    if(m_container->layout()){
        QLayout* old = m_container->layout();
        QLayoutItem* item;
        while((item = old->takeAt(0)) != nullptr){
            delete item->widget();
            delete item;
        }
        delete old;
    }

    QVBoxLayout* layout = new QVBoxLayout(m_container);

    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(!mgr || mgr->GetModelNum() == 0) return;

    LAppModel* model = mgr->GetModel(0);
    if(!model) return;

    CubismModel* cubismModel = model->GetModel();
    if(!cubismModel) return;

    // 收集所有参数信息
    int count = cubismModel->GetParameterCount();
    QMap<QString, int> paramIdToModelIndex;
    for(int i = 0; i < count; i++){
        QString paramId = cubismModel->GetParameterId(i)->GetString().GetRawString();
        paramIdToModelIndex[paramId] = i;
    }

    // 添加一行参数滑条的 lambda
    auto addParamRow = [&](QVBoxLayout* targetLayout, const QString& paramId, int modelIdx){
        float minVal = cubismModel->GetParameterMinimumValue(modelIdx);
        float maxVal = cubismModel->GetParameterMaximumValue(modelIdx);
        float defVal = cubismModel->GetParameterDefaultValue(modelIdx);
        live2dstruct::CModelParameterMetadata metadata;
        const bool hasMetadata = m_document && m_document->ParameterMetadataOverrides().contains(paramId);
        if (hasMetadata) {
            metadata = m_document->ParameterMetadata(paramId);
        }
        if (hasMetadata) {
            minVal = metadata.minimum;
            maxVal = metadata.maximum;
            defVal = metadata.defaultValue;
        }
        if (!std::isfinite(minVal) || !std::isfinite(maxVal) || minVal > maxVal) {
            minVal = 0.0f;
            maxVal = 1.0f;
        }
        if (!std::isfinite(defVal)) defVal = minVal;
        defVal = qBound(minVal, defVal, maxVal);
        float initialVal = qBound(minVal, cubismModel->GetParameterValue(modelIdx), maxVal);
        if (m_document) {
            if (m_document->ParameterValues().contains(paramId)) initialVal = m_document->ParameterValue(paramId);
            else m_document->SetParameterValue(paramId, initialVal);
        }

        QWidget* rowWidget = new QWidget(m_container);
        QHBoxLayout* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 2, 0, 2);

        // 显示名称：优先用 cid 映射的名称，否则回退到内部 ID
        QString displayName = hasMetadata && !metadata.displayName.isEmpty() ? metadata.displayName : paramId;
        if(cid){
            QString mapped = cid->getParaName(paramId);
            if(!hasMetadata && !mapped.isEmpty()) displayName = mapped;
        }

        // 融合变形参数标记
        bool isBlendShape = (blendShapeParams && blendShapeParams->contains(paramId))
                         || (cid && cid->isCombinedParameter(paramId));
        if(isBlendShape){
            displayName = QString("[融合] %1").arg(displayName);
        }
        rowWidget->setProperty("searchText", (displayName + " " + paramId).toLower());
        QStringList keyTexts;
        if (m_document) {
            for (const auto& parameter : m_document->Parameters()) {
                if (parameter.id != paramId) continue;
                for (float key : parameter.keyValues) keyTexts.append(QString::number(key, 'f', 3));
                break;
            }
        }
        const QString tooltip = keyTexts.isEmpty()
            ? paramId : QStringLiteral("%1\n关键值：%2").arg(paramId, keyTexts.join(QStringLiteral("、")));
        rowWidget->setToolTip(tooltip);

        QLabel* nameLabel = new QLabel(displayName, m_container);
        nameLabel->setFixedWidth(150);
        nameLabel->setToolTip(tooltip);
        if(isBlendShape){
            nameLabel->setObjectName(QStringLiteral("blendParamTag"));
        }
        row->addWidget(nameLabel);

        QSlider* slider = new QSlider(Qt::Horizontal, rowWidget);
        slider->setRange(0, 1000);
        int sliderDefault = 0;
        if(maxVal > minVal){
            sliderDefault = qBound(0, qRound((initialVal - minVal) * 1000.0f / (maxVal - minVal)), 1000);
        }
        slider->setValue(sliderDefault);
        row->addWidget(slider);

        QDoubleSpinBox* spinBox = new QDoubleSpinBox(rowWidget);
        spinBox->setRange(minVal, maxVal);
        spinBox->setDecimals(3);
        spinBox->setSingleStep((maxVal - minVal) > 0 ? (maxVal - minVal) / 100.0 : 1.0);
        spinBox->setValue(initialVal);
        spinBox->setFixedWidth(92);
        row->addWidget(spinBox);

        QToolButton* resetButton = new QToolButton(rowWidget);
        resetButton->setText(QStringLiteral("↺"));
        resetButton->setToolTip(QStringLiteral("恢复默认值 %1").arg(QString::number(defVal, 'f', 3)));
        resetButton->setFixedWidth(28);
        row->addWidget(resetButton);

        ParamRow pr;
        pr.widget = rowWidget;
        pr.slider = slider;
        pr.spinBox = spinBox;
        pr.resetButton = resetButton;
        pr.nameLabel = nameLabel;
        pr.valueLabel = nullptr;
        pr.minVal = minVal;
        pr.maxVal = maxVal;
        pr.defaultVal = defVal;
        pr.paramId = paramId;
        pr.modelIndex = modelIdx;
        pr.blendShape = isBlendShape;
        m_rows.append(pr);
        m_lastValues.insert(paramId, initialVal);

        int idx = m_rows.size() - 1;
        connect(resetButton, &QToolButton::clicked, this, [this, idx](){
            if (idx < 0 || idx >= m_rows.size()) return;
            ParamRow& r = m_rows[idx];
            const int sliderValue = r.maxVal > r.minVal
                ? qBound(0, qRound((r.defaultVal - r.minVal) * 1000.0f / (r.maxVal - r.minVal)), 1000)
                : 0;
            QSignalBlocker sliderBlocker(r.slider);
            QSignalBlocker spinBlocker(r.spinBox);
            r.slider->setValue(sliderValue);
            r.spinBox->setValue(r.defaultVal);
            m_userLocked.insert(idx);
            ApplyValue(idx, r.defaultVal, true);
        });
        connect(slider, &QSlider::sliderPressed, this, [this, idx](){
            m_userDragging.insert(idx);
            m_dragStarts.insert(idx, m_lastValues.value(m_rows[idx].paramId));
        });
        connect(slider, &QSlider::sliderReleased, this, [this, idx](){
            m_userDragging.remove(idx);
            m_userLocked.insert(idx); // 用户改过的参数锁定，不再被 sync 覆盖
            if (!m_dragStarts.contains(idx)) return;
            const float before = m_dragStarts.take(idx);
            const float after = m_lastValues.value(m_rows[idx].paramId);
            PushEdit(m_rows[idx].paramId, before, after);
        });
        connect(slider, &QSlider::valueChanged, this, [this, idx](int val){
            ParamRow& r = m_rows[idx];
            float realVal = r.minVal + (r.maxVal - r.minVal) * val / 1000.0f;
            QSignalBlocker blocker(r.spinBox);
            r.spinBox->setValue(realVal);
            ApplyValue(idx, realVal, !m_userDragging.contains(idx));
        });

        connect(spinBox, &QDoubleSpinBox::valueChanged, this, [this, idx](double value){
            ParamRow& r = m_rows[idx];
            if (r.maxVal > r.minVal) {
                const int sliderValue = qBound(0, qRound((value - r.minVal) * 1000.0f / (r.maxVal - r.minVal)), 1000);
                QSignalBlocker blocker(r.slider);
                r.slider->setValue(sliderValue);
            }
            ApplyValue(idx, static_cast<float>(value), true);
        });
        connect(spinBox, &QDoubleSpinBox::editingFinished, this, [this, idx](){ m_userLocked.insert(idx); });

        targetLayout->addWidget(rowWidget);
    };

    // 有分组信息时按组显示
    if(cid && !cid->getGroups().isEmpty()){
        QSet<QString> addedParams;
        for(const CVetGroup& group : cid->getGroups()){
            // 组标题
            QString groupName;
            const QMap<QString,QString>& groupMap = cid->getGroupIdToName();
            if(groupMap.contains(group.strGroup))
                groupName = groupMap[group.strGroup];
            else
                groupName = group.strGroup;

            auto* groupButton = new QToolButton(m_container);
            groupButton->setText(groupName);
            groupButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            groupButton->setArrowType(Qt::DownArrow);
            groupButton->setCheckable(true);
            groupButton->setChecked(true);
            groupButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            auto* groupBody = new QWidget(m_container);
            auto* groupLayout = new QVBoxLayout(groupBody);
            groupLayout->setContentsMargins(8, 0, 0, 0);
            groupLayout->setSpacing(0);
            layout->addWidget(groupButton);
            layout->addWidget(groupBody);
            connect(groupButton, &QToolButton::toggled, groupBody, [groupButton, groupBody](bool checked){
                groupBody->setVisible(checked);
                groupButton->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
            });

            for(const QString& paramId : group.vetParam){
                if(!paramIdToModelIndex.contains(paramId)) continue;
                addParamRow(groupLayout, paramId, paramIdToModelIndex[paramId]);
                addedParams.insert(paramId);
            }
        }

        // 未分组的参数放到最后
        for(int i = 0; i < count; i++){
            QString paramId = cubismModel->GetParameterId(i)->GetString().GetRawString();
            if(!addedParams.contains(paramId)){
                addParamRow(layout, paramId, i);
            }
        }
    } else {
        // 无分组信息，全部平铺
        for(int i = 0; i < count; i++){
            QString paramId = cubismModel->GetParameterId(i)->GetString().GetRawString();
            addParamRow(layout, paramId, i);
        }
    }

    layout->addStretch();
    for (const ParamRow& row : m_rows) {
        m_xyXSelector->addItem(row.paramId);
        m_xyYSelector->addItem(row.paramId);
    }
    if (m_rows.size() >= 2) {
        QSignalBlocker xBlocker(m_xyXSelector);
        QSignalBlocker yBlocker(m_xyYSelector);
        m_xyXSelector->setCurrentIndex(0);
        m_xyYSelector->setCurrentIndex(1);
        m_xyXSelector->parentWidget()->setEnabled(true);
    }
    RefreshXYControls();
}

void CParamPanel::RefreshParameterMetadata()
{
    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) RefreshParameterRow(rowIndex);
    RefreshXYControls();
}

void CParamPanel::RefreshXYControls()
{
    if (!m_xyXSelector || !m_xyYSelector || !m_xyPad || !m_xyBox || m_updatingXY) return;
    if (m_rows.size() < 2 || m_xyXSelector->count() < 2 || m_xyYSelector->count() < 2) {
        m_xyBox->setEnabled(false);
        return;
    }
    m_updatingXY = true;
    if (m_xyXSelector->currentText() == m_xyYSelector->currentText()) {
        const int replacement = m_xyYSelector->currentIndex() == 0 ? 1 : 0;
        QSignalBlocker blocker(m_xyYSelector);
        m_xyYSelector->setCurrentIndex(replacement);
    }
    const auto parameterValue = [this](QComboBox* selector, float* minimum, float* maximum) {
        const auto rowIt = std::find_if(m_rows.cbegin(), m_rows.cend(), [selector](const ParamRow& row){
            return row.paramId == selector->currentText();
        });
        if (rowIt == m_rows.cend()) return 0.0f;
        *minimum = rowIt->minVal;
        *maximum = rowIt->maxVal;
        return m_lastValues.value(rowIt->paramId, rowIt->defaultVal);
    };
    float xMinimum = 0.0f, xMaximum = 1.0f, yMinimum = 0.0f, yMaximum = 1.0f;
    const float xValue = parameterValue(m_xyXSelector, &xMinimum, &xMaximum);
    const float yValue = parameterValue(m_xyYSelector, &yMinimum, &yMaximum);
    m_xyPad->SetRange(xMinimum, xMaximum, yMinimum, yMaximum);
    m_xyPad->SetValues(xValue, yValue);
    m_xyBox->setEnabled(true);
    m_updatingXY = false;
}

void CParamPanel::ApplyXYPad(float xValue, float yValue)
{
    if (m_updatingXY || !m_xyXSelector || !m_xyYSelector) return;
    const auto findRow = [this](QComboBox* selector) {
        return std::find_if(m_rows.cbegin(), m_rows.cend(), [selector](const ParamRow& row){
            return row.paramId == selector->currentText();
        });
    };
    const auto xRow = findRow(m_xyXSelector);
    const auto yRow = findRow(m_xyYSelector);
    if (xRow == m_rows.cend() || yRow == m_rows.cend()) return;
    xValue = qBound(xRow->minVal, xValue, xRow->maxVal);
    yValue = qBound(yRow->minVal, yValue, yRow->maxVal);
    const QMap<QString, float> before{{xRow->paramId, m_lastValues.value(xRow->paramId, xRow->defaultVal)},
                                      {yRow->paramId, m_lastValues.value(yRow->paramId, yRow->defaultVal)}};
    const QMap<QString, float> after{{xRow->paramId, xValue}, {yRow->paramId, yValue}};
    if (before == after || !m_document) return;
    const live2dstruct::CModelEdit command{
        QStringLiteral("xy"), QStringLiteral("parameters"), live2dstruct::CModelEdit::ParameterValues,
        QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (!m_document->ExecuteEdit(command)) return;
    m_updatingXY = true;
    ApplyExternalValue(xRow->paramId, xValue, false);
    ApplyExternalValue(yRow->paramId, yValue, false);
    m_updatingXY = false;
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CParamPanel::ResetAll()
{
    m_userLocked.clear();
    m_userDragging.clear();
    m_dragStarts.clear();
    for(auto& r : m_rows){
        const float before = m_lastValues.value(r.paramId, r.defaultVal);
        int sliderDefault = 0;
        if(r.maxVal > r.minVal){
            sliderDefault = static_cast<int>((r.defaultVal - r.minVal) / (r.maxVal - r.minVal) * 1000.0f);
        }
        r.slider->blockSignals(true);
        r.slider->setValue(sliderDefault);
        r.slider->blockSignals(false);
        r.spinBox->blockSignals(true);
        r.spinBox->setValue(r.defaultVal);
        r.spinBox->blockSignals(false);
        ApplyValue(&r - m_rows.data(), r.defaultVal, false);
        PushEdit(r.paramId, before, r.defaultVal);
    }
}

void CParamPanel::SyncFromModel()
{
    LAppLive2DManager* mgr = LAppLive2DManager::GetInstance();
    if(!mgr || mgr->GetModelNum() == 0) return;

    LAppModel* model = mgr->GetModel(0);
    if(!model) return;

    CubismModel* cubismModel = model->GetModel();
    if(!cubismModel) return;

    for(int i = 0; i < m_rows.size(); i++){
        auto& r = m_rows[i];
        if(r.modelIndex < 0) continue;
        if(m_userDragging.contains(i) || m_userLocked.contains(i)) continue;
        float curVal = cubismModel->GetParameterValue(r.modelIndex);
        int sliderVal = 0;
        if(r.maxVal > r.minVal){
            sliderVal = static_cast<int>((curVal - r.minVal) / (r.maxVal - r.minVal) * 1000.0f);
            sliderVal = qBound(0, sliderVal, 1000);
        }
        r.slider->blockSignals(true);
        r.slider->setValue(sliderVal);
        r.slider->blockSignals(false);
        r.spinBox->blockSignals(true);
        r.spinBox->setValue(curVal);
        r.spinBox->blockSignals(false);
        m_lastValues.insert(r.paramId, curVal);
    }
}

QMap<QString, float> CParamPanel::CurrentValues() const
{
    QMap<QString, float> values;
    for (const ParamRow& row : m_rows) values.insert(row.paramId, m_lastValues.value(row.paramId, row.defaultVal));
    return values;
}

void CParamPanel::ApplyValues(const QMap<QString, float>& values)
{
    ApplyValuesInternal(values, false);
}

void CParamPanel::ApplyValuesInternal(const QMap<QString, float>& values, bool record)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        ParamRow& row = m_rows[i];
        if (!values.contains(row.paramId)) continue;
        const float requested = values.value(row.paramId);
        if (!std::isfinite(requested)) continue;
        const float value = qBound(row.minVal, requested, row.maxVal);
        const float before = m_lastValues.value(row.paramId, row.defaultVal);
        const int sliderValue = row.maxVal > row.minVal
            ? qBound(0, qRound((value - row.minVal) * 1000.0f / (row.maxVal - row.minVal)), 1000)
            : 0;
        QSignalBlocker sliderBlocker(row.slider);
        QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(sliderValue);
        row.spinBox->setValue(value);
        m_lastValues.insert(row.paramId, value);
        m_userLocked.insert(i);
        if (m_document) m_document->SetParameterValue(row.paramId, value);
        emit ParamChanged(row.paramId, value, false);
        if (record && !qFuzzyCompare(before + 1.0f, value + 1.0f)) PushEdit(row.paramId, before, value);
    }
    if (record) EmitHistoryState();
    else ClearHistory();
}

void CParamPanel::CopyValues()
{
    QJsonObject object;
    object.insert(QStringLiteral("format"), QStringLiteral("live2d2psd-parameters-v1"));
    const QMap<QString, float> values = CurrentValues();
    for (auto it = values.cbegin(); it != values.cend(); ++it) object.insert(it.key(), static_cast<double>(it.value()));
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

void CParamPanel::PasteValues()
{
    const QString text = QApplication::clipboard()->text();
    QMap<QString, float> values;
    if (parseParameterPreset(text.toUtf8(), &values, nullptr)) ApplyValues(values);
}

void CParamPanel::SavePreset()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存参数预设"), QString(),
        QStringLiteral("Live2D 参数预设 (*.live2d-parameters.json);;JSON (*.json)"));
    if (path.isEmpty()) return;
    QJsonObject object;
    object.insert(QStringLiteral("format"), QStringLiteral("live2d2psd-parameters-v1"));
    const QMap<QString, float> values = CurrentValues();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        object.insert(it.key(), static_cast<double>(it.value()));
    }
    QSaveFile file(path);
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存参数预设失败"), path);
        return;
    }
}

void CParamPanel::LoadPreset()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("载入参数预设"), QString(),
        QStringLiteral("Live2D 参数预设 (*.live2d-parameters.json);;JSON (*.json)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("载入参数预设失败"), path);
        return;
    }
    QMap<QString, float> values;
    QString error;
    if (!parseParameterPreset(file.readAll(), &values, &error)) {
        QMessageBox::warning(this, QStringLiteral("载入参数预设失败"), error);
        return;
    }
    ApplyValuesInternal(values, true);
}

void CParamPanel::EditParameterMetadata()
{
    if (!m_document || m_rows.isEmpty()) return;
    QStringList ids;
    for (const ParamRow& row : m_rows) ids.append(row.paramId);
    bool accepted = false;
    const QString id = QInputDialog::getItem(this, QStringLiteral("选择参数"), QStringLiteral("参数 ID"),
                                              ids, 0, false, &accepted);
    if (!accepted || id.isEmpty()) return;
    const int rowIndex = std::distance(m_rows.cbegin(), std::find_if(m_rows.cbegin(), m_rows.cend(),
        [&id](const ParamRow& row){ return row.paramId == id; }));
    if (rowIndex < 0 || rowIndex >= m_rows.size()) return;
    const live2dstruct::CModelParameterMetadata current = m_document->ParameterMetadata(id);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("编辑参数：%1").arg(id));
    auto* form = new QFormLayout(&dialog);
    auto* nameEdit = new QLineEdit(current.displayName, &dialog);
    auto* minSpin = new QDoubleSpinBox(&dialog);
    auto* maxSpin = new QDoubleSpinBox(&dialog);
    auto* defaultSpin = new QDoubleSpinBox(&dialog);
    for (QDoubleSpinBox* spin : {minSpin, maxSpin, defaultSpin}) {
        spin->setRange(-1000000000.0, 1000000000.0);
        spin->setDecimals(6);
    }
    minSpin->setValue(current.minimum);
    maxSpin->setValue(current.maximum);
    defaultSpin->setValue(current.defaultValue);
    auto* decimalsSpin = new QSpinBox(&dialog);
    decimalsSpin->setRange(0, 9);
    decimalsSpin->setValue(current.decimalPlaces);
    auto* repeatCheck = new QCheckBox(&dialog);
    repeatCheck->setChecked(current.repeat);
    form->addRow(QStringLiteral("显示名"), nameEdit);
    form->addRow(QStringLiteral("最小值"), minSpin);
    form->addRow(QStringLiteral("最大值"), maxSpin);
    form->addRow(QStringLiteral("默认值"), defaultSpin);
    form->addRow(QStringLiteral("小数位"), decimalsSpin);
    form->addRow(QStringLiteral("循环参数"), repeatCheck);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if (minSpin->value() > maxSpin->value() || defaultSpin->value() < minSpin->value() ||
        defaultSpin->value() > maxSpin->value()) {
        QMessageBox::warning(this, QStringLiteral("参数元数据无效"), QStringLiteral("默认值必须位于最小值和最大值之间"));
        return;
    }

    const live2dstruct::CModelParameterMetadata updated{
        nameEdit->text().trimmed().isEmpty() ? id : nameEdit->text().trimmed(),
        static_cast<float>(minSpin->value()), static_cast<float>(maxSpin->value()),
        static_cast<float>(defaultSpin->value()), decimalsSpin->value(), repeatCheck->isChecked()};
    const auto toVariant = [](const live2dstruct::CModelParameterMetadata& metadata) {
        QVariantMap map;
        map.insert(QStringLiteral("displayName"), metadata.displayName);
        map.insert(QStringLiteral("minimum"), metadata.minimum);
        map.insert(QStringLiteral("maximum"), metadata.maximum);
        map.insert(QStringLiteral("defaultValue"), metadata.defaultValue);
        map.insert(QStringLiteral("decimalPlaces"), metadata.decimalPlaces);
        map.insert(QStringLiteral("repeat"), metadata.repeat);
        return map;
    };
    const live2dstruct::CModelEdit edit{id, QStringLiteral("parameter"),
                                        live2dstruct::CModelEdit::ParameterMetadata,
                                        toVariant(current), toVariant(updated), -1, false};
    if (!m_document->ExecuteEdit(edit)) {
        QMessageBox::warning(this, QStringLiteral("参数元数据无效"), QStringLiteral("关键值超出新范围或当前值无法兼容"));
        return;
    }
    RefreshParameterRow(rowIndex);
    EmitHistoryState();
}

void CParamPanel::RefreshParameterRow(int rowIndex)
{
    if (!m_document || rowIndex < 0 || rowIndex >= m_rows.size()) return;
    ParamRow& row = m_rows[rowIndex];
    const auto metadata = m_document->ParameterMetadata(row.paramId);
    row.minVal = metadata.minimum;
    row.maxVal = metadata.maximum;
    row.defaultVal = metadata.defaultValue;
    const QString displayName = (row.blendShape ? QStringLiteral("[融合] ") : QString()) +
                                (metadata.displayName.isEmpty() ? row.paramId : metadata.displayName);
    row.nameLabel->setText(displayName);
    row.nameLabel->setToolTip(row.paramId);
    row.widget->setProperty("searchText", (displayName + " " + row.paramId).toLower());
    const float value = m_lastValues.value(row.paramId, metadata.defaultValue);
    QSignalBlocker sliderBlocker(row.slider);
    QSignalBlocker spinBlocker(row.spinBox);
    row.spinBox->setRange(row.minVal, row.maxVal);
    row.spinBox->setDecimals(metadata.decimalPlaces);
    row.spinBox->setSingleStep((row.maxVal - row.minVal) > 0 ? (row.maxVal - row.minVal) / 100.0 : 1.0);
    row.spinBox->setValue(value);
    row.slider->setValue(row.maxVal > row.minVal
                             ? qBound(0, qRound((value - row.minVal) * 1000.0f / (row.maxVal - row.minVal)), 1000)
                             : 0);
    row.resetButton->setToolTip(QStringLiteral("恢复默认值 %1").arg(QString::number(row.defaultVal, 'f', 3)));
    const QString query = m_search->text().trimmed().toLower();
    row.widget->setVisible(query.isEmpty() || row.widget->property("searchText").toString().contains(query));
    RefreshXYControls();
}

void CParamPanel::ApplyValue(int rowIndex, float value, bool record)
{
    if (rowIndex < 0 || rowIndex >= m_rows.size()) return;
    ParamRow& row = m_rows[rowIndex];
    const float before = m_lastValues.value(row.paramId, value);
    m_lastValues.insert(row.paramId, value);
    emit ParamChanged(row.paramId, value, false);
    RefreshXYControls();
    if (record && !qFuzzyCompare(before + 1.0f, value + 1.0f)) {
        PushEdit(row.paramId, before, value);
    }
}

void CParamPanel::PushEdit(const QString& paramId, float before, float after)
{
    if (qFuzzyCompare(before + 1.0f, after + 1.0f)) return;
    if (m_document) {
        const live2dstruct::CModelEdit command{paramId, QStringLiteral("parameter"),
                                               live2dstruct::CModelEdit::ParameterValue, before, after};
        if (!m_document->ExecuteEdit(command)) return;
    }
    EmitHistoryState();
}

void CParamPanel::EmitHistoryState()
{
    if (!m_document) {
        emit HistoryChanged(false, false);
        return;
    }
    emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CParamPanel::SyncHistoryState()
{
    EmitHistoryState();
}

void CParamPanel::ClearHistory()
{
    if (m_document) m_document->ClearEditHistory();
    EmitHistoryState();
}

void CParamPanel::Undo()
{
    if (!m_document) return;
    live2dstruct::CModelEdit pending;
    if (!m_document->PeekUndoEdit(&pending) || pending.field != live2dstruct::CModelEdit::ParameterValue) return;
    live2dstruct::CModelEdit edit;
    if (!m_document->UndoEdit(&edit) || edit.field != live2dstruct::CModelEdit::ParameterValue) return;
    const float value = edit.before.toFloat();
    const auto rowIt = std::find_if(m_rows.cbegin(), m_rows.cend(), [&edit](const ParamRow& row){ return row.paramId == edit.name; });
    const int rowIndex = rowIt == m_rows.cend() ? -1 : static_cast<int>(rowIt - m_rows.cbegin());
    if (rowIndex >= 0 && rowIndex < m_rows.size()) {
        ParamRow& row = m_rows[rowIndex];
        const int sliderValue = row.maxVal > row.minVal ? qBound(0, qRound((value - row.minVal) * 1000.0f / (row.maxVal - row.minVal)), 1000) : 0;
        QSignalBlocker sliderBlocker(row.slider);
        QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(sliderValue);
        row.spinBox->setValue(value);
        m_lastValues.insert(edit.name, value);
        emit ParamChanged(edit.name, value, false);
    }
    EmitHistoryState();
}

void CParamPanel::Redo()
{
    if (!m_document) return;
    live2dstruct::CModelEdit pending;
    if (!m_document->PeekRedoEdit(&pending) || pending.field != live2dstruct::CModelEdit::ParameterValue) return;
    live2dstruct::CModelEdit edit;
    if (!m_document->RedoEdit(&edit) || edit.field != live2dstruct::CModelEdit::ParameterValue) return;
    const float value = edit.after.toFloat();
    const auto rowIt = std::find_if(m_rows.cbegin(), m_rows.cend(), [&edit](const ParamRow& row){ return row.paramId == edit.name; });
    const int rowIndex = rowIt == m_rows.cend() ? -1 : static_cast<int>(rowIt - m_rows.cbegin());
    if (rowIndex >= 0 && rowIndex < m_rows.size()) {
        ParamRow& row = m_rows[rowIndex];
        const int sliderValue = row.maxVal > row.minVal ? qBound(0, qRound((value - row.minVal) * 1000.0f / (row.maxVal - row.minVal)), 1000) : 0;
        QSignalBlocker sliderBlocker(row.slider);
        QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(sliderValue);
        row.spinBox->setValue(value);
        m_lastValues.insert(edit.name, value);
        emit ParamChanged(edit.name, value, false);
    }
    EmitHistoryState();
}
