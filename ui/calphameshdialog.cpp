#include "calphameshdialog.h"
#include "theme.h"
#include "std/cexportcontrol.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

using Planner = live2dstruct::CAlphaMeshPlanner;

namespace {
struct PreviewGeometry
{
    QPainterPath domain, oldMesh, candidate, outlines, holes;
};
QRectF sourceBounds(const Planner::Input& source)
{
    QRectF bounds; bool first = true;
    if (source.pixelCoordinates.size() <= live2dstruct::CMocMeshStructureBuilder::MaximumVertices)
        for (const auto& p : source.pixelCoordinates) {
            if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || std::abs(p.x()) > 1e9 || std::abs(p.y()) > 1e9) continue;
            if (first) { bounds = QRectF(p, QSizeF()); first = false; }
            else bounds = QRectF(QPointF(std::min(bounds.left(), p.x()), std::min(bounds.top(), p.y())),
                                 QPointF(std::max(bounds.right(), p.x()), std::max(bounds.bottom(), p.y())));
        }
    return bounds.isValid() ? bounds : QRectF(QPointF(), source.image.size());
}
bool prepareGeometry(const Planner::Input& source, const Planner::Plan* plan, PreviewGeometry& result, CExportControl& control)
{
    if (source.pixelCoordinates.size() > live2dstruct::CMocMeshStructureBuilder::MaximumVertices || source.indices.size() > 1000000) return false;
    const auto center = sourceBounds(source).center(); PreviewGeometry next;
    const auto mesh = [&](const QVector<QPointF>& points, const QVector<quint16>& indices, QPainterPath& path, QPainterPath* domain) {
        for (int i = 0; i + 2 < indices.size(); i += 3) {
            if (i % 1023 == 0 && !control.Checkpoint()) return false;
            QPointF p[3];
            for (int j = 0; j < 3; ++j) {
                if (indices[i + j] >= points.size()) return false; p[j] = points[indices[i + j]] - center;
                if (!std::isfinite(p[j].x()) || !std::isfinite(p[j].y()) || std::abs(p[j].x()) > 2e9 || std::abs(p[j].y()) > 2e9) return false;
            }
            path.moveTo(p[0]); path.lineTo(p[1]); path.lineTo(p[2]); path.closeSubpath();
            if (domain) {
                if ((p[1].x()-p[0].x())*(p[2].y()-p[0].y()) - (p[1].y()-p[0].y())*(p[2].x()-p[0].x()) < 0) std::swap(p[1], p[2]);
                domain->moveTo(p[0]); domain->lineTo(p[1]); domain->lineTo(p[2]); domain->closeSubpath();
            }
        }
        return true;
    };
    if (!mesh(source.pixelCoordinates, source.indices, next.oldMesh, &next.domain)) return false;
    next.domain.setFillRule(Qt::WindingFill);
    if (plan) {
        if (!mesh(plan->pixelCoordinates, plan->edit.indices, next.candidate, nullptr)) return false;
        for (const auto& contour : plan->contours) {
            if (!control.Checkpoint()) return false;
            if (contour.points.isEmpty()) continue;
            auto& path = contour.hole ? next.holes : next.outlines; path.moveTo(contour.points.first() - center);
            for (int i = 1; i < contour.points.size(); ++i) path.lineTo(contour.points[i] - center);
            path.closeSubpath();
        }
    }
    result = std::move(next); return control.Checkpoint();
}
class ControlsScroll : public QScrollArea
{
public:
    using QScrollArea::QScrollArea;
    QSize minimumSizeHint() const override
    {
        auto hint = QScrollArea::minimumSizeHint();
        if (widget()) hint.setWidth(widget()->minimumSizeHint().width() + verticalScrollBar()->sizeHint().width());
        return hint;
    }
};
}


