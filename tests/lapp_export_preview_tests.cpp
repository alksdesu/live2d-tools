#include "live2d/LAppModel.hpp"
#include "live2d/LAppPal.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Physics/CubismPhysics.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include "cubism_model_fixture.h"
#include <QtTest>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>

using namespace Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;
using CubismModelFixture::GetterState;

namespace {
std::unordered_map<void*, std::size_t> allocations;
std::unordered_map<void*, std::unique_ptr<GetterState>> coreStates;
std::unique_ptr<CubismIdManager> idManager;

void* allocate(std::size_t size, std::size_t alignment = 0)
{
    void* pointer = alignment ? ::operator new(size, std::align_val_t(alignment), std::nothrow) : std::malloc(size);
    if (!pointer) throw std::bad_alloc();
    allocations.emplace(pointer, alignment);
    return pointer;
}

void release(void* pointer)
{
    if (!pointer) return;
    const auto found = allocations.find(pointer);
    if (found == allocations.end()) qFatal("Unknown allocation in export preview test");
    const auto alignment = found->second;
    allocations.erase(found);
    if (alignment) ::operator delete(pointer, std::align_val_t(alignment));
    else std::free(pointer);
}

[[noreturn]] void unexpectedEffect()
{
    qFatal("Export preview must not enter a motion, physics, audio, or wall-clock update");
}
}

void* operator new(csmSizeType size, CubismAllocationTag) { return allocate(size); }
void operator delete(void* pointer, CubismAllocationTag) { release(pointer); }

// Only construction and unused effect dependencies are doubled; update and value restoration use production methods.
namespace Live2D { namespace Cubism { namespace Framework {
void* CubismFramework::Allocate(csmSizeType size) { return allocate(size); }
void CubismFramework::Deallocate(void* pointer) { release(pointer); }
void CubismFramework::DeallocateAligned(void* pointer)
{
    if (!pointer) return;
    if (coreStates.erase(pointer) != 1) qFatal("Unknown Core fixture in export preview test");
    release(pointer);
}
CubismIdManager* CubismFramework::GetIdManager() { return idManager.get(); }
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}

CubismUserModel::CubismUserModel()
    : _moc(nullptr), _model(nullptr), _motionManager(nullptr), _expressionManager(nullptr), _eyeBlink(nullptr),
      _breath(nullptr), _modelMatrix(nullptr), _pose(nullptr), _dragManager(nullptr), _physics(nullptr),
      _modelUserData(nullptr), _initialized(false), _updating(false), _opacity(1), _lipSync(false),
      _lastLipSyncValue(0), _dragX(0), _dragY(0), _accelerationX(0), _accelerationY(0), _accelerationZ(0),
      _mocConsistency(false), _debugMode(false), _renderer(nullptr) {}
CubismUserModel::~CubismUserModel() = default;
csmBool CubismUserModel::IsInitialized() { return _initialized; }
void CubismUserModel::IsInitialized(csmBool value) { _initialized = value; }
csmBool CubismUserModel::IsUpdating() { return _updating; }
void CubismUserModel::IsUpdating(csmBool value) { _updating = value; }
void CubismUserModel::SetDragging(csmFloat32, csmFloat32) { unexpectedEffect(); }
void CubismUserModel::SetAcceleration(csmFloat32, csmFloat32, csmFloat32) { unexpectedEffect(); }
void CubismUserModel::SetOpacity(csmFloat32) { unexpectedEffect(); }
csmFloat32 CubismUserModel::GetOpacity() { return _opacity; }
csmBool CubismUserModel::LoadModel(const csmByte*, csmSizeInt, csmBool) { unexpectedEffect(); }
ACubismMotion* CubismUserModel::LoadMotion(const csmByte*, csmSizeInt, const csmChar*, ACubismMotion::FinishedMotionCallback) { unexpectedEffect(); }
ACubismMotion* CubismUserModel::LoadExpression(const csmByte*, csmSizeInt, const csmChar*) { unexpectedEffect(); }
void CubismUserModel::LoadPose(const csmByte*, csmSizeInt) { unexpectedEffect(); }
void CubismUserModel::LoadPhysics(const csmByte*, csmSizeInt) { unexpectedEffect(); }
void CubismUserModel::LoadUserData(const csmByte*, csmSizeInt) { unexpectedEffect(); }
csmBool CubismUserModel::IsHit(CubismIdHandle, csmFloat32, csmFloat32) { unexpectedEffect(); }
void CubismUserModel::MotionEventFired(const csmString&) { unexpectedEffect(); }
void CubismTargetPoint::Update(csmFloat32) { unexpectedEffect(); }
csmFloat32 CubismTargetPoint::GetX() const { unexpectedEffect(); }
csmFloat32 CubismTargetPoint::GetY() const { unexpectedEffect(); }
csmBool CubismMotionQueueManager::IsFinished() { unexpectedEffect(); }
csmBool CubismMotionManager::UpdateMotion(CubismModel*, csmFloat32) { unexpectedEffect(); }
csmBool CubismExpressionMotionManager::UpdateMotion(CubismModel*, csmFloat32) { unexpectedEffect(); }
void CubismEyeBlink::UpdateParameters(CubismModel*, csmFloat32) { unexpectedEffect(); }
void CubismBreath::UpdateParameters(CubismModel*, csmFloat32) { unexpectedEffect(); }
void CubismPose::UpdateParameters(CubismModel*, csmFloat32) { unexpectedEffect(); }
void CubismPhysics::Evaluate(CubismModel*, csmFloat32) { unexpectedEffect(); }
}}}

