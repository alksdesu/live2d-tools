#include "framework/Model/CubismModelReplacement.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include "cubism_model_fixture.h"
#include "live2d/LAppModel.hpp"
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QFile>
#include <QScopeGuard>
#include "framework/Motion/CubismMotion.hpp"
#include "framework/Motion/CubismMotionQueueEntry.hpp"
#include "framework/Effect/CubismBreath.hpp"
#include "framework/Effect/CubismEyeBlink.hpp"

#include <QtTest>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <unordered_set>

using namespace Live2D::Cubism::Framework;
using Rendering::CubismRenderer;
namespace Core = Live2D::Cubism::Core;
using CubismModelFixture::GetterState;
using CubismModelFixture::State;

namespace {
struct Allocation
{
    std::size_t alignment;
    std::size_t size;
};

struct ModelRecord
{
    std::unique_ptr<GetterState> state;
    const Core::csmMoc* moc;
    int generation;
};

enum Failure {
    None, Consistency, Revival, ModelSize, ModelInitialize, StaticDefinition, NonFiniteOutput,
    RendererFactoryNull, RendererFactoryThrow, MocAllocationNull, MocAllocationThrow,
    ModelAllocationNull, ModelAllocationThrow, ModelWrapperThrow, ModelArrayThrow
};

struct Runtime
{
    std::unordered_map<void*, Allocation> allocations;
    std::unordered_map<const Core::csmMoc*, int> mocs;
    std::unordered_map<const Core::csmModel*, ModelRecord> models;
    std::unordered_set<const CubismRenderer*> renderers;
    QStringList events;
    QVector<int> releasedModelUpdates;
    QByteArray checkedBytes;
    Failure failure = None;
    bool editClipping = false;
    bool editParameterDefinition = false;
    bool rejectMetadataRenderer = false;
    std::function<void(GetterState&)> configureModel;
    float parameterMinimum = -1.0f, parameterMaximum = 1.0f, parameterDefault = 0.0f;
    int nextGeneration = 1;
    int lastModelGeneration = 0;
    int consistencyCalls = 0, reviveCalls = 0, initializeCalls = 0, rendererCalls = 0;
    int allocationCalls = 0, throwAllocationCall = -1;
};

Runtime runtime;
std::unique_ptr<CubismIdManager> idManager;
constexpr std::array<csmByte, 16> sourceBytes{{'M', 'O', 'C', '3', 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}};
constexpr std::array<csmByte, 16> candidateBytes{{'M', 'O', 'C', '3', 3, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0}};

void* allocate(std::size_t size, std::size_t alignment)
{
    ++runtime.allocationCalls;
    if (runtime.allocationCalls == runtime.throwAllocationCall) throw std::bad_alloc();
    if ((alignment == Core::csmAlignofMoc && runtime.failure == MocAllocationNull) ||
        (alignment == Core::csmAlignofModel && runtime.failure == ModelAllocationNull)) return nullptr;
    if ((alignment == Core::csmAlignofMoc && runtime.failure == MocAllocationThrow) ||
        (alignment == Core::csmAlignofModel && runtime.failure == ModelAllocationThrow)) throw std::bad_alloc();
    void* memory = alignment ? ::operator new(size, std::align_val_t(alignment), std::nothrow) : std::malloc(size);
    if (!memory) throw std::bad_alloc();
    runtime.allocations.emplace(memory, Allocation{alignment, size});
    return memory;
}

void release(void* memory, bool aligned)
{
    if (!memory) return;
    const auto allocation = runtime.allocations.find(memory);
    if (allocation == runtime.allocations.end() || bool(allocation->second.alignment) != aligned)
        qFatal("Invalid allocation ownership in replacement test");
    const auto* modelAddress = static_cast<const Core::csmModel*>(memory);
    const auto model = runtime.models.find(modelAddress);
    if (model != runtime.models.end()) {
        if (!runtime.mocs.count(model->second.moc)) qFatal("Moc released before its model");
        for (const auto* renderer : runtime.renderers)
            if (renderer->GetModel() && renderer->GetModel()->GetModel() == modelAddress)
                qFatal("Model released before its renderer");
        runtime.events.append(QStringLiteral("model:%1").arg(model->second.generation));
        runtime.releasedModelUpdates.append(model->second.state->updates);
        runtime.models.erase(model);
    }
    const auto* mocAddress = static_cast<const Core::csmMoc*>(memory);
    const auto moc = runtime.mocs.find(mocAddress);
    if (moc != runtime.mocs.end()) {
        for (const auto& modelEntry : runtime.models)
            if (modelEntry.second.moc == mocAddress) qFatal("Moc released with a live model");
        runtime.events.append(QStringLiteral("moc:%1").arg(moc->second));
        runtime.mocs.erase(moc);
    }
    const auto alignment = allocation->second.alignment;
    runtime.allocations.erase(allocation);
    if (alignment) ::operator delete(memory, std::align_val_t(alignment));
    else std::free(memory);
}

void checkAllocation(const void* address, unsigned int size, unsigned int alignment)
{
    const auto found = runtime.allocations.find(const_cast<void*>(address));
    if (found == runtime.allocations.end() || found->second.alignment != alignment ||
        found->second.size < size || reinterpret_cast<std::uintptr_t>(address) % alignment)
        qFatal("Core double received an invalid aligned buffer");
}

class FakeRenderer final : public CubismRenderer
{
public:
    std::function<bool()> drawHook;
    explicit FakeRenderer(int generation) : m_generation(generation) { runtime.renderers.insert(this); }
    ~FakeRenderer() override
    {
        if (GetModel() && !runtime.models.count(GetModel()->GetModel()))
            qFatal("Renderer outlived its Core model");
        runtime.events.append(QStringLiteral("renderer:%1").arg(m_generation));
        runtime.renderers.erase(this);
    }

protected:
    csmBool DoDrawModel() override
    {
        if (!drawHook) qFatal("Replacement preparation must not draw");
        return drawHook();
    }
    csmBool SaveProfile() override
    {
        if (!drawHook) qFatal("Replacement preparation must not draw");
        return true;
    }
    csmBool RestoreProfile() override { return true; }

private:
    int m_generation;
};

struct Resources
{
    CubismMoc* moc = nullptr;
    CubismModel* model = nullptr;
    CubismRenderer* renderer = nullptr;
    ~Resources()
    {
        CubismRenderer::Delete(renderer);
        if (moc) moc->DeleteModel(model);
        CubismMoc::Delete(moc);
    }
};

std::unique_ptr<Resources> createSource()
{
    auto result = std::make_unique<Resources>();
    result->moc = CubismMoc::Create(sourceBytes.data(), sourceBytes.size(), true);
    if (!result->moc) throw std::runtime_error("Source MOC creation failed");
    result->model = result->moc->CreateModel();
    if (!result->model) throw std::runtime_error("Source model creation failed");
    result->model->SetParameterValue(0, 0.2f);
    result->model->SetParameterValue(1, 0.3f);
    result->model->SaveParameters();
    result->model->SetParameterValue(0, 0.7f);
    result->model->SetParameterValue(1, 0.8f);
    result->model->SetPartOpacity(0, 0.45f);
    result->model->SetModelOpacity(0.6f);
    result->model->SetParameterValue(idManager->GetId("VirtualParameter"), 2.5f);
    result->model->InitMesh();
    auto* mesh = result->model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
    if (!mesh) throw std::runtime_error("Source mesh creation failed");
    mesh->SetSelect(QRectF(-1.1, -1.1, 0.2, 0.2));
    result->model->GetMeshMap()->SetShow(QStringLiteral("MeshA"));
    if (!result->model->SetDrawableIndicesOverride(0, {0, 1, 3, 1, 2, 3}))
        throw std::runtime_error("Source index override failed");
    result->renderer = CubismRenderer::Create();
    if (!result->renderer || !result->renderer->Initialize(result->model))
        throw std::runtime_error("Source renderer creation failed");
    result->renderer->SetModelColor(0.2f, 0.4f, 0.6f, 0.8f);
    return result;
}

CubismModelReplacement::RendererPreparation normalPreparation()
{
    return [](CubismRenderer* candidate, CubismModel* model, const CubismRenderer& source, QString*) {
        const auto color = source.GetModelColor();
        candidate->SetModelColor(color.R, color.G, color.B, color.A);
        return candidate->Initialize(model);
    };
}

bool prepare(CubismModelReplacement& replacement, const Resources& source,
             const CubismModelReplacement::RendererPreparation& callback, QString* error = nullptr)
{
    return replacement.Prepare(candidateBytes.data(), candidateBytes.size(), *source.moc, *source.model,
                               *source.renderer, callback, error);
}

void verifySourceUnchanged(Resources& source, CDrawMesh* mesh, CPoint* selected)
{
    QCOMPARE(source.renderer->GetModel(), source.model);
    QCOMPARE(source.model->GetParameterValue(0), 0.7f);
    QCOMPARE(source.model->GetParameterValue(1), 0.8f);
    QCOMPARE(source.model->GetPartOpacity(0), 0.45f);
    QCOMPARE(source.model->GetModelOpacity(), 0.6f);
    QCOMPARE(source.model->GetParameterValue(idManager->GetId("VirtualParameter")), 2.5f);
    QCOMPARE(source.model->GetDrawableVertexIndices(0)[2], quint16(3));
    QCOMPARE(source.model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA")), mesh);
    QCOMPARE(mesh->GetSelectPoint().size(), size_t(1));
    QCOMPARE(mesh->GetSelectPoint().front(), selected);
    QCOMPARE(selected->GetPoint(), QVector2D(-1, -1));
    QCOMPARE(State(source.model->GetModel()).updates, 0);
    source.model->LoadParameters();
    QCOMPARE(source.model->GetParameterValue(0), 0.2f);
    QCOMPARE(source.model->GetParameterValue(1), 0.3f);
    source.model->SetParameterValue(0, 0.7f);
    source.model->SetParameterValue(1, 0.8f);
}
}

void* operator new(csmSizeType size, CubismAllocationTag) { return allocate(size, 0); }
void operator delete(void* memory, CubismAllocationTag) { release(memory, false); }

namespace Live2D { namespace Cubism { namespace Framework {
void* CubismFramework::Allocate(csmSizeType size) { return allocate(size, 0); }
void CubismFramework::Deallocate(void* memory) { release(memory, false); }
void* CubismFramework::AllocateAligned(csmSizeType size, csmUint32 alignment) { return allocate(size, alignment); }
void CubismFramework::DeallocateAligned(void* memory) { release(memory, true); }
CubismIdManager* CubismFramework::GetIdManager() { return idManager.get(); }
void CubismFramework::Initialize() { Utils::Value::StaticInitializeNotForClientCall(); }
void CubismFramework::Dispose() { Utils::Value::StaticReleaseNotForClientCall(); }
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}
namespace Rendering {
CubismRenderer* CubismRenderer::Create()
{
    ++runtime.rendererCalls;
    if (runtime.failure == RendererFactoryNull) return nullptr;
    if (runtime.failure == RendererFactoryThrow) throw std::runtime_error("Controlled renderer factory failure");
    return CSM_NEW FakeRenderer(runtime.lastModelGeneration);
}
}
}}}

namespace Live2D { namespace Cubism { namespace Core {
// Only the Core boundary is replaced; Moc, Model and replacement ownership run production code.
int csmCallingConvention csmHasMocConsistency(void* address, unsigned int size)
{
    checkAllocation(address, size, csmAlignofMoc);
    ++runtime.consistencyCalls;
    runtime.checkedBytes = QByteArray(static_cast<const char*>(address), size);
    return runtime.failure == Consistency ? 0 : 1;
}
csmMoc* csmCallingConvention csmReviveMocInPlace(void* address, unsigned int size)
{
    checkAllocation(address, size, csmAlignofMoc);
    ++runtime.reviveCalls;
    if (runtime.failure == Revival) return nullptr;
    auto* moc = static_cast<csmMoc*>(address);
    runtime.mocs.emplace(moc, runtime.nextGeneration++);
    return moc;
}
csmMocVersion csmCallingConvention csmGetMocVersion(const void*, unsigned int) { return csmMocVersion_40; }
csmMocVersion csmCallingConvention csmGetLatestMocVersion() { return csmMocVersion_50; }
unsigned int csmCallingConvention csmGetSizeofModel(const csmMoc* moc)
{
    if (!runtime.mocs.count(moc)) qFatal("Unknown MOC in model allocation");
    return runtime.failure == ModelSize ? 0 : sizeof(CubismModelFixture::ModelHandle);
}
csmModel* csmCallingConvention csmInitializeModelInPlace(const csmMoc* moc, void* address, unsigned int size)
{
    checkAllocation(address, size, csmAlignofModel);
    if (!runtime.mocs.count(moc) || size != sizeof(CubismModelFixture::ModelHandle))
        qFatal("Invalid MOC or model size in replacement fixture");
    ++runtime.initializeCalls;
    if (runtime.failure == ModelInitialize) return nullptr;
    const int generation = runtime.mocs.at(moc);
    auto data = std::make_unique<GetterState>();
    const auto* bytes = reinterpret_cast<const unsigned char*>(moc);
    if (bytes[7] == 2) {
        if (runtime.editParameterDefinition) {
            data->minimum[0] = runtime.parameterMinimum;
            data->maximum[0] = runtime.parameterMaximum;
            data->defaults[0] = runtime.parameterDefault;
        }
        for (auto& point : data->positions[0]) { point.X += 0.5f; point.Y += 0.25f; }
        if (runtime.editClipping) {
            data->maskCounts[0] = 1;
            data->masks[0][0] = 1;
            data->flags[0] ^= csmIsInvertedMask;
        }
    }
    if (runtime.configureModel) runtime.configureModel(*data);
    if (runtime.failure == StaticDefinition) data->flags[0] ^= csmBlendAdditive;
    if (runtime.failure == NonFiniteOutput) data->positions[1][3].X = std::numeric_limits<float>::quiet_NaN();
    auto* model = CubismModelFixture::AttachModel(address, data.get());
    runtime.models.emplace(model, ModelRecord{std::move(data), moc, generation});
    runtime.lastModelGeneration = generation;
    if (runtime.failure == ModelWrapperThrow) runtime.throwAllocationCall = runtime.allocationCalls + 1;
    if (runtime.failure == ModelArrayThrow) runtime.throwAllocationCall = runtime.allocationCalls + 2;
    return model;
}
}}}