class CAlphaMeshCanvas : public QWidget
{
public:
    explicit CAlphaMeshCanvas(const Planner::Input& source, QWidget* parent) : QWidget(parent), m_source(source)
    {
        setObjectName(QStringLiteral("alphaMeshCanvas")); setAccessibleName(QStringLiteral("素材透明轮廓与网格预览"));
        setFocusPolicy(Qt::StrongFocus); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_bounds = sourceBounds(source);
        auto raster = source.image; raster.setDevicePixelRatio(1); m_texture = QBrush(raster);
    }
    QSize minimumSizeHint() const override { return {fontMetrics().averageCharWidth() * 24, fontMetrics().height() * 12}; }
    void SetGeometry(const PreviewGeometry& geometry, bool candidate) { if (candidate || !m_hasPlan) m_geometry = geometry; m_hasPlan |= candidate; update(); }
    void ShowOld(bool visible) { m_old = visible; update(); }
    void ShowContour(bool visible) { m_contour = visible; update(); }
    void Fit() { m_fit = true; m_stable = {}; QueueFit(); }
protected:
    void showEvent(QShowEvent* event) override { QWidget::showEvent(event); QueueFit(); }
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        if (!m_fit && event->oldSize().isValid()) {
            m_offset += QPointF(event->size().width() - event->oldSize().width(), event->size().height() - event->oldSize().height()) / 2.0;
            if (m_pan) { m_start = m_offset; m_press = mapFromGlobal(m_pointerGlobal); }
            update();
        }
        QueueFit();
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) return;
        m_fit = false; m_pan = true; m_press = event->position(); m_pointerGlobal = event->globalPosition(); m_start = m_offset; setCursor(Qt::ClosedHandCursor); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override { if (m_pan) { m_pointerGlobal = event->globalPosition(); m_offset = m_start + event->position() - m_press; update(); event->accept(); } }
    void mouseReleaseEvent(QMouseEvent* event) override { if (m_pan) { m_pan = false; unsetCursor(); event->accept(); } }
    void hideEvent(QHideEvent* event) override { m_pan = false; unsetCursor(); QWidget::hideEvent(event); }
    void wheelEvent(QWheelEvent* event) override
    {
        if (!event->angleDelta().y()) return;
        m_fit = false;
        const auto point = (event->position() - m_offset) / m_scale;
        const double next = std::clamp(m_scale * std::pow(1.0015, event->angleDelta().y()), 1e-50, 256.0);
        m_offset = event->position() - point * next; m_scale = next; update(); event->accept();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const auto tokens = Theme::tokens(palette().color(QPalette::Window).lightness() < 128);
        const QColor base(tokens.value(QStringLiteral("@surface@"))), alternate(tokens.value(QStringLiteral("@surfaceHover@")));
        const int cell = std::max(2, fontMetrics().height() / 2); QPixmap tile(cell * 2, cell * 2); tile.fill(base);
        { QPainter pattern(&tile); pattern.fillRect(0, 0, cell, cell, alternate); pattern.fillRect(cell, cell, cell, cell, alternate); }
        painter.fillRect(rect(), QBrush(tile));
        const auto screen = [&](QPointF point) { return (point - m_bounds.center()) * m_scale + m_offset; };
        const QTransform mapping(m_scale, 0, 0, m_scale, m_offset.x(), m_offset.y());
        painter.save(); painter.setClipPath(mapping.map(m_geometry.domain));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_scale < 1);
        if (m_source.repeat) {
            QBrush texture = m_texture; QTransform transform;
            transform.translate(m_offset.x() - std::fmod(m_bounds.center().x(), std::max(1, m_source.image.width())) * m_scale,
                                m_offset.y() - std::fmod(m_bounds.center().y(), std::max(1, m_source.image.height())) * m_scale);
            transform.scale(m_scale, m_scale); texture.setTransform(transform); painter.fillRect(rect(), texture);
        } else {
            const QRectF imageRect(screen({0, 0}), QSizeF(m_source.image.width() * m_scale, m_source.image.height() * m_scale));
            if (imageRect.isValid()) painter.drawImage(imageRect, m_source.image);
        }
        painter.restore(); painter.setRenderHint(QPainter::Antialiasing);
        painter.setTransform(mapping);
        const auto drawPath = [&](const QPainterPath& path, QColor color, Qt::PenStyle style, int width) {
            QPen pen(color, width, style); pen.setCosmetic(true); painter.setPen(pen); painter.setBrush(Qt::NoBrush); painter.drawPath(path);
        };
        if (m_old) drawPath(m_geometry.oldMesh, QColor(tokens.value(QStringLiteral("@textMuted@"))), Qt::DashLine, 1);
        if (m_hasPlan) {
            drawPath(m_geometry.candidate, QColor(tokens.value(QStringLiteral("@accentInk@"))), Qt::SolidLine, 1);
            if (m_contour) {
                drawPath(m_geometry.outlines, palette().color(QPalette::Text), Qt::SolidLine, 2);
                drawPath(m_geometry.holes, palette().color(QPalette::Text), Qt::DashLine, 2);
            }
        }
    }