LAppModel::LAppModel() : _modelSetting(nullptr), _userTimeSeconds(23.0f) {}
LAppModel::~LAppModel() = default;
void LAppModel::MotionEventFired(const csmString&) { unexpectedEffect(); }
csmBool LAppModel::HitTest(const csmChar*, csmFloat32, csmFloat32) { unexpectedEffect(); }
LAppWavFileHandler::LAppWavFileHandler()
    : _rawData(nullptr), _pcmData(nullptr), _userTimeSeconds(0), _lastRms(0), _sampleOffset(0) {}
LAppWavFileHandler::~LAppWavFileHandler() = default;
csmBool LAppWavFileHandler::Update(csmFloat32) { unexpectedEffect(); }
csmFloat32 LAppWavFileHandler::GetRms() const { unexpectedEffect(); }
csmFloat32 LAppPal::GetDeltaTime() { unexpectedEffect(); }

class LAppExportPreviewTests : public QObject
{
    Q_OBJECT
    using ModelOwner = std::unique_ptr<CubismModel, void (*)(CubismModel*)>;

    static ModelOwner createModel()
    {
        auto state = std::make_unique<GetterState>();
        void* handle = allocate(sizeof(CubismModelFixture::ModelHandle), Core::csmAlignofModel);
        auto* core = CubismModelFixture::AttachModel(handle, state.get());
        coreStates.emplace(handle, std::move(state));
        ModelOwner model(new CubismModel(core), [](CubismModel* value) { delete value; });
        model->Initialize();
        return model;
    }

    static void attach(LAppModel& preview, CubismModel& model)
    {
        preview._model = &model;
        preview._initialized = true;
        preview._opacity = 0.72f;
        preview._snapshotMode = true;
        preview.m_mapParaToIndex = {{QStringLiteral("ParamA"), 0}, {QStringLiteral("ParamB"), 1}};
        model.SetParameterValue(0, 0.2f);
        model.SetParameterValue(1, -0.5f);
        model.SaveParameters();
        model.SetParameterValue(0, 0.7f);
        model.SetParameterValue(1, 1.1f);
        model.SetPartOpacity(0, 0.8f);
        model.SetPartOpacity(1, 0.6f);
        model.SetModelOpacity(0.34f);
        const auto virtualParameter = idManager->GetId("VirtualParameter");
        const auto virtualPart = idManager->GetId("VirtualPart");
        model.SetParameterValue(model.GetParameterIndex(virtualParameter), 1.25f);
        model.SetPartOpacity(model.GetPartIndex(virtualPart), 0.55f);
    }

