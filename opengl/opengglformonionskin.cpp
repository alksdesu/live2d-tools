#include "opengglform.h"
#include "conionskinrenderer.h"
#include <QPointer>
#include <QOpenGLVersionFunctionsFactory>
#include <cmath>
#include <limits>

using Skin = live2dstruct::COnionSkin;
namespace {
bool sameRequest(const Skin::Request& a, const Skin::Request& b)
{
    return a.settings == b.settings && a.timeline.Fingerprint() == b.timeline.Fingerprint() && a.time == b.time &&
        a.useFades == b.useFades && a.loop == b.loop && a.playing == b.playing && a.recording == b.recording &&
        a.suspended == b.suspended && a.suspensionReason == b.suspensionReason && a.sourceRevision == b.sourceRevision;
}
}

void OpengGlForm::SetOnionSkinRequestProvider(OnionSkinRequestProvider provider)
{
    m_onionProvider = std::move(provider);
    update();
}

bool OpengGlForm::SetOnionSkinRequest(const Skin::Request& request, QString* error)
{
    if (error) error->clear();
    if (!Skin::Validate(request.settings, error) || !std::isfinite(request.time) || request.time < 0 ||
        request.suspensionReason.size() > 32768 || request.suspensionReason.contains(QChar::Null)) {
        if (error && error->isEmpty()) *error = QStringLiteral("洋葱皮请求无效");
        return false;
    }
    if (sameRequest(request, m_onionRequest)) return true;
    if (m_onionGeneration == std::numeric_limits<quint64>::max()) {
        if (error) *error = QStringLiteral("洋葱皮请求版本已达到上限");
        return false;
    }
    m_onionRequest = request;
    m_onionRequest.suspensionReason = QString(request.suspensionReason.constData(), request.suspensionReason.size());
    ++m_onionGeneration;
    Skin::Status status; status.generation = m_onionGeneration;
    status.state = !request.settings.enabled ? Skin::Status::State::Disabled : Skin::Status::State::Preparing;
    if (request.settings.enabled && (request.suspended || request.recording || (request.playing && !request.settings.duringPlayback))) {
        status.state = Skin::Status::State::Suspended;
        status.message = request.suspensionReason.isEmpty() ? QStringLiteral("当前交互期间暂停洋葱皮") : request.suspensionReason;
    }
    PublishOnionSkinStatus(status); update(); return true;
}

void OpengGlForm::ClearOnionSkin(const QString& error)
{
    m_onionRequest = {};
    if (m_onionGeneration != std::numeric_limits<quint64>::max()) ++m_onionGeneration;
    if (m_onionRenderer) m_onionRenderer->Invalidate();
    Skin::Status status; status.generation = m_onionGeneration;
    if (!error.isEmpty()) {
        status.state = Skin::Status::State::Failed;
        status.message = QString(error.constData(), error.size());
    }
    PublishOnionSkinStatus(status);
    update();
}

void OpengGlForm::PublishOnionSkinStatus(const Skin::Status& status)
{
    if (status == m_onionStatus) return;
    m_onionStatus = status;
    if (m_onionStatusQueued) return;
    m_onionStatusQueued = true;
    const QPointer<OpengGlForm> alive(this);
    QTimer::singleShot(0, this, [alive] {
        if (!alive) return;
        alive->m_onionStatusQueued = false;
        emit alive->OnionSkinStatusChanged(alive->m_onionStatus);
    });
}

void OpengGlForm::DrawOnionSkinUnderlay()
{
    const QPointer<OpengGlForm> alive(this);
    if (m_exportSuspended || !m_runtimeReady || QOpenGLContext::currentContext() != context() ||
        LAppDelegate::GetInstance()->GetWindow() != this || LAppDelegate::GetInstance()->IsOffScreen()) return;
    const auto provider = m_onionProvider;
    if (provider) {
        Skin::Request requested; QString error;
        const bool captured = provider(requested, &error);
        if (!alive) return;
        if (!captured || !SetOnionSkinRequest(requested, &error)) {
            if (m_onionRenderer) m_onionRenderer->Invalidate();
            Skin::Status status; status.generation = m_onionGeneration; status.state = Skin::Status::State::Failed;
            status.message = error.isEmpty() ? QStringLiteral("无法读取洋葱皮预览状态") : error;
            PublishOnionSkinStatus(status); return;
        }
    }
    if (!m_onionRequest.settings.enabled) { m_onionRenderer.reset(); return; }
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    auto* delegate = LAppDelegate::GetInstance(); auto* view = delegate->GetView();
    if (!model || !view) return;
    Skin::Request request = m_onionRequest;
    if (IsKeyformDragging() || m_meshToolGesture || m_meshToolDispatchActive || m_meshEditCallbackActive || m_deformerCallbackActive || !model->CanSampleOnionSkin()) {
        request.suspended = true; request.suspensionReason = QStringLiteral("模型编辑或导出期间暂停洋葱皮");
    }
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context());
    if (!gl) return;
    GLint values[4]; gl->glGetIntegerv(GL_VIEWPORT, values);
    const QRect viewport(values[0], values[1], values[2], values[3]);
    const auto* rgba = delegate->m_clearColor; const QVector4D background(rgba[0], rgba[1], rgba[2], rgba[3]);
    if (!m_onionRenderer) m_onionRenderer = std::make_shared<COnionSkinRenderer>();
    Skin::Status status; QString error;
    if (!m_onionRenderer->Render(*model, request, view->GetMVP(), viewport, background, m_onionGeneration, status, &error)) {
        status.generation = m_onionGeneration; status.state = Skin::Status::State::Failed; status.message = error;
    }
    PublishOnionSkinStatus(status);
}
