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
#include <QScopedValueRollback>
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
#include <QPointer>
#include <QScopeGuard>
#include <algorithm>
#include <iterator>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

using namespace Live2D::Cubism::Framework;

namespace {
class CParameterMetadataDialog final : public QDialog
{
public:
    using QDialog::QDialog;
    void SetValidator(std::function<bool()> validator) { m_validator = std::move(validator); }
    void done(int result) override
    {
        if (m_validating && result == Accepted) return;
        if (result == Accepted && m_validator) {
            const QPointer<CParameterMetadataDialog> dialog(this);
            const auto validate = m_validator;
            m_validating = true;
            const auto finish = qScopeGuard([dialog] { if (dialog) dialog->m_validating = false; });
            if (!validate() || !dialog) return;
        }
        QDialog::done(result);
    }
private:
    std::function<bool()> m_validator;
    bool m_validating = false;
};

class CParameterSpinBox final : public QDoubleSpinBox
{
public:
    using QDoubleSpinBox::QDoubleSpinBox;

    struct InputState {
        QString text;
        int cursor = 0;
        int selectionStart = -1;
        int selectionLength = 0;
    };

    bool DisplaysValue(double value) const
    {
        return textFromValue(value) == textFromValue(this->value());
    }

    InputState CaptureInput() const
    {
        return {lineEdit()->text(), lineEdit()->cursorPosition(), lineEdit()->selectionStart(),
                static_cast<int>(lineEdit()->selectedText().size())};
    }

    void RestoreInput(const InputState& input)
    {
        if (lineEdit()->text() == input.text) return;
        const QSignalBlocker blocker(lineEdit());
        lineEdit()->setText(input.text);
        if (input.selectionStart >= 0) {
            const bool backwards = input.cursor == input.selectionStart;
            lineEdit()->setSelection(backwards ? input.selectionStart + input.selectionLength : input.selectionStart,
                                     backwards ? -input.selectionLength : input.selectionLength);
        } else lineEdit()->setCursorPosition(input.cursor);
    }
};

bool validParameterRange(float minimum, float maximum)
{
    return std::isfinite(minimum) && std::isfinite(maximum) && minimum <= maximum;
}

float boundedParameterValue(double value, float minimum, float maximum, float fallback)
{
    if (!validParameterRange(minimum, maximum)) return 0;
    if (!std::isfinite(value)) value = std::isfinite(fallback) ? fallback : minimum;
    return static_cast<float>(std::clamp(value, static_cast<double>(minimum), static_cast<double>(maximum)));
}

int parameterToSlider(float value, float minimum, float maximum)
{
    if (!validParameterRange(minimum, maximum) || minimum == maximum) return 0;
    const double bounded = boundedParameterValue(value, minimum, maximum, minimum);
    const double range = static_cast<double>(maximum) - minimum;
    return qBound(0, qRound((bounded - minimum) * 1000.0 / range), 1000);
}

float sliderToParameter(int position, float minimum, float maximum)
{
    if (!validParameterRange(minimum, maximum)) return 0;
    const double ratio = qBound(0, position, 1000) / 1000.0;
    return boundedParameterValue(static_cast<double>(minimum) +
                                 (static_cast<double>(maximum) - minimum) * ratio,
                                 minimum, maximum, minimum);
}

bool metadataFloat(const QString& text, float original, float& value)
{
    const QString input = text.trimmed();
    if (input == QString::number(original, 'g', 9)) {
        value = original;
        return true;
    }
    std::istringstream stream(input.toStdString());
    stream.imbue(std::locale::classic());
    float parsed = 0;
    stream >> std::noskipws >> parsed;
    if (!stream.eof() || stream.bad() || (stream.fail() && std::fpclassify(parsed) != FP_SUBNORMAL) ||
        !std::isfinite(parsed)) return false;
    if (parsed == 0) {
        const int exponent = std::max(input.indexOf(QLatin1Char('e')), input.indexOf(QLatin1Char('E')));
        const QString mantissa = exponent < 0 ? input : input.left(exponent);
        if (std::any_of(mantissa.cbegin(), mantissa.cend(), [](QChar digit) { return digit >= QLatin1Char('1') && digit <= QLatin1Char('9'); }))
            return false;
    }
    value = parsed;
    return true;
}

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
        if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
            std::abs(value.toDouble()) > std::numeric_limits<float>::max()) {
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
    if (m_document != document) {
        m_previewBaseline.clear();
        m_previewActive = false;
        m_userDragging.clear();
        m_changedDrags.clear();
        m_dragStarts.clear();
        m_rejectedSpinEdits.clear();
    }
    m_document = document;
}

void CParamPanel::SetPreEditGuard(PreEditGuard guard)
{
    if (!m_checkingEditGuard) m_preEditGuard = std::move(guard);
}

void CParamPanel::SetMetadataEditHandler(MetadataEditHandler handler)
{
    if (!m_submittingEdit) m_metadataEditHandler = std::move(handler);
}

