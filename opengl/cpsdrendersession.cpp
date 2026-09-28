#include "cpsdrendersession.h"
#include "cpsdwritelist.h"
#include "cmeshbounds.h"
#include "framework/Model/CubismModel.hpp"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "std/cexportcontrol.h"
#include <QScopeGuard>
#include <QSurface>
#include <QThread>
#include <cmath>
#include <utility>

using namespace Live2D::Cubism::Framework;

namespace {
class ContextRestore
{
public:
    ContextRestore() : m_context(QOpenGLContext::currentContext()),
        m_surface(m_context ? m_context->surface() : nullptr), m_surfaceObject(dynamic_cast<QObject*>(m_surface)) {}
    ~ContextRestore()
    {
        if (QOpenGLContext::currentContext() == m_context) return;
        if (auto* current = QOpenGLContext::currentContext()) current->doneCurrent();
        if (m_context && m_context->isValid() && m_surfaceObject) m_context->makeCurrent(m_surface);
    }
private:
    QPointer<QOpenGLContext> m_context;
    QSurface* m_surface;
    QPointer<QObject> m_surfaceObject;
};
}

CPsdRenderSession::CPsdRenderSession(QObject* owner, QOpenGLContext* context, CubismModel* model,
    Rendering::CubismRenderer_OpenGLES2* renderer, const QSize& canvas, const CubismMatrix44& matrix,
    QVector<Entry> entries, std::function<bool()> makeCurrent, Validate validate, CExportControl* control)
    : m_owner(owner), m_context(context), m_model(model), m_renderer(renderer),
      m_canvas(canvas), m_matrix(matrix), m_entries(std::move(entries)), m_makeCurrent(std::move(makeCurrent)),
      m_validate(std::move(validate)), m_control(control) {}

CPsdRenderSession::~CPsdRenderSession()
{
    if (m_started) Finish(false);
}

bool CPsdRenderSession::Fail(QString* error, const QString& reason)
{
    if (error) *error = reason;
    if (m_control) m_control->Fail(reason);
    return false;
}

bool CPsdRenderSession::Activate(QString* error)
{
    if (!m_owner || !m_context)
        return Fail(error, QStringLiteral("PSD 导出窗口或 OpenGL 上下文已失效"));
    if (m_owner->thread() != QThread::currentThread() || m_context->thread() != QThread::currentThread())
        return Fail(error, QStringLiteral("PSD 渲染只能在所属界面线程执行"));
    if (!m_context->isValid()) return Fail(error, QStringLiteral("PSD 导出的 OpenGL 上下文已失效"));
    if (m_validate && !m_validate(error)) {
        const QString reason = error && !error->isEmpty() ? *error : QStringLiteral("PSD 导出模型已改变");
        return Fail(error, reason);
    }
    if (!m_model || !m_renderer || m_renderer->GetModel() != m_model)
        return Fail(error, QStringLiteral("PSD 会话的模型与渲染器不匹配"));
    if (QOpenGLContext::currentContext() != m_context && (!m_makeCurrent || !m_makeCurrent()))
        return Fail(error, QStringLiteral("无法激活 PSD 导出的 OpenGL 上下文"));
    if (QOpenGLContext::currentContext() != m_context)
        return Fail(error, QStringLiteral("无法恢复 PSD 导出的 OpenGL 上下文"));
    return true;
}

bool CPsdRenderSession::Begin(bool includeHidden, QString* error)
{
    if (error) error->clear();
    if (m_started) return Fail(error, QStringLiteral("PSD 渲染会话已经开始"));
    for (int index = 0; index < 16; ++index)
        if (!std::isfinite(m_matrix.GetArray()[index])) return Fail(error, QStringLiteral("PSD 投影矩阵不是有限数值"));
    const ContextRestore restore;
    if (!Activate(error)) return false;
    if (!m_buffer.setGlOwningContext(m_context) || !m_buffer.CreateOffscreenSurface(m_canvas.width(), m_canvas.height()))
        return Fail(error, QStringLiteral("无法创建 PSD 离屏缓冲"));
    bool prepared = false;
    const auto discard = qScopeGuard([&] {
        if (!prepared && !m_buffer.DestroyOffscreenSurface()) Fail(nullptr, QStringLiteral("无法释放未完成的 PSD 离屏缓冲"));
    });
    if (!m_renderer->BeginPsdExport(includeHidden, m_control, error)) return false;
    m_includeHidden = includeHidden;
    m_started = true;
    prepared = true;
    return true;
}