    static void compare(const CubismModel::RuntimeValueSnapshot& actual,
                        const CubismModel::RuntimeValueSnapshot& expected)
    {
        QCOMPARE(actual.valid, expected.valid);
        QCOMPARE(actual.parameterIds, expected.parameterIds);
        QCOMPARE(actual.parameterValues, expected.parameterValues);
        QCOMPARE(actual.savedParameters, expected.savedParameters);
        QCOMPARE(actual.minimums, expected.minimums);
        QCOMPARE(actual.maximums, expected.maximums);
        QCOMPARE(actual.partIds, expected.partIds);
        QCOMPARE(actual.partOpacities, expected.partOpacities);
        QCOMPARE(actual.modelOpacity, expected.modelOpacity);
        QCOMPARE(actual.virtualParameterIds, expected.virtualParameterIds);
        QCOMPARE(actual.virtualParameterValues, expected.virtualParameterValues);
        QCOMPARE(actual.virtualPartIds, expected.virtualPartIds);
        QCOMPARE(actual.virtualPartOpacities, expected.virtualPartOpacities);
    }

private slots:
    void initTestCase() { idManager = std::make_unique<CubismIdManager>(); }
    void cleanup() { QVERIFY(coreStates.empty()); }
    void cleanupTestCase()
    {
        idManager.reset();
        QVERIFY(allocations.empty());
    }

    void timelineSuppressedGroupsKeepExplicitTracksAndPartChannels()
    {
        auto model = createModel();
        LAppModel preview;
        attach(preview, *model);
        preview._eyeBlinkIds.PushBack(idManager->GetId("ParamA"));
        preview._lipSyncIds.PushBack(idManager->GetId("ParamB"));
        preview.m_parameterReferences.eyeBlink = {"ParamA"};
        preview.m_parameterReferences.lipSync = {"ParamB"};
        CMotionPreviewFrame frame;
        frame.model = {{"EyeBlink", 0.1f}, {"LipSync", 0.4f}};
        frame.parameters = {{"ParamA", 0.7f}};
        frame.parts = {{"PartA", 0.25f}};
        QVERIFY(preview.SetTimelinePreview(frame));
        QCOMPARE(model->GetParameterValue(0), 0.7f);
        QCOMPARE(model->GetParameterValue(1), -0.5f);
        QCOMPARE(model->GetPartOpacity(0), 0.25f);
        QCOMPARE(preview.GetTimelinePreviewContext().explicitParameterIds, QSet<QString>{"ParamA"});
        preview.Update();
        QCOMPARE(model->GetParameterValue(0), 0.7f);
        QCOMPARE(model->GetParameterValue(1), -0.5f);
        preview.ClearTimelinePreview();
        QVERIFY(preview.GetTimelinePreviewContext().explicitParameterIds.isEmpty());
        frame.parameters.clear();
        QVERIFY(preview.SetTimelinePreview(frame));
        QCOMPARE(model->GetParameterValue(0), 0.2f);
        QCOMPARE(model->GetParameterValue(1), -0.5f);
        frame.parameters = {{"ParamA", 0.6f}};
        QVERIFY(preview.SetTimelinePreview(frame));
        QCOMPARE(model->GetParameterValue(0), 0.6f);
        QCOMPARE(preview._userTimeSeconds, 23.0f);
    }

    void currentPoseAndSavedBaselineDoNotAdvance()
    {
        auto model = createModel();
        LAppModel preview;
        attach(preview, *model);
        CubismModel::RuntimeValueSnapshot before;
        QVERIFY(model->CaptureRuntimeValues(before));
        auto& state = CubismModelFixture::State(model->GetModel());
        const int updates = state.updates;
        QString error;
        QVERIFY2(preview.BeginExportPreview(false, &error), qPrintable(error));
        QCOMPARE(state.updates, updates);
        QVERIFY(!preview.BeginExportPreview(true, &error));
        for (int frame = 0; frame < 4; ++frame) preview.Update();
        QCOMPARE(state.updates, updates + 4);
        CubismModel::RuntimeValueSnapshot rendered;
        QVERIFY(model->CaptureRuntimeValues(rendered));
        compare(rendered, before);
        QCOMPARE(preview._userTimeSeconds, 23.0f);
        QCOMPARE(preview._opacity, before.modelOpacity);
        QVERIFY2(preview.EndExportPreview(&error), qPrintable(error));
        QCOMPARE(state.updates, updates + 5);
        QVERIFY(!preview.HasExportPreview());
        QCOMPARE(preview._opacity, 0.72f);
        QVERIFY(preview._snapshotMode);
        CubismModel::RuntimeValueSnapshot restored;
        QVERIFY(model->CaptureRuntimeValues(restored));
        compare(restored, before);
        QVERIFY(preview.EndExportPreview());
        QCOMPARE(state.updates, updates + 5);
    }

