#include "cphysicsbake.h"
#include "live2dstruct/cmocdeformerorder.h"

#include "framework/CubismFramework.hpp"
#include "framework/Id/CubismId.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Model/CubismParameterReferences.hpp"
#include "framework/Physics/CubismPhysics.hpp"
#include "live2dstruct/cmotionblendpreview.h"
#include "live2dstruct/cphysicsdocument.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

using namespace Live2D::Cubism::Framework;

namespace {
constexpr qint64 MaximumPoints = CMotionTimeline::MaximumPointCount;
constexpr qsizetype MaximumPhysicsBytes = 16 * 1024 * 1024;
constexpr qsizetype MaximumMocBytes = 256 * 1024 * 1024;

bool fail(QString* error, const QString& text)
{
    if (error) *error = text;
    return false;
}

bool validId(const QString& id) { return !id.isEmpty() && !id.contains(QChar::Null); }
bool finiteFloat(double value) { return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(); }

bool validateDigest(const QByteArray& bytes, const QByteArray& expected, QString* error)
{
    return expected.isEmpty() || (expected.size() == 32 &&
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) == expected) ||
        fail(error, QStringLiteral("烘焙来源内容已变化，请重新捕获当前模型和物理配置"));
}

bool intervalsFor(double duration, double fps, qint64& intervals, QString* error)
{
    const double count = std::ceil(duration * fps);
    if (!std::isfinite(count) || count < 0 || count > MaximumPoints)
        return fail(error, QStringLiteral("烘焙帧数超出 100000 帧限制"));
    intervals = qint64(count);
    return true;
}
}

struct CPhysicsBake::Impl
{
    enum class State { Running, Finished, Canceled, Failed };
    State state = State::Running;
    Qt::HANDLE thread = QThread::currentThreadId();
    CubismMoc* moc = nullptr;
    CubismModel* model = nullptr;
    std::unique_ptr<CubismPhysics, void (*)(CubismPhysics*)> physics{nullptr, CubismPhysics::Delete};
    CMotionTimeline timeline;
    QMap<QString, float> base;
    QMap<QString, int> parameters, parts, outputs;
    QSet<QString> eyeIds, lipIds;
    Result result;
    QString error;
    bool fades = true, loop = false;
    double warmupPeriod = 0;
    qint64 intervals = 0, warmupIntervals = 0;
    qint64 completed = 0, total = 0;
    int warmupCycles = 0;

    ~Impl()
    {
        physics.reset();
        if (moc && model) moc->DeleteModel(model);
        if (moc) CubismMoc::Delete(moc);
    }

    bool CheckValues()
    {
        if (!physics->HasFiniteRuntimeState(&error)) return false;
        for (int index = 0; index < model->GetParameterCount(); ++index)
            if (!std::isfinite(model->GetParameterValue(index)))
                return fail(&error, QStringLiteral("物理烘焙产生了非有限参数值"));
        return true;
    }

