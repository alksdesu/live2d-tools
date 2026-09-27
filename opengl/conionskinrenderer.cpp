#include "conionskinrenderer.h"
#include "coffscreenbuffer.h"
#include "cglrenderstate.h"
#include "live2d/LAppModel.hpp"
#include "live2d/LAppDelegate.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include <QCryptographicHash>
#include <QDataStream>
#include <QOpenGLShaderProgram>
#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace Live2D::Cubism::Framework;
using Skin = live2dstruct::COnionSkin;
using Renderer = Rendering::CubismRenderer_OpenGLES2;
namespace {
bool fail(QString* error, const QString& reason) { if (error) *error = reason; return false; }
QByteArray hash(const QByteArray& value) { return QCryptographicHash::hash(value, QCryptographicHash::Sha256); }
void color(QDataStream& stream, const Rendering::CubismRenderer::CubismTextureColor& value)
{ stream << value.R << value.G << value.B << value.A; }
template<class Value> void sortedHash(QDataStream& stream, const QHash<QString, Value>& values)
{
    const auto keys = values.keys(); QStringList sorted(keys); std::sort(sorted.begin(), sorted.end());
    stream << sorted; for (const auto& key : sorted) stream << values[key];
}
void sortedSet(QDataStream& stream, const QSet<QString>& values)
{ auto sorted = values.values(); std::sort(sorted.begin(), sorted.end()); stream << sorted; }
bool finite(const QMatrix4x4& matrix, const QVector4D& background)
{
    for (int i = 0; i < 16; ++i) if (!std::isfinite(matrix.constData()[i])) return false;
    for (int i = 0; i < 4; ++i) if (!std::isfinite(background[i]) || background[i] < 0 || background[i] > 1) return false;
    return true;
}
QByteArray sourceKey(LAppModel& source, const Renderer& renderer, Renderer::SnapshotScene& scene, quint64 revision)
{
    auto* delegate = LAppDelegate::GetInstance();
    scene.hiddenDrawables = delegate->m_hiddenDrawables | delegate->m_hiddenDeformerDrawables;
    scene.opacity = delegate->m_drawableOpacityOverrides; scene.drawOrder = delegate->m_drawableDrawOrderOverrides;
    scene.orderGroups = delegate->m_drawableOrderGroupIndices; scene.groups = delegate->m_drawOrderGroups;
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << source.GetPreviewMocDigest() << quint64(reinterpret_cast<quintptr>(source.GetModel()))
           << quint64(reinterpret_cast<quintptr>(&renderer)) << source.GetTextureBindings() << revision;
    stream << renderer.IsPremultipliedAlpha() << renderer.IsUsingHighPrecisionMask() << renderer.GetAnisotropy()
           << renderer.GetRenderTextureCount() << renderer.GetClippingMaskBufferSize().X << renderer.GetClippingMaskBufferSize().Y;
    const auto rgb = renderer.GetModelColor(); stream << rgb.R << rgb.G << rgb.B;
    sortedSet(stream, scene.hiddenDrawables); sortedHash(stream, scene.opacity); sortedHash(stream, scene.drawOrder); sortedHash(stream, scene.orderGroups);
    stream << qint64(scene.groups.size()); for (const auto& group : scene.groups) stream << group.minimumDrawOrder << group.maximumDrawOrder;
    const auto* model = source.GetModel(); stream << model->GetDrawableIndicesRevision();
    const bool multiply = model->GetOverwriteFlagForModelMultiplyColors(), screen = model->GetOverwriteFlagForModelScreenColors();
    stream << multiply << screen << model->GetOverwriteFlagForModelCullings();
    for (int index = 0; index < model->GetDrawableCount(); ++index) {
        const bool m = model->GetOverwriteFlagForDrawableMultiplyColors(index), s = model->GetOverwriteFlagForDrawableScreenColors(index);
        stream << m << s << model->GetOverwriteFlagForDrawableCullings(index) << model->GetDrawableCulling(index);
        if (m || multiply) color(stream, model->GetMultiplyColor(index));
        if (s || screen) color(stream, model->GetScreenColor(index));
    }
    for (int index = 0; index < model->GetPartCount(); ++index) {
        stream << model->GetOverwriteColorForPartMultiplyColors(index) << model->GetOverwriteColorForPartScreenColors(index);
        color(stream, model->GetPartMultiplyColor(index)); color(stream, model->GetPartScreenColor(index));
    }
    return hash(bytes);
}
QByteArray planKey(const Skin::Request& request, const Skin::Base& base)
{
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << request.timeline.Fingerprint() << request.time << request.useFades << request.loop << request.settings.before
           << request.settings.after << request.settings.stepFrames << request.settings.beforeOpacity << request.settings.afterOpacity
           << base.parameters << base.parts << base.modelOpacity;
    sortedSet(stream, base.eyeBlinkIds); sortedSet(stream, base.lipSyncIds); sortedSet(stream, base.hiddenParts);
    return hash(bytes);
}
}

