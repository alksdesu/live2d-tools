#include "framework/Model/CubismModelReplacement.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include "cubism_model_fixture.h"

#include <QtTest>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
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
        for (auto& point : data->positions[0]) { point.X += 0.5f; point.Y += 0.25f; }
        if (runtime.editClipping) {
            data->maskCounts[0] = 1;
            data->masks[0][0] = 1;
            data->flags[0] ^= csmIsInvertedMask;
        }
    }
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

class ModelReplacementTests : public QObject
{
    Q_OBJECT
private slots:
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
    }

    void cleanup()
    {
        runtime.failure = None;
        runtime.throwAllocationCall = -1;
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

QTEST_GUILESS_MAIN(ModelReplacementTests)
#include "model_replacement_tests.moc"