namespace Live2D { namespace Cubism { namespace Framework {
CubismUserModel::CubismUserModel()
    : _moc(nullptr), _model(nullptr), _motionManager(nullptr), _expressionManager(nullptr), _eyeBlink(nullptr),
      _breath(nullptr), _modelMatrix(nullptr), _pose(nullptr), _dragManager(nullptr), _physics(nullptr),
      _modelUserData(nullptr), _initialized(false), _updating(false), _opacity(1), _lipSync(false),
      _lastLipSyncValue(0), _dragX(0), _dragY(0), _accelerationX(0), _accelerationY(0), _accelerationZ(0),
      _mocConsistency(false), _debugMode(false), _renderer(nullptr) {}
CubismUserModel::~CubismUserModel()
{
    CubismRenderer::Delete(_renderer);
    if (_moc && _model) _moc->DeleteModel(_model);
    CubismMoc::Delete(_moc);
    CubismPhysics::Delete(_physics);
    CubismPose::Delete(_pose);
    CSM_DELETE(_modelMatrix);
}
csmBool CubismUserModel::IsInitialized() { return _initialized; }
void CubismUserModel::IsInitialized(csmBool value) { _initialized = value; }
csmBool CubismUserModel::IsUpdating() { return _updating; }
void CubismUserModel::IsUpdating(csmBool value) { _updating = value; }
void CubismUserModel::SetDragging(csmFloat32, csmFloat32) { qFatal("Unexpected effect in metadata test"); }
void CubismUserModel::SetAcceleration(csmFloat32, csmFloat32, csmFloat32) { qFatal("Unexpected effect in metadata test"); }
void CubismUserModel::SetOpacity(csmFloat32) { qFatal("Unexpected effect in metadata test"); }
csmFloat32 CubismUserModel::GetOpacity() { return _opacity; }
csmBool CubismUserModel::LoadModel(const csmByte*, csmSizeInt, csmBool) { qFatal("Unexpected model loading in metadata test"); }
ACubismMotion* CubismUserModel::LoadMotion(const csmByte*, csmSizeInt, const csmChar*, ACubismMotion::FinishedMotionCallback) { qFatal("Unexpected effect in metadata test"); }
ACubismMotion* CubismUserModel::LoadExpression(const csmByte*, csmSizeInt, const csmChar*) { qFatal("Unexpected effect in metadata test"); }
void CubismUserModel::LoadPose(const csmByte*, csmSizeInt) { qFatal("Unexpected effect in metadata test"); }
void CubismUserModel::LoadPhysics(const csmByte*, csmSizeInt) { qFatal("Unexpected effect in metadata test"); }
void CubismUserModel::LoadUserData(const csmByte*, csmSizeInt) { qFatal("Unexpected effect in metadata test"); }
csmBool CubismUserModel::IsHit(CubismIdHandle, csmFloat32, csmFloat32) { qFatal("Unexpected interaction in metadata test"); }
void CubismUserModel::MotionEventFired(const csmString&) { qFatal("Unexpected effect in metadata test"); }
}}}

LAppModel::LAppModel() : _modelSetting(nullptr), _userTimeSeconds(23.0f), _idParamAngleX(nullptr), _idParamAngleY(nullptr),
    _idParamAngleZ(nullptr), _idParamBodyAngleX(nullptr), _idParamEyeBallX(nullptr), _idParamEyeBallY(nullptr) {}
LAppModel::~LAppModel() = default;
void LAppModel::MotionEventFired(const csmString&) { qFatal("Unexpected effect in metadata test"); }
csmBool LAppModel::HitTest(const csmChar*, csmFloat32, csmFloat32) { qFatal("Unexpected interaction in metadata test"); }
LAppWavFileHandler::LAppWavFileHandler()
    : _rawData(nullptr), _pcmData(nullptr), _userTimeSeconds(0), _lastRms(0), _sampleOffset(0) {}
LAppWavFileHandler::~LAppWavFileHandler() = default;

class ModelReplacementTests : public QObject
{
    Q_OBJECT
    friend class LAppModel;
    static bool publishPreview(LAppModel& preview, const QByteArray& bytes, const QByteArray& sourceDigest,
                               const CubismModelReplacement::ModelPreparation& prepareModel,
                               QString* error, CubismModelUpdatePolicy policy, const ParameterStructureSeeds& seeds,
                               const ParameterPhysicsReferences* physicsReferences, const LAppModel::PhysicsDraft* physicsDraft,
                               const CMeshGeometryMigration* geometry)
    {
        if (physicsDraft) qFatal("Physics draft integration is outside this renderer-boundary fixture");
        Q_UNUSED(sourceDigest);
        const auto prepareRenderer = [&](CubismRenderer* renderer, CubismModel* model, const CubismRenderer& source, QString* detail) {
            if (runtime.rejectMetadataRenderer) { if (detail) *detail = QStringLiteral("GPU preparation rejected"); return false; }
            return normalPreparation()(renderer, model, source, detail);
        };
        if (!preview.ReplaceModel(reinterpret_cast<const csmByte*>(bytes.constData()), bytes.size(),
            prepareRenderer, policy, seeds, error, prepareModel, physicsReferences, {}, geometry)) return false;
        preview.m_previewMocBytes = bytes;
        preview.m_previewMocDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        return true;
    }

