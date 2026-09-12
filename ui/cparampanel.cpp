#include "cparampanel.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "ui/para/ccidjson.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <iterator>

using namespace Live2D::Cubism::Framework;

CParamPanel::CParamPanel(QWidget *parent)
    : QDockWidget("参数控制", parent)
{
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    auto* buttonRow = new QHBoxLayout();
    QPushButton* resetBtn = new QPushButton("重置全部", wrapper);
    resetBtn->setObjectName("primaryBtn");
    connect(resetBtn, &QPushButton::clicked, this, &CParamPanel::ResetAll);
    buttonRow->addWidget(resetBtn);
    auto* copyBtn = new QPushButton(QStringLiteral("复制参数"), wrapper);
    auto* pasteBtn = new QPushButton(QStringLiteral("粘贴参数"), wrapper);
    buttonRow->addWidget(copyBtn);
    buttonRow->addWidget(pasteBtn);
    mainLayout->addLayout(buttonRow);
    connect(copyBtn, &QPushButton::clicked, this, &CParamPanel::CopyValues);
    connect(pasteBtn, &QPushButton::clicked, this, &CParamPanel::PasteValues);

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
        float initialVal = qBound(minVal, cubismModel->GetParameterValue(modelIdx), maxVal);

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

        QLabel* nameLabel = new QLabel(displayName, m_container);
        nameLabel->setFixedWidth(150);
        nameLabel->setToolTip(paramId);
        if(isBlendShape){
            nameLabel->setStyleSheet("background-color: #FFF3CD; padding: 2px; border-radius: 2px;");
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

        ParamRow pr;
        pr.widget = rowWidget;
        pr.slider = slider;
        pr.spinBox = spinBox;
        pr.valueLabel = nullptr;
        pr.minVal = minVal;
        pr.maxVal = maxVal;
        pr.defaultVal = defVal;
        pr.paramId = paramId;
        pr.modelIndex = modelIdx;
        m_rows.append(pr);
        m_lastValues.insert(paramId, initialVal);

        int idx = m_rows.size() - 1;
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
    m_replaying = true;
    for (int i = 0; i < m_rows.size(); ++i) {
        ParamRow& row = m_rows[i];
        if (!values.contains(row.paramId)) continue;
        const float value = qBound(row.minVal, values.value(row.paramId), row.maxVal);
        const int sliderValue = row.maxVal > row.minVal
            ? qBound(0, qRound((value - row.minVal) * 1000.0f / (row.maxVal - row.minVal)), 1000)
            : 0;
        QSignalBlocker sliderBlocker(row.slider);
        QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(sliderValue);
        row.spinBox->setValue(value);
        m_lastValues.insert(row.paramId, value);
        m_userLocked.insert(i);
        emit ParamChanged(row.paramId, value, false);
    }
    m_replaying = false;
    ClearHistory();
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
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        document.object().value(QStringLiteral("format")).toString() != QStringLiteral("live2d2psd-parameters-v1")) return;
    QMap<QString, float> values;
    const QJsonObject object = document.object();
    for (const QString& key : object.keys()) {
        if (key != QStringLiteral("format")) values.insert(key, static_cast<float>(object.value(key).toDouble()));
    }
    ApplyValues(values);
}

void CParamPanel::ApplyValue(int rowIndex, float value, bool record)
{
    if (rowIndex < 0 || rowIndex >= m_rows.size()) return;
    ParamRow& row = m_rows[rowIndex];
    const float before = m_lastValues.value(row.paramId, value);
    m_lastValues.insert(row.paramId, value);
    emit ParamChanged(row.paramId, value, false);
    if (record && !m_replaying && !qFuzzyCompare(before + 1.0f, value + 1.0f)) {
        PushEdit(row.paramId, before, value);
    }
}

void CParamPanel::PushEdit(const QString& paramId, float before, float after)
{
    if (qFuzzyCompare(before + 1.0f, after + 1.0f)) return;
    if (m_historyIndex < m_history.size()) m_history.resize(m_historyIndex);
    if (!m_history.isEmpty() && m_historyIndex == m_history.size()) {
        Edit& last = m_history.last();
        if (last.paramId == paramId && qFuzzyCompare(last.after + 1.0f, before + 1.0f)) {
            last.after = after;
            EmitHistoryState();
            return;
        }
    }
    m_history.push_back({paramId, before, after});
    m_historyIndex = m_history.size();
    EmitHistoryState();
}

void CParamPanel::EmitHistoryState()
{
    emit HistoryChanged(m_historyIndex > 0, m_historyIndex < m_history.size());
}

void CParamPanel::ClearHistory()
{
    m_history.clear();
    m_historyIndex = 0;
    EmitHistoryState();
}

void CParamPanel::Undo()
{
    if (m_historyIndex <= 0) return;
    const Edit edit = m_history[m_historyIndex - 1];
    m_replaying = true;
    const auto rowIt = std::find_if(m_rows.cbegin(), m_rows.cend(), [&edit](const ParamRow& row){ return row.paramId == edit.paramId; });
    const int rowIndex = rowIt == m_rows.cend() ? -1 : static_cast<int>(rowIt - m_rows.cbegin());
    if (rowIndex >= 0 && rowIndex < m_rows.size()) {
        ParamRow& row = m_rows[rowIndex];
        const int sliderValue = row.maxVal > row.minVal ? qBound(0, qRound((edit.before - row.minVal) * 1000.0f / (row.maxVal - row.minVal)), 1000) : 0;
        QSignalBlocker sliderBlocker(row.slider);
        QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(sliderValue);
        row.spinBox->setValue(edit.before);
        m_lastValues.insert(edit.paramId, edit.before);
        emit ParamChanged(edit.paramId, edit.before, false);
    }
    m_replaying = false;
    --m_historyIndex;
    EmitHistoryState();
}

void CParamPanel::Redo()
{
    if (m_historyIndex >= m_history.size()) return;
    const Edit edit = m_history[m_historyIndex];
    m_replaying = true;
    const auto rowIt = std::find_if(m_rows.cbegin(), m_rows.cend(), [&edit](const ParamRow& row){ return row.paramId == edit.paramId; });
    const int rowIndex = rowIt == m_rows.cend() ? -1 : static_cast<int>(rowIt - m_rows.cbegin());
    if (rowIndex >= 0 && rowIndex < m_rows.size()) {
        ParamRow& row = m_rows[rowIndex];
        const int sliderValue = row.maxVal > row.minVal ? qBound(0, qRound((edit.after - row.minVal) * 1000.0f / (row.maxVal - row.minVal)), 1000) : 0;
        QSignalBlocker sliderBlocker(row.slider);
        QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(sliderValue);
        row.spinBox->setValue(edit.after);
        m_lastValues.insert(edit.paramId, edit.after);
        emit ParamChanged(edit.paramId, edit.after, false);
    }
    m_replaying = false;
    ++m_historyIndex;
    EmitHistoryState();
}