    void savedSnapshotUsesRealTimelineAndVisibilityThenRestores()
    {
        auto model = createModel();
        LAppModel preview;
        attach(preview, *model);
        preview._snapshotMode = false;
        preview._timelinePreviewActive = true;
        preview._timelineParameters.insert(QStringLiteral("ParamA"), -0.25f);
        preview._timelinePreview.parts.insert(QStringLiteral("PartA"), 0.2f);
        preview._timelinePreview.model.insert(QStringLiteral("Opacity"), 0.45f);
        preview._timelineBaseParameters = {{QStringLiteral("ParamA"), 0.2f}, {QStringLiteral("ParamB"), -0.5f}};
        preview._timelineBaseParts = {{QStringLiteral("PartA"), 0.8f}, {QStringLiteral("PartB"), 0.6f}};
        preview._hiddenPartIds.insert(QStringLiteral("PartB"));
        CubismModel::RuntimeValueSnapshot before;
        QVERIFY(model->CaptureRuntimeValues(before));
        QVERIFY(preview.BeginExportPreview(true));
        for (int frame = 0; frame < 3; ++frame) preview.Update();
        CubismModel::RuntimeValueSnapshot exported;
        QVERIFY(model->CaptureRuntimeValues(exported));
        QCOMPARE(exported.parameterValues, QVector<float>({-0.25f, -0.5f}));
        QCOMPARE(exported.savedParameters, before.savedParameters);
        QCOMPARE(exported.partOpacities, QVector<float>({0.2f, 0.0f}));
        QCOMPARE(exported.modelOpacity, 0.45f);
        QCOMPARE(exported.virtualParameterValues, before.virtualParameterValues);
        QCOMPARE(exported.virtualPartOpacities, before.virtualPartOpacities);
        QCOMPARE(preview._opacity, 0.45f);
        QCOMPARE(preview._userTimeSeconds, 23.0f);
        QVERIFY(preview.EndExportPreview());
        CubismModel::RuntimeValueSnapshot restored;
        QVERIFY(model->CaptureRuntimeValues(restored));
        compare(restored, before);
        QVERIFY(preview.HasTimelinePreview());
        QCOMPARE(preview._timelineParameters.value(QStringLiteral("ParamA")), -0.25f);
        QCOMPARE(preview._timelinePreview.parts.value(QStringLiteral("PartA")), 0.2f);
        QCOMPARE(preview._timelineBaseParameters.value(QStringLiteral("ParamA")), 0.2f);
        QCOMPARE(preview._timelineBaseParts.value(QStringLiteral("PartB")), 0.6f);
        QCOMPARE(preview._hiddenPartIds, QSet<QString>({QStringLiteral("PartB")}));
        QVERIFY(!preview._snapshotMode);
        QCOMPARE(preview._opacity, 0.72f);
    }

    void editorPoseTakesPriorityOverSavedSnapshot()
    {
        auto model = createModel();
        LAppModel preview;
        attach(preview, *model);
        preview.m_editorEntryState = std::make_unique<CubismModel::RuntimeValueSnapshot>();
        QVERIFY(model->CaptureRuntimeValues(*preview.m_editorEntryState));
        preview.m_editorPose = {{QStringLiteral("ParamA"), 0.7f}, {QStringLiteral("ParamB"), 1.1f}};
        preview.m_editorBaseParameters = {{QStringLiteral("ParamA"), 0.1f}};
        auto* entry = preview.m_editorEntryState.get();
        QVERIFY(preview.BeginExportPreview(true));
        preview.Update();
        QCOMPARE(model->GetParameterValue(0), 0.7f);
        QCOMPARE(model->GetParameterValue(1), 1.1f);
        QVERIFY(preview.EndExportPreview());
        QCOMPARE(preview.m_editorEntryState.get(), entry);
        QCOMPARE(preview.m_editorPose.value(QStringLiteral("ParamA")), 0.7f);
        QCOMPARE(preview.m_editorBaseParameters.value(QStringLiteral("ParamA")), 0.1f);
    }