    static QByteArray metadataBytes()
    {
        return QByteArray(reinterpret_cast<const char*>(candidateBytes.data()), candidateBytes.size());
    }
    static void setupPreview(LAppModel& preview)
    {
        auto source = createSource();
        preview._moc = std::exchange(source->moc, nullptr);
        preview._model = std::exchange(source->model, nullptr);
        preview._renderer = std::exchange(source->renderer, nullptr);
        preview._initialized = true;
        preview._modelMatrix = CSM_NEW CubismModelMatrix(5.0f, 4.0f);
        preview.m_sourceMocDigest = QByteArray("original-source-digest");
        preview.m_previewMocBytes = QByteArray(reinterpret_cast<const char*>(sourceBytes.data()), sourceBytes.size());
        preview.m_previewMocDigest = QCryptographicHash::hash(preview.m_previewMocBytes, QCryptographicHash::Sha256);
        preview.m_mapParaToIndex = {{QStringLiteral("ParamA"), 0}, {QStringLiteral("ParamB"), 1}};
        State(preview._model->GetModel()).minimum[0] = -2.0f;
        State(preview._model->GetModel()).maximum[0] = 2.0f;
        preview._model->SetParameterValue(0, 1.5f);
        preview._model->SaveParameters();
        preview._model->SetParameterValue(0, 1.75f);
        runtime.editParameterDefinition = true;
    }
    static CubismModel::RuntimeValueSnapshot snapshot(const LAppModel& preview)
    {
        CubismModel::RuntimeValueSnapshot result;
        if (!preview._model->CaptureRuntimeValues(result)) qFatal("Unable to capture fixture model");
        return result;
    }
    static QByteArray physicsBytes()
    {
        return R"({"Version":3,"Meta":{"PhysicsSettingCount":1,"TotalInputCount":1,"TotalOutputCount":1,"VertexCount":2,"Fps":30,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}}},"PhysicsSettings":[{"Id":"one","Input":[{"Source":{"Target":"Parameter","Id":"ParamA"},"Weight":100,"Type":"X","Reflect":true}],"Output":[{"Destination":{"Target":"Parameter","Id":"ParamB"},"VertexIndex":1,"Scale":1,"Weight":100,"Type":"Angle","Reflect":false}],"Vertices":[{"Position":{"X":0,"Y":0},"Mobility":1,"Delay":1,"Acceleration":1,"Radius":0},{"Position":{"X":0,"Y":1},"Mobility":0.8,"Delay":0.5,"Acceleration":1,"Radius":1}],"Normalization":{"Position":{"Minimum":-1,"Maximum":1,"Default":0},"Angle":{"Minimum":-30,"Maximum":30,"Default":0}}}]})";
    }
    static CubismUserModel::PhysicsOwner makePhysics(float wind)
    {
        const auto bytes = physicsBytes();
        CubismUserModel::PhysicsOwner result(CubismPhysics::Create(
            reinterpret_cast<const csmByte*>(bytes.constData()), csmSizeInt(bytes.size())));
        if (!result) return result;
        auto options = result->GetOptions();
        options.Wind.X = wind;
        result->SetOptions(options);
        return result;
    }
    static void setupPhysicsPreview(LAppModel& preview)
    {
        auto source = createSource();
        preview._moc = std::exchange(source->moc, nullptr);
        preview._model = std::exchange(source->model, nullptr);
        preview._renderer = std::exchange(source->renderer, nullptr);
        preview._modelMatrix = CSM_NEW CubismModelMatrix(5.0f, 4.0f);
        preview._initialized = true;
        preview._physics = makePhysics(0.45f).release();
        if (!preview._physics) qFatal("Cannot create source physics");
        preview._physics->Evaluate(preview._model, 1.0f / 30.0f);
    }
    static void compareValues(const CubismModel::RuntimeValueSnapshot& actual,
                              const CubismModel::RuntimeValueSnapshot& expected)
    {
        QCOMPARE(actual.valid, expected.valid);
        QCOMPARE(actual.parameterIds, expected.parameterIds);
        QCOMPARE(actual.parameterValues, expected.parameterValues);
        QCOMPARE(actual.savedParameters, expected.savedParameters);
        QCOMPARE(actual.savedPresence, expected.savedPresence);
        QCOMPARE(actual.minimums, expected.minimums);
        QCOMPARE(actual.maximums, expected.maximums);
        QCOMPARE(actual.parameterTypes, expected.parameterTypes);
        QCOMPARE(actual.virtualParameterIds, expected.virtualParameterIds);
        QCOMPARE(actual.virtualParameterValues, expected.virtualParameterValues);
        QCOMPARE(actual.partIds, expected.partIds);
        QCOMPARE(actual.partOpacities, expected.partOpacities);
        QCOMPARE(actual.virtualPartIds, expected.virtualPartIds);
        QCOMPARE(actual.virtualPartOpacities, expected.virtualPartOpacities);
        QCOMPARE(actual.modelOpacity, expected.modelOpacity);
    }
private slots:
    void physicsCandidatePublishesAfterModelAndRenderer_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("replace-rig") << 0;
        QTest::newRow("clear-rig") << 1;
        QTest::newRow("omitted-callback-keeps-rig") << 2;
    }

    void physicsCandidatePublishesAfterModelAndRenderer()
    {
        QFETCH(int, mode);
        LAppModel preview;
        setupPhysicsPreview(preview);
        const auto before = snapshot(preview);
        auto* oldModel = preview._model;
        auto* oldCore = oldModel->GetModel();
        auto* oldRenderer = preview._renderer;
        auto* oldPhysics = preview._physics;
        auto* oldMatrix = preview._modelMatrix;
        auto* oldMesh = oldModel->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        const auto selected = oldMesh->GetSelectPoint().front()->GetPoint();
        CubismPhysics* preparedPhysics = nullptr;
        CubismModel* preparedModel = nullptr;
        int physicsCalls = 0;
        bool observedPreparedResources = false;
        const auto callback = [&](const CubismModel& source, const CubismModel& candidate,
                                  CubismUserModel::PhysicsOwner& physics, QString*) {
            ++physicsCalls;
            observedPreparedResources = &source == oldModel && &candidate != oldModel &&
                preview._model == oldModel && preview._renderer == oldRenderer && preview._physics == oldPhysics &&
                State(candidate.GetModel()).updates == 1 && runtime.renderers.size() == 2;
            preparedModel = const_cast<CubismModel*>(&candidate);
            if (mode == 0) physics = makePhysics(0.7f);
            preparedPhysics = physics.get();
            return mode == 1 || bool(physics);
        };
        QString error;
        QVERIFY2(preview.ReplaceModel(candidateBytes.data(), candidateBytes.size(), normalPreparation(),
            CubismModelUpdatePolicy::Strict, {}, &error, {}, nullptr,
            mode == 2 ? CubismUserModel::PhysicsPreparation{} : callback), qPrintable(error));
        QCOMPARE(physicsCalls, mode == 2 ? 0 : 1);
        if (mode != 2) {
            QVERIFY(observedPreparedResources);
            QCOMPARE(preview._model, preparedModel);
            QCOMPARE(preview._physics, preparedPhysics);
            QVERIFY(!runtime.allocations.count(oldPhysics));
        } else QCOMPARE(preview._physics, oldPhysics);
        if (mode == 0) QCOMPARE(preview._physics->GetOptions().Wind.X, 0.7f);
        if (mode == 1) QVERIFY(!preview._physics);
        QVERIFY(preview._model != oldModel);
        QVERIFY(preview._renderer != oldRenderer);
        QVERIFY(!runtime.models.count(oldCore));
        QVERIFY(!runtime.renderers.count(oldRenderer));
        QCOMPARE(preview._renderer->GetModel(), preview._model);
        QCOMPARE(preview._modelMatrix, oldMatrix);
        QCOMPARE(State(preview._model->GetModel()).updates, 1);
        compareValues(snapshot(preview), before);
        auto* mesh = preview._model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        QCOMPARE(mesh->GetSelectPoint().size(), size_t(1));
        QCOMPARE(mesh->GetSelectPoint().front()->GetPoint(), selected + QVector2D(0.5f, 0.25f));
        QVERIFY(!preview._updating);
    }

    void physicsCandidateFailurePreservesSource_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("callback-false-before-allocation") << 0;
        QTest::newRow("callback-false-with-owned-rig") << 1;
        QTest::newRow("callback-throws-with-owned-rig") << 2;
        QTest::newRow("commit-source-identity-changed") << 3;
        QTest::newRow("renderer-fails-before-physics") << 4;
    }

    void physicsCandidateFailurePreservesSource()
    {
        QFETCH(int, failure);
        LAppModel preview;
        setupPhysicsPreview(preview);
        const auto before = snapshot(preview);
        auto* oldMoc = preview._moc;
        auto* oldModel = preview._model;
        auto* oldCore = oldModel->GetModel();
        auto* oldRenderer = preview._renderer;
        auto* oldPhysics = preview._physics;
        auto* oldMatrix = preview._modelMatrix;
        const auto digest = oldPhysics->SourceDigest();
        auto* oldMesh = oldModel->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        auto* selected = oldMesh->GetSelectPoint().front();
        const auto allocationCount = runtime.allocations.size();
        const auto rendererCount = runtime.renderers.size();
        const auto modelCount = runtime.models.size();
        const auto mocCount = runtime.mocs.size();
        CubismPhysics* discarded = nullptr;
        int physicsCalls = 0;
        const auto callback = [&](const CubismModel&, const CubismModel&,
                                  CubismUserModel::PhysicsOwner& physics, QString* detail) {
            ++physicsCalls;
            if (failure != 0) physics = makePhysics(0.7f);
            discarded = physics.get();
            if (failure == 2) throw std::runtime_error("controlled physics preparation exception");
            if (failure == 3) {
                preview._moc = nullptr;
                return true;
            }
            if (detail) *detail = QStringLiteral("controlled physics preparation rejection");
            return false;
        };
        const auto renderer = [&](CubismRenderer* candidate, CubismModel* model,
                                  const CubismRenderer& previous, QString* detail) {
            if (failure == 4) { if (detail) *detail = QStringLiteral("controlled renderer rejection"); return false; }
            return normalPreparation()(candidate, model, previous, detail);
        };
        QString error;
        const bool accepted = preview.ReplaceModel(candidateBytes.data(), candidateBytes.size(), renderer,
            CubismModelUpdatePolicy::Strict, {}, &error, {}, nullptr, callback);
        const bool externalIdentityRetained = failure != 3 || !preview._moc;
        preview._moc = oldMoc;
        QVERIFY(!accepted);
        QVERIFY(!error.isEmpty());
        QVERIFY(externalIdentityRetained);
        QCOMPARE(physicsCalls, failure == 4 ? 0 : 1);
        QCOMPARE(preview._model, oldModel);
        QCOMPARE(preview._renderer, oldRenderer);
        QCOMPARE(preview._physics, oldPhysics);
        QCOMPARE(preview._modelMatrix, oldMatrix);
        QCOMPARE(preview._physics->SourceDigest(), digest);
        QCOMPARE(preview._physics->GetOptions().Wind.X, 0.45f);
        QCOMPARE(runtime.allocations.size(), allocationCount);
        QCOMPARE(runtime.renderers.size(), rendererCount);
        QCOMPARE(runtime.models.size(), modelCount);
        QCOMPARE(runtime.mocs.size(), mocCount);
        if (discarded) QVERIFY(!runtime.allocations.count(discarded));
        QVERIFY(runtime.models.count(oldCore));
        QCOMPARE(State(oldCore).updates, 0);
        compareValues(snapshot(preview), before);
        QCOMPARE(oldModel->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA")), oldMesh);
        QCOMPARE(oldMesh->GetSelectPoint().front(), selected);
        QVERIFY(!preview._updating);
        QVERIFY2(preview.ReplaceModel(candidateBytes.data(), candidateBytes.size(), normalPreparation(),
            CubismModelUpdatePolicy::Strict, {}, &error, {}, nullptr,
            [&](const CubismModel&, const CubismModel&, CubismUserModel::PhysicsOwner& physics, QString*) {
                physics = makePhysics(0.7f);
                return bool(physics);
            }), qPrintable(error));
        QCOMPARE(preview._physics->GetOptions().Wind.X, 0.7f);
    }

    void editorContextRestoresAfterStructuralExit_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("success") << 0;
        QTest::newRow("invalid-pose") << 1;
        QTest::newRow("renderer-refusal") << 2;
        QTest::newRow("wrong-source") << 3;
    }

    void editorContextRestoresAfterStructuralExit()
    {
        QFETCH(int, failure);
        LAppModel preview;
        setupPreview(preview);
        const auto entry = snapshot(preview);
        preview.m_editorEntryState = std::make_unique<CubismModel::RuntimeValueSnapshot>(entry);
        preview.m_editorBaseParameters = entry.Parameters();
        preview.m_editorPose = {{"ParamA", 0.5f}, {"ParamB", 0.7f}};
        QVERIFY(preview._model->ApplyParameterPose(preview.m_editorPose));
        preview._model->SetPartOpacity(0, 0.35f);
        preview._model->SetModelOpacity(0.45f);
        LAppModel::EditorPreviewContext context;
        ParameterStructureSeeds seeds;
        QString error;
        QVERIFY(preview.CaptureEditorPreviewContext(context, &error));
        QVERIFY(preview.CaptureParameterStructureSeeds({"ParamA"}, seeds, &error));
        runtime.configureModel = [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); };
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest, {}, {}, false, &error), qPrintable(error));
        QVERIFY(!preview.HasEditorPreview());
        const auto previous = snapshot(preview);
        auto* previousModel = preview._model;
        runtime.configureModel = [](GetterState& state) {
            CubismModelFixture::SetParameterLayout(state, {"ParamA", "ParamB"});
            state.minimum[0] = -2; state.maximum[0] = 2;
        };
        if (failure == 1) context.pose["ParamA"] = std::numeric_limits<float>::quiet_NaN();
        if (failure == 2) runtime.rejectMetadataRenderer = true;
        if (failure == 3) context.sourceDigest = "foreign";
        const bool accepted = preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            seeds, {}, true, &error, {}, nullptr, &context);
        QVERIFY2(accepted == (failure == 0), qPrintable(error));
        if (failure) {
            QVERIFY(!preview.HasEditorPreview());
            QCOMPARE(preview._model, previousModel);
            QCOMPARE(snapshot(preview).parameterValues, previous.parameterValues);
            QCOMPARE(snapshot(preview).savedParameters, previous.savedParameters);
            QVERIFY(!error.isEmpty());
            return;
        }
        QVERIFY(preview.HasEditorPreview());
        QCOMPARE(preview.m_editorPose, context.pose);
        QCOMPARE(preview.m_editorBaseParameters, context.base);
        QCOMPARE(preview.m_editorEntryState->parameterValues, entry.parameterValues);
        QCOMPARE(preview.m_editorEntryState->savedParameters, entry.savedParameters);
        QCOMPARE(snapshot(preview).parameterValues, QVector<float>({0.5f, 0.7f}));
        QCOMPARE(snapshot(preview).savedParameters, entry.savedParameters);
        QCOMPARE(snapshot(preview).partOpacities, context.current.partOpacities);
        QCOMPARE(snapshot(preview).modelOpacity, context.current.modelOpacity);
        QVERIFY(preview._model->RestoreRuntimeValues(*preview.m_editorEntryState));
        QCOMPARE(snapshot(preview).parameterValues, entry.parameterValues);
    }

    void referenceMasksCommitWithModelAndSurviveSameIdRecreation_data()
    {
        QTest::addColumn<bool>("rejectRenderer");
        QTest::newRow("commit") << false;
        QTest::newRow("GPU-failure") << true;
    }

    void referenceMasksCommitWithModelAndSurviveSameIdRecreation()
    {
        QFETCH(bool, rejectRenderer);
        LAppModel preview;
        setupPreview(preview);
        runtime.editParameterDefinition = false;
        State(preview._model->GetModel()).minimum[0] = 0;
        State(preview._model->GetModel()).maximum[0] = 1;
        preview._model->SetParameterValue(0, 0.1f);
        preview._model->SaveParameters();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        preview._modelHomeDir = directory.path().toUtf8().constData();
        preview.m_modelSettingsDigest = QByteArray(32, 'm');
        const QByteArray motionBytes = R"({"Version":3,"Meta":{"Duration":10,"Fps":30,"Loop":false,"AreBeziersRestricted":true,"CurveCount":2,"TotalSegmentCount":2,"TotalPointCount":4,"UserDataCount":0,"TotalUserDataSize":0},"FadeInTime":0,"FadeOutTime":0,"Curves":[{"Target":"Parameter","Id":"ParamA","Segments":[0,0.9,0,10,0.9]},{"Target":"PartOpacity","Id":"ProxyPart","Segments":[0,0.35,0,10,0.35]}]})";
        const auto write = [&](const QString& name, const QByteArray& bytes) {
            QFile file(directory.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        QVERIFY(write("first.motion3.json", motionBytes));
        QVERIFY(write("second.motion3.json", motionBytes));
        auto* first = CubismMotion::Create(reinterpret_cast<const csmByte*>(motionBytes.constData()), motionBytes.size());
        auto* second = CubismMotion::Create(reinterpret_cast<const csmByte*>(motionBytes.constData()), motionBytes.size());
        QVERIFY(first && second);
        const auto cleanupMotions = qScopeGuard([&] { ACubismMotion::Delete(first); ACubismMotion::Delete(second); });
        QVERIFY(preview.ConfigureMotionReferences(first, "first.motion3.json"));
        QVERIFY(preview.ConfigureMotionReferences(second, "second.motion3.json"));
        preview._motions["first"] = first;
        preview._motions["second"] = second;
        ParameterReferenceCatalog catalog;
        QString catalogError;
        QVERIFY2(preview.CaptureParameterReferenceCatalog(catalog, &catalogError), qPrintable(catalogError));
        QCOMPARE(catalog.motions.size(), 2);
        QCOMPARE(catalog.motions.value("first.motion3.json"), first->SourceDigest());
        QCOMPARE(catalog.modelSettingsDigest, preview.m_modelSettingsDigest);
        const auto catalogBefore = catalog.motions;
        preview.m_modelSettingsDigest.clear();
        QVERIFY(!preview.CaptureParameterReferenceCatalog(catalog, &catalogError));
        QCOMPARE(catalog.motions, catalogBefore);
        preview.m_modelSettingsDigest = QByteArray(32, 'm');
        ParameterReferenceState references;
        references.modelSettingsDigest = preview.m_modelSettingsDigest;
        references.motions["first.motion3.json"] = {first->SourceDigest(), {{0, "ParamA"}}};
        runtime.configureModel = [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); };
        runtime.rejectMetadataRenderer = rejectRenderer;
        auto* oldModel = preview._model;
        QString error;
        const bool accepted = preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {}, {}, false, &error, {}, &references);
        QCOMPARE(accepted, !rejectRenderer);
        CubismMotionQueueEntry entry;
        entry.SetStartTime(0);
        if (rejectRenderer) {
            QCOMPARE(preview._model, oldModel);
            QVERIFY(preview.GetParameterReferenceState().motions.isEmpty());
            first->DoUpdateParameters(preview._model, 1, 1, &entry);
            QVERIFY(qAbs(preview._model->GetParameterValue(0) - 0.9f) < 1e-6f);
            return;
        }
        QCOMPARE(preview.GetParameterReferenceState().motions.size(), 1);
        const auto paramA = idManager->GetId("ParamA");
        const auto partProxy = idManager->GetId("ProxyPart");
        preview._model->SetParameterValue(paramA, 0.2f);
        first->DoUpdateParameters(preview._model, 1, 1, &entry);
        QCOMPARE(preview._model->GetParameterValue(paramA), 0.2f);
        QVERIFY(qAbs(preview._model->GetParameterValue(partProxy) - 0.35f) < 1e-6f);
        second->DoUpdateParameters(preview._model, 1, 1, &entry);
        QVERIFY(qAbs(preview._model->GetParameterValue(paramA) - 0.9f) < 1e-6f);
        runtime.configureModel = [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamA", "ParamB"}); };
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {{"ParamA", ParameterStructureSeed{}}}, {}, false, &error), qPrintable(error));
        preview._model->SetParameterValue(0, 0.15f);
        first->DoUpdateParameters(preview._model, 2, 1, &entry);
        QCOMPARE(preview._model->GetParameterValue(0), 0.15f);
        ParameterReferenceState cleared;
        cleared.modelSettingsDigest = preview.m_modelSettingsDigest;
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {}, {}, false, &error, {}, &cleared), qPrintable(error));
        first->DoUpdateParameters(preview._model, 3, 1, &entry);
        QVERIFY(qAbs(preview._model->GetParameterValue(0) - 0.9f) < 1e-6f);
    }

    void expressionSuppressionSkipsPersistentManagerCache()
    {
        auto source = createSource();
        const QByteArray bytes = R"({"Type":"Live2D Expression","FadeInTime":0,"FadeOutTime":0,"Parameters":[{"Id":"ParamA","Value":0.8,"Blend":"Overwrite"}]})";
        auto* expression = CubismExpressionMotion::Create(reinterpret_cast<const csmByte*>(bytes.constData()), bytes.size());
        QVERIFY(expression);
        const auto cleanupExpression = qScopeGuard([&] { ACubismMotion::Delete(expression); });
        CubismExpressionMotionManager manager;
        manager.StartMotionPriority(expression, false, 1);
        QVERIFY(manager.UpdateMotion(source->model, 0.1f));
        QVERIFY(qAbs(source->model->GetParameterValue(0) - 0.8f) < 1e-6f);
        QVector<quint8> mask;
        QString error;
        QVERIFY(expression->PrepareParameterReferences({expression->SourceDigest(), {{0, "ParamA"}}}, mask, &error));
        expression->CommitParameterReferences(mask);
        QSet<CubismIdHandle> affected{idManager->GetId("ParamA")};
        manager.CommitParameterReferenceSuppression(affected);
        source->model->SetParameterValue(0, 0.25f);
        QVERIFY(manager.UpdateMotion(source->model, 0.1f));
        QCOMPARE(source->model->GetParameterValue(0), 0.25f);
        manager.StopAllMotions();
        source->model->SetParameterValue(0, 0.4f);
        manager.UpdateMotion(source->model, 0.1f);
        QCOMPARE(source->model->GetParameterValue(0), 0.4f);
        QVERIFY(expression->PrepareParameterReferences({}, mask, &error));
        expression->CommitParameterReferences(mask);
        affected.clear(); manager.CommitParameterReferenceSuppression(affected);
        manager.StartMotionPriority(expression, false, 1);
        QVERIFY(manager.UpdateMotion(source->model, 0.1f));
        QVERIFY(qAbs(source->model->GetParameterValue(0) - 0.8f) < 1e-6f);
    }

    void runtimeReferencePlansRejectStaleOrIncorrectRows_data()
    {
        QTest::addColumn<int>("kind");
        for (const char* name : {"wrong-hash", "changed-file", "unknown-path", "wrong-id", "wrong-target", "wrong-row", "wrong-model"})
            QTest::newRow(name) << (QStringList{"wrong-hash", "changed-file", "unknown-path", "wrong-id", "wrong-target", "wrong-row", "wrong-model"}.indexOf(name));
    }

    void runtimeReferencePlansRejectStaleOrIncorrectRows()
    {
        QFETCH(int, kind);
        LAppModel preview;
        setupPreview(preview);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        preview._modelHomeDir = directory.path().toUtf8().constData();
        preview.m_modelSettingsDigest = QByteArray(32, 'm');
        const QByteArray bytes = R"({"Version":3,"Meta":{"Duration":10,"Fps":30,"Loop":false,"AreBeziersRestricted":true,"CurveCount":1,"TotalSegmentCount":1,"TotalPointCount":2,"UserDataCount":0,"TotalUserDataSize":0},"Curves":[{"Target":"PartOpacity","Id":"ParamA","Segments":[0,0.3,0,10,0.3]}]})";
        QFile file(directory.filePath("pose.motion3.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size()); file.close();
        auto* motion = CubismMotion::Create(reinterpret_cast<const csmByte*>(bytes.constData()), bytes.size());
        const auto cleanupMotion = qScopeGuard([&] { ACubismMotion::Delete(motion); });
        QVERIFY(preview.ConfigureMotionReferences(motion, "pose.motion3.json"));
        preview._motions["pose"] = motion;
        ParameterReferenceState state;
        state.modelSettingsDigest = preview.m_modelSettingsDigest;
        ParameterResourceReferences rows{motion->SourceDigest(), {}};
        if (kind == 0) rows.sourceDigest = QByteArray(32, 'x');
        if (kind == 1) {
            QVERIFY(file.open(QIODevice::Append)); QVERIFY(file.write(" ") == 1); file.close();
        }
        if (kind == 3) rows.suppressedParameters = {{0, "different"}};
        if (kind == 4) rows.suppressedParameters = {{0, "ParamA"}};
        if (kind == 5) rows.suppressedParameters = {{1, "ParamA"}};
        if (kind == 6) state.modelSettingsDigest = QByteArray(32, 'x');
        state.motions.insert(kind == 2 ? "missing.motion3.json" : "pose.motion3.json", rows);
        const auto before = snapshot(preview);
        auto* model = preview._model;
        const int rendererCalls = runtime.rendererCalls;
        QString error;
        QVERIFY(!preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest, {}, {}, false, &error, {}, &state));
        QVERIFY(!error.isEmpty());
        QCOMPARE(preview._model, model);
        QCOMPARE(snapshot(preview).parameterValues, before.parameterValues);
        QCOMPARE(snapshot(preview).savedParameters, before.savedParameters);
        QCOMPARE(runtime.rendererCalls, rendererCalls);
        QVERIFY(preview.GetParameterReferenceState().motions.isEmpty());
    }

    void builtInMasksLeaveOtherDriversActive()
    {
        auto source = createSource();
        const auto a = idManager->GetId("ParamA"), b = idManager->GetId("ParamB");
        auto* breath = CubismBreath::Create();
        auto* eye = CubismEyeBlink::Create();
        const auto cleanupEffects = qScopeGuard([&] { CubismBreath::Delete(breath); CubismEyeBlink::Delete(eye); });
        csmVector<CubismBreath::BreathParameterData> values;
        values.PushBack({a, 0.2f, 0, 1, 1}); values.PushBack({b, 0.2f, 0, 1, 1});
        breath->SetParameters(values);
        csmVector<CubismIdHandle> eyes; eyes.PushBack(a); eyes.PushBack(b); eye->SetParameterIds(eyes);
        QVector<quint8> breathMask, eyeMask;
        QString error;
        QVERIFY(breath->PrepareParameterReferences({"ParamA"}, breathMask, &error));
        QVERIFY(eye->PrepareParameterReferences({"ParamA"}, eyeMask, &error));
        breath->CommitParameterReferences(breathMask); eye->CommitParameterReferences(eyeMask);
        source->model->SetParameterValue(a, 0.1f); source->model->SetParameterValue(b, 0.2f);
        breath->UpdateParameters(source->model, 0.1f);
        QCOMPARE(source->model->GetParameterValue(a), 0.1f);
        QVERIFY(qAbs(source->model->GetParameterValue(b) - 0.4f) < 1e-6f);
        eye->UpdateParameters(source->model, 0.1f);
        QCOMPARE(source->model->GetParameterValue(a), 0.1f);
        QCOMPARE(source->model->GetParameterValue(b), 1.0f);
        const auto previousMask = eyeMask;
        QVERIFY(!eye->PrepareParameterReferences({"missing"}, eyeMask, &error));
        QCOMPARE(eyeMask, previousMask);
        QVERIFY(breath->PrepareParameterReferences({}, breathMask, &error));
        breath->CommitParameterReferences(breathMask);
        breath->UpdateParameters(source->model, 0.1f);
        QVERIFY(qAbs(source->model->GetParameterValue(a) - 0.3f) < 1e-6f);
    }

    void timelinePreparationIsAtomicAndEditorPoseHasPriority_data()
    {
        QTest::addColumn<bool>("editor"); QTest::addColumn<bool>("reject");
        QTest::newRow("timeline") << false << false;
        QTest::newRow("editor-over-timeline") << true << false;
        QTest::newRow("rejected-timeline") << false << true;
        QTest::newRow("rejected-editor-timeline") << true << true;
    }

    void timelinePreparationIsAtomicAndEditorPoseHasPriority()
    {
        QFETCH(bool, editor); QFETCH(bool, reject);
        LAppModel preview; setupPreview(preview);
        const auto entry = snapshot(preview);
        preview._timelinePreviewActive = true;
        preview._timelineBaseParameters = entry.Parameters();
        preview._timelineBaseParameters["ParamA"] = 0.2f;
        preview._timelineBaseParts = {{entry.partIds[0], 0.6f}, {entry.partIds[1], 0.7f}};
        preview._timelineBaseOpacity = 0.75f;
        preview._timelinePreview.parameters = {{"ParamA", 0.8f}};
        preview._timelineParameters = preview._timelinePreview.parameters;
        preview.m_timelineExplicitParameterIds = {"ParamA"};
        if (editor) {
            preview.m_editorEntryState = std::make_unique<CubismModel::RuntimeValueSnapshot>(entry);
            preview.m_editorBaseParameters = entry.Parameters();
            preview.m_editorPose = {{"ParamA", 0.5f}, {"ParamB", 0.7f}};
            QVERIFY(preview._model->ApplyParameterPose(preview.m_editorPose));
        }
        const auto before = snapshot(preview);
        const auto beforeBase = preview._timelineBaseParameters;
        auto* previousModel = preview._model;
        int prepared = 0;
        const auto timeline = [&](const QMap<QString, float>& base, const QSet<QString>& eyes, const QSet<QString>& lips,
                                  LAppModel::PreparedTimelinePreview& frame, QString* error) {
            ++prepared;
            if (base.value("ParamA") != 0.3f || !eyes.isEmpty() || !lips.isEmpty()) return false;
            frame.active = true; frame.explicitParameterIds = {"ParamA"};
            frame.frame.parameters = {{"ParamA", 0.9f}};
            frame.frame.parts = {{entry.partIds[0], 0.4f}};
            frame.frame.model = {{"Opacity", 0.5f}};
            if (reject) { if (error) *error = "timeline refusal"; return false; }
            return true;
        };
        QString error;
        const auto check = [&](const CubismModel& model, QString*) {
            CubismModel::RuntimeValueSnapshot values;
            return model.CaptureRuntimeValues(values) && values.parameterValues[0] == (editor ? 0.5f : 0.9f) &&
                values.partOpacities[0] == (editor ? before.partOpacities[0] : 0.4f) &&
                values.modelOpacity == (editor ? before.modelOpacity : 0.5f);
        };
        const bool accepted = preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {}, {{"ParamA", 0.3f}}, editor, &error, check, nullptr, nullptr, timeline);
        QVERIFY2(accepted == !reject, qPrintable(error));
        QCOMPARE(prepared, 1);
        if (reject) {
            QCOMPARE(preview._model, previousModel);
            QCOMPARE(preview._timelineBaseParameters, beforeBase);
            QCOMPARE(preview._timelineParameters.value("ParamA"), 0.8f);
            QCOMPARE(snapshot(preview).savedParameters, before.savedParameters);
        } else {
            QCOMPARE(preview._timelineBaseParameters.value("ParamA"), 0.3f);
            QCOMPARE(preview._timelineParameters.value("ParamA"), 0.9f);
            QCOMPARE(snapshot(preview).savedParameters[0], 0.3f);
            QCOMPARE(preview.GetTimelinePreviewContext().explicitParameterIds, QSet<QString>{"ParamA"});
        }
        QCOMPARE(preview.HasEditorPreview(), editor);
        QCOMPARE(preview._userTimeSeconds, 23.0f);
    }

    void timelineDeactivationRestoresBaselineAtomically()
    {
        LAppModel preview; setupPreview(preview);
        preview._timelinePreviewActive = true;
        preview._timelineBaseParameters = {{"ParamA", 0.3f}, {"ParamB", 0.4f}};
        const auto before = snapshot(preview);
        preview._timelineBaseParts = {{before.partIds[0], 0.6f}, {before.partIds[1], 0.7f}};
        preview._timelineBaseOpacity = 0.75f;
        preview._timelinePreview.parameters = {{"ParamA", 0.8f}};
        preview._timelineParameters = preview._timelinePreview.parameters;
        preview.m_timelineExplicitParameterIds = {"ParamA"};
        QString error;
        const auto inactive = [](const auto&, const auto&, const auto&, LAppModel::PreparedTimelinePreview& prepared, QString*) {
            prepared.active = false; return true;
        };
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {}, {}, false, &error, {}, nullptr, nullptr, inactive), qPrintable(error));
        const auto after = snapshot(preview);
        QCOMPARE(after.parameterValues, QVector<float>({0.3f, 0.4f}));
        QCOMPARE(after.partOpacities, QVector<float>({0.6f, 0.7f}));
        QCOMPARE(after.modelOpacity, 0.75f);
        QVERIFY(!preview.HasTimelinePreview());
        QVERIFY(preview.GetTimelinePreviewContext().frame.IsEmpty());
        QVERIFY(preview.GetTimelinePreviewContext().explicitParameterIds.isEmpty());
        QVERIFY(preview._timelineParameters.isEmpty());
        QVERIFY(preview._timelineBaseParameters.isEmpty());
    }

    void resumedTimelineUsesArchivedBaselineWithoutSavedValue_data()
    {
        QTest::addColumn<float>("minimum");
        QTest::addColumn<bool>("explicitBase");
        QTest::newRow("unchanged-range") << -2.0f << false;
        QTest::newRow("narrowed-range") << 0.4f << false;
        QTest::newRow("explicit-base") << -2.0f << true;
    }

    void resumedTimelineUsesArchivedBaselineWithoutSavedValue()
    {
        QFETCH(float, minimum); QFETCH(bool, explicitBase);
        LAppModel preview; setupPreview(preview);
        runtime.editParameterDefinition = false;
        auto entry = snapshot(preview);
        entry.savedPresence = {0, 1};
        entry.parameterValues[0] = 0.8f;
        QVERIFY(preview._model->RestoreRuntimeValues(entry));
        preview._timelinePreviewActive = true;
        preview._timelineBaseParameters = {{"ParamA", 0.2f}, {"ParamB", 0.3f}};
        preview._timelinePreview.parameters = {{"ParamA", 0.8f}};
        preview._timelineParameters = preview._timelinePreview.parameters;
        preview.m_timelineExplicitParameterIds = {"ParamA"};
        ParameterStructureSeeds seeds;
        QString error;
        QVERIFY(preview.CaptureParameterStructureSeeds({"ParamA"}, seeds, &error));
        QVERIFY(!seeds["ParamA"].runtime.saved);
        QCOMPARE(seeds["ParamA"].timelineBase.value(), 0.2f);
        runtime.configureModel = [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); };
        const auto inactive = [](const auto&, const auto&, const auto&, LAppModel::PreparedTimelinePreview& prepared, QString*) {
            prepared.active = false; return true;
        };
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {}, {}, false, &error, {}, nullptr, nullptr, inactive), qPrintable(error));
        QVERIFY(!preview.HasTimelinePreview());
        runtime.configureModel = [minimum](GetterState& state) {
            CubismModelFixture::SetParameterLayout(state, {"ParamA", "ParamB"});
            state.minimum[0] = minimum; state.maximum[0] = 2;
            state.defaults[0] = std::max(state.defaults[0], minimum);
        };
        const float expected = explicitBase ? 0.35f : std::max(0.2f, minimum);
        bool prepared = false;
        const auto resume = [&](const auto& base, const auto&, const auto&, LAppModel::PreparedTimelinePreview& frame, QString*) {
            prepared = base.value("ParamA") == expected;
            frame.active = true; frame.explicitParameterIds = {"ParamA"};
            frame.frame.parameters = {{"ParamA", 0.8f}};
            return prepared;
        };
        const QMap<QString, float> patch = explicitBase ? QMap<QString, float>{{"ParamA", 0.35f}} : QMap<QString, float>{};
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            seeds, patch, false, &error, {}, nullptr, nullptr, resume), qPrintable(error));
        QVERIFY(prepared);
        QVERIFY(preview.HasTimelinePreview());
        QCOMPARE(preview.GetTimelineBaseParameters().value("ParamA"), expected);
        QCOMPARE(preview._model->GetParameterValue(0), 0.8f);
        QCOMPARE(snapshot(preview).HasSavedParameter(0), explicitBase);
    }

    void removedTimelineCachesDoNotReviveOnSameIdRecreation()
    {
        LAppModel preview; setupPreview(preview);
        runtime.editParameterDefinition = false;
        preview._timelinePreviewActive = true;
        preview._timelineBaseParameters = {{"ParamA", 0.2f}, {"ParamB", 0.3f}};
        preview._timelinePreview.parameters = {{"ParamA", 0.8f}};
        preview._timelineParameters = preview._timelinePreview.parameters;
        preview._eyeBlinkIds.PushBack(idManager->GetId("ParamA"));
        preview.m_parameterReferences.eyeBlink = {"ParamA"};
        runtime.configureModel = [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); };
        QString error;
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest, {}, {}, false, &error), qPrintable(error));
        QVERIFY(!preview._timelineParameters.contains("ParamA"));
        QVERIFY(!preview._timelinePreview.parameters.contains("ParamA"));
        runtime.configureModel = [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamA", "ParamB"}); };
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {{"ParamA", ParameterStructureSeed{}}}, {}, false, &error), qPrintable(error));
        QVERIFY(!preview._timelineParameters.contains("ParamA"));
        const auto explicitTrack = [](const auto&, const QSet<QString>& eyes, const auto&, LAppModel::PreparedTimelinePreview& frame, QString*) {
            if (eyes.contains("ParamA")) return false;
            frame.active = true; frame.explicitParameterIds = {"ParamA"}; frame.frame.parameters = {{"ParamA", 0.65f}};
            return true;
        };
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {}, {}, false, &error, {}, nullptr, nullptr, explicitTrack), qPrintable(error));
        QCOMPARE(preview._timelineParameters.value("ParamA"), 0.65f);
        QCOMPARE(preview._model->GetParameterValue(0), 0.65f);
    }

    void structuralPreviewReordersCurrentSavedAndVirtualValues()
    {
        LAppModel preview;
        setupPreview(preview);
        const auto before = snapshot(preview);
        const auto* ghost = idManager->GetId("Ghost");
        preview._model->SetParameterValue(ghost, -0.0f);
        runtime.configureModel = [](GetterState& state) {
            CubismModelFixture::SetParameterLayout(state, {"ParamC", "ParamB", "ParamA"});
            state.minimum[2] = -2; state.maximum[2] = 2;
        };
        QString error;
        const auto* old = preview._model;
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            {{QStringLiteral("ParamC"), ParameterStructureSeed{}}}, {}, false, &error), qPrintable(error));
        QVERIFY(preview._model != old);
        const QMap<QString, int> expectedIndices{{QStringLiteral("ParamC"), 0}, {QStringLiteral("ParamB"), 1}, {QStringLiteral("ParamA"), 2}};
        QCOMPARE(preview.m_mapParaToIndex, expectedIndices);
        const auto current = snapshot(preview);
        QCOMPARE(current.parameterValues, QVector<float>({0.4f, before.parameterValues[1], before.parameterValues[0]}));
        QCOMPARE(current.savedParameters, QVector<float>({0.4f, before.savedParameters[1], before.savedParameters[0]}));
        QCOMPARE(preview._model->GetParameterIndex(ghost), 3);
        QVERIFY(std::signbit(preview._model->GetParameterValue(ghost)));
        QCOMPARE(preview._userTimeSeconds, 23.0f);
        QCOMPARE(preview.m_sourceMocDigest, QByteArray("original-source-digest"));
    }

    void structuralHistoryRestoresEditorAndTimeline_data()
    {
        QTest::addColumn<bool>("keepEditor");
        QTest::newRow("retain-editor") << true;
        QTest::newRow("exit-editor") << false;
    }

    void structuralHistoryRestoresEditorAndTimeline()
    {
        QFETCH(bool, keepEditor);
        LAppModel preview;
        setupPreview(preview);
        const auto entry = snapshot(preview);
        preview.m_editorEntryState = std::make_unique<CubismModel::RuntimeValueSnapshot>(entry);
        preview.m_editorBaseParameters = entry.Parameters();
        preview._model->SetParameterValue(0, 0.5f);
        preview.m_editorPose = snapshot(preview).Parameters();
        preview._timelinePreviewActive = true;
        preview._timelineBaseParameters = entry.Parameters();
        preview._timelineBaseParameters[QStringLiteral("ParamA")] = entry.savedParameters[0];
        ParameterStructureSeeds removed;
        QString error;
        QVERIFY(preview.CaptureParameterStructureSeeds({QStringLiteral("ParamA")}, removed, &error));
        const QVariant payload = QVariant::fromValue(removed);
        QCOMPARE(payload.value<ParameterStructureSeeds>()[QStringLiteral("ParamA")].runtime.current, 0.5f);
        runtime.configureModel = [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); };
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest, {}, {}, true, &error), qPrintable(error));
        QVERIFY(preview.HasEditorPreview());
        QCOMPARE(preview.m_editorPose.size(), 1);
        QVERIFY(!preview._timelineBaseParameters.contains(QStringLiteral("ParamA")));
        const auto* virtualA = idManager->GetId("ParamA");
        preview._model->SetParameterValue(virtualA, 0.99f);
        runtime.configureModel = [](GetterState& state) {
            CubismModelFixture::SetParameterLayout(state, {"ParamA", "ParamB"});
            state.minimum[0] = -2; state.maximum[0] = 2;
        };
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest,
            payload.value<ParameterStructureSeeds>(), {}, keepEditor, &error), qPrintable(error));
        QCOMPARE(preview.HasEditorPreview(), keepEditor);
        QCOMPARE(preview._model->GetParameterValue(0), keepEditor ? 0.5f : entry.parameterValues[0]);
        QCOMPARE(snapshot(preview).savedParameters, entry.savedParameters);
        QCOMPARE(preview._timelineBaseParameters[QStringLiteral("ParamA")], entry.savedParameters[0]);
        QVERIFY(preview._timelinePreviewActive);
        if (keepEditor) {
            QCOMPARE(preview.m_editorEntryState->parameterValues, entry.parameterValues);
            QCOMPARE(preview.m_editorBaseParameters[QStringLiteral("ParamA")], entry.parameterValues[0]);
        }
        QCOMPARE(preview._userTimeSeconds, 23.0f);
    }

    void structuralRejectionPreservesSourceAndHistorySeeds_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("prepared-view") << 0;
        QTest::newRow("renderer") << 1;
        QTest::newRow("physics-reference") << 2;
    }

    void structuralRejectionPreservesSourceAndHistorySeeds()
    {
        QFETCH(int, failure);
        LAppModel preview;
        setupPreview(preview);
        const auto before = snapshot(preview);
        auto* oldModel = preview._model;
        const auto oldBytes = preview.m_previewMocBytes;
        const auto oldMap = preview.m_mapParaToIndex;
        ParameterStructureSeeds saved;
        QVERIFY(preview.CaptureParameterStructureSeeds({QStringLiteral("ParamA")}, saved));
        runtime.configureModel = [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); };
        if (failure == 1) runtime.rejectMetadataRenderer = true;
        if (failure == 2) {
            const QByteArray json = R"({"Version":3,"Meta":{"PhysicsSettingCount":1,"TotalInputCount":1,"TotalOutputCount":0,"VertexCount":1,"Fps":30,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}}},"PhysicsSettings":[{"Id":"one","Input":[{"Source":{"Target":"Parameter","Id":"ParamA"},"Weight":100,"Type":"X","Reflect":true}],"Output":[],"Vertices":[{"Position":{"X":0,"Y":0},"Mobility":1,"Delay":1,"Acceleration":1,"Radius":0}],"Normalization":{"Position":{"Minimum":-1,"Maximum":1,"Default":0},"Angle":{"Minimum":-30,"Maximum":30,"Default":0}}}]})";
            preview._physics = CubismPhysics::Create(reinterpret_cast<const csmByte*>(json.constData()), json.size());
            QVERIFY(preview._physics);
        }
        QString error;
        const auto prepare = [failure](const CubismModel&, QString* detail) {
            if (failure == 0) { if (detail) *detail = QStringLiteral("CPU view rejected"); return false; }
            return true;
        };
        QVERIFY(!preview.ApplyParameterStructurePreview(metadataBytes(), preview.m_sourceMocDigest, {}, {}, false, &error, prepare));
        QVERIFY(!error.isEmpty());
        if (failure == 2) QVERIFY(error.contains(QStringLiteral("ParamA")));
        QCOMPARE(preview._model, oldModel);
        QCOMPARE(preview.m_previewMocBytes, oldBytes);
        QCOMPARE(preview.m_mapParaToIndex, oldMap);
        QCOMPARE(snapshot(preview).parameterValues, before.parameterValues);
        QCOMPARE(snapshot(preview).savedParameters, before.savedParameters);
        QCOMPARE(saved[QStringLiteral("ParamA")].runtime.current, before.parameterValues[0]);
        QCOMPARE(preview._userTimeSeconds, 23.0f);
        QVERIFY(!preview._updating);
    }

    void metadataRangeMigrationAndBasePatchUndo()
    {
        LAppModel preview;
        setupPreview(preview);
        const auto before = snapshot(preview);
        const auto originalDigest = preview.m_sourceMocDigest;
        int previewPreparations = 0;
        QString error;
        const auto preparePreview = [&](const CubismModel& candidate, QString*) {
            CubismModel::RuntimeValueSnapshot values;
            if (!candidate.CaptureRuntimeValues(values)) return false;
            ++previewPreparations;
            return values.parameterValues[0] == 1.0f && values.savedParameters[0] == 1.0f;
        };
        QVERIFY2(preview.ApplyParameterDefinitionPreview(metadataBytes(), originalDigest, {{"ParamA", 1.0f}}, true,
                                                         &error, preparePreview), qPrintable(error));
        QCOMPARE(previewPreparations, 1);
        auto values = snapshot(preview);
        QCOMPARE(values.parameterValues[0], 1.0f);
        QCOMPARE(values.savedParameters[0], 1.0f);
        QCOMPARE(values.parameterValues[1], before.parameterValues[1]);
        QCOMPARE(values.savedParameters[1], before.savedParameters[1]);
        QCOMPARE(values.virtualParameterValues, before.virtualParameterValues);
        QCOMPARE(values.partOpacities, before.partOpacities);
        QCOMPARE(values.modelOpacity, before.modelOpacity);
        QCOMPARE(preview._userTimeSeconds, 23.0f);
        QCOMPARE(preview.m_sourceMocDigest, originalDigest);
        runtime.parameterMinimum = -2.0f;
        runtime.parameterMaximum = 2.0f;
        QVERIFY(preview.ApplyParameterDefinitionPreview(metadataBytes(), originalDigest, {{"ParamA", 1.5f}}, true, &error));
        values = snapshot(preview);
        QCOMPARE(values.parameterValues[0], 1.5f);
        QCOMPARE(values.savedParameters[0], 1.5f);
        QCOMPARE(values.savedParameters[1], before.savedParameters[1]);
        QCOMPARE(values.maximums[0], 2.0f);
    }

    void metadataTimelineKeepsDisplayedPoseAndDefaultOnlyDoesNotReset()
    {
        LAppModel preview;
        setupPreview(preview);
        preview._timelinePreviewActive = true;
        preview._timelineBaseParameters = {{"ParamA", 1.5f}, {"ParamB", 0.3f}};
        preview._timelineParameters = {{"ParamA", -1.25f}};
        preview._model->SetParameterValue(0, -1.25f);
        QString error;
        QVERIFY2(preview.ApplyParameterDefinitionPreview(metadataBytes(), preview.m_sourceMocDigest,
                                                         {{"ParamA", 0.9f}}, true, &error), qPrintable(error));
        auto values = snapshot(preview);
        QCOMPARE(values.parameterValues[0], -1.0f);
        QCOMPARE(values.savedParameters[0], 0.9f);
        QCOMPARE(preview._timelineBaseParameters.value("ParamA"), 0.9f);
        QCOMPARE(preview._timelineParameters.value("ParamA"), -1.25f);
        QVERIFY(preview._timelinePreviewActive);
        runtime.parameterDefault = 0.75f;
        QVERIFY(preview.ApplyParameterDefinitionPreview(metadataBytes(), preview.m_sourceMocDigest, {}, true, &error));
        values = snapshot(preview);
        QCOMPARE(values.parameterValues[0], -1.0f);
        QCOMPARE(values.savedParameters[0], 0.9f);
        QCOMPARE(preview._model->GetParameterDefaultValue(0), 0.75f);
        QCOMPARE(preview._userTimeSeconds, 23.0f);
    }

    void metadataEditorStateMigratesOrExitsOnlyAfterCommit_data()
    {
        QTest::addColumn<bool>("keep");
        QTest::addColumn<bool>("timeline");
        QTest::newRow("keep-editor") << true << false;
        QTest::newRow("end-editor") << false << false;
        QTest::newRow("keep-editor-from-timeline") << true << true;
        QTest::newRow("end-editor-from-timeline") << false << true;
    }
    void metadataEditorStateMigratesOrExitsOnlyAfterCommit()
    {
        QFETCH(bool, keep);
        QFETCH(bool, timeline);
        LAppModel preview;
        setupPreview(preview);
        preview._model->SetParameterValue(0, 0.1f);
        preview._model->SaveParameters();
        preview._model->SetParameterValue(0, 0.8f);
        preview.m_editorEntryState = std::make_unique<CubismModel::RuntimeValueSnapshot>(snapshot(preview));
        preview.m_editorBaseParameters = preview.m_editorEntryState->Parameters();
        preview.m_editorPose = {{"ParamA", -1.5f}, {"ParamB", 0.6f}};
        QVERIFY(preview._model->ApplyParameterPose(preview.m_editorPose));
        preview._timelinePreviewActive = timeline;
        if (timeline) preview._timelineBaseParameters = {{"ParamA", 1.5f}, {"ParamB", 0.3f}};
        QString error;
        QVERIFY2(preview.ApplyParameterDefinitionPreview(metadataBytes(), preview.m_sourceMocDigest,
                                                         {{"ParamA", 0.2f}}, keep, &error), qPrintable(error));
        QCOMPARE(preview.HasEditorPreview(), keep);
        if (timeline) {
            QCOMPARE(preview._timelineBaseParameters.value("ParamA"), 0.2f);
            QCOMPARE(preview._timelineBaseParameters.value("ParamB"), 0.3f);
        } else QVERIFY(preview._timelineBaseParameters.isEmpty());
        QCOMPARE(preview._timelinePreviewActive, timeline);
        const float entryCurrent = timeline ? 0.8f : 0.2f;
        const auto values = snapshot(preview);
        QCOMPARE(values.savedParameters[0], 0.2f);
        if (keep) {
            QCOMPARE(preview.m_editorPose.value("ParamA"), -1.0f);
            QCOMPARE(preview.m_editorBaseParameters.value("ParamA"), entryCurrent);
            QCOMPARE(preview.m_editorEntryState->parameterValues[0], entryCurrent);
            QCOMPARE(preview.m_editorEntryState->savedParameters[0], 0.2f);
            QCOMPARE(preview.m_editorEntryState->maximums[0], 1.0f);
            QCOMPARE(values.parameterValues[0], -1.0f);
            QVERIFY(preview._model->RestoreRuntimeValues(*preview.m_editorEntryState));
        } else {
            QCOMPARE(values.parameterValues[0], entryCurrent);
            QVERIFY(preview.m_editorPose.isEmpty());
            QVERIFY(preview.m_editorBaseParameters.isEmpty());
        }
    }

    void metadataFailurePreservesModelAndEveryPreviewBaseline_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("unknown-base") << 0;
        QTest::newRow("base-outside-range") << 1;
        QTest::newRow("base-nonfinite") << 2;
        QTest::newRow("overlay-refusal") << 3;
        QTest::newRow("overlay-exception") << 4;
        QTest::newRow("GPU-refusal") << 5;
        QTest::newRow("Core-refusal") << 6;
    }
    void metadataFailurePreservesModelAndEveryPreviewBaseline()
    {
        QFETCH(int, failure);
        LAppModel preview;
        setupPreview(preview);
        preview.m_editorEntryState = std::make_unique<CubismModel::RuntimeValueSnapshot>(snapshot(preview));
        preview.m_editorBaseParameters = preview.m_editorEntryState->Parameters();
        preview.m_editorPose = {{"ParamA", -1.5f}, {"ParamB", 0.6f}};
        QVERIFY(preview._model->ApplyParameterPose(preview.m_editorPose));
        preview._timelineBaseParameters = {{"ParamA", 1.5f}, {"ParamB", 0.3f}};
        preview._timelinePreviewActive = true;
        auto* old = preview._model;
        const auto oldValues = snapshot(preview);
        const auto oldEntry = *preview.m_editorEntryState;
        const auto oldBase = preview.m_editorBaseParameters, oldPose = preview.m_editorPose;
        const auto oldTimeline = preview._timelineBaseParameters;
        const auto oldDigest = preview.m_previewMocDigest;
        QMap<QString, float> patch{{"ParamA", 0.9f}};
        if (failure == 0) patch.insert("unknown", 0.5f);
        if (failure == 1) patch["ParamA"] = 1.25f;
        if (failure == 2) patch["ParamA"] = std::numeric_limits<float>::quiet_NaN();
        if (failure == 5) runtime.rejectMetadataRenderer = true;
        if (failure == 6) runtime.failure = Consistency;
        const auto preparePreview = [&](const CubismModel&, QString*) {
            if (failure == 4) throw std::runtime_error("overlay preparation exception");
            return failure != 3;
        };
        QString error;
        QVERIFY(!preview.ApplyParameterDefinitionPreview(metadataBytes(), preview.m_sourceMocDigest, patch, false, &error, preparePreview));
        QVERIFY(!error.isEmpty());
        QCOMPARE(preview._model, old);
        QCOMPARE(snapshot(preview).parameterValues, oldValues.parameterValues);
        QCOMPARE(snapshot(preview).savedParameters, oldValues.savedParameters);
        QCOMPARE(preview.m_editorEntryState->parameterValues, oldEntry.parameterValues);
        QCOMPARE(preview.m_editorEntryState->minimums, oldEntry.minimums);
        QCOMPARE(preview.m_editorEntryState->maximums, oldEntry.maximums);
        QCOMPARE(preview.m_editorBaseParameters, oldBase);
        QCOMPARE(preview.m_editorPose, oldPose);
        QCOMPARE(preview._timelineBaseParameters, oldTimeline);
        QCOMPARE(preview.m_previewMocDigest, oldDigest);
        QCOMPARE(preview._userTimeSeconds, 23.0f);
        QVERIFY(preview.HasEditorPreview());
    }

    void geometryReplacementRemapsSelectionAndDropsSourceEbo()
    {
        auto source=createSource();auto* oldMesh=source->model->GetMeshMap()->GetDrawMesh("MeshA");
        oldMesh->GetMesh()->GetWolrd()[3]->SetIsSelect(true);
        CMeshEditorSnapshot original;QVERIFY(source->model->GetMeshMap()->CaptureEditorState(original));
        CMeshGeometryMigration migration;migration.remaps.insert("MeshA",{4,3,{2,0,1,-1}});
        runtime.configureModel=[](GetterState& data){data.vertexCounts[0]=3;data.indexCounts[0]=3;data.uvs[0][0].X=.2f;};
        const auto policy=CubismModelUpdatePolicy::AllowDrawableGeometryEdits;
        CubismModelReplacement next;QString error;
        QVERIFY2(next.Prepare(candidateBytes.data(),candidateBytes.size(),*source->moc,*source->model,*source->renderer,
            normalPreparation(),policy,{},&error,{},&migration),qPrintable(error));
        auto* model=next.GetModel();QVERIFY(!model->HasDrawableIndicesOverrides());QVERIFY(source->model->HasDrawableIndicesOverrides());
        auto* mesh=model->GetMeshMap()->GetDrawMesh("MeshA");QVERIFY(mesh);QCOMPARE(mesh->GetSelectPoint().size(),size_t(1));
        QCOMPARE(mesh->GetSelectPoint().front(),mesh->GetMesh()->GetWolrd()[2]);QVERIFY(mesh->IsShown());
        QVERIFY(next.Commit(source->moc,source->model,source->renderer));
        QCOMPARE(source->model->GetDrawableVertexCount(0),3);QCOMPARE(source->model->GetDrawableVertexIndices(0)[2],quint16(2));
        QCOMPARE(source->model->GetParameterValue(0),.7f);QCOMPARE(source->model->GetPartOpacity(0),.45f);QCOMPARE(source->model->GetModelOpacity(),.6f);
        runtime.configureModel={};CMeshGeometryMigration undo;undo.remaps.insert("MeshA",{3,4,{1,2,0}});undo.targetState=original;
        CubismModelReplacement restored;
        QVERIFY2(restored.Prepare(sourceBytes.data(),sourceBytes.size(),*source->moc,*source->model,*source->renderer,
            normalPreparation(),policy,{},&error,{},&undo),qPrintable(error));
        QVERIFY(restored.Commit(source->moc,source->model,source->renderer));
        auto* restoredMesh=source->model->GetMeshMap()->GetDrawMesh("MeshA");QCOMPARE(restoredMesh->GetSelectPoint().size(),size_t(2));
        QVERIFY(restoredMesh->GetMesh()->GetWolrd()[3]->GetIsSelect());
        source->model->LoadParameters();QCOMPARE(source->model->GetParameterValue(0),.2f);
    }

    void geometryFailureKeepsOldObjectsAndSelection_data()
    {
        QTest::addColumn<int>("failure");for(int i=0;i<6;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void geometryFailureKeepsOldObjectsAndSelection()
    {
        QFETCH(int,failure);auto source=createSource();auto* model=source->model;auto* renderer=source->renderer;auto* moc=source->moc;
        auto* mesh=model->GetMeshMap()->GetDrawMesh("MeshA");auto* selected=mesh->GetSelectPoint().front();const auto allocations=runtime.allocations.size();
        runtime.configureModel=[failure](GetterState& data){data.vertexCounts[0]=3;data.indexCounts[0]=3;data.uvs[0][0].X=.2f;if(failure==4)data.indices[0][2]=3;};
        CMeshGeometryMigration migration;migration.remaps.insert("MeshA",{4,3,{2,0,1,-1}});
        if(failure==0)migration.remaps.clear();if(failure==1)migration.remaps["MeshA"].oldToNew={0,0,1,-1};
        if(failure==5){CMeshEditorSnapshot bad;QVERIFY(model->GetMeshMap()->CaptureEditorState(bad));migration.targetState=bad;}
        const auto preparation=[failure](CubismRenderer* r,CubismModel* m,const CubismRenderer& old,QString* e){
            if(failure==2){if(e)*e="GPU rejection";return false;}if(failure==3)throw std::runtime_error("GPU exception");return normalPreparation()(r,m,old,e);
        };
        CubismModelReplacement next;QString error;
        QVERIFY(!next.Prepare(candidateBytes.data(),candidateBytes.size(),*moc,*model,*renderer,preparation,
            CubismModelUpdatePolicy::AllowDrawableGeometryEdits,{},&error,{},&migration));QVERIFY(!error.isEmpty());
        QVERIFY(!next.GetModel());QCOMPARE(source->model,model);QCOMPARE(source->moc,moc);QCOMPARE(source->renderer,renderer);
        QCOMPARE(mesh->GetSelectPoint().front(),selected);QVERIFY(model->HasDrawableIndicesOverrides());
        QCOMPARE(model->GetParameterValue(0),.7f);QCOMPARE(model->GetPartOpacity(0),.45f);QCOMPARE(runtime.allocations.size(),allocations);
    }

    void lappGeometryChecksDigestAndKeepsEditorTimelineValues()
    {
        LAppModel preview;setupPreview(preview);runtime.parameterMinimum=-2;runtime.parameterMaximum=2;
        auto entry=std::make_unique<CubismModel::RuntimeValueSnapshot>(snapshot(preview));
        preview.m_editorBaseParameters=entry->Parameters();preview.m_editorPose=entry->Parameters();
        preview.m_editorPose["ParamA"]=.9f;preview.m_editorEntryState=std::move(entry);preview._model->SetParameterValue(0,.9f);
        preview._timelinePreviewActive=true;preview._timelineBaseParameters={{"ParamA",.2f},{"ParamB",.3f}};
        const auto baseline=preview._timelineBaseParameters;const auto editorBase=preview.m_editorBaseParameters;
        const auto before=snapshot(preview);auto* old=preview._model;
        CMeshGeometryMigration migration;migration.sourcePreviewDigest="stale";migration.remaps.insert("MeshA",{4,3,{2,0,1,-1}});
        runtime.configureModel=[](GetterState& data){data.vertexCounts[0]=3;data.indexCounts[0]=3;data.uvs[0][0].X=.2f;};
        QString error;QVERIFY(!preview.ApplyMeshGeometryPreview(metadataBytes(),preview.m_sourceMocDigest,migration,&error));QCOMPARE(preview._model,old);
        migration.sourcePreviewDigest=preview.GetPreviewMocDigest();runtime.rejectMetadataRenderer=true;
        QVERIFY(!preview.ApplyMeshGeometryPreview(metadataBytes(),preview.m_sourceMocDigest,migration,&error));QCOMPARE(preview._model,old);
        runtime.rejectMetadataRenderer=false;QVERIFY2(preview.ApplyMeshGeometryPreview(metadataBytes(),preview.m_sourceMocDigest,migration,&error),qPrintable(error));
        QVERIFY(preview._model!=old);QVERIFY(preview.HasEditorPreview());QVERIFY(preview.HasTimelinePreview());
        QCOMPARE(preview._timelineBaseParameters,baseline);QCOMPARE(preview.m_editorBaseParameters,editorBase);
        QCOMPARE(snapshot(preview).savedParameters,before.savedParameters);QCOMPARE(snapshot(preview).parameterValues,before.parameterValues);
        QCOMPARE(snapshot(preview).partOpacities,before.partOpacities);QCOMPARE(snapshot(preview).modelOpacity,before.modelOpacity);
        CMeshEditorSnapshot selected;QVERIFY(preview.CaptureMeshEditorState(selected));QCOMPARE(selected["MeshA"].selectedVertices,QVector<int>({2}));
    }

    void combinedParameterAndGeometryPreviewCommitsOneCandidate()
    {
        LAppModel preview;setupPreview(preview);runtime.editParameterDefinition=false;
        runtime.configureModel=[](GetterState& data){
            CubismModelFixture::SetParameterLayout(data,{"ParamA","ParamC"});data.minimum[0]=-2;data.maximum[0]=2;
            data.vertexCounts[0]=3;data.indexCounts[0]=3;data.uvs[0][0].X=.25f;
        };
        auto entry=std::make_unique<CubismModel::RuntimeValueSnapshot>(snapshot(preview));
        preview.m_editorPose=entry->Parameters();preview.m_editorBaseParameters=entry->Parameters();preview.m_editorEntryState=std::move(entry);
        CMeshGeometryMigration geometry;geometry.sourcePreviewDigest=preview.GetPreviewMocDigest();geometry.remaps.insert("MeshA",{4,3,{-1,-1,-1,-1}});
        auto* original=preview._model;const int calls=runtime.initializeCalls;
        runtime.rejectMetadataRenderer=true;QString error;
        QVERIFY(!preview.ApplyParameterStructurePreview(metadataBytes(),preview.m_sourceMocDigest,
            {{"ParamC",ParameterStructureSeed{}}},{},false,&error,{},nullptr,nullptr,{},nullptr,&geometry));
        QCOMPARE(preview._model,original);QVERIFY(preview.HasEditorPreview());QCOMPARE(runtime.initializeCalls,calls+1);
        runtime.rejectMetadataRenderer=false;
        QVERIFY2(preview.ApplyParameterStructurePreview(metadataBytes(),preview.m_sourceMocDigest,
            {{"ParamC",ParameterStructureSeed{}}},{},false,&error,{},nullptr,nullptr,{},nullptr,&geometry),qPrintable(error));
        QCOMPARE(runtime.initializeCalls,calls+2);QVERIFY(!preview.HasEditorPreview());QVERIFY(preview._model!=original);
        QCOMPARE(preview._model->GetDrawableVertexCount(0),3);QCOMPARE(preview._model->GetParameterCount(),2);
        QCOMPARE(preview._model->GetParameterId(1)->GetString().GetRawString(),QByteArray("ParamC").constData());
        QVERIFY(preview._model->GetMeshMap()->GetDrawMesh("MeshA")->GetSelectPoint().empty());
        QCOMPARE(preview._model->GetParameterValue(0),1.75f);
    }

    void topologyReplacementPreservesSelectionAndRuntimeValues_data()
    {
        QTest::addColumn<int>("count"); QTest::addColumn<bool>("clipping");
        for (int count : {0, 3, 6}) for (bool clipping : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(count).arg(clipping))) << count << clipping;
    }

    void topologyReplacementPreservesSelectionAndRuntimeValues()
    {
        QFETCH(int, count); QFETCH(bool, clipping);
        auto source = createSource();
        source->model->ClearDrawableIndicesOverrides();
        auto* oldMesh = source->model->GetMeshMap()->GetDrawMesh("MeshA");
        const auto oldSelection = oldMesh->GetSelectPoint();
        const auto oldPositions = oldMesh->Positions();
        runtime.editClipping = clipping;
        runtime.configureModel = [count](GetterState& data) {
            data.indexCounts[0] = count;
            data.indices[0] = {0, 1, 3, 1, 2, 3};
            if (!count) data.indexPointers[0] = nullptr;
        };
        CubismModelReplacement denied;
        QString error;
        QVERIFY(!prepare(denied, *source, normalPreparation(), &error));
        const auto policy = clipping ? CubismModelUpdatePolicy::AllowDrawableTopologyEdits | CubismModelUpdatePolicy::AllowClippingEdits
                                     : CubismModelUpdatePolicy::AllowDrawableTopologyEdits;
        CubismModelReplacement candidate;
        QVERIFY2(candidate.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
            *source->renderer, normalPreparation(), policy, &error), qPrintable(error));
        auto* next = candidate.GetModel();
        QCOMPARE(next->GetDrawableVertexIndexCount(0), count);
        auto* mesh = next->GetMeshMap()->GetDrawMesh("MeshA"); QVERIFY(mesh);
        QCOMPARE(mesh->GetMesh()->GetEBOSize(), count);
        QCOMPARE(mesh->GetMesh()->GetWolrd().size(), size_t(4));
        QCOMPARE(mesh->GetSelectPoint().size(), oldSelection.size());
        QVERIFY(mesh->IsShown());
        for (size_t index = 0; index < oldSelection.size(); ++index) {
            QCOMPARE(mesh->GetSelectPoint()[index], mesh->GetMesh()->GetWolrd()[index]);
            QVERIFY(mesh->GetSelectPoint()[index] != oldSelection[index]);
        }
        QCOMPARE(next->GetParameterValue(0), 0.7f);
        QCOMPARE(next->GetPartOpacity(0), 0.45f); QCOMPARE(next->GetModelOpacity(), 0.6f);
        QCOMPARE(oldMesh->Positions(), oldPositions); QVERIFY(oldMesh->GetSelectPoint() == oldSelection);
        QCOMPARE(State(next->GetModel()).updates, 1);
        QVERIFY(candidate.Commit(source->moc, source->model, source->renderer));
        source->model->LoadParameters(); QCOMPARE(source->model->GetParameterValue(0), 0.2f);
        source->model->SetParameterValue(0, 0.7f);
        runtime.configureModel = {}; runtime.editClipping = false;
        CubismModelReplacement restored;
        QVERIFY2(restored.Prepare(sourceBytes.data(), sourceBytes.size(), *source->moc, *source->model,
            *source->renderer, normalPreparation(), policy, &error), qPrintable(error));
        QCOMPARE(restored.GetModel()->GetDrawableVertexIndexCount(0), 6);
        QCOMPARE(restored.GetModel()->GetMeshMap()->GetDrawMesh("MeshA")->GetSelectPoint().size(), oldSelection.size());
        QVERIFY(restored.Commit(source->moc, source->model, source->renderer));
        QCOMPARE(source->model->GetParameterValue(0), 0.7f);
    }

    void topologyReplacementFailureLeavesSourceOwnedState_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("renderer") << 0;
        QTest::newRow("runtime-values") << 1;
        QTest::newRow("invalid-indices") << 2;
    }

    void topologyReplacementFailureLeavesSourceOwnedState()
    {
        QFETCH(int, failure);
        auto source = createSource(); source->model->ClearDrawableIndicesOverrides();
        auto* oldMoc = source->moc; auto* oldModel = source->model; auto* oldRenderer = source->renderer;
        auto* oldMesh = oldModel->GetMeshMap()->GetDrawMesh("MeshA");
        const auto oldSelection = oldMesh->GetSelectPoint(); const auto oldPositions = oldMesh->Positions();
        runtime.configureModel = [failure](GetterState& data) {
            data.indexCounts[0] = 3;
            if (failure == 2) data.indices[0][2] = 4;
        };
        if (failure == 1) State(oldModel->GetModel()).values[0] = std::numeric_limits<float>::quiet_NaN();
        int rendererCalls = 0;
        const auto reject = [&](CubismRenderer*, CubismModel*, const CubismRenderer&, QString* error) {
            ++rendererCalls; if (error) *error = "controlled topology renderer rejection"; return false;
        };
        CubismModelReplacement candidate;
        QString error;
        QVERIFY(!candidate.Prepare(candidateBytes.data(), candidateBytes.size(), *oldMoc, *oldModel, *oldRenderer,
            reject, CubismModelUpdatePolicy::AllowDrawableTopologyEdits, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(!candidate.GetModel());
        QVERIFY(!candidate.Commit(source->moc, source->model, source->renderer));
        QCOMPARE(source->moc, oldMoc); QCOMPARE(source->model, oldModel); QCOMPARE(source->renderer, oldRenderer);
        QCOMPARE(rendererCalls, failure == 0 ? 1 : 0);
        QCOMPARE(oldMesh->Positions(), oldPositions); QVERIFY(oldMesh->GetSelectPoint() == oldSelection); QVERIFY(oldMesh->IsShown());
        QCOMPARE(oldModel->GetDrawableVertexIndexCount(0), 6);
        QCOMPARE(State(oldModel->GetModel()).updates, 0);
        if (failure == 1) QVERIFY(std::isnan(oldModel->GetParameterValue(0)));
        else QCOMPARE(oldModel->GetParameterValue(0), 0.7f);
        State(oldModel->GetModel()).values[0] = 0.7f;
    }

    void explicitClippingPolicyReachesStateCopyAndCommit()
    {
        auto source = createSource();
        runtime.editClipping = true;
        CubismModelReplacement strict;
        QString error;
        QVERIFY(!prepare(strict, *source, normalPreparation(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!strict.GetModel());
        QCOMPARE(source->model->GetDrawableMaskCounts()[0], 0);
        CubismModelReplacement candidate;
        QVERIFY2(candidate.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
            *source->renderer, normalPreparation(), CubismModelUpdatePolicy::AllowClippingEdits, &error), qPrintable(error));
        QCOMPARE(candidate.GetModel()->GetDrawableMaskCounts()[0], 1);
        QVERIFY(candidate.GetModel()->GetDrawableInvertedMask(0));
        QCOMPARE(candidate.GetModel()->GetParameterValue(0), 0.7f);
        QCOMPARE(State(candidate.GetModel()->GetModel()).updates, 1);
        QCOMPARE(source->model->GetDrawableMaskCounts()[0], 0);
        QVERIFY(!source->model->GetDrawableInvertedMask(0));
        QVERIFY(candidate.Commit(source->moc, source->model, source->renderer));
        QCOMPARE(source->model->GetDrawableMaskCounts()[0], 1);
        QVERIFY(source->model->GetDrawableInvertedMask(0));
        source->model->LoadParameters();
        QCOMPARE(source->model->GetParameterValue(0), 0.2f);
        source->model->SetParameterValue(0, 0.7f);
    }

    void clippingReplacementFailureDoesNotPublishCandidate_data()
    {
        QTest::addColumn<bool>("stateFailure");
        QTest::newRow("renderer") << false;
        QTest::newRow("runtime-state") << true;
    }

    void clippingReplacementFailureDoesNotPublishCandidate()
    {
        QFETCH(bool, stateFailure);
        auto source = createSource();
        runtime.editClipping = true;
        auto* originalMoc = source->moc;
        auto* originalModel = source->model;
        auto* originalRenderer = source->renderer;
        if (stateFailure) State(source->model->GetModel()).values[0] = std::numeric_limits<float>::quiet_NaN();
        int rendererCalls = 0;
        const auto reject = [&](CubismRenderer*, CubismModel*, const CubismRenderer&, QString* error) {
            ++rendererCalls;
            *error = "controlled clipping GPU failure";
            return false;
        };
        CubismModelReplacement candidate;
        QString error;
        QVERIFY(!candidate.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
            *source->renderer, reject, CubismModelUpdatePolicy::AllowClippingEdits, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!candidate.GetModel());
        QVERIFY(!candidate.Commit(source->moc, source->model, source->renderer));
        QCOMPARE(rendererCalls, stateFailure ? 0 : 1);
        QCOMPARE(source->moc, originalMoc);
        QCOMPARE(source->model, originalModel);
        QCOMPARE(source->renderer, originalRenderer);
        QCOMPARE(source->model->GetDrawableMaskCounts()[0], 0);
        QVERIFY(!source->model->GetDrawableInvertedMask(0));
        QCOMPARE(State(source->model->GetModel()).updates, 0);
        State(source->model->GetModel()).values[0] = 0.7f;
    }

    void init()
    {
        QVERIFY(runtime.allocations.empty());
        QVERIFY(runtime.models.empty());
        QVERIFY(runtime.mocs.empty());
        QVERIFY(runtime.renderers.empty());
        runtime = Runtime{};
        idManager = std::make_unique<CubismIdManager>();
        CubismFramework::Initialize();
    }

    void cleanup()
    {
        runtime.failure = None;
        runtime.throwAllocationCall = -1;
        CubismFramework::Dispose();
        idManager.reset();
        QVERIFY(runtime.renderers.empty());
        QVERIFY(runtime.models.empty());
        QVERIFY(runtime.mocs.empty());
        QVERIFY(runtime.allocations.empty());
    }

    void successfulCommitPreservesStateAndSelection()
    {
        static_assert(!std::is_copy_constructible_v<CubismModelReplacement>);
        static_assert(!std::is_copy_assignable_v<CubismModelReplacement>);
        auto source = createSource();
        auto* oldMoc = source->moc;
        auto* oldModel = source->model;
        auto* oldRenderer = source->renderer;
        auto* oldMesh = oldModel->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        auto* oldSelected = oldMesh->GetSelectPoint().front();
        int callbackCalls = 0;
        {
            CubismModelReplacement replacement;
            QString error = QStringLiteral("stale");
            QVERIFY2(prepare(replacement, *source,
                [&](CubismRenderer* renderer, CubismModel* model, const CubismRenderer& previous, QString* error) {
                    ++callbackCalls;
                    if (model == oldModel || &previous != oldRenderer || State(model->GetModel()).updates != 1) return false;
                    auto* mesh = model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
                    if (!mesh || mesh->GetSelectPoint().size() != 1 || mesh->GetSelectPoint().front() == oldSelected ||
                        mesh->GetSelectPoint().front()->GetPoint() != QVector2D(-0.5f, -0.75f)) return false;
                    return normalPreparation()(renderer, model, previous, error);
                }, &error), qPrintable(error));
            QVERIFY(error.isEmpty());
            QCOMPARE(callbackCalls, 1);
            QCOMPARE(runtime.checkedBytes, QByteArray(reinterpret_cast<const char*>(candidateBytes.data()), candidateBytes.size()));
            QCOMPARE(runtime.consistencyCalls, 2);
            QCOMPARE(runtime.reviveCalls, 2);
            verifySourceUnchanged(*source, oldMesh, oldSelected);
            auto* candidate = replacement.GetModel();
            QVERIFY(candidate);
            QCOMPARE(candidate->GetParameterValue(0), 0.7f);
            QCOMPARE(candidate->GetParameterValue(1), 0.8f);
            QVERIFY(replacement.Commit(source->moc, source->model, source->renderer));
            QCOMPARE(source->model, candidate);
            QVERIFY(source->moc != oldMoc);
            QVERIFY(source->model != oldModel);
            QVERIFY(source->renderer != oldRenderer);
            QCOMPARE(source->renderer->GetModel(), candidate);
            QCOMPARE(source->renderer->GetModelColor().A, 0.8f);
            QVERIFY(runtime.events.isEmpty());
            QVERIFY(!replacement.Commit(source->moc, source->model, source->renderer));
            QVERIFY(!prepare(replacement, *source, normalPreparation()));
            QCOMPARE(candidate->GetDrawableVertexPositions(0)[0].X, -0.5f);
            QCOMPARE(candidate->GetDrawableVertexPositions(0)[0].Y, -0.75f);
        }
        QCOMPARE(runtime.events, QStringList({QStringLiteral("renderer:1"), QStringLiteral("model:1"), QStringLiteral("moc:1")}));
        QCOMPARE(runtime.models.size(), size_t(1));
        QCOMPARE(runtime.mocs.size(), size_t(1));
        QCOMPARE(runtime.renderers.size(), size_t(1));
        source->model->LoadParameters();
        QCOMPARE(source->model->GetParameterValue(0), 0.2f);
        QCOMPARE(source->model->GetParameterValue(1), 0.3f);
        QCOMPARE(source->model->GetParameterValue(idManager->GetId("VirtualParameter")), 2.5f);
        auto* candidateMesh = source->model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        QCOMPARE(candidateMesh->GetSelectPoint().size(), size_t(1));
        QCOMPARE(candidateMesh->GetSelectPoint().front()->GetPoint(), QVector2D(-0.5f, -0.75f));
        source.reset();
        QCOMPARE(runtime.events.mid(3), QStringList({QStringLiteral("renderer:2"), QStringLiteral("model:2"), QStringLiteral("moc:2")}));
    }

    void modelPreparationRunsAfterStateCopyAndBeforeEvaluation()
    {
        auto source = createSource();
        auto* oldModel = source->model;
        bool modelPrepared = false, rendererPrepared = false;
        const QMap<QString, float> pose{{"ParamA", -0.5f}, {"ParamB", 1.5f}};
        CubismModelReplacement replacement;
        QString error;
        const auto prepareModel = [&](CubismModel* candidate, QString* message) {
            CubismModel::RuntimeValueSnapshot copied;
            if (State(candidate->GetModel()).updates != 0 || !candidate->CaptureRuntimeValues(copied, message) ||
                copied.parameterValues != QVector<float>({0.7f, 0.8f}) ||
                copied.savedParameters != QVector<float>({0.2f, 0.3f})) return false;
            modelPrepared = candidate->ApplyParameterPose(pose, message);
            return modelPrepared;
        };
        const auto prepareRenderer = [&](CubismRenderer* renderer, CubismModel* candidate,
                                          const CubismRenderer& previous, QString* message) {
            CubismModel::RuntimeValueSnapshot evaluated;
            if (!modelPrepared || State(candidate->GetModel()).updates != 1 ||
                !candidate->CaptureRuntimeValues(evaluated, message) || evaluated.Parameters() != pose ||
                evaluated.savedParameters != QVector<float>({0.2f, 0.3f})) return false;
            rendererPrepared = true;
            return normalPreparation()(renderer, candidate, previous, message);
        };
        QVERIFY2(replacement.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
                                     *source->renderer, prepareRenderer, &error, prepareModel), qPrintable(error));
        QVERIFY(modelPrepared && rendererPrepared);
        QCOMPARE(source->model, oldModel);
        QCOMPARE(source->model->GetParameterValue(0), 0.7f);
        QVERIFY(replacement.Commit(source->moc, source->model, source->renderer));
        QCOMPARE(source->model->GetParameterValue(0), -0.5f);
        QCOMPARE(source->model->GetParameterValue(1), 1.5f);
        source->model->LoadParameters();
        QCOMPARE(source->model->GetParameterValue(0), 0.2f);
        QCOMPARE(source->model->GetParameterValue(1), 0.3f);
    }

    void modelPreparationFailureRollsBackBeforeCoreUpdate_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("callback-false") << 0;
        QTest::newRow("callback-throws") << 1;
        QTest::newRow("out-of-range-pose") << 2;
    }

    void modelPreparationFailureRollsBackBeforeCoreUpdate()
    {
        QFETCH(int, failure);
        auto source = createSource();
        auto* mesh = source->model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        auto* selected = mesh->GetSelectPoint().front();
        const auto allocationCount = runtime.allocations.size();
        int calls = 0;
        CubismModelReplacement replacement;
        QString error;
        const auto prepareModel = [&](CubismModel* candidate, QString* message) {
            ++calls;
            if (failure == 2) return candidate->ApplyParameterPose({{"ParamA", 99}, {"ParamB", 0}}, message);
            if (!candidate->ApplyParameterPose({{"ParamA", -0.5f}, {"ParamB", 1.5f}}, message)) return false;
            if (failure == 1) throw std::runtime_error("Controlled pose preparation failure");
            return false;
        };
        QVERIFY(!replacement.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
                                      *source->renderer, normalPreparation(), &error, prepareModel));
        QVERIFY(!error.isEmpty());
        QCOMPARE(calls, 1);
        QCOMPARE(runtime.rendererCalls, 1);
        QCOMPARE(runtime.allocations.size(), allocationCount);
        QCOMPARE(runtime.releasedModelUpdates, QVector<int>({0}));
        QVERIFY(!replacement.GetModel());
        verifySourceUnchanged(*source, mesh, selected);
        const auto validPose = [](CubismModel* candidate, QString* message) {
            return candidate->ApplyParameterPose({{"ParamA", -0.5f}, {"ParamB", 1.5f}}, message);
        };
        QVERIFY2(replacement.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
                                     *source->renderer, normalPreparation(), &error, validPose), qPrintable(error));
        QVERIFY(replacement.Commit(source->moc, source->model, source->renderer));
    }

    void identicalMocBytesCanChangePoseThenRestoreEntryValuesWithEditedGeometry()
    {
        auto source = createSource();
        CubismModel::RuntimeValueSnapshot entry;
        QVERIFY(source->model->CaptureRuntimeValues(entry));
        const QVector<QMap<QString, float>> poses{
            {{"ParamA", -0.5f}, {"ParamB", 1.5f}}, {{"ParamA", 0.9f}, {"ParamB", -1.5f}}
        };
        for (const auto& pose : poses) {
            auto* previous = source->model;
            CubismModelReplacement replacement;
            const auto apply = [&](CubismModel* candidate, QString* error) { return candidate->ApplyParameterPose(pose, error); };
            QVERIFY(replacement.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
                                        *source->renderer, normalPreparation(), nullptr, apply));
            QVERIFY(replacement.Commit(source->moc, source->model, source->renderer));
            QVERIFY(source->model != previous);
            CubismModel::RuntimeValueSnapshot values;
            QVERIFY(source->model->CaptureRuntimeValues(values));
            QCOMPARE(values.Parameters(), pose);
            QCOMPARE(values.savedParameters, QVector<float>({0.2f, 0.3f}));
            QCOMPARE(source->model->GetDrawableVertexPositions(0)[0].X, -0.5f);
            QCOMPARE(State(source->model->GetModel()).updates, 1);
        }
        QCOMPARE(runtime.initializeCalls, 3);
        QCOMPARE(entry.Parameters(), (QMap<QString, float>{{"ParamA", 0.7f}, {"ParamB", 0.8f}}));
        source->model->SetPartOpacity(0, 0);
        source->model->SetModelOpacity(0.1f);
        source->model->SetParameterValue(idManager->GetId("VirtualParameter"), -99);
        const auto restore = [&](CubismModel* candidate, QString* error) { return candidate->RestoreRuntimeValues(entry, error); };
        {
            CubismModelReplacement rejected;
            const auto rejectRenderer = [](CubismRenderer*, CubismModel*, const CubismRenderer&, QString*) { return false; };
            QVERIFY(!rejected.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
                                       *source->renderer, rejectRenderer, nullptr, restore));
            QCOMPARE(source->model->GetParameterValue(0), 0.9f);
            QCOMPARE(source->model->GetPartOpacity(0), 0.0f);
            QCOMPARE(source->model->GetModelOpacity(), 0.1f);
        }
        CubismModelReplacement replacement;
        QVERIFY(replacement.Prepare(candidateBytes.data(), candidateBytes.size(), *source->moc, *source->model,
                                    *source->renderer, normalPreparation(), nullptr, restore));
        QVERIFY(replacement.Commit(source->moc, source->model, source->renderer));
        QCOMPARE(source->model->GetParameterValue(0), 0.7f);
        QCOMPARE(source->model->GetParameterValue(1), 0.8f);
        QCOMPARE(source->model->GetPartOpacity(0), 0.45f);
        QCOMPARE(source->model->GetModelOpacity(), 0.6f);
        QCOMPARE(source->model->GetParameterValue(idManager->GetId("VirtualParameter")), 2.5f);
        QCOMPARE(source->model->GetDrawableVertexPositions(0)[0].X, -0.5f);
        QCOMPARE(source->model->GetDrawableVertexPositions(0)[0].Y, -0.75f);
        source->model->LoadParameters();
        QCOMPARE(source->model->GetParameterValue(0), 0.2f);
        QCOMPARE(source->model->GetParameterValue(1), 0.3f);
    }

    void coreAndFactoryFailuresReleaseCandidateImmediately_data()
    {
        QTest::addColumn<int>("failure");
        QTest::addColumn<bool>("hasModel");
        QTest::addColumn<bool>("hasMoc");
        QTest::newRow("consistency") << int(Consistency) << false << false;
        QTest::newRow("revival") << int(Revival) << false << false;
        QTest::newRow("model-size") << int(ModelSize) << false << true;
        QTest::newRow("model-initialize") << int(ModelInitialize) << false << true;
        QTest::newRow("wrong-static-definition") << int(StaticDefinition) << true << true;
        QTest::newRow("nonfinite-output") << int(NonFiniteOutput) << true << true;
        QTest::newRow("null-renderer") << int(RendererFactoryNull) << true << true;
        QTest::newRow("throwing-renderer") << int(RendererFactoryThrow) << true << true;
        QTest::newRow("null-moc-allocation") << int(MocAllocationNull) << false << false;
        QTest::newRow("throwing-moc-allocation") << int(MocAllocationThrow) << false << false;
        QTest::newRow("null-model-allocation") << int(ModelAllocationNull) << false << true;
        QTest::newRow("throwing-model-allocation") << int(ModelAllocationThrow) << false << true;
        QTest::newRow("throwing-model-wrapper") << int(ModelWrapperThrow) << true << true;
        QTest::newRow("throwing-model-array") << int(ModelArrayThrow) << true << true;
    }

    void coreAndFactoryFailuresReleaseCandidateImmediately()
    {
        QFETCH(int, failure);
        QFETCH(bool, hasModel);
        QFETCH(bool, hasMoc);
        auto source = createSource();
        auto* mesh = source->model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        auto* selected = mesh->GetSelectPoint().front();
        const auto allocationCount = runtime.allocations.size();
        runtime.failure = static_cast<Failure>(failure);
        CubismModelReplacement replacement;
        int callbackCalls = 0;
        QString error;
        QVERIFY(!prepare(replacement, *source,
                         [&](CubismRenderer*, CubismModel*, const CubismRenderer&, QString*) { ++callbackCalls; return true; }, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(callbackCalls, 0);
        QVERIFY(!replacement.GetModel());
        QCOMPARE(runtime.allocations.size(), allocationCount);
        QCOMPARE(runtime.models.size(), size_t(1));
        QCOMPARE(runtime.mocs.size(), size_t(1));
        QCOMPARE(runtime.renderers.size(), size_t(1));
        if (failure == Consistency) {
            QCOMPARE(runtime.consistencyCalls, 2);
            QCOMPARE(runtime.reviveCalls, 1);
        }
        QStringList expected;
        if (hasModel) expected.append(QStringLiteral("model:2"));
        if (hasMoc) expected.append(QStringLiteral("moc:2"));
        QCOMPARE(runtime.events, expected);
        verifySourceUnchanged(*source, mesh, selected);
        QVERIFY(!replacement.Commit(source->moc, source->model, source->renderer));
        runtime.failure = None;
        runtime.throwAllocationCall = -1;
        QVERIFY(prepare(replacement, *source, normalPreparation(), &error));
        QVERIFY(error.isEmpty());
        verifySourceUnchanged(*source, mesh, selected);
    }

    void callbackFailuresReleaseRendererBeforeModel_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("false-before-initialize") << 0;
        QTest::newRow("false-after-initialize") << 1;
        QTest::newRow("throw-before-initialize") << 2;
        QTest::newRow("throw-after-initialize") << 3;
        QTest::newRow("success-without-initialize") << 4;
        QTest::newRow("bound-to-old-model") << 5;
        QTest::newRow("custom-diagnostic") << 6;
    }

    void callbackFailuresReleaseRendererBeforeModel()
    {
        QFETCH(int, failure);
        auto source = createSource();
        auto* mesh = source->model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        auto* selected = mesh->GetSelectPoint().front();
        const auto allocationCount = runtime.allocations.size();
        CubismModelReplacement replacement;
        QString error;
        QVERIFY(!prepare(replacement, *source,
            [&](CubismRenderer* renderer, CubismModel* model, const CubismRenderer& previous, QString* error) {
                switch (failure) {
                case 0: return false;
                case 1: renderer->Initialize(model); return false;
                case 2: throw std::runtime_error("Before renderer initialize");
                case 3: renderer->Initialize(model); throw std::runtime_error("After renderer initialize");
                case 4: return true;
                case 5: return renderer->Initialize(previous.GetModel());
                default: *error = QStringLiteral("controlled GPU preparation failure"); return false;
                }
            }, &error));
        QVERIFY(!error.isEmpty());
        if (failure == 6) QCOMPARE(error, QStringLiteral("controlled GPU preparation failure"));
        QCOMPARE(runtime.events, QStringList({QStringLiteral("renderer:2"), QStringLiteral("model:2"), QStringLiteral("moc:2")}));
        QCOMPARE(runtime.allocations.size(), allocationCount);
        QCOMPARE(runtime.renderers.size(), size_t(1));
        QCOMPARE(runtime.models.size(), size_t(1));
        QVERIFY(!replacement.GetModel());
        verifySourceUnchanged(*source, mesh, selected);
        QVERIFY(prepare(replacement, *source, normalPreparation(), &error));
    }

    void abandoningPreparedCandidateLeavesSourceAlive()
    {
        auto source = createSource();
        auto* mesh = source->model->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        auto* selected = mesh->GetSelectPoint().front();
        const auto allocationCount = runtime.allocations.size();
        {
            CubismModelReplacement replacement;
            QVERIFY(prepare(replacement, *source, normalPreparation()));
            auto* preparedModel = replacement.GetModel();
            QVERIFY(preparedModel);
            QVERIFY(!prepare(replacement, *source, normalPreparation()));
            QCOMPARE(replacement.GetModel(), preparedModel);
        }
        QCOMPARE(runtime.allocations.size(), allocationCount);
        QCOMPARE(runtime.events, QStringList({QStringLiteral("renderer:2"), QStringLiteral("model:2"), QStringLiteral("moc:2")}));
        verifySourceUnchanged(*source, mesh, selected);
    }

    void commitRequiresEveryOriginalResourceIdentity_data()
    {
        QTest::addColumn<int>("changedResource");
        QTest::newRow("moc") << 0;
        QTest::newRow("model") << 1;
        QTest::newRow("renderer") << 2;
    }

    void commitRequiresEveryOriginalResourceIdentity()
    {
        QFETCH(int, changedResource);
        auto source = createSource();
        CubismModelReplacement replacement;
        QVERIFY(prepare(replacement, *source, normalPreparation()));
        auto* candidate = replacement.GetModel();
        auto* moc = source->moc;
        auto* model = source->model;
        auto* renderer = source->renderer;
        if (changedResource == 0) moc = nullptr;
        if (changedResource == 1) model = nullptr;
        if (changedResource == 2) renderer = nullptr;
        const auto* beforeMoc = moc;
        const auto* beforeModel = model;
        const auto* beforeRenderer = renderer;
        QVERIFY(!replacement.Commit(moc, model, renderer));
        QCOMPARE(moc, beforeMoc);
        QCOMPARE(model, beforeModel);
        QCOMPARE(renderer, beforeRenderer);
        QCOMPARE(replacement.GetModel(), candidate);
        QVERIFY(runtime.events.isEmpty());
        QVERIFY(replacement.Commit(source->moc, source->model, source->renderer));
        QCOMPARE(source->model, candidate);
    }

    void invalidPreparationInputsDoNotAllocate_data()
    {
        QTest::addColumn<int>("invalid");
        QTest::newRow("null-bytes") << 0;
        QTest::newRow("empty-bytes") << 1;
        QTest::newRow("empty-callback") << 2;
        QTest::newRow("renderer-without-model") << 3;
        QTest::newRow("renderer-with-another-model") << 4;
    }

    void invalidPreparationInputsDoNotAllocate()
    {
        QFETCH(int, invalid);
        auto source = createSource();
        auto other = createSource();
        std::unique_ptr<CubismRenderer, decltype(&CubismRenderer::Delete)> unbound(
            CSM_NEW FakeRenderer(0), &CubismRenderer::Delete);
        const auto* bytes = invalid == 0 ? nullptr : candidateBytes.data();
        const auto size = invalid == 1 ? 0 : candidateBytes.size();
        const auto callback = invalid == 2 ? CubismModelReplacement::RendererPreparation{} : normalPreparation();
        const auto* renderer = invalid == 3 ? unbound.get() : invalid == 4 ? other->renderer : source->renderer;
        const int allocations = runtime.allocationCalls;
        const int consistencyCalls = runtime.consistencyCalls;
        CubismModelReplacement replacement;
        QString error;
        QVERIFY(!replacement.Prepare(bytes, size, *source->moc, *source->model, *renderer, callback, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!replacement.GetModel());
        QCOMPARE(runtime.allocationCalls, allocations);
        QCOMPARE(runtime.consistencyCalls, consistencyCalls);
    }

    void drawingBlocksReplacementAndRestoresItsGuard()
    {
        auto source = createSource();
        auto* renderer = static_cast<FakeRenderer*>(source->renderer);
        CubismModelReplacement replacement;
        bool drawing = false, rejected = false, nestedRejected = false;
        QString error;
        const int allocations = runtime.allocationCalls;
        renderer->drawHook = [&] {
            drawing = renderer->IsDrawing();
            rejected = !prepare(replacement, *source, normalPreparation(), &error);
            nestedRejected = !renderer->DrawModel();
            return true;
        };
        QVERIFY(renderer->DrawModel());
        QVERIFY(drawing && rejected && nestedRejected);
        QVERIFY(!renderer->IsDrawing());
        QVERIFY(!error.isEmpty());
        QCOMPARE(runtime.allocationCalls, allocations);
        renderer->drawHook = []() -> bool { throw std::runtime_error("Interrupted frame"); };
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, renderer->DrawModel());
        QVERIFY(!renderer->IsDrawing());
        QVERIFY(prepare(replacement, *source, normalPreparation(), &error));
    }

    void incompatibleEditorCacheDoesNotMutateSource()
    {
        auto source = createSource();
        source->model->GetMeshMap()->insert(QStringLiteral("UnknownMesh"), nullptr);
        const auto allocationCount = runtime.allocations.size();
        CubismModelReplacement replacement;
        QString error;
        QVERIFY(!prepare(replacement, *source, normalPreparation(), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(runtime.rendererCalls, 1);
        QCOMPARE(runtime.allocations.size(), allocationCount);
        QCOMPARE(source->model->GetMeshMap()->GetMapMesh().size(), 3);
        QVERIFY(source->model->GetMeshMap()->GetMapMesh().contains(QStringLiteral("UnknownMesh")));
        QCOMPARE(runtime.events, QStringList({QStringLiteral("model:2"), QStringLiteral("moc:2")}));
        source->model->GetMeshMap()->GetMapMesh().remove(QStringLiteral("UnknownMesh"));
        QVERIFY(prepare(replacement, *source, normalPreparation(), &error));
    }
};

bool LAppModel::ApplyPreviewTransaction(const QByteArray& bytes, const QByteArray& sourceDigest,
                                       const CubismModelReplacement::ModelPreparation& prepareModel,
                                       QString* error, CubismModelUpdatePolicy policy, const ParameterStructureSeeds& seeds,
                                       const ParameterPhysicsReferences* physicsReferences, const PhysicsDraft* physicsDraft,
                                       const CMeshGeometryMigration* geometry)
{
    return ModelReplacementTests::publishPreview(*this, bytes, sourceDigest, prepareModel, error, policy, seeds, physicsReferences, physicsDraft, geometry);
}

QTEST_GUILESS_MAIN(ModelReplacementTests)
#include "model_replacement_tests.moc"
