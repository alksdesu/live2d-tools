#include "live2d/cphysicsbake.h"
#include "live2dstruct/cmotionblendpreview.h"
#include "live2dstruct/cphysicsdocument.h"
#include "framework/CubismFramework.hpp"
#include "framework/ICubismAllocator.hpp"
#include "framework/Id/CubismId.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Model/CubismParameterReferences.hpp"
#include "framework/Physics/CubismPhysics.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <new>

using namespace Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;

// 本目标不创建渲染器，Framework 的进程清理无需释放 GL 单例。
void Rendering::CubismRenderer::StaticRelease() {}

namespace {
class Allocator : public ICubismAllocator
{
public:
    void* Allocate(csmSizeType size) override
    {
        void* value = std::malloc(size);
        if (value) allocations.insert(value);
        return value;
    }
    void Deallocate(void* value) override { allocations.remove(value); std::free(value); }
    void* AllocateAligned(csmSizeType size, csmUint32 alignment) override
    {
        void* value = ::operator new(size, std::align_val_t(alignment), std::nothrow);
        if (value) aligned.emplace(value, alignment);
        return value;
    }
    void DeallocateAligned(void* value) override
    {
        if (!value) return;
        const auto found = aligned.find(value);
        if (found == aligned.end()) qFatal("Unknown aligned allocation");
        ::operator delete(value, std::align_val_t(found->second));
        aligned.erase(found);
    }
    QSet<void*> allocations;
    std::map<void*, unsigned> aligned;
};

QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

class Simulation
{
public:
    ~Simulation() { physics.reset(); if (moc && model) moc->DeleteModel(model); if (moc) CubismMoc::Delete(moc); }
    bool Load(const QByteArray& mocBytes, const QByteArray& physicsBytes = {})
    {
        moc = CubismMoc::Create(reinterpret_cast<const csmByte*>(mocBytes.constData()), mocBytes.size(), true);
        if (!moc || !(model = moc->CreateModel())) return false;
        for (int i = 0; i < model->GetParameterCount(); ++i) {
            const auto id = QString::fromUtf8(model->GetParameterId(i)->GetString().GetRawString());
            indices.insert(id, i);
            base.insert(id, model->GetParameterDefaultValue(i));
        }
        if (!physicsBytes.isEmpty()) {
            physics.reset(CubismPhysics::Create(reinterpret_cast<const csmByte*>(physicsBytes.constData()), physicsBytes.size(), &error));
            CubismPhysics::ParameterDefinitionState binding;
            if (!physics || !physics->PrepareModelBinding(*model, {}, binding, &error)) return false;
            physics->CommitParameterDefinitionChange(binding);
        }
        return true;
    }
    void Set(const QMap<QString, float>& values)
    {
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            const auto index = indices.constFind(it.key());
            if (index != indices.cend()) model->SetParameterValue(index.value(), it.value());
        }
    }
    float Value(const QString& id) { return model->GetParameterValue(indices.value(id)); }
    QByteArray Values() const
    {
        return QByteArray(reinterpret_cast<const char*>(Core::csmGetParameterValues(model->GetModel())),
                          model->GetParameterCount() * int(sizeof(float)));
    }
    CubismMoc* moc = nullptr;
    CubismModel* model = nullptr;
    std::unique_ptr<CubismPhysics, void (*)(CubismPhysics*)> physics{nullptr, CubismPhysics::Delete};
    QMap<QString, int> indices;
    QMap<QString, float> base;
    QString error;
};

CPhysicsBake::Request requestFor(const QString& name = QStringLiteral("Mark"))
{
    CPhysicsBake::Request request;
    const QString path = QString(TEST_RESOURCES) + '/' + name + '/' + name;
    request.mocBytes = read(path + ".moc3");
    request.physicsBytes = read(path + ".physics3.json");
    Simulation model;
    if (!model.Load(request.mocBytes)) return request;
    request.baseParameters = model.base;
    const auto physics = QJsonDocument::fromJson(request.physicsBytes).object();
    for (const auto& group : physics.value("PhysicsSettings").toArray())
        for (const auto& output : group.toObject().value("Output").toArray())
            request.outputIds.insert(output.toObject().value("Destination").toObject().value("Id").toString());
    request.timeline.AddPoint("ParamAngleX", 0, 0);
    request.timeline.AddPoint("ParamAngleX", .5f, 30);
    request.timeline.AddPoint("ParamAngleX", 1, -20);
    request.timeline.SetTiming(30, 1);
    request.useFades = false;
    request.duration = .43;
    request.fps = 30;
    request.warmupCycles = 1;
    return request;
}

