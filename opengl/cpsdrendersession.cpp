#include "cpsdrendersession.h"
#include "cpsdwritelist.h"
#include "framework/Model/CubismModel.hpp"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "std/cexportcontrol.h"
#include <QScopeGuard>
#include <QSurface>
#include <QThread>
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
        candidate.type = CPsdExportItem::Type::Layer;
        const float opacity = entry.opacityOverride >= 0 ? entry.opacityOverride :
            m_model->GetDrawableOpacityAll(entry.drawable, m_includeHidden);
        candidate.opacity = static_cast<quint8>(qBound(0.0f, opacity * m_model->GetModelOpacity() * 255.0f, 255.0f));
        candidate.blendMode = CPsdWriteList::CubismBlendModeToPsd(m_model->GetDrawableBlendMode(entry.drawable));
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