private:
    void QueueFit()
    {
        if (!m_fit || m_fitQueued || !isVisible()) return; m_fitQueued = true;
        QTimer::singleShot(0, this, [this] {
            m_fitQueued = false; if (!m_fit || !isVisible()) return;
            if (m_stable != size()) { m_stable = size(); QueueFit(); return; }
            const int margin = fontMetrics().height(); if (width() <= 2 * margin || height() <= 2 * margin) return;
            m_scale = std::clamp(std::min((width() - 2 * margin) / std::max(1.0, m_bounds.width()), (height() - 2 * margin) / std::max(1.0, m_bounds.height())), 1e-50, 256.0);
            m_offset = QPointF(width() / 2.0, height() / 2.0); update();
        });
    }
    Planner::Input m_source;
    PreviewGeometry m_geometry;
    QBrush m_texture;
    QRectF m_bounds;
    QPointF m_offset, m_press, m_start, m_pointerGlobal;
    QSize m_stable;
    double m_scale = 1;
    bool m_hasPlan = false, m_old = false, m_contour = true, m_fit = true, m_fitQueued = false, m_pan = false;
};

struct CAlphaMeshDialog::Job
{
    CExportControl control;
    Planner::Plan plan;
    Planner::Settings settings;
    PreviewGeometry geometry;
    bool hasGeometry = false;
    QString error;
    bool success = false;
};