bool CPsdRenderSession::Next(CPsdExportItem& item, QString* error)
{
    if (error) error->clear();
    if (m_control && !m_control->Report(CExportControl::Phase::Rendering, m_index, m_entries.size(),
                                       m_index < m_entries.size() ? m_entries[m_index].name : QString())) {
        if (error) *error = m_control->Error();
        return false;
    }
    const ContextRestore restore;
    if (!m_started || !Activate(error)) return false;
    CPsdExportItem candidate;
    if (m_index == m_entries.size()) {
        candidate.type = CPsdExportItem::Type::Done;
        item = std::move(candidate);
        return true;
    }
    const auto& entry = m_entries[m_index];
    candidate.name = entry.name;
    if (entry.type != CPsdExportItem::Type::Layer) {
        candidate.type = entry.type;
        item = std::move(candidate);
        ++m_index;
        return true;
    }
    if (entry.drawable < 0 || entry.drawable >= m_model->GetDrawableCount())
        return Fail(error, QStringLiteral("PSD 图层的运行时对象索引无效"));
    if (!std::isfinite(entry.opacityOverride)) return Fail(error, QStringLiteral("PSD 图层透明度不是有限数值"));
    using Reason = CPsdExportCoverage::Reason;
    if (m_metadataIndex != m_index) {
        candidate.runtimeIndex = entry.drawable;
        candidate.psdLayerId = quint32(entry.drawable) + 1;
        const float* matrix = m_matrix.GetArray();
        const auto projected = MeshBounds::ProjectViewport(m_model->GetDrawableVertices(entry.drawable),
            m_model->GetDrawableVertexCount(entry.drawable), [&](float x, float y) {
                const double w = double(matrix[3]) * x + double(matrix[7]) * y + matrix[15];
                return QPointF((double(matrix[0]) * x + double(matrix[4]) * y + matrix[12]) / w,
                               (double(matrix[1]) * x + double(matrix[5]) * y + matrix[13]) / w);
            }, m_canvas);
        switch (projected.state) {
        case MeshBounds::ProjectionState::Invalid:
            return Fail(error, QStringLiteral("PSD 图层的投影坐标无效"));
        case MeshBounds::ProjectionState::NoVertices: candidate.emptyReason = Reason::NoVertices; break;
        case MeshBounds::ProjectionState::DegenerateBounds: candidate.emptyReason = Reason::DegenerateBounds; break;
        case MeshBounds::ProjectionState::OutsideViewport: candidate.emptyReason = Reason::OutsideCanvas; break;
        case MeshBounds::ProjectionState::Pixels: break;
        }
        const int indices = m_model->GetDrawableVertexIndexCount(entry.drawable);
        const int vertices = m_model->GetDrawableVertexCount(entry.drawable);
        const auto* topology = m_model->GetDrawableVertexIndices(entry.drawable);
        if (indices < 0 || indices > 3000000 || indices % 3 || (indices && (!vertices || !topology)))
            return Fail(error, QStringLiteral("PSD 图层的三角索引无效"));
        if (candidate.emptyReason != Reason::None)
            for (int index = 0; index < indices; ++index) {
                if (index % 1024 == 0 && m_control && !m_control->Checkpoint()) return Fail(error, m_control->Error());
                if (topology[index] >= vertices) return Fail(error, QStringLiteral("PSD 空图层的三角索引越界"));
            }
        if (!indices && candidate.emptyReason != Reason::NoVertices) candidate.emptyReason = Reason::NoTriangles;
        const float opacity = entry.opacityOverride >= 0 ? entry.opacityOverride :
            m_model->GetDrawableOpacityAll(entry.drawable, m_includeHidden);
        const float alpha = opacity * m_model->GetModelOpacity();
        if (!std::isfinite(alpha)) return Fail(error, QStringLiteral("PSD 图层透明度不是有限数值"));
        candidate.opacity = static_cast<quint8>(qBound(0.0f, alpha * 255.0f, 255.0f));
        candidate.blendMode = CPsdWriteList::CubismBlendModeToPsd(m_model->GetDrawableBlendMode(entry.drawable));
        m_metadata = candidate;
        m_metadataIndex = m_index;
    } else candidate = m_metadata;
    if (candidate.emptyReason != Reason::None) {
        candidate.type = CPsdExportItem::Type::EmptyLayer;
        item = std::move(candidate);
        ++m_index;
        return true;
    }
    if (!m_buffer.BeginDraw()) return Fail(error, QStringLiteral("无法绑定 PSD 离屏缓冲"));
    bool drawing = true;
    const auto endDraw = qScopeGuard([&] {
        if (drawing && !m_buffer.EndDraw()) Fail(nullptr, QStringLiteral("无法清理中断的 PSD 帧缓冲绘制"));
    });
    bool ready = false;
    const bool rendered = m_renderer->DrawPsdStep(entry.drawable, entry.opacityOverride, m_matrix, m_canvas,
                                                 candidate.image, candidate.rect, ready, error);
    if (!m_buffer.EndDraw()) return Fail(error, QStringLiteral("无法恢复 PSD 图层绘制前的帧缓冲"));
    drawing = false;
    if (!rendered) return false;
    if (ready) {
        if (candidate.rect.isEmpty() || candidate.image.isNull())
            return Fail(error, QStringLiteral("PSD 图层渲染结果与有效投影边界不一致"));
        candidate.type = CPsdExportItem::Type::Layer;
        ++m_index;
    }
    item = std::move(candidate);
    return true;
}

bool CPsdRenderSession::Finish(bool complete, QString* error)
{
    if (error) error->clear();
    if (!m_started) return true;
    const ContextRestore restore;
    if (!Activate(error)) return false;
    m_renderer->EndPsdExport(complete);
    if (!m_buffer.DestroyOffscreenSurface()) return Fail(error, QStringLiteral("无法释放 PSD 离屏缓冲"));
    m_started = false;
    return true;
}

void CPsdRenderSession::DetachControl()
{
    m_control = nullptr;
    if (m_started && m_renderer) m_renderer->DetachPsdExportControl();
}