QByteArray withFps(const QByteArray& bytes, double fps)
{
    auto object = QJsonDocument::fromJson(bytes).object();
    auto meta = object.value("Meta").toObject();
    meta.insert("Fps", fps);
    object.insert("Meta", meta);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray withOutputScale(const QByteArray& bytes, double scale)
{
    auto object = QJsonDocument::fromJson(bytes).object();
    auto groups = object.value("PhysicsSettings").toArray();
    for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        auto group = groups[groupIndex].toObject();
        auto outputs = group.value("Output").toArray();
        for (int outputIndex = 0; outputIndex < outputs.size(); ++outputIndex) {
            auto output = outputs[outputIndex].toObject();
            output.insert("Scale", scale);
            outputs[outputIndex] = output;
        }
        group.insert("Output", outputs);
        groups[groupIndex] = group;
    }
    object.insert("PhysicsSettings", groups);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QMap<QString, QVector<QPointF>> reference(const CPhysicsBake::Request& request)
{
    Simulation simulation;
    if (!simulation.Load(request.mocBytes, request.physicsBytes)) return {};
    const auto input = [&](double time, bool loop) {
        simulation.Set(request.baseParameters);
        const double duration = request.timeline.Timing().duration;
        CMotionPreviewFrame frame;
        if (request.useFades)
            frame = CMotionBlendPreview::Evaluate(request.timeline, time, request.baseParameters,
                                                  request.eyeBlinkIds, request.lipSyncIds, loop);
        else {
            const double t = duration > 0 ? (loop ? std::fmod(time, duration) : std::min(time, duration)) : time;
            frame = request.timeline.EvaluateFrame(float(t));
        }
        simulation.Set(frame.ResolveParameters(request.eyeBlinkIds, request.lipSyncIds));
    };
    input(0, request.warmupCycles ? true : request.loop);
    simulation.physics->Stabilization(simulation.model);
    simulation.model->Update();
    const double period = request.timeline.HasAnimation() && request.timeline.Timing().duration > 0
        ? request.timeline.Timing().duration : request.duration;
    for (int cycle = 0; cycle < request.warmupCycles; ++cycle) {
        double previous = 0;
        for (int step = 1; step <= int(std::ceil(period * request.fps)); ++step) {
            const double time = std::min(step / request.fps, period);
            input(time == period ? 0 : time, true);
            simulation.physics->Evaluate(simulation.model, float(time - previous));
            simulation.model->Update();
            previous = time;
        }
    }
    QMap<QString, QVector<QPointF>> result;
    for (int frame = 0; frame <= int(std::ceil(request.duration * request.fps)); ++frame) {
        if (frame) {
            input(frame / request.fps, request.loop);
            simulation.physics->Evaluate(simulation.model, float(1 / request.fps));
            simulation.model->Update();
        }
        for (const auto& id : request.outputIds) result[id].append(QPointF(frame / request.fps, simulation.Value(id)));
    }
    return result;
}

bool finish(CPhysicsBake& bake, QString* error, int batch = 7)
{
    while (!bake.IsFinished()) if (!bake.Step(batch, error)) return false;
    return true;
}
}

class PhysicsBakeTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(CubismFramework::StartUp(&allocator));
        CubismFramework::Initialize();
        QVERIFY(CubismFramework::IsInitialized());
    }
    void cleanupTestCase()
    {
        CubismFramework::Dispose();
        CubismFramework::CleanUp();
        QCOMPARE(allocator.aligned.size(), size_t(0));
        QCOMPARE(allocator.allocations.size(), 0);
    }
    void originalModelsMatchIndependentReplay_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("warmup");
        QTest::addColumn<double>("physicsFps");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"})
            for (int warmup : {0, 1, 3})
                for (double fps : {0.0, 60.0})
                    QTest::newRow(qPrintable(QString("%1-warm%2-fps%3").arg(name).arg(warmup).arg(fps))) << QString(name) << warmup << fps;
    }
    void originalModelsMatchIndependentReplay()
    {
        QFETCH(QString, name);
        QFETCH(int, warmup);
        QFETCH(double, physicsFps);
        auto request = requestFor(name);
        QVERIFY(!request.mocBytes.isEmpty());
        QVERIFY(!request.physicsBytes.isEmpty());
        request.physicsBytes = withFps(request.physicsBytes, physicsFps);
        request.warmupCycles = warmup;
        request.expectedMocDigest = QCryptographicHash::hash(request.mocBytes, QCryptographicHash::Sha256);
        request.expectedPhysicsDigest = QCryptographicHash::hash(request.physicsBytes, QCryptographicHash::Sha256);
        const auto mocBefore = request.mocBytes, physicsBefore = request.physicsBytes;
        const auto originalTimeline = request.timeline;
        Simulation preview;
        QVERIFY(preview.Load(request.mocBytes));
        auto previewValues = preview.base;
        previewValues["ParamAngleX"] = 7.5f;
        preview.Set(previewValues);
        preview.model->SaveParameters();
        const auto previewBefore = preview.Values();
        const auto expected = reference(request);
        QVERIFY(!expected.isEmpty());
        CPhysicsBake bake;
        QString error;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QCOMPARE(bake.CompletedSteps(), 0);
        QVERIFY(bake.TotalSteps() >= qint64(std::ceil(request.duration * request.fps)) + 2);
        while (!bake.IsFinished()) {
            const auto previous = bake.CompletedSteps();
            QVERIFY2(bake.Step(3, &error), qPrintable(error));
            QVERIFY(bake.CompletedSteps() - previous <= 3);
        }
        CPhysicsBake::Result result;
        QVERIFY2(bake.CaptureResult(result, &error), qPrintable(error));
        QCOMPARE(result.endTime, std::ceil(request.duration * request.fps) / request.fps);
        QCOMPARE(result.curves, expected);
        QCOMPARE(result.curves.first().size(), 14);
        QCOMPARE(bake.CompletedSteps(), bake.TotalSteps());
        QCOMPARE(preview.Values(), previewBefore);
        preview.model->LoadParameters();
        QCOMPARE(preview.Values(), previewBefore);
        QCOMPARE(request.mocBytes, mocBefore);
        QCOMPARE(request.physicsBytes, physicsBefore);
        QVERIFY(request.timeline.HasSameContent(originalTimeline));
    }
    void loopFadeAndGroupEffects()
    {
        auto request = requestFor();
        request.duration = 2.1;
        request.useFades = true;
        request.loop = true;
        request.eyeBlinkIds = {"ParamAngleX"};
        request.lipSyncIds = {"ParamAngleZ"};
        QVERIFY(request.timeline.AddPoint({CMotionTarget::Model, "EyeBlink"}, 0, .25f));
        QVERIFY(request.timeline.AddPoint({CMotionTarget::Model, "EyeBlink"}, 1, .75f));
        QVERIFY(request.timeline.AddPoint({CMotionTarget::Model, "LipSync"}, 0, 2));
        QVERIFY(request.timeline.AddPoint({CMotionTarget::Model, "LipSync"}, 1, -2));
        QVERIFY(request.timeline.AddPoint("VirtualTrack", 0, 123));
        QVERIFY(request.timeline.AddPoint("VirtualTrack", 1, -123));
        const auto expected = reference(request);
        CPhysicsBake bake;
        QString error;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QVERIFY2(finish(bake, &error, 1), qPrintable(error));
        CPhysicsBake::Result result;
        QVERIFY(bake.CaptureResult(result));
        QCOMPARE(result.curves, expected);
        QVERIFY(!result.curves.contains("VirtualTrack"));
        const auto inputBefore = request.timeline;
        request.newAnimation = true;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QVERIFY2(finish(bake, &error), qPrintable(error));
        QVERIFY(bake.CaptureResult(result));
        QCOMPARE(result.curves, expected);
        QVERIFY(request.timeline.HasSameContent(inputBefore));
    }
    void crossGroupInputUsesEarlierPhysicsOutput_data()
    {
        QTest::addColumn<bool>("newAnimation");
        QTest::newRow("merge") << false;
        QTest::newRow("new-animation") << true;
    }
    void crossGroupInputUsesEarlierPhysicsOutput()
    {
        QFETCH(bool, newAnimation);
        auto request = requestFor();
        request.newAnimation = newAnimation;
        auto document = QJsonDocument::fromJson(request.physicsBytes).object();
        auto groups = document.value("PhysicsSettings").toArray();
        auto first = groups.first().toObject();
        auto outputs = first.value("Output").toArray();
        first["Output"] = QJsonArray{outputs[0]};
        auto second = first;
        second["Id"] = "Cascade";
        auto input = first.value("Input").toArray().first().toObject();
        input["Source"] = QJsonObject{{"Target", "Parameter"}, {"Id", "ParamHairFront"}};
        input["Weight"] = 100;
        second["Input"] = QJsonArray{input};
        second["Output"] = QJsonArray{outputs[1]};
        document["PhysicsSettings"] = QJsonArray{first, second};
        auto meta = document.value("Meta").toObject();
        meta["PhysicsSettingCount"] = 2;
        meta["TotalInputCount"] = 5;
        meta["TotalOutputCount"] = 2;
        meta["VertexCount"] = 4;
        meta["PhysicsDictionary"] = QJsonArray{QJsonObject{{"Id", first.value("Id")}, {"Name", "first"}},
                                                QJsonObject{{"Id", "Cascade"}, {"Name", "second"}}};
        document["Meta"] = meta;
        request.physicsBytes = QJsonDocument(document).toJson();
        request.outputIds = {"ParamHairFront", "ParamHairSide"};
        request.duration = 1;
        const auto expected = reference(request);
        QVERIFY(!expected.isEmpty());
        CPhysicsBake bake;
        QString error;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QVERIFY2(finish(bake, &error), qPrintable(error));
        CPhysicsBake::Result result;
        QVERIFY(bake.CaptureResult(result));
        QCOMPARE(result.curves, expected);
        QVERIFY(result.curves["ParamHairSide"] != result.curves["ParamHairFront"]);
    }
    void tZeroAndEmptyTimeline()
    {
        auto request = requestFor();
        request.timeline.Clear();
        request.duration = 0;
        request.baseParameters["ParamAngleX"] = 20;
        CPhysicsBake bake;
        QString error;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QCOMPARE(bake.TotalSteps(), 2);
        QVERIFY(bake.Step(1));
        QVERIFY(!bake.IsFinished());
        QVERIFY(bake.Step(1));
        CPhysicsBake::Result result;
        QVERIFY(bake.CaptureResult(result));
        QCOMPARE(result.endTime, 0.0);
        QCOMPARE(result.curves, reference(request));
        for (const auto& curve : result.curves) QCOMPARE(curve.size(), 1);
        request.duration = .23;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QCOMPARE(bake.TotalSteps(), 16);
        QVERIFY2(finish(bake, &error), qPrintable(error));
        QVERIFY(bake.CaptureResult(result));
        QCOMPARE(result.curves, reference(request));
    }
    void fractionalSamplingAndWarmStart()
    {
        auto request = requestFor();
        request.fps = 29.97;
        request.duration = .1001;
        request.warmupCycles = 0;
        CPhysicsBake cold;
        QString error;
        QVERIFY2(cold.Prepare(request, &error), qPrintable(error));
        QVERIFY2(finish(cold, &error), qPrintable(error));
        CPhysicsBake::Result coldResult;
        QVERIFY(cold.CaptureResult(coldResult));
        QCOMPARE(coldResult.curves, reference(request));
        request.warmupCycles = 1;
        CPhysicsBake warm;
        QVERIFY2(warm.Prepare(request, &error), qPrintable(error));
        QVERIFY2(finish(warm, &error), qPrintable(error));
        CPhysicsBake::Result result;
        QVERIFY(warm.CaptureResult(result));
        QCOMPARE(result.curves, reference(request));
        QCOMPARE(result.endTime, 3.0 / 29.97);
        QVERIFY(result.curves["ParamHairFront"].first().y() != coldResult.curves["ParamHairFront"].first().y());
        for (const auto& curve : result.curves) {
            QCOMPARE(curve.size(), 4);
            QCOMPARE(curve.first().x(), 0.0);
            QCOMPARE(curve.last().x(), result.endTime);
        }
    }
    void outputDiagnosticsSeparateWarmupAndRecordedClipping()
    {
        auto request = requestFor("Mark");
        request.physicsBytes = withOutputScale(request.physicsBytes, 10000.0);
        request.warmupCycles = 1;
        CPhysicsBake bake;
        QString error;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QVERIFY2(finish(bake, &error), qPrintable(error));
        CPhysicsBake::Result result;
        QVERIFY2(bake.CaptureResult(result, &error), qPrintable(error));
        QVERIFY(!result.diagnostics.isEmpty());
        bool recorded = false, warmup = false;
        for (auto it = result.diagnostics.cbegin(); it != result.diagnostics.cend(); ++it) {
            const auto& diagnostic = it.value();
            QVERIFY(diagnostic.hasRawSample);
            QVERIFY(std::isfinite(diagnostic.rawMinimum));
            QVERIFY(std::isfinite(diagnostic.rawMaximum));
            recorded |= diagnostic.recordedClippedSteps > 0;
            warmup |= diagnostic.warmupClippedSteps > 0;
        }
        QVERIFY(recorded);
        QVERIFY(warmup);
    }
    void optimizedCurvesKeepEndpointsAndReducePoints()
    {
        auto request = requestFor("Mark");
        request.warmupCycles = 0;
        request.optimizeCurves = true;
        request.curveTolerance = 100.0;
        CPhysicsBake bake;
        QString error;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QVERIFY2(finish(bake, &error), qPrintable(error));
        CPhysicsBake::Result result;
        QVERIFY2(bake.CaptureResult(result, &error), qPrintable(error));
        QCOMPARE(result.sourcePointCount, qint64(request.outputIds.size() * 14));
        QVERIFY(result.outputPointCount < result.sourcePointCount);
        for (const auto& curve : result.curves) {
            QVERIFY(curve.size() >= 2);
            QCOMPARE(curve.first().x(), 0.0);
            QCOMPARE(curve.last().x(), result.endTime);
        }
    }
    void cancellationAndFailedPrepareAreAtomic()
    {
        auto request = requestFor();
        CPhysicsBake bake;
        CPhysicsBake::Result sentinel;
        sentinel.curves["untouched"] = {QPointF(7, 8)};
        sentinel.endTime = 19;
        QString error;
        QVERIFY(!bake.CaptureResult(sentinel, &error));
        QVERIFY(bake.Prepare(request));
        QVERIFY(bake.Step(2));
        const auto progress = bake.CompletedSteps();
        auto invalid = request;
        invalid.expectedMocDigest = QByteArray(32, 'x');
        QVERIFY(!bake.Prepare(invalid, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(bake.CompletedSteps(), progress);
        QVERIFY(!bake.Step(0));
        QCOMPARE(bake.CompletedSteps(), progress);
        bake.Cancel();
        QVERIFY(bake.IsCanceled());
        QVERIFY(!bake.IsFinished());
        QVERIFY(!bake.Failed());
        QVERIFY(!bake.Step());
        QVERIFY(!bake.CaptureResult(sentinel));
        QCOMPARE(sentinel.curves["untouched"], QVector<QPointF>{QPointF(7, 8)});
        QCOMPARE(sentinel.endTime, 19.0);
        QVERIFY(bake.Prepare(request));
        QVERIFY(finish(bake, &error));
        CPhysicsBake::Result original;
        QVERIFY(bake.CaptureResult(original));
        invalid = request;
        invalid.expectedPhysicsDigest = QByteArray(32, 'x');
        QVERIFY(!bake.Prepare(invalid));
        bake.Cancel();
        QVERIFY(bake.CaptureResult(sentinel));
        QCOMPARE(sentinel.curves, original.curves);
    }
    void invalidRequests_data()
    {
        QTest::addColumn<int>("mutation");
        for (int mutation = 0; mutation < 18; ++mutation) QTest::newRow(qPrintable(QString::number(mutation))) << mutation;
    }
    void excessivePhysicsSubstepsKeepPreparedTask()
    {
        auto request = requestFor();
        CPhysicsBake bake;
        QVERIFY(bake.Prepare(request));
        const auto total = bake.TotalSteps();
        for (double physicsFps : {1000.0, double(std::numeric_limits<float>::max())}) {
            auto excessive = request;
            excessive.fps = .2;
            excessive.duration = 1;
            excessive.physicsBytes = withFps(excessive.physicsBytes, physicsFps);
            excessive.expectedPhysicsDigest.clear();
            QString error;
            QVERIFY(!bake.Prepare(excessive, &error));
            QVERIFY(error.contains(physicsFps == 1000.0 ? QStringLiteral("1024") : QStringLiteral("物理帧率")));
            QCOMPARE(bake.TotalSteps(), total);
            QCOMPARE(bake.CompletedSteps(), 0);
            QVERIFY(!bake.Failed());
        }
        QVERIFY(bake.Step(1));
        QCOMPARE(bake.CompletedSteps(), 1);
        request.physicsBytes = withFps(request.physicsBytes, 1000);
        request.expectedPhysicsDigest.clear();
        QVERIFY(bake.Prepare(request));
    }
    void invalidRequests()
    {
        QFETCH(int, mutation);
        auto request = requestFor();
        switch (mutation) {
        case 0: request.mocBytes.clear(); break;
        case 1: request.mocBytes = QByteArray(128, 'x'); break;
        case 2: request.physicsBytes = "{"; break;
        case 3: request.fps = 0; break;
        case 4: request.fps = std::numeric_limits<double>::infinity(); break;
        case 5: request.duration = -1; break;
        case 6: request.duration = std::numeric_limits<double>::quiet_NaN(); break;
        case 7: request.warmupCycles = 4; break;
        case 8: request.outputIds.clear(); break;
        case 9: request.outputIds = {"ParamAngleX"}; break;
        case 10: request.outputIds = {"Missing"}; break;
        case 11: request.baseParameters.remove("ParamAngleX"); break;
        case 12: request.baseParameters["ParamAngleX"] = 1000; break;
        case 13: request.baseParameters["ParamAngleX"] = std::numeric_limits<float>::quiet_NaN(); break;
        case 14: request.timeline.StartRecording(); break;
        case 15: request.duration = 100000; break;
        case 16: request.eyeBlinkIds.insert(QString(QChar::Null)); break;
        case 17: {
            auto root = QJsonDocument::fromJson(request.physicsBytes).object();
            auto groups = root.value("PhysicsSettings").toArray();
            auto group = groups[0].toObject();
            auto inputs = group.value("Input").toArray();
            auto input = inputs[0].toObject();
            input["Source"] = QJsonObject{{"Target", "Parameter"}, {"Id", "MissingReal"}};
            inputs[0] = input; group["Input"] = inputs; groups[0] = group; root["PhysicsSettings"] = groups;
            request.physicsBytes = QJsonDocument(root).toJson();
            break;
        }
        }
        CPhysicsBake bake;
        QString error;
        QVERIFY(!bake.Prepare(request, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(bake.TotalSteps(), 0);
    }
    void budgetIncludesRetainedTracksAndReplacesOnlySelectedOutput()
    {
        auto request = requestFor();
        request.outputIds = {"ParamHairFront"};
        request.fps = 1;
        request.duration = 99998;
        request.warmupCycles = 0;
        CPhysicsBake bake;
        QString error;
        QVERIFY(!bake.Prepare(request, &error));
        QVERIFY(error.contains("100000"));
        request.timeline.Clear();
        QVERIFY(request.timeline.AddPoint("ParamHairFront", 0, 0));
        QVERIFY(request.timeline.AddPoint("ParamHairFront", 1, 1));
        request.duration = 99999;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QCOMPARE(bake.TotalSteps(), 100001);
        QVERIFY(request.timeline.AddPoint({CMotionTarget::PartOpacity, "ParamHairFront"}, 0, 1));
        QVERIFY(!bake.Prepare(request, &error));
        QVERIFY(error.contains("100000"));
        const auto inputBefore = request.timeline;
        request.newAnimation = true;
        QVERIFY2(bake.Prepare(request, &error), qPrintable(error));
        QCOMPARE(bake.TotalSteps(), 100001);
        QVERIFY(request.timeline.HasSameContent(inputBefore));
    }
private:
    Allocator allocator;
};

QTEST_GUILESS_MAIN(PhysicsBakeTests)
#include "physics_bake_tests.moc"