CAlphaMeshDialog::CAlphaMeshDialog(const Source& source, QWidget* parent) : QDialog(parent), m_source(source)
{
    setObjectName(QStringLiteral("alphaMeshDialog")); setWindowTitle(QStringLiteral("自动生成网格")); setModal(true);
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("自动网格预览"), this); title->setTextFormat(Qt::PlainText); title->setWordWrap(true); layout->addWidget(title);
    auto* description = new QLabel(QStringLiteral("根据素材透明轮廓生成网格。范围限制在当前 UV 覆盖内，保留并迁移全部关键形；重新布点可能改变变形细节。"), this);
    description->setObjectName(QStringLiteral("alphaMeshDescription")); description->setWordWrap(true); description->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); layout->addWidget(description);
    m_splitter = new QSplitter(Qt::Horizontal, this); m_splitter->setChildrenCollapsible(false); layout->addWidget(m_splitter, 1);
    auto* preview = new QWidget(m_splitter); auto* previewLayout = new QVBoxLayout(preview); previewLayout->setContentsMargins(0, 0, 0, 0);
    auto* tools = new QFormLayout;
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setRowWrapPolicy(QFormLayout::WrapLongRows);
    tools->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* toggles = new QHBoxLayout;
    auto* fit = new QPushButton(QStringLiteral("适应范围"), preview); fit->setObjectName(QStringLiteral("alphaMeshFit"));
    m_showOld = new QCheckBox(QStringLiteral("原网格"), preview); m_showOld->setObjectName(QStringLiteral("alphaMeshShowOld")); toggles->addWidget(m_showOld);
    m_showContour = new QCheckBox(QStringLiteral("轮廓与孔洞"), preview); m_showContour->setObjectName(QStringLiteral("alphaMeshShowContours")); m_showContour->setChecked(true); toggles->addWidget(m_showContour); toggles->addStretch(); tools->addRow(fit, toggles); previewLayout->addLayout(tools);
    m_canvas = new CAlphaMeshCanvas(source.input, preview); previewLayout->addWidget(m_canvas, 1);
    connect(fit, &QPushButton::clicked, m_canvas, &CAlphaMeshCanvas::Fit);
    connect(m_showOld, &QCheckBox::toggled, m_canvas, &CAlphaMeshCanvas::ShowOld);
    connect(m_showContour, &QCheckBox::toggled, m_canvas, &CAlphaMeshCanvas::ShowContour);
    auto* previewHint = new QLabel(QStringLiteral("素材坐标中的网格预览 · 拖动平移，滚轮缩放 · 蓝线为候选，虚线为原网格"), preview); previewHint->setWordWrap(true); previewHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); previewLayout->addWidget(previewHint);
    auto* scroll = new ControlsScroll(m_splitter); scroll->setObjectName(QStringLiteral("alphaMeshControlsScroll")); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true);
    m_controls = new QWidget(scroll); auto* controls = new QVBoxLayout(m_controls); controls->setSizeConstraint(QLayout::SetMinimumSize);
    auto* origin = new QLabel(QStringLiteral("目标网格：%1\n%2\n%3").arg(source.input.artMeshId, source.label, source.detail), m_controls); origin->setObjectName(QStringLiteral("alphaMeshSource")); origin->setTextFormat(Qt::PlainText); origin->setWordWrap(true); origin->setTextInteractionFlags(Qt::TextSelectableByMouse); origin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); controls->addWidget(origin);
    auto* density = new QHBoxLayout; auto* densityGroup = new QButtonGroup(this); densityGroup->setObjectName(QStringLiteral("alphaMeshDensityGroup")); densityGroup->setExclusive(true);
    const QStringList labels{QStringLiteral("低密度"), QStringLiteral("标准"), QStringLiteral("高密度")};
    for (int i = 0; i < labels.size(); ++i) {
        auto* button = new QPushButton(labels[i], m_controls); button->setObjectName(QStringLiteral("alphaMeshDensity%1").arg(i)); button->setAccessibleName(labels[i]); button->setCheckable(true); densityGroup->addButton(button, i); density->addWidget(button); m_density.append(button);
        connect(button, &QPushButton::clicked, this, [this, i] { SelectDensity(Planner::Density(i)); });
    }
    controls->addLayout(density);
    auto* advancedToggle = new QToolButton(m_controls); advancedToggle->setText(QStringLiteral("高级参数")); advancedToggle->setObjectName(QStringLiteral("alphaMeshAdvanced")); advancedToggle->setCheckable(true); advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); advancedToggle->setArrowType(Qt::RightArrow); controls->addWidget(advancedToggle);
    auto* advanced = new QWidget(m_controls); auto* form = new QFormLayout(advanced); form->setRowWrapPolicy(QFormLayout::WrapLongRows); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); form->setContentsMargins(0, 0, 0, 0);
    const auto numeric = [&](const char* name, const QString& label, double minimum) {
        auto* spin = new QDoubleSpinBox(advanced); spin->setObjectName(QString::fromLatin1(name)); spin->setAccessibleName(label); spin->setDecimals(3); spin->setRange(minimum, minimum == 0 ? 1024 : 4096); spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); form->addRow(label, spin); return spin;
    };
    m_boundary = numeric("alphaMeshBoundarySpacing", QStringLiteral("轮廓点间距"), .001); m_interior = numeric("alphaMeshInteriorSpacing", QStringLiteral("内部点间距"), .001); m_margin = numeric("alphaMeshMargin", QStringLiteral("边缘余量"), 0);
    m_threshold = new QSpinBox(advanced); m_threshold->setObjectName(QStringLiteral("alphaMeshThreshold")); m_threshold->setRange(0, 255); m_threshold->setAccessibleName(QStringLiteral("透明度阈值")); form->addRow(QStringLiteral("透明度阈值"), m_threshold);
    auto* alphaHint = new QLabel(QStringLiteral("Alpha 小于或等于阈值的像素视为透明。提高阈值可忽略半透明杂点，也会减少柔和边缘。"), advanced); alphaHint->setWordWrap(true); alphaHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); form->addRow(alphaHint); controls->addWidget(advanced); advanced->hide();
    connect(advancedToggle, &QToolButton::toggled, advanced, [advanced, advancedToggle](bool visible) { advanced->setVisible(visible); advancedToggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow); });
    m_units = new QLabel(m_controls); m_units->setObjectName(QStringLiteral("alphaMeshUnits")); m_units->setWordWrap(true); m_units->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); controls->addWidget(m_units);
    m_counts = new QLabel(m_controls); m_counts->setObjectName(QStringLiteral("alphaMeshCounts")); m_counts->setTextFormat(Qt::PlainText); m_counts->setWordWrap(true); m_counts->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); controls->addWidget(m_counts);
    m_feedback = new QPlainTextEdit(m_controls); m_feedback->setObjectName(QStringLiteral("alphaMeshFeedback")); m_feedback->setReadOnly(true); m_feedback->setMinimumHeight(fontMetrics().height() * 4); controls->addWidget(m_feedback, 1);
    auto* actions = new QHBoxLayout; m_generate = new QPushButton(QStringLiteral("更新预览"), m_controls); m_generate->setObjectName(QStringLiteral("alphaMeshGenerate")); actions->addWidget(m_generate);
    m_stop = new QPushButton(QStringLiteral("停止计算"), m_controls); m_stop->setObjectName(QStringLiteral("alphaMeshStop")); actions->addWidget(m_stop); controls->addLayout(actions); scroll->setWidget(m_controls);
    m_splitter->setStretchFactor(0, 1); m_splitter->setStretchFactor(1, 0);
    m_status = new QLabel(this); m_status->setObjectName(QStringLiteral("alphaMeshStatus")); m_status->setTextFormat(Qt::PlainText); m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); layout->addWidget(m_status);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this); m_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); m_buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("alphaMeshApply")); m_buttons->button(QDialogButtonBox::Apply)->setText(QStringLiteral("应用网格")); layout->addWidget(m_buttons);
    connect(m_buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &CAlphaMeshDialog::accept); connect(m_buttons, &QDialogButtonBox::rejected, this, &CAlphaMeshDialog::reject);
    connect(m_generate, &QPushButton::clicked, this, &CAlphaMeshDialog::RequestGeneration); connect(m_stop, &QPushButton::clicked, this, &CAlphaMeshDialog::StopGeneration);
    const auto initial = Planner::Preset(Planner::Density::Standard);
    m_boundary->setValue(initial.boundarySpacing); m_interior->setValue(initial.interiorSpacing); m_margin->setValue(initial.margin); m_threshold->setValue(initial.alphaThreshold); m_density[1]->setChecked(true);
    for (auto* spin : {m_boundary, m_interior, m_margin}) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, &CAlphaMeshDialog::RequestGeneration);
        connect(spin, &QDoubleSpinBox::textChanged, this, [this] { if (!m_refreshing) Refresh(); });
    }
    connect(m_threshold, &QSpinBox::valueChanged, this, &CAlphaMeshDialog::RequestGeneration); connect(m_threshold, &QSpinBox::textChanged, this, [this] { if (!m_refreshing) Refresh(); });
    m_valid = !source.input.artMeshId.isEmpty() && !source.input.image.isNull();
    if (!m_valid) m_error = QStringLiteral("素材图像或网格身份无效，请重新打开");
    resize(fontMetrics().averageCharWidth() * 140, fontMetrics().height() * 36);
    Refresh(); if (m_valid) RequestGeneration();
}