struct COnionSkinRenderer::Impl {
    QPointer<QOpenGLContext> context;
    CubismMoc* moc = nullptr;
    CubismModel* model = nullptr;
    std::unique_ptr<Rendering::CubismRenderer, void(*)(Rendering::CubismRenderer*)> renderer{nullptr, &Rendering::CubismRenderer::Delete};
    std::array<std::unique_ptr<COffscreenBuffer>, 3> buffers;
    std::unique_ptr<QOpenGLShaderProgram> program;
    GLuint vao = 0;
    Skin::Plan plan;
    Renderer::SnapshotScene scene;
    QByteArray sourceToken, planToken, renderToken;
    QByteArray failureToken;
    QString failureMessage;
    qint64 reservedBytes = 0;
    QSize size;
    int sample = 0, accumulated = 0;
    bool ready = false;
    Statistics stats;

    ~Impl() {
        buffers = {}; program.reset();
        if (context && context == QOpenGLContext::currentContext() && vao) functions()->glDeleteVertexArrays(1, &vao);
        resetModel();
    }
    QOpenGLFunctions_3_3_Core* functions() const {
        if (!context || QOpenGLContext::currentContext() != context) return nullptr;
        auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context);
        return gl && gl->initializeOpenGLFunctions() ? gl : nullptr;
    }
    void resetModel() {
        renderer.reset(); if (moc && model) moc->DeleteModel(model); model = nullptr;
        if (moc) CubismMoc::Delete(moc); moc = nullptr; sourceToken.clear();
    }
    bool prepareProgram(QString* error) {
        if (program) return true;
        auto candidate = std::make_unique<QOpenGLShaderProgram>();
        const char* vertex = "#version 330 core\nout vec2 uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);uv=p;gl_Position=vec4(p*2.0-1.0,0,1);}";
        const char* fragment = "#version 330 core\nin vec2 uv;uniform sampler2D beforeImage;uniform sampler2D renderedImage;uniform float amount;out vec4 result;void main(){result=mix(texture(beforeImage,uv),texture(renderedImage,uv),amount);}";
        if (!candidate->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
            !candidate->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) || !candidate->link()) return fail(error, candidate->log());
        if (!vao) functions()->glGenVertexArrays(1, &vao);
        if (!vao) return fail(error, QStringLiteral("无法创建洋葱皮合成资源"));
        program = std::move(candidate); return true;
    }
    bool prepareModel(LAppModel& live, Renderer& previous, QString* error) {
        resetModel(); reservedBytes = 0;
        const auto* source = live.GetModel();
        for (int index = 0; index < source->GetDrawableCount(); ++index) {
            const qint64 bytes = qint64(source->GetDrawableVertexCount(index)) * 32 + qint64(source->GetDrawableVertexIndexCount(index)) * 4;
            if (bytes < 0 || bytes > Skin::MaximumGpuBytes - reservedBytes) return fail(error, QStringLiteral("模型网格超过洋葱皮显存预算"));
            reservedBytes += bytes;
        }
        const auto masks = previous.GetClippingMaskBufferSize();
        const double maskBytes = double(previous.GetRenderTextureCount()) * masks.X * masks.Y * 4;
        if (!std::isfinite(maskBytes) || maskBytes < 0 || maskBytes > Skin::MaximumGpuBytes - reservedBytes)
            return fail(error, QStringLiteral("模型蒙版超过洋葱皮显存预算"));
        reservedBytes += qint64(std::ceil(maskBytes));
        if (reservedBytes > Skin::MaximumGpuBytes - 4096) return fail(error, QStringLiteral("洋葱皮没有可用显存预算"));
        reservedBytes += 4096;
        const auto& bytes = live.GetPreviewMocBytes();
        if (bytes.isEmpty() || bytes.size() > std::numeric_limits<csmSizeInt>::max()) return fail(error, QStringLiteral("洋葱皮模型快照无效"));
        moc = CubismMoc::Create(reinterpret_cast<const csmByte*>(bytes.constData()), csmSizeInt(bytes.size()), true);
        if (!moc || !(model = moc->CreateModel()) || !model->CopyRuntimeStateFrom(*source, error)) return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("无法创建独立洋葱皮模型"));
        renderer.reset(Rendering::CubismRenderer::Create());
        auto* glRenderer = static_cast<Renderer*>(renderer.get());
        const auto& bindings = live.GetTextureBindings();
        if (!glRenderer || !glRenderer->PrepareModelReplacement(model, previous, error, &bindings)) return false;
        ++stats.modelBuilds; return true;
    }
    bool prepareBuffers(const QSize& requested, QString* error) {
        GLint maximum = 0; functions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
        QSize next; if (!Skin::SelectResolution(requested, maximum, reservedBytes, next, error)) return false;
        if (size == next && buffers[0] && buffers[1] && buffers[2]) return true;
        buffers = {}; size = {};
        for (auto& buffer : buffers) {
            buffer = std::make_unique<COffscreenBuffer>();
            if (!buffer->setGlOwningContext(context) || !buffer->CreateOffscreenSurface(next.width(), next.height())) {
                buffers = {}; return fail(error, QStringLiteral("无法分配洋葱皮离屏缓冲"));
            }
        }
        size = next; return true;
    }
    bool mix(GLuint before, GLuint rendered, float amount, const QRect& viewport, QString* error) {
        auto* gl = functions(); if (!gl || !prepareProgram(error)) return false;
        gl->glDisable(GL_BLEND); gl->glDisable(GL_DEPTH_TEST); gl->glDisable(GL_STENCIL_TEST); gl->glDisable(GL_CULL_FACE); gl->glDisable(GL_SCISSOR_TEST);
        gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); gl->glViewport(viewport.x(), viewport.y(), viewport.width(), viewport.height());
        gl->glBindSampler(0, 0); gl->glBindSampler(1, 0);
        if (!program->bind()) return fail(error, QStringLiteral("无法绑定洋葱皮合成程序"));
        program->setUniformValue("beforeImage", 0); program->setUniformValue("renderedImage", 1); program->setUniformValue("amount", amount);
        gl->glActiveTexture(GL_TEXTURE0); gl->glBindTexture(GL_TEXTURE_2D, before); gl->glActiveTexture(GL_TEXTURE1); gl->glBindTexture(GL_TEXTURE_2D, rendered);
        gl->glBindVertexArray(vao); gl->glDrawArrays(GL_TRIANGLES, 0, 3); ++stats.composites;
        return gl->glGetError() == GL_NO_ERROR || fail(error, QStringLiteral("洋葱皮合成失败"));
    }
    bool renderSample(const Skin::Sample& value, const Skin::Base& base, const QMatrix4x4& matrix, QString* error) {
        auto* gl = functions();
        for (int index = 0; index < model->GetParameterCount(); ++index) {
            const QString id = QString::fromUtf8(model->GetParameterId(index)->GetString().GetRawString());
            if (!value.frame.parameters.contains(id)) return fail(error, QStringLiteral("洋葱皮样本缺少基础参数"));
            model->SetParameterValue(index, value.frame.parameters[id]);
        }
        for (int index = 0; index < model->GetPartCount(); ++index) {
            const QString id = QString::fromUtf8(model->GetPartId(index)->GetString().GetRawString());
            model->SetPartOpacity(index, base.hiddenParts.contains(id) ? 0.f : std::clamp(value.frame.parts.value(id, base.parts.value(id, 1)), 0.f, 1.f));
        }
        model->SetModelOpacity(std::clamp(value.frame.model.value(QStringLiteral("Opacity"), base.modelOpacity), 0.f, 1.f)); model->Update();
        if (!model->HasFiniteDrawableState()) return fail(error, QStringLiteral("洋葱皮帧包含非有限几何"));
        const int output = 1 - accumulated;
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, buffers[accumulated]->GetRenderTexture());
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, buffers[2]->GetRenderTexture());
        gl->glDisable(GL_SCISSOR_TEST);
        gl->glBlitFramebuffer(0, 0, size.width(), size.height(), 0, 0, size.width(), size.height(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl->glViewport(0, 0, size.width(), size.height()); gl->glBindSampler(0, 0); gl->glBindSampler(1, 0);
        auto* target = static_cast<Renderer*>(renderer.get()); CubismMatrix44 mvp; auto matrixCopy = matrix; mvp.SetMatrix(matrixCopy.data()); target->SetMvpMatrix(&mvp);
        const auto rgb = target->GetModelColor(); target->SetModelColor(rgb.R, rgb.G, rgb.B, model->GetModelOpacity());
        if (!target->DrawSnapshotModel(scene, error)) return false;
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, buffers[output]->GetRenderTexture());
        if (!mix(buffers[accumulated]->GetColorBuffer(), buffers[2]->GetColorBuffer(), value.opacity, QRect(QPoint(), size), error)) return false;
        accumulated = output; ++stats.samples; return true;
    }
};