    bool ApplyInput(double time, bool wrapping)
    {
        for (auto it = base.cbegin(); it != base.cend(); ++it)
            model->SetParameterValue(parameters.value(it.key()), it.value());
        CMotionPreviewFrame frame;
        if (fades) frame = CMotionBlendPreview::Evaluate(timeline, time, base, eyeIds, lipIds, wrapping);
        else {
            const double duration = timeline.Timing().duration;
            if (duration > 0) time = wrapping ? std::fmod(time, duration) : std::min(time, duration);
            frame = timeline.EvaluateFrame(float(time));
        }
        if (!frame.IsFinite()) return fail(&error, QStringLiteral("时间轴产生了非有限姿态"));
        const auto values = frame.ResolveParameters(eyeIds, lipIds);
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            const auto found = parameters.constFind(it.key());
            if (found != parameters.cend()) model->SetParameterValue(found.value(), it.value());
        }
        for (auto it = frame.parts.cbegin(); it != frame.parts.cend(); ++it) {
            const auto found = parts.constFind(it.key());
            if (found != parts.cend()) model->SetPartOpacity(found.value(), std::clamp(it.value(), 0.0f, 1.0f));
        }
        const auto opacity = frame.model.constFind(QStringLiteral("Opacity"));
        if (opacity != frame.model.cend()) model->SetModelOpacity(std::clamp(opacity.value(), 0.0f, 1.0f));
        return true;
    }

    bool Advance(double time, double delta, bool wrapping)
    {
        if (!ApplyInput(time, wrapping)) return false;
        physics->Evaluate(model, float(delta));
        if (!CheckValues()) return false;
        model->Update();
        return true;
    }

    void Record(double time)
    {
        for (auto it = outputs.cbegin(); it != outputs.cend(); ++it)
            result.curves[it.key()].append(QPointF(time, model->GetParameterValue(it.value())));
    }

    bool Next()
    {
        if (completed == 0) {
            if (!ApplyInput(0, warmupCycles ? true : loop)) return false;
            physics->Stabilization(model);
            if (!CheckValues()) return false;
            model->Update();
        } else if (completed <= warmupIntervals * warmupCycles) {
            const qint64 step = (completed - 1) % warmupIntervals + 1;
            const double previous = double(step - 1) / result.fps;
            const double time = step == warmupIntervals ? warmupPeriod : double(step) / result.fps;
            if (!Advance(step == warmupIntervals ? 0.0 : time, time - previous, true)) return false;
        } else {
            const qint64 frame = completed - 1 - warmupIntervals * warmupCycles;
            const double time = double(frame) / result.fps;
            if (frame && !Advance(time, 1.0 / result.fps, loop)) return false;
            Record(time);
        }
        ++completed;
        if (completed == total) state = State::Finished;
        return true;
    }
};

CPhysicsBake::CPhysicsBake() = default;
CPhysicsBake::~CPhysicsBake() = default;