void CParamPanel::SetHistoryHandler(HistoryHandler handler)
{
    if (!m_submittingEdit) m_historyHandler = std::move(handler);
}

void CParamPanel::RestoreControls()
{
    for (const auto& row : m_rows) {
        const float value = m_lastValues.value(row.paramId, row.defaultVal);
        const QSignalBlocker sliderBlocker(row.slider);
        const QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(parameterToSlider(value, row.minVal, row.maxVal));
        row.spinBox->setValue(value);
    }
    RefreshXYControls();
}

bool CParamPanel::AllowUserEdit()
{
    if (m_checkingEditGuard || m_submittingEdit) {
        RestoreControls();
        return false;
    }
    if (m_editGuardApproved || !m_preEditGuard) return true;
    const QPointer<CParamPanel> panel(this);
    const auto guard = m_preEditGuard;
    m_checkingEditGuard = true;
    const auto checking = qScopeGuard([panel] { if (panel) panel->m_checkingEditGuard = false; });
    QString error;
    bool accepted = false;
    try {
        accepted = guard(&error);
    } catch (...) {
        error = QStringLiteral("参数编辑前检查失败");
    }
    if (!panel) return false;
    if (accepted) return true;
    RestoreControls();
    emit EditRejected(error.isEmpty() ? QStringLiteral("参数编辑已取消") : error);
    return false;
}

float CParamPanel::ValueBeforeEdit(const QString& paramId, float fallback) const
{
    return m_previewBaseline.value(paramId, m_lastValues.value(paramId, fallback));
}

void CParamPanel::SetPreviewValues(const QMap<QString, float>& values,
                                  const QMap<QString, float>& baselineValues)
{
    if (!m_previewActive) {
        for (const ParamRow& row : m_rows) {
            const float value = baselineValues.value(row.paramId, m_lastValues.value(row.paramId, row.defaultVal));
            if (std::isfinite(value)) m_previewBaseline.insert(row.paramId, boundedParameterValue(value, row.minVal, row.maxVal, row.defaultVal));
        }
        m_previewActive = true;
    }
    const QSignalBlocker blocker(this);
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        ApplyExternalValue(it.key(), it.value(), false, false, false);
}

void CParamPanel::ClearPreviewValues(const QMap<QString, float>& restoredValues,
                                    const QString& preservedParameterId)
{
    const auto values = restoredValues.isEmpty() ? m_previewBaseline : restoredValues;
    m_previewBaseline.clear();
    m_previewActive = false;
    const QSignalBlocker blocker(this);
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key() != preservedParameterId) ApplyExternalValue(it.key(), it.value(), false, false, false);
    }
}

void CParamPanel::ApplyExternalValue(const QString& paramId, float value, bool record, bool userEdit, bool writeDocument)
{
    if (!std::isfinite(value)) return;
    if (userEdit && !AllowUserEdit()) return;
    auto rowIt = std::find_if(m_rows.cbegin(), m_rows.cend(), [&paramId](const ParamRow& row){ return row.paramId == paramId; });
    if (rowIt == m_rows.cend()) return;
    const int rowIndex = static_cast<int>(rowIt - m_rows.cbegin());
    ParamRow& row = m_rows[rowIndex];
    value = boundedParameterValue(value, row.minVal, row.maxVal, row.defaultVal);
    const float before = ValueBeforeEdit(paramId, row.defaultVal);
    if (userEdit || writeDocument) m_previewBaseline.remove(paramId);
    const int sliderValue = parameterToSlider(value, row.minVal, row.maxVal);
    QSignalBlocker sliderBlocker(row.slider);
    QSignalBlocker spinBlocker(row.spinBox);
    row.slider->setValue(sliderValue);
    row.spinBox->setValue(value);
    if (userEdit) m_userLocked.insert(rowIndex);
    m_lastValues.insert(paramId, value);
    if (m_document && writeDocument) m_document->SetParameterValue(paramId, value);
    emit ParamChanged(paramId, value, !userEdit);
    RefreshXYControls();
    if (record && before != value) PushEdit(paramId, before, value);
}