CAlphaMeshDialog::~CAlphaMeshDialog() { if (m_job) m_job->control.Cancel(); }
void CAlphaMeshDialog::SetSubmissionHandler(SubmissionHandler handler) { if (!m_submitting && !m_finished) m_submit = std::move(handler); Refresh(); }
bool CAlphaMeshDialog::IsBusy() const { return m_submitting || m_queued || bool(m_job); }
bool CAlphaMeshDialog::IsReady() const { return !m_finished && m_valid && m_hasPlan && !IsBusy() && InputsValid() && Settings() == m_planSettings; }
Planner::Settings CAlphaMeshDialog::Settings() const { return {m_boundary->value(), m_interior->value(), m_margin->value(), m_threshold->value()}; }
bool CAlphaMeshDialog::InputsValid(QString* error) const
{
    if (!m_boundary->hasAcceptableInput() || !m_interior->hasAcceptableInput() || !m_margin->hasAcceptableInput() || !m_threshold->hasAcceptableInput()) { if (error) *error = QStringLiteral("请先修正未完成的参数输入"); return false; }
    for (const auto* spin : {m_boundary, m_interior, m_margin}) {
        bool parsed = false; const double displayed = spin->locale().toDouble(spin->cleanText(), &parsed);
        if (!parsed || displayed != spin->value()) { if (error) *error = QStringLiteral("请完成当前参数输入后更新预览"); return false; }
    }
    bool parsed = false; const int displayed = m_threshold->locale().toInt(m_threshold->cleanText(), &parsed);
    if (!parsed || displayed != m_threshold->value()) { if (error) *error = QStringLiteral("请完成当前参数输入后更新预览"); return false; }
    return Planner::ValidateSettings(Settings(), error);
}
void CAlphaMeshDialog::SetError(const QString& error) { m_error = error; Refresh(); }
void CAlphaMeshDialog::SelectDensity(Planner::Density density)
{
    const auto preset = Planner::Preset(density); m_refreshing = true;
    { const QSignalBlocker a(m_boundary), b(m_interior); m_boundary->setValue(preset.boundarySpacing); m_interior->setValue(preset.interiorSpacing); }
    m_refreshing = false; RequestGeneration();
}
void CAlphaMeshDialog::RequestGeneration()
{
    if (m_refreshing || m_submitting || m_finished || !m_valid) return;
    ++m_generation; if (m_job) m_job->control.Cancel();
    QString error; if (!InputsValid(&error)) { m_queued = false; SetError(error); return; }
    m_error.clear(); m_queued = true; Refresh();
    QTimer::singleShot(0, this, &CAlphaMeshDialog::StartGeneration);
}
void CAlphaMeshDialog::StartGeneration()
{
    if (!m_queued || m_job || m_finished || m_submitting) return;
    m_queued = false;
    QString error; if (!InputsValid(&error)) { SetError(error); return; }
    const auto settings = Settings();
    if (m_hasPlan && settings == m_planSettings) { Refresh(); return; }
    auto job = std::make_shared<Job>(); job->settings = settings; m_job = job;
    const auto input = m_source.input; const auto generation = m_generation;
    auto* thread = QThread::create([job, input] {
        try {
            job->success = Planner::Build(input, job->settings, job->plan, &job->error, &job->control);
            if (job->success) job->success = Planner::Validate(input, job->plan, &job->error);
            job->hasGeometry = prepareGeometry(input, job->success ? &job->plan : nullptr, job->geometry, job->control);
            if (job->success && !job->hasGeometry) { job->success = false; job->error = QStringLiteral("无法准备网格预览，模型保持不变；请重试或取消"); }
        }
        catch (...) { job->success = false; job->error = QStringLiteral("网格计算失败，模型保持不变"); }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, job, generation] {
        if (m_job != job) return; m_job.reset();
        if (m_finished) return;
        if (generation != m_generation || job->control.IsCancellationRequested()) { Refresh(); if (m_queued) StartGeneration(); return; }
        if (job->hasGeometry) m_canvas->SetGeometry(job->geometry, job->success);
        if (!job->success) { SetError(job->error.isEmpty() ? QStringLiteral("没有生成可用网格，请调整密度或透明度阈值") : job->error); return; }
        m_plan = std::move(job->plan); m_planSettings = job->settings; m_hasPlan = true; m_error.clear(); Refresh(); emit Ready();
    }, Qt::QueuedConnection);
    Refresh(); thread->start();
}
void CAlphaMeshDialog::StopGeneration()
{
    if (m_submitting || m_finished) return; ++m_generation; m_queued = false; if (m_job) m_job->control.Cancel();
    SetError(QStringLiteral("已停止计算；保留上一次候选，模型没有改变"));
}
void CAlphaMeshDialog::Refresh()
{
    if (!m_buttons) return;
    const bool ready = IsReady(); const bool editable = m_valid && !m_submitting && !m_finished;
    m_controls->setEnabled(editable); m_stop->setEnabled(editable && (m_job || m_queued)); m_generate->setEnabled(editable && !m_queued);
    m_buttons->button(QDialogButtonBox::Apply)->setEnabled(ready && bool(m_submit) && !m_finished);
    m_buttons->button(QDialogButtonBox::Cancel)->setEnabled(!m_submitting && !m_finished);
    RefreshStatus();
    QStringList messages; if (!m_error.isEmpty()) messages.append(m_error); if (m_hasPlan) messages.append(m_plan.warnings); m_feedback->setPlainText(messages.join(QLatin1Char('\n'))); m_feedback->setVisible(!messages.isEmpty());
    if (m_hasPlan) {
        const auto& s = m_plan.statistics;
        m_counts->setText(QStringLiteral("%1 → %2 个顶点 · %3 个三角形\n%4 个独立区域 · %5 个孔洞 · 保留 %6 个接缝顶点%7")
            .arg(s.oldVertices).arg(s.newVertices).arg(s.triangles).arg(s.islands).arg(s.holes).arg(s.preservedPins).arg(ready ? QString() : QStringLiteral("\n上一次候选；尚未应用当前输入")));
        m_units->setText(QStringLiteral("数值按 1024 参考尺寸归一化。此候选实际间距：轮廓 %1 px、内部 %2 px；余量 %3 px。")
            .arg(s.boundarySpacingPixels, 0, 'g', 6).arg(s.interiorSpacingPixels, 0, 'g', 6).arg(s.marginPixels, 0, 'g', 6));
    } else { m_counts->setText(QStringLiteral("尚未生成候选网格")); m_units->setText(QStringLiteral("间距和余量按完整来源的长边 / 1024 换算为像素；不受画布缩放或屏幕 DPI 影响。")); }
    auto* group = findChild<QButtonGroup*>(QStringLiteral("alphaMeshDensityGroup")); group->setExclusive(false);
    for (int i = 0; i < m_density.size(); ++i) {
        const auto preset = Planner::Preset(Planner::Density(i)); const QSignalBlocker block(m_density[i]);
        const bool selected = m_boundary->value() == preset.boundarySpacing && m_interior->value() == preset.interiorSpacing;
        m_density[i]->setChecked(selected);
        m_density[i]->setText((selected ? QStringLiteral("✓ ") : QString()) + m_density[i]->accessibleName());
    }
    group->setExclusive(true);
}
void CAlphaMeshDialog::RefreshStatus()
{
    if (!m_status) return;
    QString text = m_submitting ? QStringLiteral("正在应用网格…") : IsBusy() ? QStringLiteral("正在生成预览，可停止或取消…")
        : IsReady() ? QStringLiteral("预览已就绪，应用后可在模型历史中撤销") : QStringLiteral("参数已改变，请更新预览后应用");
    if (!m_valid) text = QStringLiteral("无法生成网格");
    if (!IsBusy() && !m_error.isEmpty()) text = QStringLiteral("未能完成操作：%1").arg(m_error.simplified());
    m_status->setText(m_status->fontMetrics().elidedText(text, Qt::ElideRight, std::max(1, m_status->contentsRect().width())));
    m_status->setAccessibleDescription(text);
    m_status->setToolTip(QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()));
}
void CAlphaMeshDialog::accept()
{
    if (m_submitting || m_finished) return;
    QString error; if (!InputsValid(&error)) { SetError(error); return; }
    if (!IsReady()) { SetError(QStringLiteral("请等待当前参数的预览生成后再应用")); return; }
    if (!m_submit) { SetError(QStringLiteral("模型提交接口不可用，预览尚未应用")); return; }
    m_submitting = true; Refresh(); const auto submit = m_submit; const auto plan = m_plan; const QPointer<CAlphaMeshDialog> alive(this);
    bool accepted = false;
    try { accepted = submit(plan, &error); } catch (...) { error = QStringLiteral("网格提交失败，预览保留供重试"); }
    if (!alive) return; m_submitting = false;
    if (!accepted) { SetError(error.isEmpty() ? QStringLiteral("模型未接受此网格，预览保留供重试") : error); return; }
    m_finished = true; QDialog::accept();
}
void CAlphaMeshDialog::reject()
{
    if (m_submitting || m_finished) return; m_finished = true; ++m_generation; m_queued = false;
    if (m_job) m_job->control.Cancel(); QDialog::reject();
}
void CAlphaMeshDialog::done(int result) { if (m_finished) QDialog::done(result); else if (result == QDialog::Accepted) accept(); else reject(); }
void CAlphaMeshDialog::closeEvent(QCloseEvent* event) { if (m_submitting) { event->ignore(); return; } reject(); event->accept(); }
void CAlphaMeshDialog::resizeEvent(QResizeEvent* event) { QDialog::resizeEvent(event); UpdateOrientation(); RefreshStatus(); }
void CAlphaMeshDialog::UpdateOrientation()
{
    if (!m_splitter || !m_canvas || !m_controls) return;
    const auto required = m_splitter->widget(0)->minimumSizeHint().width() + m_splitter->widget(1)->minimumSizeHint().width() + m_splitter->handleWidth();
    const auto orientation = m_splitter->width() >= required ? Qt::Horizontal : Qt::Vertical;
    if (m_splitter->orientation() != orientation) m_splitter->setOrientation(orientation);
}