bool CPhysicsBake::Prepare(const Request& request, QString* error)
{
    if (error) error->clear();
    if (QCoreApplication::instance() && QThread::currentThread() != QCoreApplication::instance()->thread())
        return fail(error, QStringLiteral("物理烘焙必须在应用主线程准备"));
    if (!CubismFramework::IsInitialized()) return fail(error, QStringLiteral("Cubism Framework 尚未初始化"));
    if (request.mocBytes.isEmpty() || request.mocBytes.size() > MaximumMocBytes ||
        request.physicsBytes.isEmpty() || request.physicsBytes.size() > MaximumPhysicsBytes)
        return fail(error, QStringLiteral("烘焙模型或物理配置为空或超出大小限制"));
    if (!validateDigest(request.mocBytes, request.expectedMocDigest, error) ||
        !validateDigest(request.physicsBytes, request.expectedPhysicsDigest, error)) return false;
    if (!finiteFloat(request.fps) || request.fps <= 0 || !finiteFloat(request.duration) || request.duration < 0 ||
        request.warmupCycles < 0 || request.warmupCycles > 3 || request.outputIds.isEmpty() || request.timeline.IsRecording())
        return fail(error, QStringLiteral("烘焙帧率、时长、预热次数或输出参数无效，录制期间不能烘焙"));
    try {
        auto next = std::make_unique<Impl>();
        QJsonObject timelineState;
        if (!request.timeline.SaveState(&timelineState, request.loop, 0, error) ||
            !next->timeline.RestoreState(timelineState, nullptr, error)) return false;
        next->base = request.baseParameters;
        next->fades = request.useFades;
        next->loop = request.loop;
        next->warmupCycles = request.warmupCycles;
        next->result.fps = request.fps;
        next->warmupPeriod = next->timeline.HasAnimation() && next->timeline.Timing().duration > 0
            ? next->timeline.Timing().duration : request.duration;
        if (!intervalsFor(request.duration, request.fps, next->intervals, error) ||
            (next->warmupCycles && !intervalsFor(next->warmupPeriod, request.fps, next->warmupIntervals, error))) return false;
        next->result.endTime = double(next->intervals) / request.fps;
        const double delta = 1.0 / request.fps;
        if (!finiteFloat(next->result.endTime) || !finiteFloat(delta) || float(delta) <= 0 ||
            ((next->intervals || next->warmupIntervals) && delta > 5.0))
            return fail(error, QStringLiteral("采样间隔超出物理求值可表示范围（最长 5 秒）"));
        qint64 retained = request.newAnimation ? 0 : next->timeline.TotalPointCount();
        if (!request.newAnimation)
            for (auto it = next->timeline.Tracks().cbegin(); it != next->timeline.Tracks().cend(); ++it)
                if (it.key().target == CMotionTarget::Parameter && request.outputIds.contains(it.key().id)) retained -= it.value().size();
        if (retained < 0 || retained > MaximumPoints ||
            request.outputIds.size() > (MaximumPoints - retained) / (next->intervals + 1))
            return fail(error, QStringLiteral("烘焙结果与保留轨道合计超过 100000 个关键点"));
        float previous = -1;
        for (qint64 index = 0; index <= next->intervals; ++index) {
            const float time = float(double(index) / request.fps);
            if (!std::isfinite(time) || time <= previous)
                return fail(error, QStringLiteral("采样时间无法在时间轴精度内区分"));
            previous = time;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(request.physicsBytes, &parseError);
        QJsonObject normalized;
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return fail(error, QStringLiteral("物理配置 JSON 无效"));
        if (!live2dstruct::CPhysicsDocument::Normalize(document.object(), normalized, error)) return false;
        const float physicsFps = float(normalized.value(QStringLiteral("Meta")).toObject().value(QStringLiteral("Fps")).toDouble());
        if (physicsFps > 0 && (next->intervals || (next->warmupCycles && next->warmupIntervals))) {
            const float physicsStep = 1.0f / physicsFps;
            constexpr int MaximumSubstepsPerFrame = 1024;
            if (physicsStep <= 0 || std::ceil(double(float(delta)) / physicsStep) + 1 > MaximumSubstepsPerFrame)
                return fail(error, QStringLiteral("单帧物理子步超过 1024，请提高烘焙采样帧率或降低物理 FPS"));
        }
        QSet<QString> physicsOutputs;
        for (const auto& group : normalized.value(QStringLiteral("PhysicsSettings")).toArray())
            for (const auto& output : group.toObject().value(QStringLiteral("Output")).toArray())
                physicsOutputs.insert(output.toObject().value(QStringLiteral("Destination")).toObject().value(QStringLiteral("Id")).toString());
        QByteArray runtimeMoc;
        if (!live2dstruct::CMocDeformerOrder::Normalize(request.mocBytes, runtimeMoc, error)) return false;
        next->moc = CubismMoc::Create(reinterpret_cast<const csmByte*>(runtimeMoc.constData()), csmSizeInt(runtimeMoc.size()), true);
        if (!next->moc || !(next->model = next->moc->CreateModel()))
            return fail(error, QStringLiteral("Core 无法创建独立烘焙模型"));
        if (next->base.size() != next->model->GetParameterCount())
            return fail(error, QStringLiteral("烘焙基线必须包含全部实际模型参数"));
        for (int index = 0; index < next->model->GetParameterCount(); ++index) {
            const auto id = QString::fromUtf8(next->model->GetParameterId(index)->GetString().GetRawString());
            const auto value = next->base.constFind(id);
            const float minimum = next->model->GetParameterMinimumValue(index), maximum = next->model->GetParameterMaximumValue(index);
            if (!validId(id) || next->parameters.contains(id) || value == next->base.cend() || !std::isfinite(value.value()) ||
                !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum || value.value() < minimum || value.value() > maximum)
                return fail(error, QStringLiteral("烘焙参数基线或模型范围无效：%1").arg(id));
            next->parameters.insert(id, index);
        }
        for (int index = 0; index < next->model->GetPartCount(); ++index)
            next->parts.insert(QString::fromUtf8(next->model->GetPartId(index)->GetString().GetRawString()), index);
        for (const auto& id : request.outputIds) {
            if (!validId(id) || !next->parameters.contains(id) || !physicsOutputs.contains(id))
                return fail(error, QStringLiteral("所选参数不是有效物理输出：%1").arg(id));
            next->outputs.insert(id, next->parameters.value(id));
            next->result.curves[id].reserve(int(next->intervals + 1));
        }
        for (const auto* ids : {&request.eyeBlinkIds, &request.lipSyncIds})
            for (const auto& id : *ids)
                if (!validId(id)) return fail(error, QStringLiteral("自动效果参数标识无效"));
        next->eyeIds = request.eyeBlinkIds;
        next->lipIds = request.lipSyncIds;
        const QSet<QString> realIds(next->parameters.keyBegin(), next->parameters.keyEnd());
        next->eyeIds.intersect(realIds);
        next->lipIds.intersect(realIds);
        next->physics.reset(CubismPhysics::Create(reinterpret_cast<const csmByte*>(request.physicsBytes.constData()),
            csmSizeInt(request.physicsBytes.size()), error));
        if (!next->physics) return false;
        CubismPhysics::ParameterDefinitionState binding;
        if (!next->physics->PrepareModelBinding(*next->model, {}, binding, error)) return false;
        next->physics->CommitParameterDefinitionChange(binding);
        next->total = 1 + next->warmupIntervals * next->warmupCycles + next->intervals + 1;
        m_impl.swap(next);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法分配或准备独立物理烘焙状态"));
    }
}

bool CPhysicsBake::Step(int maximumFrames, QString* error)
{
    if (error) error->clear();
    if (!m_impl) return fail(error, QStringLiteral("物理烘焙尚未准备"));
    if (QThread::currentThreadId() != m_impl->thread) return fail(error, QStringLiteral("物理烘焙不能跨线程执行"));
    if (maximumFrames <= 0) return fail(error, QStringLiteral("每批烘焙帧数必须大于零"));
    if (m_impl->state == Impl::State::Finished) return true;
    if (m_impl->state != Impl::State::Running) return fail(error, m_impl->error);
    try {
        for (int frame = 0; frame < maximumFrames && m_impl->state == Impl::State::Running; ++frame)
            if (!m_impl->Next()) {
                m_impl->state = Impl::State::Failed;
                return fail(error, m_impl->error);
            }
        return true;
    } catch (const std::exception&) {
        m_impl->state = Impl::State::Failed;
        m_impl->error = QStringLiteral("物理烘焙执行失败，未发布任何曲线");
        return fail(error, m_impl->error);
    }
}

void CPhysicsBake::Cancel() noexcept
{
    if (m_impl && m_impl->state == Impl::State::Running) {
        m_impl->state = Impl::State::Canceled;
        m_impl->error = QStringLiteral("物理烘焙已取消");
    }
}
bool CPhysicsBake::IsFinished() const noexcept { return m_impl && m_impl->state == Impl::State::Finished; }
bool CPhysicsBake::IsCanceled() const noexcept { return m_impl && m_impl->state == Impl::State::Canceled; }
bool CPhysicsBake::Failed() const noexcept { return m_impl && m_impl->state == Impl::State::Failed; }
qint64 CPhysicsBake::CompletedSteps() const noexcept { return m_impl ? m_impl->completed : 0; }
qint64 CPhysicsBake::TotalSteps() const noexcept { return m_impl ? m_impl->total : 0; }
QString CPhysicsBake::Error() const { return m_impl ? m_impl->error : QString(); }

bool CPhysicsBake::CaptureResult(Result& result, QString* error) const
{
    if (error) error->clear();
    if (!IsFinished()) return fail(error, m_impl && !m_impl->error.isEmpty() ? m_impl->error : QStringLiteral("物理烘焙尚未完成"));
    try {
        Result candidate = m_impl->result;
        result.curves.swap(candidate.curves);
        result.endTime = candidate.endTime;
        result.fps = candidate.fps;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法复制物理烘焙曲线"));
    }
}