void CParamPanel::BuildParams(CCidJson* cid, const QSet<QString>* blendShapeParams)
{
    m_groupButtons.clear();
    m_rows.clear();
    m_lastValues.clear();
    m_previewBaseline.clear();
    m_previewActive = false;
    ClearHistory();
    m_userDragging.clear();
    m_changedDrags.clear();
    m_dragStarts.clear();
    m_userLocked.clear();
    m_rejectedSpinEdits.clear();
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

    struct ParameterSource {
        QString id;
        int modelIndex = -1;
        live2dstruct::CModelParameterMetadata metadata;
        float value = 0;
        bool hasMetadata = false;
        bool blendShape = false;
    };
    QVector<ParameterSource> parameters;
    const bool documentReady = m_document && m_document->IsLoaded();
    if (documentReady) {
        parameters.reserve(m_document->Parameters().size());
        for (int index = 0; index < m_document->Parameters().size(); ++index) {
            const auto& parameter = m_document->Parameters()[index];
            const auto metadata = m_document->ParameterMetadata(parameter.id);
            const float value = m_document->ParameterValue(parameter.id);
            if (parameter.id.isEmpty() || !std::isfinite(metadata.minimum) || !std::isfinite(metadata.maximum) ||
                !std::isfinite(metadata.defaultValue) || !std::isfinite(value) || metadata.minimum > metadata.maximum ||
                metadata.defaultValue < metadata.minimum || metadata.defaultValue > metadata.maximum ||
                value < metadata.minimum || value > metadata.maximum ||
                metadata.decimalPlaces < 0 || metadata.decimalPlaces > 9) continue;
            parameters.append({parameter.id, index, metadata, value,
                               m_document->ParameterMetadataOverrides().contains(parameter.id), parameter.type == 1});
        }
    } else {
        LAppLive2DManager* manager = LAppLive2DManager::GetInstance();
        if (!manager || manager->GetModelNum() == 0) return;
        LAppModel* model = manager->GetModel(0);
        CubismModel* runtime = model ? model->GetModel() : nullptr;
        if (!runtime) return;
        for (int index = 0; index < runtime->GetParameterCount(); ++index) {
            ParameterSource parameter;
            parameter.id = QString::fromUtf8(runtime->GetParameterId(index)->GetString().GetRawString());
            parameter.modelIndex = index;
            parameter.blendShape = runtime->GetParameterType(index) == Live2D::Cubism::Core::csmParameterType_BlendShape;
            parameter.metadata.minimum = runtime->GetParameterMinimumValue(index);
            parameter.metadata.maximum = runtime->GetParameterMaximumValue(index);
            parameter.metadata.defaultValue = runtime->GetParameterDefaultValue(index);
            parameter.metadata.decimalPlaces = 3;
            if (!std::isfinite(parameter.metadata.minimum) || !std::isfinite(parameter.metadata.maximum) ||
                parameter.metadata.minimum > parameter.metadata.maximum) {
                parameter.metadata.minimum = 0;
                parameter.metadata.maximum = 1;
            }
            if (!std::isfinite(parameter.metadata.defaultValue)) parameter.metadata.defaultValue = parameter.metadata.minimum;
            parameter.metadata.defaultValue = std::clamp(parameter.metadata.defaultValue, parameter.metadata.minimum,
                                                         parameter.metadata.maximum);
            const float current = runtime->GetParameterValue(index);
            parameter.value = std::isfinite(current) ? std::clamp(current, parameter.metadata.minimum, parameter.metadata.maximum)
                                                     : parameter.metadata.defaultValue;
            parameters.append(parameter);
        }
    }
    QMap<QString, int> parameterRows;
    for (int index = 0; index < parameters.size(); ++index) parameterRows.insert(parameters[index].id, index);

    auto addParamRow = [&](QVBoxLayout* targetLayout, const QString& paramId, int sourceIndex) {
        const ParameterSource& parameter = parameters[sourceIndex];
        const auto& metadata = parameter.metadata;
        const bool hasMetadata = parameter.hasMetadata;
        const float minVal = metadata.minimum;
        const float maxVal = metadata.maximum;
        const float defVal = metadata.defaultValue;
        const float initialVal = parameter.value;
        const double range = static_cast<double>(maxVal) - minVal;

        QWidget* rowWidget = new QWidget(m_container);
        QHBoxLayout* row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 2, 0, 2);

        // 显示名称：优先用 cid 映射的名称，否则回退到内部 ID
        QString originalName = paramId;
        if(cid){
            QString mapped = cid->getParaName(paramId);
            if(!mapped.isEmpty()) originalName = mapped;
        }
        QString displayName = hasMetadata && !metadata.displayName.isEmpty() && metadata.displayName != paramId
            ? metadata.displayName : originalName;

        // 融合变形参数标记
        const bool isBlendShape = parameter.blendShape || (blendShapeParams && blendShapeParams->contains(paramId));
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
        nameLabel->setTextFormat(Qt::PlainText);
        nameLabel->setFixedWidth(150);
        nameLabel->setToolTip(tooltip);
        if(isBlendShape){
            nameLabel->setObjectName(QStringLiteral("blendParamTag"));
        }
        row->addWidget(nameLabel);

        QSlider* slider = new QSlider(Qt::Horizontal, rowWidget);
        slider->setRange(0, 1000);
        slider->setValue(parameterToSlider(initialVal, minVal, maxVal));
        row->addWidget(slider);

        auto* spinBox = new CParameterSpinBox(rowWidget);
        spinBox->setDecimals(metadata.decimalPlaces);
        spinBox->setRange(minVal, maxVal);
        spinBox->setSingleStep(range > 0 ? range / 100.0 : 1.0);
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
        pr.originalName = originalName;
        pr.modelIndex = parameter.modelIndex;
        pr.blendShape = isBlendShape;
        m_rows.append(pr);
        m_lastValues.insert(paramId, initialVal);

        int idx = m_rows.size() - 1;
        connect(resetButton, &QToolButton::clicked, this, [this, idx](){
            if (idx < 0 || idx >= m_rows.size()) return;
            if (ApplyValue(idx, m_rows[idx].defaultVal, true)) m_userLocked.insert(idx);
        });
        connect(slider, &QSlider::sliderPressed, this, [this, idx](){ BeginParameterDrag(idx); });
        connect(slider, &QSlider::sliderReleased, this, [this, idx](){ EndParameterDrag(idx); });
        connect(slider, &QSlider::valueChanged, this, [this, idx](int val){
            ParamRow& r = m_rows[idx];
            const float realVal = sliderToParameter(val, r.minVal, r.maxVal);
            ApplyValue(idx, realVal, !m_userDragging.contains(idx));
        });

        connect(spinBox, &QDoubleSpinBox::valueChanged, this, [this, idx, spinBox](double value){
            ParamRow& r = m_rows[idx];
            if (spinBox->DisplaysValue(m_lastValues.value(r.paramId, r.defaultVal))) return;
            if (ApplyValue(idx, boundedParameterValue(value, r.minVal, r.maxVal, r.defaultVal), true, true))
                m_rejectedSpinEdits.remove(idx);
            else m_rejectedSpinEdits.insert(idx);
        });
        connect(spinBox, &QDoubleSpinBox::editingFinished, this, [this, idx](){
            if (!m_rejectedSpinEdits.contains(idx)) m_userLocked.insert(idx);
        });

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
            m_groupButtons.insert(groupBody, groupButton);
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
                if (!parameterRows.contains(paramId) || addedParams.contains(paramId)) continue;
                addParamRow(groupLayout, paramId, parameterRows[paramId]);
                addedParams.insert(paramId);
            }
        }

        // 未分组的参数放到最后
        for (int index = 0; index < parameters.size(); ++index) {
            const QString& paramId = parameters[index].id;
            if (!addedParams.contains(paramId)) addParamRow(layout, paramId, index);
        }
    } else {
        // 无分组信息，全部平铺
        for (int index = 0; index < parameters.size(); ++index)
            addParamRow(layout, parameters[index].id, index);
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
    emit ParameterMetadataChanged();
}