    void ordinaryParameterAndTimelineWritesAreBlocked()
    {
        auto model = createModel();
        LAppModel preview;
        attach(preview, *model);
        preview._timelinePreviewActive = true;
        preview._timelineParameters = {{QStringLiteral("ParamA"), 0.4f}};
        CubismModel::RuntimeValueSnapshot before;
        QVERIFY(model->CaptureRuntimeValues(before));
        QVERIFY(preview.BeginExportPreview(false));
        preview.UpdateParaValue(QStringLiteral("ParamA"), -0.8f, true);
        preview.UpdateParaValue(QStringLiteral("ParamB"), 1.8f, false);
        CMotionPreviewFrame replacement;
        replacement.parameters.insert(QStringLiteral("ParamA"), -0.9f);
        QVERIFY(!preview.SetTimelinePreview(replacement));
        preview.ClearTimelinePreview();
        preview.SetSnapshotMode(false);
        preview.SetAutoEyeBlink(false);
        preview.SetAutoBreath(false);
        preview.SetAutoPhysics(false);
        preview.SetEnableMouseFollow(true);
        preview.SetDragging(1.0f, 1.0f);
        QVERIFY(preview._autoEyeBlink && preview._autoBreath && preview._autoPhysics);
        QVERIFY(!preview._enableMouseFollow);
        QVERIFY(preview._snapshotMode);
        QVERIFY(preview.HasTimelinePreview());
        QCOMPARE(preview._timelineParameters.value(QStringLiteral("ParamA")), 0.4f);
        CubismModel::RuntimeValueSnapshot after;
        QVERIFY(model->CaptureRuntimeValues(after));
        compare(after, before);
        QCOMPARE(CubismModelFixture::State(model->GetModel()).updates, 0);
        QVERIFY(preview.EndExportPreview());
    }

    void captureFailureDoesNotPublishOrChangeValues()
    {
        auto model = createModel();
        LAppModel preview;
        attach(preview, *model);
        auto& state = CubismModelFixture::State(model->GetModel());
        state.values[0] = std::numeric_limits<float>::quiet_NaN();
        QString error;
        QVERIFY(!preview.BeginExportPreview(false, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!preview.HasExportPreview());
        QVERIFY(std::isnan(state.values[0]));
        QCOMPARE(state.values[1], 1.1f);
        QCOMPARE(state.updates, 0);
        state.values[0] = 0.7f;
        preview._updating = true;
        QVERIFY(!preview.BeginExportPreview(false, &error));
        QVERIFY(!preview.HasExportPreview());
        preview._updating = false;
        QVERIFY(preview.BeginExportPreview(false, &error));
        QVERIFY(preview.EndExportPreview());
    }

    void restoreFailureKeepsFreezeAndCanRetry()
    {
        auto model = createModel();
        auto foreign = createModel();
        LAppModel preview;
        attach(preview, *model);
        CubismModel::RuntimeValueSnapshot before;
        QVERIFY(model->CaptureRuntimeValues(before));
        QVERIFY(preview.BeginExportPreview(true));
        preview.Update();
        auto& state = CubismModelFixture::State(model->GetModel());
        const int exportedUpdates = state.updates;
        preview._model = foreign.get();
        QString error;
        QVERIFY(!preview.EndExportPreview(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(preview.HasExportPreview());
        preview.Update();
        QCOMPARE(CubismModelFixture::State(foreign->GetModel()).updates, 0);
        preview._model = model.get();
        state.maximum[0] = 2.0f;
        const float exportedValue = model->GetParameterValue(0);
        QVERIFY(!preview.EndExportPreview(&error));
        QVERIFY(preview.HasExportPreview());
        QCOMPARE(model->GetParameterValue(0), exportedValue);
        QCOMPARE(state.updates, exportedUpdates);
        state.maximum[0] = 1.0f;
        QVERIFY2(preview.EndExportPreview(&error), qPrintable(error));
        QVERIFY(!preview.HasExportPreview());
        CubismModel::RuntimeValueSnapshot restored;
        QVERIFY(model->CaptureRuntimeValues(restored));
        compare(restored, before);
    }
};

QTEST_GUILESS_MAIN(LAppExportPreviewTests)
#include "lapp_export_preview_tests.moc"
