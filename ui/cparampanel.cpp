#include "cparampanel.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "live2dstruct/cmodeldocument.h"
#include "ui/para/ccidjson.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
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
    buttonRow->addWidget(copyBtn, 0, 1);
    buttonRow->addWidget(pasteBtn, 1, 0);
    buttonRow->addWidget(savePresetBtn, 1, 1);
    buttonRow->addWidget(loadPresetBtn, 2, 0, 1, 2);
    mainLayout->addLayout(buttonRow);
    connect(copyBtn, &QPushButton::clicked, this, &CParamPanel::CopyValues);
    connect(pasteBtn, &QPushButton::clicked, this, &CParamPanel::PasteValues);
    connect(savePresetBtn, &QPushButton::clicked, this, &CParamPanel::SavePreset);
    connect(loadPresetBtn, &QPushButton::clicked, this, &CParamPanel::LoadPreset);

    m_search = new QLineEdit(wrapper);
    m_search->setPlaceholderText("搜索参数名称或 ID");
    m_search->setClearButtonEnabled(true);
    mainLayout->addWidget(m_search);

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

void CParamPanel::ApplyExternalValue(const QString& paramId, float value, bool record)
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
    m_userLocked.insert(rowIndex);
    m_lastValues.insert(paramId, value);
    if (m_document) m_document->SetParameterValue(paramId, value);
    emit ParamChanged(paramId, value, false);
    if (record && !qFuzzyCompare(before + 1.0f, value + 1.0f)) PushEdit(paramId, before, value);
}

void CParamPanel::BuildParams(CCidJson* cid, const QSet<QString>* blendShapeParams)
{
    m_rows.clear();
    m_lastValues.clear();
    ClearHistory();

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
        QString displayName = paramId;
        if(cid){
            QString mapped = cid->getParaName(paramId);
            if(!mapped.isEmpty()) displayName = mapped;
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
        pr.valueLabel = nullptr;
        pr.minVal = minVal;
        pr.maxVal = maxVal;
        pr.defaultVal = defVal;
        pr.paramId = paramId;
        pr.modelIndex = modelIdx;
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

void CParamPanel::ApplyValue(int rowIndex, float value, bool record)
{
    if (rowIndex < 0 || rowIndex >= m_rows.size()) return;
    ParamRow& row = m_rows[rowIndex];
    const float before = m_lastValues.value(row.paramId, value);
    m_lastValues.insert(row.paramId, value);
    emit ParamChanged(row.paramId, value, false);
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