bool CParamPanel::FocusParameter(const QString& id)
{
    const auto found = std::find_if(m_rows.cbegin(), m_rows.cend(), [&](const auto& row) { return row.paramId == id; });
    if (found == m_rows.cend()) return false;
    const QPointer<CParamPanel> panel = this;
    const QPointer<QWidget> widget = found->widget;
    const QPointer<QDoubleSpinBox> spin = found->spinBox;
    m_search->clear();
    if (!panel || !widget || !spin) return false;
    for (auto* parent = widget->parentWidget(); parent && parent != m_container; parent = parent->parentWidget()) {
        if (auto* button = m_groupButtons.value(parent, nullptr)) button->setChecked(true);
        if (!panel || !widget || !spin) return false;
    }
    if (m_container->layout()) m_container->layout()->activate();
    m_scroll->ensureWidgetVisible(widget);
    spin->setFocus(Qt::OtherFocusReason);
    if (!panel || !widget || !spin) return false;
    spin->selectAll();
    return true;
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
    if (m_updatingXY || !m_xyXSelector || !m_xyYSelector || !std::isfinite(xValue) || !std::isfinite(yValue)) return;
    if (!AllowUserEdit()) return;
    const QScopedValueRollback<bool> approved(m_editGuardApproved, true);
    const auto findRow = [this](QComboBox* selector) {
        return std::find_if(m_rows.cbegin(), m_rows.cend(), [selector](const ParamRow& row){
            return row.paramId == selector->currentText();
        });
    };
    const auto xRow = findRow(m_xyXSelector);
    const auto yRow = findRow(m_xyYSelector);
    if (xRow == m_rows.cend() || yRow == m_rows.cend()) return;
    xValue = boundedParameterValue(xValue, xRow->minVal, xRow->maxVal, xRow->defaultVal);
    yValue = boundedParameterValue(yValue, yRow->minVal, yRow->maxVal, yRow->defaultVal);
    const QMap<QString, float> before{{xRow->paramId, ValueBeforeEdit(xRow->paramId, xRow->defaultVal)},
                                      {yRow->paramId, ValueBeforeEdit(yRow->paramId, yRow->defaultVal)}};
    const QMap<QString, float> after{{xRow->paramId, xValue}, {yRow->paramId, yValue}};
    if (!m_document || (before == after && !m_previewActive)) return;
    const live2dstruct::CModelEdit command{
        QStringLiteral("xy"), QStringLiteral("parameters"), live2dstruct::CModelEdit::ParameterValues,
        QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
    if (before != after && !m_document->ExecuteEdit(command)) return;
    m_updatingXY = true;
    ApplyExternalValue(xRow->paramId, xValue, false);
    ApplyExternalValue(yRow->paramId, yValue, false);
    m_updatingXY = false;
    if (before != after) emit HistoryChanged(m_document->CanUndoEdit(), m_document->CanRedoEdit());
}

void CParamPanel::ResetAll()
{
    if (!AllowUserEdit()) return;
    const QScopedValueRollback<bool> approved(m_editGuardApproved, true);
    m_userLocked.clear();
    m_userDragging.clear();
    m_changedDrags.clear();
    m_dragStarts.clear();
    for(auto& r : m_rows){
        const float before = ValueBeforeEdit(r.paramId, r.defaultVal);
        const float defaultValue = boundedParameterValue(r.defaultVal, r.minVal, r.maxVal, r.minVal);
        const int sliderDefault = parameterToSlider(defaultValue, r.minVal, r.maxVal);
        r.slider->blockSignals(true);
        r.slider->setValue(sliderDefault);
        r.slider->blockSignals(false);
        r.spinBox->blockSignals(true);
        r.spinBox->setValue(defaultValue);
        r.spinBox->blockSignals(false);
        ApplyValue(&r - m_rows.data(), defaultValue, false);
        PushEdit(r.paramId, before, defaultValue);
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
        const float rawValue = cubismModel->GetParameterValue(r.modelIndex);
        if (!std::isfinite(rawValue)) continue;
        const float curVal = boundedParameterValue(rawValue, r.minVal, r.maxVal, r.defaultVal);
        const int sliderVal = parameterToSlider(curVal, r.minVal, r.maxVal);
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

bool CParamPanel::CurrentValue(const QString& id, float& value) const
{
    const auto found = m_lastValues.constFind(id);
    if (found == m_lastValues.cend()) return false;
    value = found.value();
    return true;
}

void CParamPanel::ApplyValues(const QMap<QString, float>& values)
{
    ApplyValuesInternal(values, false);
}

void CParamPanel::ApplyValuesInternal(const QMap<QString, float>& values, bool record)
{
    if (!AllowUserEdit()) return;
    const QScopedValueRollback<bool> approved(m_editGuardApproved, true);
    for (int i = 0; i < m_rows.size(); ++i) {
        ParamRow& row = m_rows[i];
        if (!values.contains(row.paramId)) continue;
        const float requested = values.value(row.paramId);
        if (!std::isfinite(requested)) continue;
        const float value = boundedParameterValue(requested, row.minVal, row.maxVal, row.defaultVal);
        const float before = ValueBeforeEdit(row.paramId, row.defaultVal);
        m_previewBaseline.remove(row.paramId);
        const int sliderValue = parameterToSlider(value, row.minVal, row.maxVal);
        QSignalBlocker sliderBlocker(row.slider);
        QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(sliderValue);
        row.spinBox->setValue(value);
        m_lastValues.insert(row.paramId, value);
        m_userLocked.insert(i);
        if (m_document) m_document->SetParameterValue(row.paramId, value);
        emit ParamChanged(row.paramId, value, false);
        if (record && before != value) PushEdit(row.paramId, before, value);
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
    if (!m_document || m_rows.isEmpty() || m_metadataDialogOpen || m_submittingEdit) return;
    const QPointer<CParamPanel> panel(this);
    m_metadataDialogOpen = true;
    const auto closeEditing = qScopeGuard([panel] { if (panel) panel->m_metadataDialogOpen = false; });
    auto* const document = m_document;
    const QByteArray digest = document->SourceDigest();
    const quint64 revision = document->Revision();
    const auto currentDocument = [panel, document, digest, revision] {
        return panel && panel->m_document == document && document->SourceDigest() == digest && document->Revision() == revision;
    };
    const auto rejectStale = [panel] {
        if (panel) emit panel->EditRejected(QStringLiteral("编辑期间模型数据已改变，请重新打开参数设置"));
    };
    QStringList ids;
    for (const ParamRow& row : m_rows) ids.append(row.paramId);
    QPointer<QInputDialog> picker = new QInputDialog(this);
    const auto closePicker = qScopeGuard([picker] { if (picker) delete picker.data(); });
    picker->setObjectName(QStringLiteral("metadataParameterPicker"));
    picker->setWindowTitle(QStringLiteral("选择参数"));
    picker->setLabelText(QStringLiteral("参数 ID"));
    picker->setComboBoxItems(ids);
    picker->setComboBoxEditable(false);
    picker->setTextValue(ids.front());
    const int picked = picker->exec();
    if (!panel || !picker) return;
    if (picked != QDialog::Accepted) return;
    if (!currentDocument()) { rejectStale(); return; }
    const QString id = picker->textValue();
    delete picker.data();
    if (!ids.contains(id)) return;
    const auto current = document->ParameterMetadata(id);
    const auto sourceRow = std::find_if(m_rows.cbegin(), m_rows.cend(), [&id](const auto& row) { return row.paramId == id; });
    const QString originalName = sourceRow == m_rows.cend() || sourceRow->originalName.isEmpty() ? id : sourceRow->originalName;
    const QString initialName = current.displayName.isEmpty() || current.displayName == id ? originalName : current.displayName;

    QPointer<CParameterMetadataDialog> dialog = new CParameterMetadataDialog(this);
    const auto closeDialog = qScopeGuard([dialog] { if (dialog) delete dialog.data(); });
    dialog->setObjectName(QStringLiteral("parameterMetadataDialog"));
    dialog->setWindowTitle(QStringLiteral("编辑参数：%1").arg(id));
    auto* form = new QFormLayout(dialog);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setSizeConstraint(QLayout::SetMinimumSize);
    auto* nameEdit = new QLineEdit(initialName, dialog);
    nameEdit->setObjectName(QStringLiteral("metadataName"));
    const auto makeNumber = [&](const QString& name, float value) {
        auto* edit = new QLineEdit(QString::number(value, 'g', 9), dialog);
        edit->setObjectName(name);
        return edit;
    };
    auto* minEdit = makeNumber(QStringLiteral("metadataMinimum"), current.minimum);
    auto* maxEdit = makeNumber(QStringLiteral("metadataMaximum"), current.maximum);
    auto* defaultEdit = makeNumber(QStringLiteral("metadataDefault"), current.defaultValue);
    auto* decimalsSpin = new QSpinBox(dialog);
    decimalsSpin->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    decimalsSpin->setObjectName(QStringLiteral("metadataDecimals"));
    decimalsSpin->setRange(0, 9);
    decimalsSpin->setValue(current.decimalPlaces);
    auto* repeatCheck = new QCheckBox(dialog);
    repeatCheck->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    repeatCheck->setObjectName(QStringLiteral("metadataRepeat"));
    repeatCheck->setChecked(current.repeat);
    form->addRow(QStringLiteral("显示名"), nameEdit);
    form->addRow(QStringLiteral("最小值"), minEdit);
    form->addRow(QStringLiteral("最大值"), maxEdit);
    form->addRow(QStringLiteral("默认值"), defaultEdit);
    form->addRow(QStringLiteral("小数位"), decimalsSpin);
    form->addRow(QStringLiteral("循环参数"), repeatCheck);
    auto* inlineError = new QLabel(dialog);
    inlineError->setObjectName(QStringLiteral("metadataError"));
    inlineError->setTextFormat(Qt::PlainText);
    inlineError->setWordWrap(true);
    inlineError->hide();
    form->addRow(inlineError);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Save)->setDefault(true);
    form->addRow(buttons);
    auto updated = current;
    const auto validateNumbers = [&]() {
        const auto invalid = [&](QLineEdit* field, const QString& reason) {
            inlineError->setText(QStringLiteral("无法保存：%1").arg(reason));
            inlineError->show();
            field->setFocus(Qt::OtherFocusReason);
            field->selectAll();
            emit panel->EditRejected(reason);
            return false;
        };
        updated = current;
        if (!metadataFloat(minEdit->text(), current.minimum, updated.minimum))
            return invalid(minEdit, QStringLiteral("请输入有效的最小值，支持科学计数法"));
        if (!metadataFloat(maxEdit->text(), current.maximum, updated.maximum))
            return invalid(maxEdit, QStringLiteral("请输入有效的最大值，支持科学计数法"));
        if (!metadataFloat(defaultEdit->text(), current.defaultValue, updated.defaultValue))
            return invalid(defaultEdit, QStringLiteral("请输入有效的默认值，支持科学计数法"));
        if (updated.minimum > updated.maximum)
            return invalid(minEdit, QStringLiteral("最小值不能大于最大值"));
        if (updated.defaultValue < updated.minimum || updated.defaultValue > updated.maximum)
            return invalid(defaultEdit, QStringLiteral("默认值必须位于最小值和最大值之间"));
        inlineError->clear();
        inlineError->hide();
        return true;
    };
    dialog->SetValidator([&] {
        if (!currentDocument()) {
            rejectStale();
            if (dialog) dialog->reject();
            return false;
        }
        return validateNumbers();
    });
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const int accepted = dialog->exec();
    if (!panel || !dialog) return;
    if (accepted != QDialog::Accepted) return;
    if (!currentDocument()) { rejectStale(); return; }
    if (!validateNumbers()) return;
    if (nameEdit->text() != initialName)
        updated.displayName = nameEdit->text().trimmed().isEmpty() ? id : nameEdit->text().trimmed();
    updated.decimalPlaces = decimalsSpin->value();
    updated.repeat = repeatCheck->isChecked();
    if (updated == current) return;
    QString error;
    live2dstruct::CModelEdit edit;
    if (!document->MakeParameterMetadataEdit(id, updated, edit, &error)) {
        emit EditRejected(error.isEmpty() ? QStringLiteral("参数元数据无效") : error);
        return;
    }
    if (edit.before == edit.after) return;
    const auto handler = m_metadataEditHandler;
    if (!handler) {
        if (!AllowUserEdit() || !panel) return;
        if (!currentDocument()) { rejectStale(); return; }
    }
    m_submittingEdit = true;
    const auto finishSubmission = qScopeGuard([panel] { if (panel) panel->m_submittingEdit = false; });
    bool submitted = false;
    try {
        submitted = handler ? handler(edit, &error) : document->ExecuteEdit(edit);
    } catch (...) {
        error = QStringLiteral("参数元数据提交失败");
    }
    if (!panel) return;
    if (m_document != document || document->SourceDigest() != digest) { rejectStale(); return; }
    if (!submitted) {
        RestoreControls();
        emit EditRejected(error.isEmpty() ? QStringLiteral("参数元数据修改未通过校验") : error);
        return;
    }
    RefreshAcceptedMetadata(edit, true);
    if (panel) EmitHistoryState();
}

void CParamPanel::RefreshParameterRow(int rowIndex)
{
    if (!m_document || rowIndex < 0 || rowIndex >= m_rows.size()) return;
    ParamRow& row = m_rows[rowIndex];
    const auto metadata = m_document->ParameterMetadata(row.paramId);
    if (!validParameterRange(metadata.minimum, metadata.maximum) || !std::isfinite(metadata.defaultValue) ||
        metadata.defaultValue < metadata.minimum || metadata.defaultValue > metadata.maximum ||
        metadata.decimalPlaces < 0 || metadata.decimalPlaces > 9) return;
    row.minVal = metadata.minimum;
    row.maxVal = metadata.maximum;
    row.defaultVal = metadata.defaultValue;
    const QString originalName = row.originalName.isEmpty() ? row.paramId : row.originalName;
    const QString name = m_document->ParameterMetadataOverrides().contains(row.paramId) && !metadata.displayName.isEmpty() && metadata.displayName != row.paramId
        ? metadata.displayName : originalName;
    const QString displayName = (row.blendShape ? QStringLiteral("[融合] ") : QString()) + name;
    row.nameLabel->setTextFormat(Qt::PlainText);
    row.nameLabel->setText(displayName);
    row.nameLabel->setToolTip(row.paramId);
    row.widget->setProperty("searchText", (displayName + " " + row.paramId).toLower());
    const float value = boundedParameterValue(m_lastValues.value(row.paramId, metadata.defaultValue),
                                               row.minVal, row.maxVal, row.defaultVal);
    if (m_previewBaseline.contains(row.paramId))
        m_previewBaseline[row.paramId] = boundedParameterValue(m_previewBaseline.value(row.paramId),
                                                              row.minVal, row.maxVal, row.defaultVal);
    m_lastValues.insert(row.paramId, value);
    QSignalBlocker sliderBlocker(row.slider);
    QSignalBlocker spinBlocker(row.spinBox);
    row.spinBox->setDecimals(metadata.decimalPlaces);
    row.spinBox->setRange(row.minVal, row.maxVal);
    const double range = static_cast<double>(row.maxVal) - row.minVal;
    row.spinBox->setSingleStep(range > 0 ? range / 100.0 : 1.0);
    row.spinBox->setValue(value);
    row.slider->setValue(parameterToSlider(value, row.minVal, row.maxVal));
    row.resetButton->setToolTip(QStringLiteral("恢复默认值 %1").arg(QString::number(row.defaultVal, 'f', 3)));
    const QString query = m_search->text().trimmed().toLower();
    row.widget->setVisible(query.isEmpty() || row.widget->property("searchText").toString().contains(query));
    RefreshXYControls();
}

void CParamPanel::RefreshAcceptedMetadata(const live2dstruct::CModelEdit& edit, bool forward)
{
    if (!m_document) return;
    const auto row = std::find_if(m_rows.cbegin(), m_rows.cend(), [&edit](const auto& item) { return item.paramId == edit.name; });
    if (row == m_rows.cend()) return;
    const auto before = edit.before.toMap();
    const auto after = edit.after.toMap();
    if (before.value(QStringLiteral("value")) != after.value(QStringLiteral("value"))) {
        const float value = (forward ? after : before).value(QStringLiteral("value")).toFloat();
        if (m_previewActive) {
            if (m_previewBaseline.contains(edit.name)) m_previewBaseline[edit.name] = value;
        } else m_lastValues[edit.name] = value;
    }
    RefreshParameterRow(static_cast<int>(row - m_rows.cbegin()));
    emit ParameterMetadataChanged();
}

void CParamPanel::BeginParameterDrag(int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= m_rows.size()) return;
    const ParamRow& row = m_rows[rowIndex];
    m_userDragging.insert(rowIndex);
    m_changedDrags.remove(rowIndex);
    m_dragStarts.insert(rowIndex, ValueBeforeEdit(row.paramId, row.defaultVal));
}

void CParamPanel::EndParameterDrag(int rowIndex)
{
    m_userDragging.remove(rowIndex);
    const bool changed = m_changedDrags.remove(rowIndex);
    if (!m_dragStarts.contains(rowIndex)) return;
    const float before = m_dragStarts.take(rowIndex);
    if (!changed || rowIndex < 0 || rowIndex >= m_rows.size()) return;
    m_userLocked.insert(rowIndex);
    const ParamRow& row = m_rows[rowIndex];
    PushEdit(row.paramId, before, m_lastValues.value(row.paramId, row.defaultVal));
}

bool CParamPanel::ApplyValue(int rowIndex, float value, bool record, bool preserveSpinText)
{
    if (rowIndex < 0 || rowIndex >= m_rows.size() || !std::isfinite(value)) return false;
    if (m_userDragging.contains(rowIndex) && value == m_lastValues.value(m_rows[rowIndex].paramId, m_rows[rowIndex].defaultVal)) return true;
    const QPointer<CParameterSpinBox> inputBox = preserveSpinText
        ? dynamic_cast<CParameterSpinBox*>(m_rows[rowIndex].spinBox) : nullptr;
    const auto input = inputBox ? inputBox->CaptureInput() : CParameterSpinBox::InputState{};
    if (!AllowUserEdit() || rowIndex >= m_rows.size()) return false;
    ParamRow& row = m_rows[rowIndex];
    value = boundedParameterValue(value, row.minVal, row.maxVal, row.defaultVal);
    if (m_userDragging.contains(rowIndex)) {
        if (!m_changedDrags.contains(rowIndex))
            m_dragStarts.insert(rowIndex, ValueBeforeEdit(row.paramId, row.defaultVal));
        m_changedDrags.insert(rowIndex);
    }
    const float before = ValueBeforeEdit(row.paramId, value);
    {
        const QSignalBlocker sliderBlocker(row.slider);
        const QSignalBlocker spinBlocker(row.spinBox);
        row.slider->setValue(parameterToSlider(value, row.minVal, row.maxVal));
        const auto* spinBox = dynamic_cast<const CParameterSpinBox*>(row.spinBox);
        if (!preserveSpinText || !spinBox || !spinBox->DisplaysValue(value))
            row.spinBox->setValue(value);
        if (inputBox && inputBox == row.spinBox) inputBox->RestoreInput(input);
    }
    m_previewBaseline.remove(row.paramId);
    m_lastValues.insert(row.paramId, value);
    emit ParamChanged(row.paramId, value, false);
    RefreshXYControls();
    if (record && before != value) {
        PushEdit(row.paramId, before, value);
    }
    return true;
}

void CParamPanel::PushEdit(const QString& paramId, float before, float after)
{
    if (before == after) return;
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
    ApplyHistory(true);
}

void CParamPanel::Redo()
{
    ApplyHistory(false);
}

void CParamPanel::ApplyHistory(bool undo)
{
    if (!m_document || m_metadataDialogOpen || m_submittingEdit) return;
    live2dstruct::CModelEdit pending;
    if (!(undo ? m_document->PeekUndoEdit(&pending) : m_document->PeekRedoEdit(&pending)) ||
        (pending.field != live2dstruct::CModelEdit::ParameterValue &&
         pending.field != live2dstruct::CModelEdit::ParameterValues &&
         pending.field != live2dstruct::CModelEdit::ParameterMetadata)) return;
    const QPointer<CParamPanel> panel(this);
    auto* const document = m_document;
    const auto digest = document->SourceDigest();
    const auto handler = m_historyHandler;
    if (!handler && (!AllowUserEdit() || !panel || m_document != document)) return;
    m_submittingEdit = true;
    const auto finishSubmission = qScopeGuard([panel] { if (panel) panel->m_submittingEdit = false; });
    live2dstruct::CModelEdit edit;
    QString error;
    bool accepted = false;
    try {
        accepted = handler ? handler(undo, edit, &error)
                           : (undo ? document->UndoEdit(&edit) : document->RedoEdit(&edit));
    } catch (...) {
        error = QStringLiteral("参数历史恢复失败");
    }
    if (!panel || m_document != document || document->SourceDigest() != digest) return;
    if (!accepted) {
        RestoreControls();
        emit EditRejected(error.isEmpty() ? QStringLiteral("参数历史未通过校验") : error);
        return;
    }
    if (edit.field == live2dstruct::CModelEdit::ParameterMetadata) {
        RefreshAcceptedMetadata(edit, !undo);
    } else {
        const QVariant& state = undo ? edit.before : edit.after;
        const QMap<QString, float> values = edit.field == live2dstruct::CModelEdit::ParameterValue
            ? QMap<QString, float>{{edit.name, state.toFloat()}} : state.value<QMap<QString, float>>();
        for (auto value = values.cbegin(); value != values.cend(); ++value) {
            const auto rowIt = std::find_if(m_rows.cbegin(), m_rows.cend(), [&value](const auto& row) { return row.paramId == value.key(); });
            if (rowIt == m_rows.cend()) continue;
            {
                const QSignalBlocker sliderBlocker(rowIt->slider);
                const QSignalBlocker spinBlocker(rowIt->spinBox);
                rowIt->slider->setValue(parameterToSlider(value.value(), rowIt->minVal, rowIt->maxVal));
                rowIt->spinBox->setValue(value.value());
            }
            m_lastValues.insert(value.key(), value.value());
            m_previewBaseline.remove(value.key());
            emit ParamChanged(value.key(), value.value(), false);
            if (!panel) return;
        }
        RefreshXYControls();
    }
    if (panel) EmitHistoryState();
}