COnionSkinRenderer::COnionSkinRenderer() : m_impl(std::make_unique<Impl>()) {}
COnionSkinRenderer::~COnionSkinRenderer() = default;
void COnionSkinRenderer::Invalidate() noexcept
{ m_impl->renderToken.clear(); m_impl->failureToken.clear(); m_impl->failureMessage.clear(); m_impl->ready = false; }
COnionSkinRenderer::Statistics COnionSkinRenderer::Counters() const { return m_impl->stats; }

bool COnionSkinRenderer::Render(LAppModel& live, const Skin::Request& request, const QMatrix4x4& matrix,
                               const QRect& viewport, const QVector4D& background, quint64 generation,
                               Skin::Status& status, QString* error) try
{
    if (error) error->clear(); auto& data = *m_impl;
    status = {}; status.generation = generation; status.requestedSize = viewport.size();
    if (!request.settings.enabled) { Invalidate(); return true; }
    if (request.suspended || request.recording || (request.playing && !request.settings.duringPlayback)) {
        Invalidate(); status.state = Skin::Status::State::Suspended;
        status.message = request.suspensionReason.isEmpty() ? QStringLiteral("当前交互期间暂停洋葱皮") : request.suspensionReason; return true;
    }
    if (!finite(matrix, background) || viewport.isEmpty() || !request.timeline.IsValid()) return fail(error, QStringLiteral("洋葱皮画布或时间轴无效"));
    auto* context = QOpenGLContext::currentContext();
    auto* previous = live.GetRenderer<Renderer>();
    if (!context || !previous || !previous->setGlOwningContext(context) || previous->IsDrawing() || previous->HasPsdExport())
        return fail(error, QStringLiteral("洋葱皮需要空闲的当前模型渲染上下文"));
    if (data.context && data.context != context) return fail(error, QStringLiteral("洋葱皮上下文已经改变"));
    data.context = context; auto* gl = data.functions(); if (!gl) return fail(error, QStringLiteral("洋葱皮 OpenGL 上下文不可用"));
    CGlRenderState saved; if (!saved.Save(context)) return fail(error, QStringLiteral("无法保存洋葱皮绘制前的图形状态"));
    const auto restore = qScopeGuard([&] { saved.Restore(); });
    Skin::Base base; if (!live.CaptureOnionSkinBase(base, error)) return false;
    const auto source = sourceKey(live, *previous, data.scene, request.sourceRevision);
    const auto timeline = planKey(request, base);
    QByteArray attempt; QDataStream attemptStream(&attempt, QIODevice::WriteOnly); attemptStream.setVersion(QDataStream::Qt_6_0);
    attemptStream << source << timeline << matrix << viewport << background; const auto attemptToken = hash(attempt);
    if (data.failureToken == attemptToken) return fail(error, data.failureMessage);
    bool successful = false;
    const auto retainFailure = qScopeGuard([&] {
        if (successful) return;
        data.failureToken = attemptToken;
        data.failureMessage = error && !error->isEmpty() ? *error : QStringLiteral("无法准备洋葱皮");
        data.ready = false; data.renderToken.clear(); data.buffers = {}; data.size = {}; data.resetModel();
    });
    if (timeline != data.planToken) {
        Skin::Plan plan; if (!Skin::Build(request, base, plan, error)) return false;
        data.plan = std::move(plan); data.planToken = timeline; Invalidate(); ++data.stats.plans;
    }
    status.omitted = data.plan.omitted; for (const auto& sample : data.plan.samples) status.sampleTimes.append(sample.time);
    if (data.plan.samples.isEmpty()) {
        Invalidate(); data.buffers = {}; data.size = {}; data.resetModel();
        status.state = Skin::Status::State::Ready; status.message = QStringLiteral("当前范围没有可显示的邻帧"); successful = true; return true;
    }
    if (source != data.sourceToken) {
        data.buffers = {}; data.size = {}; Invalidate();
        if (!data.prepareModel(live, *previous, error)) return false;
        data.sourceToken = source;
    }
    if (!data.prepareBuffers(viewport.size(), error)) return false;
    status.renderedSize = data.size; status.reducedResolution = data.size != viewport.size();
    QByteArray input; QDataStream stream(&input, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << source << timeline << matrix << viewport << data.size << background; const auto key = hash(input);
    if (data.renderToken != key) {
        data.ready = false; data.sample = 0; data.accumulated = 0; data.renderToken = key;
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, data.buffers[0]->GetRenderTexture()); gl->glDisable(GL_SCISSOR_TEST); gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        const GLfloat rgba[]{background.x(), background.y(), background.z(), background.w()}; gl->glClearBufferfv(GL_COLOR, 0, rgba);
    }
    if (!data.ready) {
        do {
            if (!data.renderSample(data.plan.samples[data.sample], base, matrix, error)) { Invalidate(); return false; }
            ++data.sample; data.ready = data.sample == data.plan.samples.size();
        } while (request.playing && request.settings.duringPlayback && !data.ready);
    }
    if (!data.ready) { status.state = Skin::Status::State::Preparing; status.message = QStringLiteral("正在准备邻帧 %1 / %2").arg(data.sample).arg(data.plan.samples.size()); successful = true; return true; }
    if (!saved.Restore()) return fail(error, QStringLiteral("无法恢复洋葱皮取样后的图形状态"));
    CGlRenderState composite; if (!composite.Save(context)) return fail(error, QStringLiteral("无法保存洋葱皮合成前状态"));
    const auto restoreComposite = qScopeGuard([&] { composite.Restore(); });
    const GLuint texture = data.buffers[data.accumulated]->GetColorBuffer();
    if (!data.mix(texture, texture, 0, viewport, error)) return false;
    ++data.stats.cacheDraws; status.state = Skin::Status::State::Ready;
    status.message = status.reducedResolution ? QStringLiteral("洋葱皮已按显存预算降低预览分辨率") : QStringLiteral("洋葱皮取自时间轴曲线；实时物理需先烘焙");
    successful = true;
    return true;
}
catch (const std::exception&) { m_impl->ready = false; m_impl->renderToken.clear(); return fail(error, QStringLiteral("无法准备洋葱皮；当前模型保持不变")); }
