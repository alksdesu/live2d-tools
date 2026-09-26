#include "framework/Model/CubismModel.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include "cubism_model_fixture.h"
#include "framework/Physics/CubismPhysics.hpp"
#include "framework/Model/CubismParameterReferences.hpp"
#include "framework/Utils/CubismJson.hpp"

#include <QtTest>
#include "live2dstruct/cphysicsdocument.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <functional>
#include <new>
#include <unordered_map>

using namespace Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;
using CubismModelFixture::GetterState;
using CubismModelFixture::State;

namespace {
std::unordered_map<void*, size_t> allocations;
std::unordered_map<void*, std::unique_ptr<GetterState>> coreStates;
std::unique_ptr<CubismIdManager> idManager;
bool rejectSdkAllocation = false;
int sdkAllocationCalls = 0;

void* allocate(csmSizeType size, size_t alignment = 0)
{
    ++sdkAllocationCalls;
    if (rejectSdkAllocation) throw std::bad_alloc();
    void* memory = alignment ? ::operator new(size, std::align_val_t(alignment), std::nothrow) : std::malloc(size);
    if (!memory) throw std::bad_alloc();
    allocations.emplace(memory, alignment);
    return memory;
}

void release(void* memory)
{
    if (!memory) return;
    const auto found = allocations.find(memory);
    if (found == allocations.end()) qFatal("Unknown SDK allocation in physics definition test");
    const size_t alignment = found->second;
    allocations.erase(found);
    if (alignment) ::operator delete(memory, std::align_val_t(alignment));
    else std::free(memory);
}

}

void* operator new(csmSizeType size, CubismAllocationTag) { return allocate(size); }
void operator delete(void* memory, CubismAllocationTag) { release(memory); }

namespace Live2D { namespace Cubism { namespace Framework {
void* CubismFramework::Allocate(csmSizeType size) { return allocate(size); }
void CubismFramework::Deallocate(void* memory) { release(memory); }
void CubismFramework::DeallocateAligned(void* memory)
{
    if (!memory) return;
    if (coreStates.erase(memory) != 1) qFatal("Unknown Core allocation in physics definition test");
    release(memory);
}
CubismIdManager* CubismFramework::GetIdManager() { return idManager.get(); }
void CubismFramework::Initialize() { Utils::Value::StaticInitializeNotForClientCall(); }
void CubismFramework::Dispose() { Utils::Value::StaticReleaseNotForClientCall(); }
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}
}}}


class PhysicsParameterDefinitionTests : public QObject
{
    Q_OBJECT
    using ModelOwner = std::unique_ptr<CubismModel, void (*)(CubismModel*)>;
    using PhysicsOwner = std::unique_ptr<CubismPhysics, void (*)(CubismPhysics*)>;
    static ModelOwner model(const std::function<void(GetterState&)>& configure = {})
    {
        auto data = std::make_unique<GetterState>();
        if (configure) configure(*data);
        void* handle = allocate(sizeof(CubismModelFixture::ModelHandle), Core::csmAlignofModel);
        auto* core = CubismModelFixture::AttachModel(handle, data.get());
        coreStates.emplace(handle, std::move(data));
        ModelOwner result(new CubismModel(core), [](CubismModel* value) { delete value; });
        result->Initialize();
        return result;
    }
    static QByteArray physicsJson()
    {
        return R"({"Version":3,"Meta":{"PhysicsSettingCount":1,"TotalInputCount":1,"TotalOutputCount":1,"VertexCount":2,"Fps":30,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}}},"PhysicsSettings":[{"Id":"one","Input":[{"Source":{"Target":"Parameter","Id":"ParamA"},"Weight":100,"Type":"X","Reflect":true}],"Output":[{"Destination":{"Target":"Parameter","Id":"ParamB"},"VertexIndex":1,"Scale":1,"Weight":100,"Type":"X","Reflect":false}],"Vertices":[{"Position":{"X":0,"Y":0},"Mobility":1,"Delay":1,"Acceleration":1,"Radius":0},{"Position":{"X":0,"Y":1},"Mobility":0.8,"Delay":0.2,"Acceleration":1,"Radius":1}],"Normalization":{"Position":{"Minimum":-1,"Maximum":1,"Default":0},"Angle":{"Minimum":-30,"Maximum":30,"Default":0}}}]})";
    }
    static PhysicsOwner physics(const QByteArray& json = physicsJson())
    {
        PhysicsOwner value(CubismPhysics::Create(reinterpret_cast<const csmByte*>(json.constData()), json.size()), CubismPhysics::Delete);
        if (!value) qFatal("Physics fixture JSON was rejected");
        return value;
    }
    static QByteArray changedPhysics(const std::function<void(QJsonObject&, QJsonObject&)>& change)
    {
        auto root = QJsonDocument::fromJson(physicsJson()).object();
        auto group = root["PhysicsSettings"].toArray()[0].toObject();
        change(root, group);
        root["PhysicsSettings"] = QJsonArray{group};
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }
    static QByteArray dynamicState(const CubismPhysics& physics)
    {
        QByteArray result;
        const auto append = [&](const auto& value) { result.append(reinterpret_cast<const char*>(&value), sizeof(value)); };
        append(physics._currentRemainTime);
        for (const auto* values : {&physics._parameterCaches, &physics._parameterInputCaches}) {
            const auto count = values->size(); append(count);
            for (float value : *values) append(value);
        }
        for (int i = 0; i < physics._physicsRig->Particles.GetSize(); ++i) {
            const auto& particle = physics._physicsRig->Particles[i];
            for (const auto* value : {&particle.Position, &particle.LastPosition, &particle.LastGravity, &particle.Velocity, &particle.Force}) {
                append(value->X); append(value->Y);
            }
        }
        for (int i = 0; i < physics._physicsRig->Settings.GetSize(); ++i)
            for (int j = 0; j < physics._physicsRig->Settings[i].OutputCount; ++j) {
                append(physics._currentRigOutputs[i].outputs[j]); append(physics._previousRigOutputs[i].outputs[j]);
            }
        for (int i = 0; i < physics._physicsRig->Outputs.GetSize(); ++i) {
            append(physics._physicsRig->Outputs[i].ValueBelowMinimum); append(physics._physicsRig->Outputs[i].ValueExceededMaximum);
        }
        return result;
    }
    static void warm(CubismPhysics& physics)
    {
        physics._currentRemainTime = 0.0125f;
        physics._parameterCaches = {0.6f, -0.0f};
        physics._parameterInputCaches = {-0.4f, 1.25f};
        physics._physicsRig->Particles[1].Position = CubismVector2(0.25f, 0.75f);
        physics._physicsRig->Particles[1].LastPosition = CubismVector2(-0.125f, 0.875f);
        physics._physicsRig->Particles[1].LastGravity = CubismVector2(0.125f, 0.875f);
        physics._physicsRig->Particles[1].Velocity = CubismVector2(0.375f, -0.625f);
        physics._physicsRig->Particles[1].Force = CubismVector2(-0.25f, 0.5f);
        physics._currentRigOutputs[0].outputs[0] = 0.375f;
        physics._previousRigOutputs[0].outputs[0] = -0.625f;
        physics._physicsRig->Outputs[0].ValueBelowMinimum = -3.0f;
        physics._physicsRig->Outputs[0].ValueExceededMaximum = 4.0f;
    }
    static QJsonObject observationState(const PhysicsObservation& observation)
    {
        const auto row = [](const PhysicsInputObservation& value) {
            return QJsonObject{{"id", value.id}, {"type", value.type}, {"active", value.active}, {"sampled", value.sampled},
                {"range", value.hasRange}, {"value", value.value}, {"minimum", value.minimum}, {"maximum", value.maximum}, {"default", value.defaultValue}};
        };
        QJsonArray inputs, outputs, particles;
        for (const auto& value : observation.inputs) inputs.append(row(value));
        for (const auto& value : observation.outputs) {
            auto fields = row(value); fields["raw"] = value.rawValue; fields["clamped"] = value.clampedValue;
            fields["clipped"] = value.clipped; outputs.append(fields);
        }
        for (const auto& point : observation.particles) particles.append(QJsonArray{point.x(), point.y()});
        return {{"group", observation.groupIndex}, {"id", observation.groupId}, {"digest", QString::fromLatin1(observation.sourceDigest.toHex())},
            {"generation", QString::number(observation.generation)}, {"sequence", QString::number(observation.sequence)}, {"time", observation.time},
            {"sampled", observation.sampled}, {"particles", particles}, {"inputs", inputs}, {"outputs", outputs}};
    }
private slots:
    void init()
    {
        QVERIFY(allocations.empty());
        rejectSdkAllocation = false;
        idManager = std::make_unique<CubismIdManager>();
        CubismFramework::Initialize();
    }
    void cleanup()
    {
        CubismFramework::Dispose();
        idManager.reset();
        QVERIFY(coreStates.empty());
        QVERIFY(allocations.empty());
    }

    void createNormalizesCountsBeforeSdkAllocationAndKeepsRawDigest()
    {
        auto root = QJsonDocument::fromJson(physicsJson()).object();
        auto meta = root["Meta"].toObject();
        for (const auto& field : {"PhysicsSettingCount", "TotalInputCount", "TotalOutputCount", "VertexCount"}) meta[field] = 2147483647;
        root["Meta"] = meta;
        auto bytes = QJsonDocument(root).toJson(); bytes.replace("\n", "\r\n");
        auto simulation = physics(bytes);
        QCOMPARE(simulation->SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        QCOMPARE(simulation->_physicsRig->Settings.GetSize(), 1u);
        QCOMPARE(simulation->_physicsRig->Inputs.GetSize(), 1u);
        QCOMPARE(simulation->_physicsRig->Outputs.GetSize(), 1u);
        QCOMPARE(simulation->_physicsRig->Particles.GetSize(), 2u);
        QVERIFY(simulation->SourceDigest() != QCryptographicHash::hash(simulation->_normalizedBytes, QCryptographicHash::Sha256));
    }

    void originalModelsCreateWithTheDeclaredForces_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void originalModelsCreateWithTheDeclaredForces()
    {
        QFETCH(QString, name);
        const auto path = QFINDTESTDATA(qPrintable(QStringLiteral("../Resources/%1/%1.physics3.json").arg(name)));
        QVERIFY(!path.isEmpty()); QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll();
        const auto root = QJsonDocument::fromJson(bytes).object(); const auto forces = root["Meta"].toObject()["EffectiveForces"].toObject();
        auto simulation = physics(bytes); const auto options = simulation->GetOptions();
        QCOMPARE(options.Gravity.X, float(forces["Gravity"].toObject()["X"].toDouble(0)));
        QCOMPARE(options.Gravity.Y, float(forces["Gravity"].toObject()["Y"].toDouble(-1)));
        QCOMPARE(options.Wind.X, float(forces["Wind"].toObject()["X"].toDouble(0)));
        QCOMPARE(options.Wind.Y, float(forces["Wind"].toObject()["Y"].toDouble(0)));
        QCOMPARE(simulation->_physicsRig->Settings.GetSize(), root["PhysicsSettings"].toArray().size());
        QCOMPARE(simulation->SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
    }

    void invalidJsonIsRejectedBeforeSdkAllocation_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("broken") << QByteArray("{");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("invalid-input") << changedPhysics([](QJsonObject&, QJsonObject& group) { group["Input"] = false; });
        QTest::newRow("weight") << changedPhysics([](QJsonObject&, QJsonObject& group) { auto rows = group["Input"].toArray(); auto row = rows[0].toObject(); row["Weight"] = 101; rows[0] = row; group["Input"] = rows; });
        QTest::newRow("missing-root") << changedPhysics([](QJsonObject&, QJsonObject& group) { group["Vertices"] = QJsonArray{}; });
        QTest::newRow("invalid-output") << changedPhysics([](QJsonObject&, QJsonObject& group) { auto rows = group["Output"].toArray(); auto row = rows[0].toObject(); row["VertexIndex"] = 2; rows[0] = row; group["Output"] = rows; });
        QTest::newRow("invalid-force") << changedPhysics([](QJsonObject& root, QJsonObject&) { auto meta = root["Meta"].toObject(); meta["EffectiveForces"] = QJsonObject{{"Gravity", QJsonObject{{"Y", "bad"}}}}; root["Meta"] = meta; });
        QJsonArray groups; const QJsonValue group = QJsonDocument::fromJson(physicsJson()).object()["PhysicsSettings"].toArray()[0];
        for (int i = 0; i < 10001; ++i) groups.append(group);
        QTest::newRow("group-limit") << QJsonDocument(QJsonObject{{"PhysicsSettings", groups}}).toJson(QJsonDocument::Compact);
        QTest::newRow("byte-limit") << QByteArray(16 * 1024 * 1024 + 1, ' ');
    }
    void invalidJsonIsRejectedBeforeSdkAllocation()
    {
        QFETCH(QByteArray, bytes);
        const auto before = sdkAllocationCalls; QString error;
        auto* result = CubismPhysics::Create(reinterpret_cast<const csmByte*>(bytes.constData()), bytes.size(), &error);
        QVERIFY(!result); QVERIFY(!error.isEmpty()); QCOMPARE(sdkAllocationCalls, before);
    }

    void optionsReadOnlyExplicitComponents_data()
    {
        QTest::addColumn<QJsonObject>("forces"); QTest::addColumn<float>("gx"); QTest::addColumn<float>("gy");
        QTest::addColumn<float>("wx"); QTest::addColumn<float>("wy");
        QTest::newRow("missing") << QJsonObject{} << 0.f << -1.f << 0.f << 0.f;
        QTest::newRow("gravity-x") << QJsonObject{{"Gravity", QJsonObject{{"X", .25}}}} << .25f << -1.f << 0.f << 0.f;
        QTest::newRow("gravity-zero") << QJsonObject{{"Gravity", QJsonObject{{"Y", 0}}}} << 0.f << 0.f << 0.f << 0.f;
        QTest::newRow("wind-y") << QJsonObject{{"Wind", QJsonObject{{"Y", -.75}}}} << 0.f << -1.f << 0.f << -.75f;
        QTest::newRow("all") << QJsonObject{{"Gravity", QJsonObject{{"X", .25}, {"Y", -.5}}}, {"Wind", QJsonObject{{"X", .75}, {"Y", -.125}}}} << .25f << -.5f << .75f << -.125f;
    }
    void optionsReadOnlyExplicitComponents()
    {
        QFETCH(QJsonObject, forces); QFETCH(float, gx); QFETCH(float, gy); QFETCH(float, wx); QFETCH(float, wy);
        const auto bytes = changedPhysics([&](QJsonObject& root, QJsonObject&) {
            auto meta = root["Meta"].toObject(); meta.remove("EffectiveForces");
            if (!forces.isEmpty()) meta["EffectiveForces"] = forces; root["Meta"] = meta;
        });
        auto simulation = physics(bytes); const auto options = simulation->GetOptions();
        QCOMPARE(options.Gravity.X, gx); QCOMPARE(options.Gravity.Y, gy); QCOMPARE(options.Wind.X, wx); QCOMPARE(options.Wind.Y, wy);
    }

    void emptyAndMissingInputOutputArraysAreSafe_data()
    {
        QTest::addColumn<int>("mode");
        for (int mode = 0; mode < 5; ++mode) QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
    void emptyAndMissingInputOutputArraysAreSafe()
    {
        QFETCH(int, mode);
        auto bytes = changedPhysics([&](QJsonObject&, QJsonObject& group) {
            if (mode == 0 || mode == 2) group.remove("Input");
            if (mode == 1 || mode == 2) group.remove("Output");
            if (mode == 3) { group["Input"] = QJsonArray{}; group["Output"] = QJsonArray{}; group["Vertices"] = QJsonArray{group["Vertices"].toArray()[0]}; }
        });
        if (mode == 4) bytes = R"({"Version":3,"PhysicsSettings":[]})";
        auto simulation = physics(bytes); auto target = model();
        CubismPhysics::ParameterDefinitionState binding; QVERIFY(simulation->PrepareModelBinding(*target, {}, binding));
        simulation->CommitParameterDefinitionChange(binding);
        simulation->Stabilization(target.get()); simulation->Evaluate(target.get(), 1.f / 30);
        QVERIFY(simulation->HasFiniteRuntimeState());
        for (float value : State(target->GetModel()).values) QVERIFY(std::isfinite(value));
    }

    void translationsUseTheirScaleDuringStabilization_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<float>("expected");
        QTest::newRow("x") << QString("X") << .75f;
        QTest::newRow("y") << QString("Y") << 1.f;
    }
    void translationsUseTheirScaleDuringStabilization()
    {
        QFETCH(QString, type); QFETCH(float, expected);
        const auto bytes = changedPhysics([&](QJsonObject& root, QJsonObject& group) {
            auto meta = root["Meta"].toObject(); auto forces = meta["EffectiveForces"].toObject();
            forces["Wind"] = QJsonObject{{"X", 3}, {"Y", 3}}; meta["EffectiveForces"] = forces; root["Meta"] = meta;
            auto output = group["Output"].toArray()[0].toObject(); output["Type"] = type; output["Scale"] = 1.25;
            group["Output"] = QJsonArray{output};
        });
        auto simulation = physics(bytes); auto target = model();
        simulation->Stabilization(target.get());
        QVERIFY(std::abs(target->GetParameterValue(1) - expected) < .000001f);
    }

    void preparedRuntimePreservesDynamicsAndMigratesCachesByIdWithoutTouchingModels()
    {
        auto previousModel = model();
        State(previousModel->GetModel()).minimum[0] = -2; State(previousModel->GetModel()).maximum[0] = 2;
        auto nextModel = model([](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB", "ParamA", "ParamC"}); state.maximum[1] = .5f; });
        auto previous = physics(); warm(*previous);
        const auto oldState = dynamicState(*previous);
        auto changed = physics(changedPhysics([](QJsonObject& root, QJsonObject& group) {
            auto meta = root["Meta"].toObject(); meta["EffectiveForces"] = QJsonObject{{"Wind", QJsonObject{{"X", .25}}}}; root["Meta"] = meta;
            auto inputs = group["Input"].toArray(); auto input = inputs[0].toObject(); input["Weight"] = 50; inputs[0] = input; group["Input"] = inputs;
            auto outputs = group["Output"].toArray(); auto output = outputs[0].toObject(); output["Scale"] = 2; output["Weight"] = 25; outputs[0] = output; group["Output"] = outputs;
            auto vertices = group["Vertices"].toArray(); auto vertex = vertices[1].toObject(); vertex["Mobility"] = .5; vertex["Delay"] = .4; vertex["Acceleration"] = .75; vertices[1] = vertex; group["Vertices"] = vertices;
        }));
        const auto newState = dynamicState(*changed);
        const auto oldValues = State(previousModel->GetModel()).values, newValues = State(nextModel->GetModel()).values;
        const auto oldSaved = previousModel->_savedParameters, newSaved = nextModel->_savedParameters;
        CubismPhysics::PreparedRuntimeState prepared; QString error;
        QVERIFY2(changed->PrepareRuntimeState(previous.get(), *previousModel, *nextModel, {}, prepared,
            CubismPhysics::StatePolicy::PreserveCompatible, &error), qPrintable(error));
        QVERIFY(!prepared.WasStabilized()); QVERIFY(changed->ValidatePreparedRuntimeState(prepared));
        QCOMPARE(dynamicState(*previous), oldState); QCOMPARE(dynamicState(*changed), newState);
        QCOMPARE(State(previousModel->GetModel()).values, oldValues); QCOMPARE(State(nextModel->GetModel()).values, newValues);
        QCOMPARE(previousModel->_savedParameters, oldSaved); QCOMPARE(nextModel->_savedParameters, newSaved);
        const int calls = sdkAllocationCalls; rejectSdkAllocation = true;
        const bool committed = changed->CommitRuntimeState(prepared); rejectSdkAllocation = false;
        QVERIFY(committed); QCOMPARE(sdkAllocationCalls, calls); QVERIFY(!changed->CommitRuntimeState(prepared));
        QVERIFY(!changed->ValidatePreparedRuntimeState(prepared));
        QCOMPARE(changed->_parameterCaches.size(), 3);
        QVERIFY(std::signbit(changed->_parameterCaches[0])); QCOMPARE(changed->_parameterCaches[1], .5f); QCOMPARE(changed->_parameterCaches[2], .4f);
        QCOMPARE(changed->_parameterInputCaches, QVector<float>({1.25f, -.4f, .4f}));
        QCOMPARE(changed->_currentRemainTime, .0125f);
        QCOMPARE(changed->_physicsRig->Particles[1].Position.X, .25f); QCOMPARE(changed->_physicsRig->Particles[1].Velocity.Y, -.625f);
        QCOMPARE(changed->_physicsRig->Particles[1].Force.Y, .5f); QCOMPARE(changed->_physicsRig->Particles[1].LastGravity.X, .125f);
        QCOMPARE(changed->_currentRigOutputs[0].outputs[0], .375f); QCOMPARE(changed->_previousRigOutputs[0].outputs[0], -.625f);
        QCOMPARE(changed->_physicsRig->Outputs[0].ValueBelowMinimum, -3.f); QCOMPARE(changed->_physicsRig->Outputs[0].ValueExceededMaximum, 4.f);
        QCOMPARE(changed->_physicsRig->Inputs[0].SourceParameterIndex, 1); QCOMPARE(changed->_physicsRig->Outputs[0].DestinationParameterIndex, 0);
        QCOMPARE(changed->_physicsRig->Inputs[0].Weight, 50.f); QCOMPARE(changed->_physicsRig->Outputs[0].Weight, 25.f);
        QCOMPARE(changed->_physicsRig->Outputs[0].TranslationScale.X, 2.f); QCOMPARE(changed->_physicsRig->Particles[1].Mobility, .5f);
        QCOMPARE(changed->GetOptions().Wind.X, .25f);
        QCOMPARE(dynamicState(*previous), oldState); QCOMPARE(State(nextModel->GetModel()).values, newValues); QCOMPARE(nextModel->_savedParameters, newSaved);
    }

    void topologyChangesStabilizeInPrivateBuffers_data()
    {
        QTest::addColumn<QString>("field");
        for (const char* field : {"group-id", "missing-id", "input-id", "input-type", "input-reflect", "output-id", "output-type",
             "output-reflect", "output-vertex", "particle-count", "radius", "position", "fps", "forced"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void topologyChangesStabilizeInPrivateBuffers()
    {
        QFETCH(QString, field); auto target = model(); target->SetParameterValue(0, .4f);
        auto previous = physics(); warm(*previous);
        const auto bytes = changedPhysics([&](QJsonObject& root, QJsonObject& group) {
            if (field == "group-id") group["Id"] = "different";
            if (field == "missing-id") group.remove("Id");
            auto inputs = group["Input"].toArray(); auto input = inputs[0].toObject();
            if (field == "input-id") input["Source"] = QJsonObject{{"Id", "ParamB"}, {"Target", "Parameter"}};
            if (field == "input-type") input["Type"] = "Y";
            if (field == "input-reflect") input["Reflect"] = false;
            inputs[0] = input; group["Input"] = inputs;
            auto outputs = group["Output"].toArray(); auto output = outputs[0].toObject();
            if (field == "output-id") output["Destination"] = QJsonObject{{"Id", "ParamA"}, {"Target", "Parameter"}};
            if (field == "output-type") output["Type"] = "Angle";
            if (field == "output-reflect") output["Reflect"] = true;
            if (field == "output-vertex") output["VertexIndex"] = 0;
            outputs[0] = output; group["Output"] = outputs;
            auto vertices = group["Vertices"].toArray(); auto vertex = vertices[1].toObject();
            if (field == "radius") vertex["Radius"] = 1.5;
            if (field == "position") vertex["Position"] = QJsonObject{{"X", .25}, {"Y", 1}};
            vertices[1] = vertex;
            if (field == "particle-count") vertices.append(vertex);
            group["Vertices"] = vertices;
            if (field == "fps") { auto meta = root["Meta"].toObject(); meta["Fps"] = 60; root["Meta"] = meta; }
        });
        auto changed = physics(bytes); const auto oldState = dynamicState(*previous), before = dynamicState(*changed);
        const auto values = State(target->GetModel()).values; const auto saved = target->_savedParameters;
        CubismPhysics::PreparedRuntimeState prepared; QString error;
        const auto policy = field == "forced" ? CubismPhysics::StatePolicy::Stabilize : CubismPhysics::StatePolicy::PreserveCompatible;
        QVERIFY2(changed->PrepareRuntimeState(previous.get(), *target, *target, {}, prepared, policy, &error), qPrintable(error));
        QVERIFY(prepared.WasStabilized()); QCOMPARE(dynamicState(*previous), oldState); QCOMPARE(dynamicState(*changed), before);
        QCOMPARE(State(target->GetModel()).values, values); QCOMPARE(target->_savedParameters, saved);
        QVERIFY(changed->CommitRuntimeState(prepared)); QCOMPARE(changed->_currentRemainTime, 0.f);
        QVERIFY(changed->HasFiniteRuntimeState()); QCOMPARE(State(target->GetModel()).values, values); QCOMPARE(target->_savedParameters, saved);
        QCOMPARE(changed->_currentRigOutputs[0].outputs[0], changed->_previousRigOutputs[0].outputs[0]);
    }

    void missingAndDuplicateGroupIdsNeverPretendToPreserveDynamics()
    {
        for (bool duplicate : {false, true}) {
            auto root = QJsonDocument::fromJson(physicsJson()).object(); auto groups = root["PhysicsSettings"].toArray();
            if (duplicate) groups.append(groups[0]);
            else { auto group = groups[0].toObject(); group.remove("Id"); groups[0] = group; }
            root["PhysicsSettings"] = groups; const auto bytes = QJsonDocument(root).toJson();
            auto previous = physics(bytes), replacement = physics(bytes); warm(*previous); auto target = model();
            CubismPhysics::PreparedRuntimeState prepared;
            QVERIFY(replacement->PrepareRuntimeState(previous.get(), *target, *target, {}, prepared)); QVERIFY(prepared.WasStabilized());
        }
    }

    void explicitBindingRejectsUnknownRealIdsAndCanClearMasks()
    {
        auto target = model(), missing = model([](GetterState& value) { CubismModelFixture::SetParameterLayout(value, {"ParamC"}); });
        auto simulation = physics(); CubismPhysics::ParameterDefinitionState state; QString error;
        QVERIFY(!simulation->PrepareModelBinding(*missing, {}, state, &error)); QVERIFY(error.contains("ParamA"));
        ParameterPhysicsReferences refs; refs.sourceDigest = simulation->SourceDigest(); refs.suppressedInputs[0] = "ParamA"; refs.suppressedOutputs[0] = "ParamB";
        QVERIFY(simulation->PrepareModelBinding(*missing, refs, state, &error)); simulation->CommitParameterDefinitionChange(state);
        QCOMPARE(simulation->_activeInputs, QVector<quint8>{0}); QCOMPARE(simulation->_activeOutputs, QVector<quint8>{0});
        QVERIFY(!simulation->PrepareModelBinding(*missing, {}, state, &error));
        QCOMPARE(simulation->_activeInputs, QVector<quint8>{0});
        QVERIFY(simulation->PrepareModelBinding(*target, {}, state, &error)); simulation->CommitParameterDefinitionChange(state);
        QCOMPARE(simulation->_activeInputs, QVector<quint8>{1}); QCOMPARE(simulation->_activeOutputs, QVector<quint8>{1});
        const auto before = State(target->GetModel()).values; State(target->GetModel()).values[0] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!simulation->PrepareModelBinding(*target, {}, state, &error)); State(target->GetModel()).values = before;
        refs.sourceDigest = QByteArray(32, 'x'); QVERIFY(!simulation->PrepareModelBinding(*target, refs, state, &error));
        QCOMPARE(simulation->_activeInputs, QVector<quint8>{1});
    }

    void failedPreparationAndForeignOrStaleCommitKeepPreviousCandidates()
    {
        auto target = model(); auto previous = physics(), replacement = physics(), other = physics(); warm(*previous);
        CubismPhysics::PreparedRuntimeState prepared; QString error;
        QVERIFY(replacement->PrepareRuntimeState(previous.get(), *target, *target, {}, prepared));
        QVERIFY(!other->CommitRuntimeState(prepared)); QVERIFY(replacement->ValidatePreparedRuntimeState(prepared));
        const auto oldState = dynamicState(*replacement);
        rejectSdkAllocation = true;
        const bool accepted = replacement->PrepareRuntimeState(previous.get(), *target, *target, {}, prepared,
            CubismPhysics::StatePolicy::PreserveCompatible, &error);
        rejectSdkAllocation = false;
        QVERIFY(!accepted); QVERIFY(!error.isEmpty()); QVERIFY(replacement->ValidatePreparedRuntimeState(prepared));
        QCOMPARE(dynamicState(*replacement), oldState);
        QVERIFY(!replacement->PrepareRuntimeState(previous.get(), *target, *target, {}, prepared,
            static_cast<CubismPhysics::StatePolicy>(9), &error)); QVERIFY(replacement->ValidatePreparedRuntimeState(prepared));
        previous->_parameterCaches[0] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!replacement->PrepareRuntimeState(previous.get(), *target, *target, {}, prepared));
        QVERIFY(replacement->CommitRuntimeState(prepared)); QCOMPARE(replacement->_parameterCaches[0], .6f);
        previous->_parameterCaches[0] = .6f;
        QVERIFY(replacement->PrepareRuntimeState(previous.get(), *target, *target, {}, prepared));
        auto options = replacement->GetOptions(); options.Wind.X = .25f; replacement->SetOptions(options);
        const auto state = dynamicState(*replacement); QVERIFY(!replacement->CommitRuntimeState(prepared)); QCOMPARE(dynamicState(*replacement), state);
    }

    void masksChangingRequireStabilizationAndUseCandidateSourceRows()
    {
        auto target = model(); auto previous = physics(), replacement = physics(); warm(*previous);
        ParameterPhysicsReferences mask; mask.sourceDigest = replacement->SourceDigest();
        mask.suppressedInputs[0] = "ParamA"; mask.suppressedOutputs[0] = "ParamB";
        CubismPhysics::PreparedRuntimeState prepared;
        QVERIFY(replacement->PrepareRuntimeState(previous.get(), *target, *target, mask, prepared)); QVERIFY(prepared.WasStabilized());
        QVERIFY(replacement->CommitRuntimeState(prepared));
        QCOMPARE(replacement->_activeInputs, QVector<quint8>{0}); QCOMPARE(replacement->_activeOutputs, QVector<quint8>{0});
        auto restored = physics();
        QVERIFY(restored->PrepareRuntimeState(replacement.get(), *target, *target, {}, prepared)); QVERIFY(prepared.WasStabilized());
        QVERIFY(restored->CommitRuntimeState(prepared));
        QCOMPARE(restored->_activeInputs, QVector<quint8>{1}); QCOMPARE(restored->_activeOutputs, QVector<quint8>{1});
    }

    void sdkVectorSwapTransfersOwnershipWithoutAllocation()
    {
        csmVector<int> first, second; first.PushBack(11); first.PushBack(12); second.PushBack(21);
        const auto* firstPointer = first.GetPtr(); const auto* secondPointer = second.GetPtr();
        const auto calls = sdkAllocationCalls; rejectSdkAllocation = true;
        first.Swap(second); first.Swap(first); rejectSdkAllocation = false;
        QCOMPARE(sdkAllocationCalls, calls); QCOMPARE(first.GetPtr(), secondPointer); QCOMPARE(second.GetPtr(), firstPointer);
        QCOMPARE(first.GetSize(), 1u); QCOMPARE(first[0], 21); QCOMPARE(second.GetSize(), 2u); QCOMPARE(second[1], 12);
        csmVector<int> empty; second.Swap(empty); QCOMPARE(second.GetSize(), 0u); QCOMPARE(empty.GetPtr(), firstPointer);
    }

    void preservedRigContinuesTheSameMultiFrameSimulation()
    {
        auto first = model(), second = model(); auto previous = physics();
        for (int i = 0; i < 5; ++i) { first->SetParameterValue(0, float(i) / 5); previous->Evaluate(first.get(), 1.f / 60); }
        auto replacement = physics();
        State(second->GetModel()).values = State(first->GetModel()).values;
        CubismPhysics::PreparedRuntimeState prepared;
        QVERIFY(replacement->PrepareRuntimeState(previous.get(), *first, *second, {}, prepared)); QVERIFY(!prepared.WasStabilized());
        QVERIFY(replacement->CommitRuntimeState(prepared)); QCOMPARE(dynamicState(*replacement), dynamicState(*previous));
        for (int i = 0; i < 24; ++i) {
            const float input = std::sin(float(i) / 5); first->SetParameterValue(0, input); second->SetParameterValue(0, input);
            previous->Evaluate(first.get(), 1.f / 60); replacement->Evaluate(second.get(), 1.f / 60);
            QCOMPARE(State(first->GetModel()).values, State(second->GetModel()).values);
            QCOMPARE(dynamicState(*previous), dynamicState(*replacement));
        }
    }

    void zeroForceAndZeroRadiusRemainFiniteAcrossFrames_data()
    {
        QTest::addColumn<float>("radius"); QTest::addColumn<float>("acceleration");
        QTest::newRow("stationary") << 1.f << 0.f;
        QTest::newRow("coincident") << 0.f << 1.f;
        QTest::newRow("stationary-coincident") << 0.f << 0.f;
    }
    void zeroForceAndZeroRadiusRemainFiniteAcrossFrames()
    {
        QFETCH(float, radius); QFETCH(float, acceleration);
        auto simulation = physics(changedPhysics([&](QJsonObject&, QJsonObject& group) {
            auto vertices = group["Vertices"].toArray(); auto vertex = vertices[1].toObject();
            vertex["Radius"] = radius; vertex["Acceleration"] = acceleration; vertex["Mobility"] = 0; vertex["Delay"] = 0;
            vertices[1] = vertex; group["Vertices"] = vertices;
        }));
        auto target = model(); const auto before = State(target->GetModel()).values;
        CubismPhysics::PreparedRuntimeState prepared;
        QVERIFY(simulation->PrepareRuntimeState(nullptr, *target, *target, {}, prepared)); QVERIFY(prepared.WasStabilized());
        QCOMPARE(State(target->GetModel()).values, before); QVERIFY(simulation->CommitRuntimeState(prepared));
        for (int i = 0; i < 12; ++i) {
            target->SetParameterValue(0, float(i) / 12); simulation->Evaluate(target.get(), 1.f / 30);
            QVERIFY(simulation->HasFiniteRuntimeState());
            for (float value : State(target->GetModel()).values) QVERIFY(std::isfinite(value));
        }
    }

    void capturedConfigurationUsesActualRowMasksWithoutChangingSimulation()
    {
        auto root = QJsonDocument::fromJson(physicsJson()).object(); auto groups = root["PhysicsSettings"].toArray();
        auto first = groups[0].toObject(); first["Extra"] = QJsonObject{{"keep", 9}}; groups[0] = first;
        auto second = first; second["Id"] = "second"; groups.append(second); root["PhysicsSettings"] = groups;
        root["RootExtension"] = "preserved"; const auto bytes = QJsonDocument(root).toJson(); auto simulation = physics(bytes); auto target = model();
        ParameterPhysicsReferences refs; refs.sourceDigest = simulation->SourceDigest(); refs.suppressedInputs[0] = "ParamA"; refs.suppressedOutputs[1] = "ParamB";
        CubismPhysics::ParameterDefinitionState binding; QVERIFY(simulation->PrepareModelBinding(*target, refs, binding)); simulation->CommitParameterDefinitionChange(binding);
        QVERIFY(simulation->SetObservationGroup(1)); simulation->Evaluate(target.get(), 1.f / 30);
        const auto before = dynamicState(*simulation); const auto revision = simulation->_stateRevision; const auto values = State(target->GetModel()).values;
        PhysicsObservation observation; QVERIFY(simulation->CaptureObservation(*target, observation)); const auto observed = observationState(observation);
        QJsonObject actual; QString error; QVERIFY2(simulation->CaptureConfiguration(actual, &error), qPrintable(error));
        first["Input"] = QJsonArray{}; second["Output"] = QJsonArray{}; root["PhysicsSettings"] = QJsonArray{first, second};
        QJsonObject expected; QVERIFY(live2dstruct::CPhysicsDocument::Normalize(root, expected)); QCOMPARE(actual, expected);
        QCOMPARE(actual["Meta"].toObject()["TotalInputCount"].toInt(), 1); QCOMPARE(actual["Meta"].toObject()["TotalOutputCount"].toInt(), 1);
        QCOMPARE(actual["Meta"].toObject()["VertexCount"].toInt(), 4); QCOMPARE(actual["PhysicsSettings"].toArray()[1].toObject()["Id"].toString(), QString("second"));
        QCOMPARE(dynamicState(*simulation), before); QCOMPARE(simulation->_stateRevision, revision); QCOMPARE(State(target->GetModel()).values, values);
        QVERIFY(simulation->CaptureObservation(*target, observation)); QCOMPARE(observationState(observation), observed);
        QCOMPARE(simulation->SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        auto filtered = physics(QJsonDocument(actual).toJson()); QVERIFY(filtered->PrepareModelBinding(*target, {}, binding)); filtered->CommitParameterDefinitionChange(binding);
        QJsonObject again; QVERIFY(filtered->CaptureConfiguration(again)); QCOMPARE(again, actual);
    }

    void capturedConfigurationPreservesSourcePrecisionAndTracksLiveForces_data()
    {
        QTest::addColumn<bool>("present"); QTest::newRow("default-omitted") << false; QTest::newRow("precise-explicit") << true;
    }
    void capturedConfigurationPreservesSourcePrecisionAndTracksLiveForces()
    {
        QFETCH(bool, present); auto root = QJsonDocument::fromJson(physicsJson()).object(); auto meta = root["Meta"].toObject();
        if (present) meta["EffectiveForces"] = QJsonObject{{"Extra", "keep"}, {"Gravity", QJsonObject{{"X", .1234567890123}, {"Y", -.999999999}, {"Extra", 5}}},
            {"Wind", QJsonObject{{"X", 1e-50}, {"Y", -0.0}}}};
        else meta.remove("EffectiveForces");
        root["Meta"] = meta; auto simulation = physics(QJsonDocument(root).toJson());
        QJsonObject actual, expected; QVERIFY(simulation->CaptureConfiguration(actual)); QVERIFY(live2dstruct::CPhysicsDocument::Normalize(root, expected)); QCOMPARE(actual, expected);
        const auto before = dynamicState(*simulation); const auto revision = simulation->_stateRevision;
        auto options = simulation->GetOptions(); options.Wind.Y = .75f; simulation->SetOptions(options);
        QVERIFY(simulation->CaptureConfiguration(actual));
        auto forces = expected["Meta"].toObject()["EffectiveForces"].toObject(); auto wind = forces["Wind"].toObject(); wind["Y"] = .75; forces["Wind"] = wind;
        auto expectedMeta = expected["Meta"].toObject(); expectedMeta["EffectiveForces"] = forces; expected["Meta"] = expectedMeta;
        QCOMPARE(actual, expected); QCOMPARE(simulation->_stateRevision, revision + 1);
        QCOMPARE(dynamicState(*simulation), before);
        if (present) QCOMPARE(actual["Meta"].toObject()["EffectiveForces"].toObject()["Gravity"].toObject()["X"].toDouble(), .1234567890123);
        auto recreated = physics(QJsonDocument(actual).toJson()); QCOMPARE(recreated->GetOptions().Wind.Y, .75f);
        QCOMPARE(recreated->GetOptions().Gravity.X, options.Gravity.X); QCOMPARE(recreated->GetOptions().Gravity.Y, options.Gravity.Y);
    }

    void configurationCaptureHandlesEmptyAndRejectsInvalidCandidatesAtomically()
    {
        auto empty = physics(R"({"PhysicsSettings":[]})"); QJsonObject output{{"sentinel", 7}};
        QVERIFY(empty->CaptureConfiguration(output)); QCOMPARE(output["PhysicsSettings"].toArray().size(), 0);
        auto source = physics(changedPhysics([](QJsonObject&, QJsonObject& group) { group.remove("Input"); group.remove("Output"); }));
        QVERIFY(source->CaptureConfiguration(output)); const auto group = output["PhysicsSettings"].toArray()[0].toObject();
        QVERIFY(group["Input"].isArray() && group["Output"].isArray()); QVERIFY(group["Input"].toArray().isEmpty() && group["Output"].toArray().isEmpty());
        auto simulation = physics(); const auto sentinel = output; QString error;
        const auto raw = simulation->_normalizedBytes; simulation->_normalizedBytes = "{";
        QVERIFY(!simulation->CaptureConfiguration(output, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(output, sentinel); simulation->_normalizedBytes = raw;
        simulation->_activeInputs[0] = 2; QVERIFY(!simulation->CaptureConfiguration(output, &error)); QCOMPARE(output, sentinel); simulation->_activeInputs[0] = 1;
        simulation->_activeOutputs.clear(); QVERIFY(!simulation->CaptureConfiguration(output, &error)); QCOMPARE(output, sentinel); simulation->_activeOutputs = {1};
        auto options = simulation->GetOptions(); options.Gravity.X = 100001; simulation->SetOptions(options);
        QVERIFY(!simulation->CaptureConfiguration(output, &error)); QCOMPARE(output, sentinel);
        simulation->_options.Gravity.X = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!simulation->CaptureConfiguration(output, &error)); QCOMPARE(output, sentinel);
    }

    void observationSelectionAndColdCaptureDoNotChangePhysicsOrResolveIndices()
    {
        auto simulation = physics(); auto target = model();
        const auto before = dynamicState(*simulation); const auto revision = simulation->_stateRevision;
        PhysicsObservation result; result.groupId = "sentinel"; result.sequence = 71; result.particles = {{3, 4}};
        const auto sentinel = observationState(result); QString error;
        QVERIFY(!simulation->CaptureObservation(*target, result, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(observationState(result), sentinel);
        QVERIFY(!simulation->SetObservationGroup(-2)); QVERIFY(!simulation->SetObservationGroup(1));
        QVERIFY(simulation->SetObservationGroup(0));
        QCOMPARE(simulation->_physicsRig->Inputs[0].SourceParameterIndex, -1);
        QCOMPARE(simulation->_physicsRig->Outputs[0].DestinationParameterIndex, -1);
        QVERIFY2(simulation->CaptureObservation(*target, result, &error), qPrintable(error));
        QVERIFY(error.isEmpty()); QVERIFY(!result.sampled); QVERIFY(result.particles.isEmpty());
        QCOMPARE(result.groupIndex, 0); QCOMPARE(result.groupId, QString("one")); QCOMPARE(result.sourceDigest, simulation->SourceDigest());
        QCOMPARE(result.generation, quint64(0)); QCOMPARE(result.sequence, quint64(0)); QCOMPARE(result.time, 0.0);
        QCOMPARE(result.inputs.size(), 1); QCOMPARE(result.outputs.size(), 1);
        QCOMPARE(result.inputs[0].id, QString("ParamA")); QCOMPARE(result.inputs[0].type, QString("X"));
        QVERIFY(result.inputs[0].active && result.inputs[0].hasRange && !result.inputs[0].sampled);
        QCOMPARE(result.inputs[0].minimum, -1.f); QCOMPARE(result.inputs[0].maximum, 1.f); QCOMPARE(result.inputs[0].defaultValue, 0.f);
        QCOMPARE(result.outputs[0].id, QString("ParamB")); QVERIFY(result.outputs[0].active && !result.outputs[0].sampled);
        QCOMPARE(simulation->_physicsRig->Inputs[0].SourceParameterIndex, -1);
        QCOMPARE(simulation->_physicsRig->Outputs[0].DestinationParameterIndex, -1);
        QCOMPARE(dynamicState(*simulation), before); QCOMPARE(simulation->_stateRevision, revision); QCOMPARE(State(target->GetModel()).updates, 0);
        QVERIFY(simulation->SetObservationGroup(-1)); const auto old = observationState(result);
        QVERIFY(!simulation->CaptureObservation(*target, result)); QCOMPARE(observationState(result), old);
        simulation->Evaluate(target.get(), 1.f / 30);
        QCOMPARE(simulation->_observationSequence, quint64(0)); QVERIFY(simulation->_observation.inputs.isEmpty()); QVERIFY(simulation->_observation.particles.isEmpty());
        auto empty = physics(R"({"PhysicsSettings":[]})"); QVERIFY(!empty->SetObservationGroup(0)); QVERIFY(empty->SetObservationGroup(-1));
        QVERIFY(!empty->CaptureObservation(*target, result)); QCOMPARE(observationState(result), old);
    }

    void observationInputsComeFromTheLastSubstepAndSurviveLaterParameterWrites()
    {
        auto simulation = physics(changedPhysics([](QJsonObject&, QJsonObject& group) {
            auto output = group["Output"].toArray()[0].toObject();
            output["Destination"] = QJsonObject{{"Target", "Parameter"}, {"Id", "ParamA"}};
            output["Type"] = "Y"; output["Scale"] = 2; group["Output"] = QJsonArray{output};
        }));
        auto target = model(); target->SetParameterValue(0, .8f); QVERIFY(simulation->SetObservationGroup(0));
        simulation->Evaluate(target.get(), 1.f / 30);
        PhysicsObservation first; QVERIFY(simulation->CaptureObservation(*target, first));
        QVERIFY(first.sampled); QVERIFY(first.inputs[0].sampled); QVERIFY(first.outputs[0].sampled);
        QCOMPARE(first.inputs[0].value, .8f); QCOMPARE(target->GetParameterValue(0), 0.f);
        QVERIFY(simulation->_parameterCaches[0] != first.inputs[0].value);
        QCOMPARE(first.particles.size(), 2);
        QCOMPARE(first.particles[0].x(), double(simulation->_physicsRig->Particles[0].Position.X));
        QCOMPARE(first.particles[1].y(), double(simulation->_physicsRig->Particles[1].Position.Y));
        simulation->_parameterInputCaches[0] = -.6f; target->SetParameterValue(0, 1);
        simulation->Evaluate(target.get(), 2.f / 30);
        PhysicsObservation last; QVERIFY(simulation->CaptureObservation(*target, last));
        QCOMPARE(last.inputs[0].value, 1.f); QCOMPARE(last.sequence, first.sequence + 1); QVERIFY(last.time > first.time);
        target->SetParameterValue(0, -.5f); simulation->Evaluate(target.get(), 1.f / 120);
        PhysicsObservation between; QVERIFY(simulation->CaptureObservation(*target, between));
        QVERIFY(between.inputs[0].sampled); QCOMPARE(between.inputs[0].value, 1.f);
        QCOMPARE(between.sequence, last.sequence + 1); QVERIFY(between.time > last.time);
        const auto recorded = observationState(between); const auto revision = simulation->_stateRevision;
        target->SetParameterValue(0, -1); target->SetParameterValue(1, 1.5f);
        PhysicsObservation captured; QVERIFY(simulation->CaptureObservation(*target, captured));
        QCOMPARE(observationState(captured), recorded); QCOMPARE(simulation->_stateRevision, revision);
        captured.inputs[0].value = 999; captured.particles[0] = QPointF(999, 999); captured.generation = 17;
        QVERIFY(simulation->CaptureObservation(*target, captured)); QCOMPARE(observationState(captured), recorded);
    }

    void observationOutputsIncludeScaleClampingAndTheAppliedWeight_data()
    {
        QTest::addColumn<float>("translation"); QTest::addColumn<float>("scale"); QTest::addColumn<float>("weight");
        QTest::addColumn<float>("raw"); QTest::addColumn<float>("clamped"); QTest::addColumn<float>("applied"); QTest::addColumn<bool>("clipped");
        QTest::newRow("upper") << .75f << 8.f << 25.f << 6.f << 2.f << .875f << true;
        QTest::newRow("lower") << -.75f << 8.f << 25.f << -6.f << -2.f << -.125f << true;
        QTest::newRow("negative-scale") << .75f << -8.f << 25.f << -6.f << -2.f << -.125f << true;
        QTest::newRow("zero-weight") << .75f << 8.f << 0.f << 6.f << 2.f << .5f << true;
        QTest::newRow("full-weight") << .75f << 8.f << 100.f << 6.f << 2.f << 2.f << true;
        QTest::newRow("unclipped") << .5f << 2.f << 50.f << 1.f << 1.f << .75f << false;
    }
    void observationOutputsIncludeScaleClampingAndTheAppliedWeight()
    {
        QFETCH(float, translation); QFETCH(float, scale); QFETCH(float, weight);
        QFETCH(float, raw); QFETCH(float, clamped); QFETCH(float, applied); QFETCH(bool, clipped);
        auto simulation = physics(changedPhysics([&](QJsonObject&, QJsonObject& group) {
            auto output = group["Output"].toArray()[0].toObject(); output["Scale"] = scale; output["Weight"] = weight;
            group["Output"] = QJsonArray{output};
        }));
        auto target = model(); target->SetParameterValue(1, .5f);
        simulation->_previousRigOutputs[0].outputs[0] = translation; simulation->_currentRigOutputs[0].outputs[0] = translation;
        QVERIFY(simulation->SetObservationGroup(0)); simulation->Evaluate(target.get(), 1.f / 120);
        PhysicsObservation sample; QVERIFY(simulation->CaptureObservation(*target, sample));
        QVERIFY(sample.sampled); QVERIFY(!sample.inputs[0].sampled); QCOMPARE(sample.outputs.size(), 1);
        const auto& output = sample.outputs[0]; QVERIFY(output.active && output.sampled && output.hasRange);
        QCOMPARE(output.rawValue, raw); QCOMPARE(output.clampedValue, clamped); QCOMPARE(output.value, applied); QCOMPARE(output.clipped, clipped);
        QCOMPARE(output.minimum, -2.f); QCOMPARE(output.maximum, 2.f); QCOMPARE(output.defaultValue, .25f);
        QCOMPARE(target->GetParameterValue(1), applied);
        target->SetParameterValue(1, -1.5f); PhysicsObservation after; QVERIFY(simulation->CaptureObservation(*target, after));
        QCOMPARE(observationState(after), observationState(sample));
    }

    void observationSelectsOnlyItsGroupAndHonorsInactiveSourceRows()
    {
        auto root = QJsonDocument::fromJson(physicsJson()).object(); auto groups = root["PhysicsSettings"].toArray();
        auto second = groups[0].toObject(); second["Id"] = "second";
        auto input = second["Input"].toArray()[0].toObject(); input["Source"] = QJsonObject{{"Target", "Parameter"}, {"Id", "ParamC"}};
        auto output = second["Output"].toArray()[0].toObject(); output["Destination"] = QJsonObject{{"Target", "Parameter"}, {"Id", "ParamD"}};
        second["Input"] = QJsonArray{input}; second["Output"] = QJsonArray{output}; groups.append(second); root["PhysicsSettings"] = groups;
        auto simulation = physics(QJsonDocument(root).toJson()); auto target = model();
        ParameterPhysicsReferences refs; refs.sourceDigest = simulation->SourceDigest(); refs.suppressedInputs[1] = "ParamC"; refs.suppressedOutputs[1] = "ParamD";
        CubismPhysics::ParameterDefinitionState binding; QVERIFY(simulation->PrepareModelBinding(*target, refs, binding)); simulation->CommitParameterDefinitionChange(binding);
        const auto virtuals = target->_notExistParameterValues; const auto virtualIds = target->_notExistParameterId;
        QVERIFY(simulation->SetObservationGroup(1)); simulation->Evaluate(target.get(), 1.f / 30);
        PhysicsObservation sample; QVERIFY(simulation->CaptureObservation(*target, sample));
        QCOMPARE(sample.groupIndex, 1); QCOMPARE(sample.groupId, QString("second")); QCOMPARE(sample.inputs.size(), 1); QCOMPARE(sample.outputs.size(), 1); QCOMPARE(sample.particles.size(), 2);
        QVERIFY(sample.sampled); QVERIFY(!sample.inputs[0].active && !sample.inputs[0].sampled && !sample.inputs[0].hasRange);
        QVERIFY(!sample.outputs[0].active && !sample.outputs[0].sampled && !sample.outputs[0].hasRange);
        QCOMPARE(sample.inputs[0].id, QString("ParamC")); QCOMPARE(sample.outputs[0].id, QString("ParamD"));
        QCOMPARE(target->_notExistParameterValues, virtuals); QCOMPARE(target->_notExistParameterId, virtualIds);
        const auto previous = observationState(sample); const auto revision = simulation->_stateRevision;
        QVERIFY(!simulation->SetObservationGroup(2)); QVERIFY(simulation->CaptureObservation(*target, sample)); QCOMPARE(observationState(sample), previous);
        QVERIFY(simulation->SetObservationGroup(1)); QVERIFY(simulation->CaptureObservation(*target, sample)); QCOMPARE(observationState(sample), previous);
        QVERIFY(simulation->SetObservationGroup(0)); QVERIFY(simulation->CaptureObservation(*target, sample));
        QVERIFY(!sample.sampled); QVERIFY(sample.particles.isEmpty()); QCOMPARE(sample.groupId, QString("one")); QCOMPARE(sample.time, 0.0);
        QCOMPARE(simulation->_stateRevision, revision);
        simulation->Evaluate(target.get(), 1.f / 30); QVERIFY(simulation->CaptureObservation(*target, sample)); QCOMPARE(sample.sequence, quint64(2));
    }

    void observationRejectsForeignModelsAndInvalidatesResetsAndRigCommits()
    {
        auto simulation = physics(); auto target = model(), foreign = model();
        QVERIFY(simulation->SetObservationGroup(0)); simulation->Evaluate(target.get(), 1.f / 30);
        PhysicsObservation sample; QVERIFY(simulation->CaptureObservation(*target, sample)); const auto old = observationState(sample); QString error;
        QVERIFY(!simulation->CaptureObservation(*foreign, sample, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(observationState(sample), old);
        State(target->GetModel()).minimum[0] = -2;
        QVERIFY(!simulation->CaptureObservation(*target, sample, &error)); QCOMPARE(observationState(sample), old);
        State(target->GetModel()).minimum[0] = -1;
        simulation->Reset(); QVERIFY(simulation->CaptureObservation(*target, sample)); QVERIFY(!sample.sampled); QVERIFY(sample.particles.isEmpty()); QCOMPARE(sample.time, 0.0);
        simulation->Evaluate(target.get(), 1.f / 30); QVERIFY(simulation->CaptureObservation(*target, sample)); QCOMPARE(sample.sequence, quint64(2));
        simulation->Evaluate(target.get(), std::numeric_limits<float>::quiet_NaN());
        QVERIFY(simulation->CaptureObservation(*target, sample)); QVERIFY(!sample.sampled); QCOMPARE(sample.sequence, quint64(2));
        simulation->Evaluate(target.get(), 1.f / 30);
        CubismPhysics::ParameterDefinitionState binding; QVERIFY(simulation->PrepareModelBinding(*target, {}, binding)); simulation->CommitParameterDefinitionChange(binding);
        QVERIFY(simulation->CaptureObservation(*target, sample)); QVERIFY(!sample.sampled);
        simulation->Evaluate(target.get(), 1.f / 30); QVERIFY(simulation->CaptureObservation(*target, sample));
        const auto sourceFrame = observationState(sample);
        auto replacement = physics(); CubismPhysics::PreparedRuntimeState prepared;
        QVERIFY(replacement->PrepareRuntimeState(simulation.get(), *target, *target, {}, prepared)); QVERIFY(replacement->CommitRuntimeState(prepared));
        QVERIFY(!replacement->CaptureObservation(*target, sample)); QCOMPARE(observationState(sample), sourceFrame);
        QVERIFY(replacement->SetObservationGroup(0)); QVERIFY(replacement->CaptureObservation(*target, sample)); QVERIFY(!sample.sampled); QCOMPARE(sample.sequence, quint64(0));
        replacement->Evaluate(target.get(), 1.f / 30); QVERIFY(replacement->CaptureObservation(*target, sample)); QVERIFY(sample.sampled);
        QVERIFY(replacement->PrepareRuntimeState(simulation.get(), *target, *target, {}, prepared)); QVERIFY(replacement->CommitRuntimeState(prepared));
        QVERIFY(replacement->CaptureObservation(*target, sample)); QVERIFY(!sample.sampled); QVERIFY(sample.particles.isEmpty());
        QVERIFY(simulation->CaptureObservation(*target, sample)); QCOMPARE(observationState(sample), sourceFrame);
    }

    void observingNeverChangesTheOriginalFrameSequence()
    {
        auto baseline = physics(), observed = physics(); auto first = model(), second = model();
        quint64 sequence = 0;
        for (int frame = 0; frame < 48; ++frame) {
            const bool enabled = frame % 9 < 6;
            const auto revision = observed->_stateRevision; const auto before = dynamicState(*observed);
            QVERIFY(observed->SetObservationGroup(enabled ? 0 : -1)); QCOMPARE(observed->_stateRevision, revision); QCOMPARE(dynamicState(*observed), before);
            const float input = std::sin(float(frame) / 5), output = std::cos(float(frame) / 7);
            first->SetParameterValue(0, input); second->SetParameterValue(0, input); first->SetParameterValue(1, output); second->SetParameterValue(1, output);
            const float dt = frame % 3 == 0 ? 1.f / 20 : 1.f / 120;
            baseline->Evaluate(first.get(), dt); observed->Evaluate(second.get(), dt);
            QCOMPARE(State(first->GetModel()).values, State(second->GetModel()).values);
            QCOMPARE(dynamicState(*baseline), dynamicState(*observed)); QCOMPARE(baseline->_stateRevision, observed->_stateRevision);
            PhysicsObservation snapshot;
            if (enabled) {
                QVERIFY(observed->CaptureObservation(*second, snapshot)); QVERIFY(snapshot.sampled); QVERIFY(snapshot.sequence > sequence); sequence = snapshot.sequence;
                const auto captured = observationState(snapshot); QVERIFY(observed->CaptureObservation(*second, snapshot)); QCOMPARE(observationState(snapshot), captured);
                QCOMPARE(baseline->_stateRevision, observed->_stateRevision); QCOMPARE(dynamicState(*baseline), dynamicState(*observed));
            } else QVERIFY(!observed->CaptureObservation(*second, snapshot));
        }
    }
    void suppressedReferencesMigrateWithoutChangingSimulationState_data()
    {
        QTest::addColumn<bool>("warm");
        QTest::addColumn<bool>("input");
        for (bool warm : {false, true}) for (bool input : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(warm).arg(input))) << warm << input;
    }

    void suppressedReferencesMigrateWithoutChangingSimulationState()
    {
        QFETCH(bool, warm); QFETCH(bool, input);
        auto before = model();
        auto missing = model([&](GetterState& value) { CubismModelFixture::SetParameterLayout(value, input ? std::initializer_list<const char*>{"ParamB"} : std::initializer_list<const char*>{"ParamA"}); });
        auto simulation = physics();
        const auto digest = QCryptographicHash::hash(physicsJson(), QCryptographicHash::Sha256);
        QCOMPARE(simulation->SourceDigest(), digest);
        if (warm) { simulation->_parameterCaches = {0.3f, 0.5f}; simulation->_parameterInputCaches = {-0.0f, 0.75f}; }
        simulation->_currentRemainTime = 0.0125f;
        simulation->_currentRigOutputs[0].outputs[0] = 0.375f;
        simulation->_previousRigOutputs[0].outputs[0] = -0.25f;
        simulation->_physicsRig->Particles[1].Velocity.X = 0.625f;
        const auto caches = simulation->_parameterCaches;
        ParameterPhysicsReferences references;
        references.sourceDigest = digest;
        (input ? references.suppressedInputs : references.suppressedOutputs).insert(0, input ? "ParamA" : "ParamB");
        CubismPhysics::ParameterDefinitionState prepared;
        QString error;
        QVERIFY2(simulation->PrepareParameterStructureChange(*before, *missing, prepared, &error, &references), qPrintable(error));
        QCOMPARE(simulation->_activeInputs, QVector<quint8>({1}));
        QCOMPARE(simulation->_activeOutputs, QVector<quint8>({1}));
        QCOMPARE(simulation->_parameterCaches, caches);
        simulation->CommitParameterDefinitionChange(prepared);
        QCOMPARE(simulation->_activeInputs, QVector<quint8>({quint8(input ? 0 : 1)}));
        QCOMPARE(simulation->_activeOutputs, QVector<quint8>({quint8(input ? 1 : 0)}));
        QCOMPARE(simulation->_physicsRig->Inputs[0].SourceParameterIndex, input ? -1 : 0);
        QCOMPARE(simulation->_physicsRig->Outputs[0].DestinationParameterIndex, input ? 0 : -1);
        QCOMPARE(simulation->_parameterCaches.size(), warm ? 1 : 0);
        if (warm) QCOMPARE(simulation->_parameterCaches[0], input ? 0.5f : 0.3f);
        QCOMPARE(simulation->_currentRemainTime, 0.0125f);
        QCOMPARE(simulation->_currentRigOutputs[0].outputs[0], 0.375f);
        QCOMPARE(simulation->_previousRigOutputs[0].outputs[0], -0.25f);
        QCOMPARE(simulation->_physicsRig->Particles[1].Velocity.X, 0.625f);
        auto metadata = model([&](GetterState& value) {
            CubismModelFixture::SetParameterLayout(value, input ? std::initializer_list<const char*>{"ParamB"} : std::initializer_list<const char*>{"ParamA"});
            value.minimum[0] = -0.1f; value.maximum[0] = 0.1f;
        });
        QVERIFY(simulation->PrepareParameterDefinitionChange(*missing, *metadata, prepared, &error));
        simulation->CommitParameterDefinitionChange(prepared);
        QCOMPARE(simulation->_activeInputs[0], quint8(input ? 0 : 1));
        QCOMPARE(simulation->_activeOutputs[0], quint8(input ? 1 : 0));
        auto recreated = model();
        QVERIFY(simulation->PrepareParameterStructureChange(*metadata, *recreated, prepared, &error));
        simulation->CommitParameterDefinitionChange(prepared);
        QCOMPARE(simulation->_activeInputs[0], quint8(input ? 0 : 1));
        QCOMPARE(simulation->_activeOutputs[0], quint8(input ? 1 : 0));
        ParameterPhysicsReferences restored;
        QVERIFY(simulation->PrepareParameterStructureChange(*recreated, *before, prepared, &error, &restored));
        simulation->CommitParameterDefinitionChange(prepared);
        QCOMPARE(simulation->_activeInputs, QVector<quint8>({1}));
        QCOMPARE(simulation->_activeOutputs, QVector<quint8>({1}));
        QCOMPARE(simulation->_physicsRig->Inputs[0].SourceParameterIndex, 0);
        QCOMPARE(simulation->_physicsRig->Outputs[0].DestinationParameterIndex, 1);
        QCOMPARE(simulation->_currentRemainTime, 0.0125f);
        QCOMPARE(simulation->_physicsRig->Particles[1].Velocity.X, 0.625f);
        QCOMPARE(simulation->_currentRigOutputs[0].outputs[0], 0.375f);
    }

    void inactiveRowsNeverTouchRealVirtualOrPartValues_data()
    {
        QTest::addColumn<int>("method");
        QTest::addColumn<bool>("noRealParameters");
        for (bool empty : {false, true}) {
            QTest::newRow(empty ? "evaluate-empty" : "evaluate") << 0 << empty;
            QTest::newRow(empty ? "stabilization-empty" : "stabilization") << 1 << empty;
            QTest::newRow(empty ? "interpolate-empty" : "interpolate") << 2 << empty;
        }
    }

    void inactiveRowsNeverTouchRealVirtualOrPartValues()
    {
        QFETCH(int, method); QFETCH(bool, noRealParameters);
        auto previous = model();
        auto next = model([&](GetterState& value) {
            CubismModelFixture::SetParameterLayout(value, noRealParameters ? std::initializer_list<const char*>{} : std::initializer_list<const char*>{"ParamC"});
            value.partIds = {{"ParamA", "ParamB"}};
        });
        const auto a = idManager->GetId("ParamA"), b = idManager->GetId("ParamB");
        const int virtualA = next->GetParameterIndex(a), virtualB = next->GetParameterIndex(b);
        QVERIFY(virtualA >= next->GetParameterCount());
        QVERIFY(virtualB >= next->GetParameterCount());
        next->SetParameterValue(virtualA, 17); next->SetParameterValue(virtualB, 19);
        const auto realValues = State(next->GetModel()).values;
        const auto parts = State(next->GetModel()).partOpacities;
        const auto virtualValues = next->_notExistParameterValues;
        auto simulation = physics();
        ParameterPhysicsReferences references;
        references.sourceDigest = simulation->SourceDigest();
        references.suppressedInputs.insert(0, "ParamA"); references.suppressedOutputs.insert(0, "ParamB");
        CubismPhysics::ParameterDefinitionState prepared;
        QVERIFY(simulation->PrepareParameterStructureChange(*previous, *next, prepared, nullptr, &references));
        simulation->CommitParameterDefinitionChange(prepared);
        simulation->_physicsRig->Inputs[0].SourceParameterIndex = std::numeric_limits<int>::max();
        simulation->_physicsRig->Outputs[0].DestinationParameterIndex = std::numeric_limits<int>::max();
        simulation->_currentRigOutputs[0].outputs[0] = 0.375f;
        simulation->_previousRigOutputs[0].outputs[0] = -0.25f;
        if (method == 0) simulation->Evaluate(next.get(), 0.1f);
        else if (method == 1) simulation->Stabilization(next.get());
        else simulation->Interpolate(next.get(), 0.5f);
        QCOMPARE(State(next->GetModel()).values, realValues);
        QCOMPARE(State(next->GetModel()).partOpacities, parts);
        QCOMPARE(next->_notExistParameterValues, virtualValues);
        QCOMPARE(simulation->_currentRigOutputs[0].outputs[0], 0.375f);
        QCOMPARE(simulation->_previousRigOutputs[0].outputs[0], -0.25f);
        next->SetParameterValue(0, 0.8f);
        QCOMPARE(next->GetParameterValue(0), 0.8f);
        next->SetParameterValue(virtualA, 23);
        QCOMPARE(next->GetParameterValue(virtualA), 23.0f);
    }

    void invalidSuppressionCannotReplacePreparedState_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 10; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }

    void invalidSuppressionCannotReplacePreparedState()
    {
        QFETCH(int, kind);
        auto before = model();
        auto missing = model([](GetterState& value) { CubismModelFixture::SetParameterLayout(value, {"ParamC"}); });
        auto simulation = physics();
        simulation->_parameterCaches = {0.3f, -0.0f};
        simulation->_currentRemainTime = 0.01f;
        ParameterPhysicsReferences valid;
        valid.sourceDigest = simulation->SourceDigest(); valid.suppressedInputs.insert(0, "ParamA"); valid.suppressedOutputs.insert(0, "ParamB");
        CubismPhysics::ParameterDefinitionState prepared;
        QVERIFY(simulation->PrepareParameterStructureChange(*before, *missing, prepared, nullptr, &valid));
        auto invalid = valid;
        if (kind == 0) invalid.suppressedInputs = {{-1, "ParamA"}};
        if (kind == 1) invalid.suppressedInputs = {{1, "ParamA"}};
        if (kind == 2) invalid.suppressedInputs[0] = "ParamB";
        if (kind == 3) invalid.suppressedOutputs = {{-1, "ParamB"}};
        if (kind == 4) invalid.suppressedOutputs = {{1, "ParamB"}};
        if (kind == 5) invalid.suppressedOutputs[0] = "ParamA";
        if (kind == 6) invalid.sourceDigest.clear();
        if (kind == 7) invalid.sourceDigest = QByteArray(31, 'x');
        if (kind == 8) invalid.sourceDigest = QByteArray(32, 'x');
        if (kind == 9) { invalid = {}; invalid.sourceDigest = QByteArray(32, 'x'); }
        QString error;
        QVERIFY(!simulation->PrepareParameterStructureChange(*before, *missing, prepared, &error, &invalid));
        QVERIFY(!error.isEmpty());
        QCOMPARE(simulation->_activeInputs, QVector<quint8>({1}));
        QCOMPARE(simulation->_activeOutputs, QVector<quint8>({1}));
        QCOMPARE(simulation->_parameterCaches, QVector<float>({0.3f, -0.0f}));
        QCOMPARE(simulation->_currentRemainTime, 0.01f);
        simulation->CommitParameterDefinitionChange(prepared);
        QCOMPARE(simulation->_activeInputs, QVector<quint8>({0}));
        QCOMPARE(simulation->_activeOutputs, QVector<quint8>({0}));
        QCOMPARE(simulation->_physicsRig->Inputs[0].SourceParameterIndex, -1);
        QCOMPARE(simulation->_physicsRig->Outputs[0].DestinationParameterIndex, -1);
        QCOMPARE(simulation->_currentRemainTime, 0.01f);
    }

    void recreatedRealIdsStaySuppressedUntilExplicitRestoration()
    {
        auto original = model();
        auto absent = model([](GetterState& value) { CubismModelFixture::SetParameterLayout(value, {"ParamC"}); });
        auto simulation = physics();
        ParameterPhysicsReferences references;
        references.sourceDigest = simulation->SourceDigest();
        references.suppressedInputs.insert(0, "ParamA"); references.suppressedOutputs.insert(0, "ParamB");
        CubismPhysics::ParameterDefinitionState prepared;
        QVERIFY(simulation->PrepareParameterStructureChange(*original, *absent, prepared, nullptr, &references));
        simulation->CommitParameterDefinitionChange(prepared);
        auto recreated = model();
        QVERIFY(simulation->PrepareParameterStructureChange(*absent, *recreated, prepared));
        simulation->CommitParameterDefinitionChange(prepared);
        recreated->SetParameterValue(0, 1); recreated->SetParameterValue(1, 0.75f);
        simulation->Stabilization(recreated.get());
        QCOMPARE(simulation->_physicsRig->Particles[0].Position.X, 0.0f);
        QCOMPARE(recreated->GetParameterValue(1), 0.75f);
        ParameterPhysicsReferences restored;
        restored.sourceDigest = simulation->SourceDigest();
        QVERIFY(simulation->PrepareParameterStructureChange(*recreated, *recreated, prepared, nullptr, &restored));
        simulation->CommitParameterDefinitionChange(prepared);
        simulation->Stabilization(recreated.get());
        QCOMPARE(simulation->_physicsRig->Particles[0].Position.X, 1.0f);
        QCOMPARE(recreated->GetParameterValue(1), 0.0f);
    }

    void digestMatchesOriginalBytesEvenWhenJsonMeaningMatches()
    {
        const QByteArray first = physicsJson(), second = first + ' ';
        auto initial = physics(first), other = physics(second);
        QVERIFY(initial->SourceDigest() != other->SourceDigest());
        auto original = model(), replacement = model();
        ParameterPhysicsReferences references;
        references.sourceDigest = initial->SourceDigest(); references.suppressedInputs.insert(0, "ParamA");
        CubismPhysics::ParameterDefinitionState prepared;
        QString error;
        QVERIFY(!other->PrepareParameterStructureChange(*original, *replacement, prepared, &error, &references));
        QCOMPARE(other->_activeInputs, QVector<quint8>({1}));
        references.sourceDigest = other->SourceDigest();
        QVERIFY(other->PrepareParameterStructureChange(*original, *replacement, prepared, &error, &references));
        initial->CommitParameterDefinitionChange(prepared);
        QCOMPARE(initial->_activeInputs, QVector<quint8>({1}));
        other->CommitParameterDefinitionChange(prepared);
        QCOMPARE(other->_activeInputs, QVector<quint8>({0}));
    }

    void individualRowsKeepTheirOriginalWeights()
    {
        auto root = QJsonDocument::fromJson(physicsJson()).object();
        auto settings = root.value("PhysicsSettings").toArray(); auto setting = settings[0].toObject();
        auto input = setting.value("Input").toArray()[0].toObject(); input["Weight"] = 25;
        auto otherInput = input; otherInput["Weight"] = 75; otherInput["Source"] = QJsonObject{{"Target", "Parameter"}, {"Id", "ParamB"}};
        setting["Input"] = QJsonArray{input, otherInput};
        auto output = setting.value("Output").toArray()[0].toObject(); output["Weight"] = 25; output["Type"] = "Angle";
        output["Destination"] = QJsonObject{{"Target", "Parameter"}, {"Id", "ParamC"}};
        auto otherOutput = output; otherOutput["Weight"] = 75;
        setting["Output"] = QJsonArray{output, otherOutput}; settings[0] = setting; root["PhysicsSettings"] = settings;
        auto meta = root.value("Meta").toObject(); meta["TotalInputCount"] = 2; meta["TotalOutputCount"] = 2; root["Meta"] = meta;
        const auto json = QJsonDocument(root).toJson(QJsonDocument::Compact);
        auto before = model([](GetterState& value) { CubismModelFixture::SetParameterLayout(value, {"ParamA", "ParamB", "ParamC"}); });
        auto next = model([](GetterState& value) { CubismModelFixture::SetParameterLayout(value, {"ParamA", "ParamB", "ParamC"}); });
        next->SetParameterValue(0, 1); next->SetParameterValue(1, 1); next->SetParameterValue(2, 0);
        auto simulation = physics(json);
        ParameterPhysicsReferences references;
        references.sourceDigest = simulation->SourceDigest(); references.suppressedInputs.insert(0, "ParamA"); references.suppressedOutputs.insert(1, "ParamC");
        CubismPhysics::ParameterDefinitionState prepared;
        QVERIFY(simulation->PrepareParameterStructureChange(*before, *next, prepared, nullptr, &references));
        simulation->CommitParameterDefinitionChange(prepared);
        simulation->Stabilization(next.get());
        QCOMPARE(simulation->_physicsRig->Particles[0].Position.X, 0.375f);
        QCOMPARE(simulation->_physicsRig->Inputs[0].Weight, 25.0f);
        QCOMPARE(simulation->_physicsRig->Inputs[1].Weight, 75.0f);
        next->SetParameterValue(2, 0);
        simulation->_currentRigOutputs[0].outputs[0] = simulation->_previousRigOutputs[0].outputs[0] = 1;
        simulation->_currentRigOutputs[0].outputs[1] = simulation->_previousRigOutputs[0].outputs[1] = -1;
        simulation->Interpolate(next.get(), 0.5f);
        QCOMPARE(next->GetParameterValue(2), 0.25f);
        QCOMPARE(simulation->_physicsRig->Outputs[0].Weight, 25.0f);
        QCOMPARE(simulation->_physicsRig->Outputs[1].Weight, 75.0f);
    }

    void suppressionRowsUseOriginalFlattenedGroups()
    {
        auto root = QJsonDocument::fromJson(physicsJson()).object();
        auto settings = root.value("PhysicsSettings").toArray(); auto second = settings[0].toObject(); second["Id"] = "two";
        auto inputs = second.value("Input").toArray(); auto input = inputs[0].toObject(); input["Source"] = QJsonObject{{"Target", "Parameter"}, {"Id", "ParamC"}}; inputs[0] = input; second["Input"] = inputs;
        auto outputs = second.value("Output").toArray(); auto output = outputs[0].toObject(); output["Destination"] = QJsonObject{{"Target", "Parameter"}, {"Id", "ParamD"}}; outputs[0] = output; second["Output"] = outputs;
        settings.append(second); root["PhysicsSettings"] = settings;
        auto meta = root.value("Meta").toObject(); meta["PhysicsSettingCount"] = 2; meta["TotalInputCount"] = 2; meta["TotalOutputCount"] = 2; meta["VertexCount"] = 4; root["Meta"] = meta;
        auto simulation = physics(QJsonDocument(root).toJson(QJsonDocument::Compact));
        auto before = model([](GetterState& value) { CubismModelFixture::SetParameterLayout(value, {"ParamA", "ParamB", "ParamC", "ParamD"}); });
        auto next = model(); next->SetParameterValue(0, 0.5f);
        ParameterPhysicsReferences references;
        references.sourceDigest = simulation->SourceDigest(); references.suppressedInputs.insert(1, "ParamC"); references.suppressedOutputs.insert(1, "ParamD");
        CubismPhysics::ParameterDefinitionState prepared;
        QVERIFY(simulation->PrepareParameterStructureChange(*before, *next, prepared, nullptr, &references));
        simulation->CommitParameterDefinitionChange(prepared);
        QCOMPARE(simulation->_activeInputs, QVector<quint8>({1, 0}));
        QCOMPARE(simulation->_activeOutputs, QVector<quint8>({1, 0}));
        simulation->Stabilization(next.get());
        QCOMPARE(simulation->_physicsRig->Particles[0].Position.X, 0.5f);
        QCOMPARE(simulation->_physicsRig->Particles[2].Position.X, 0.0f);
        QVERIFY(next->_notExistParameterId.isEmpty());
    }

    void structuralMigrationPreparesIndicesAndWarmCachesById()
    {
        auto before = model();
        auto next = model([](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamC", "ParamB", "ParamA"}); });
        auto simulation = physics();
        simulation->_parameterCaches = {0.3f, -0.0f};
        simulation->_parameterInputCaches = {-0.0f, 0.5f};
        simulation->_currentRemainTime = 0.0125f;
        simulation->_physicsRig->Particles[1].Velocity.X = 0.625f;
        const int oldInput = simulation->_physicsRig->Inputs[0].SourceParameterIndex;
        CubismPhysics::ParameterDefinitionState prepared;
        QString error;
        QVERIFY2(simulation->PrepareParameterStructureChange(*before, *next, prepared, &error), qPrintable(error));
        QCOMPARE(simulation->_parameterCaches.size(), 2);
        QCOMPARE(simulation->_physicsRig->Inputs[0].SourceParameterIndex, oldInput);
        simulation->CommitParameterDefinitionChange(prepared);
        QCOMPARE(simulation->_parameterCaches, QVector<float>({0.4f, -0.0f, 0.3f}));
        QCOMPARE(simulation->_parameterInputCaches, QVector<float>({0.4f, 0.5f, -0.0f}));
        QVERIFY(std::signbit(simulation->_parameterCaches[1]));
        QVERIFY(std::signbit(simulation->_parameterInputCaches[2]));
        QCOMPARE(simulation->_physicsRig->Inputs[0].SourceParameterIndex, 2);
        QCOMPARE(simulation->_physicsRig->Outputs[0].DestinationParameterIndex, 1);
        QCOMPARE(simulation->_currentRemainTime, 0.0125f);
        QCOMPARE(simulation->_physicsRig->Particles[1].Velocity.X, 0.625f);
        simulation->_physicsRig->Inputs[0].SourceParameterIndex = 0;
        simulation->_physicsRig->Outputs[0].DestinationParameterIndex = 999;
        simulation->Evaluate(next.get(), 0.01f);
        QCOMPARE(simulation->_physicsRig->Inputs[0].SourceParameterIndex, 2);
        QCOMPARE(simulation->_physicsRig->Outputs[0].DestinationParameterIndex, 1);
    }

    void missingRealPhysicsReferencesRejectAndCannotReadVirtualIndices()
    {
        auto before = model();
        auto missing = model([](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); });
        auto simulation = physics();
        simulation->_parameterCaches = {0.3f, 0.5f};
        simulation->_currentRemainTime = 0.015f;
        CubismPhysics::ParameterDefinitionState prepared;
        QString error;
        QVERIFY(!simulation->PrepareParameterStructureChange(*before, *missing, prepared, &error));
        QVERIFY(error.contains(QStringLiteral("ParamA")));
        const auto original = State(missing->GetModel()).values;
        simulation->Evaluate(missing.get(), 0.1f);
        simulation->Stabilization(missing.get());
        simulation->Interpolate(missing.get(), 0.5f);
        QCOMPARE(State(missing->GetModel()).values, original);
        QCOMPARE(simulation->_currentRemainTime, 0.015f);
        QCOMPARE(simulation->_parameterCaches, QVector<float>({0.3f, 0.5f}));
        QVERIFY(missing->_notExistParameterId.isEmpty());
    }

    void migrationOnlyChangesAffectedAbsoluteCachesAfterCommit()
    {
        auto before = model(), next = model();
        State(before->GetModel()).minimum[0] = -2;
        State(before->GetModel()).maximum[0] = 2;
        auto simulation = physics();
        simulation->_parameterCaches.append(1.5f);
        simulation->_parameterCaches.append(-0.0f);
        simulation->_parameterInputCaches.append(-1.5f);
        simulation->_parameterInputCaches.append(3.5f);
        simulation->_currentRemainTime = 0.017f;
        simulation->_currentRigOutputs[0].outputs[0] = 0.37f;
        simulation->_previousRigOutputs[0].outputs[0] = -0.42f;
        auto& particle = simulation->_physicsRig->Particles[1];
        particle.Position = CubismVector2(0.25f, 0.75f);
        particle.Velocity = CubismVector2(0.33f, -0.44f);
        CubismPhysics::ParameterDefinitionState pending;
        QString error;
        QVERIFY2(simulation->PrepareParameterDefinitionChange(*before, *next, pending, &error), qPrintable(error));
        QCOMPARE(simulation->_parameterCaches[0], 1.5f);
        QCOMPARE(simulation->_parameterInputCaches[0], -1.5f);
        State(next->GetModel()).maximum[0] = std::numeric_limits<float>::infinity();
        QVERIFY(!simulation->PrepareParameterDefinitionChange(*before, *next, pending, &error));
        State(next->GetModel()).maximum[0] = 1;
        simulation->CommitParameterDefinitionChange(pending);
        QCOMPARE(simulation->_parameterCaches[0], 1.0f);
        QCOMPARE(simulation->_parameterInputCaches[0], -1.0f);
        QVERIFY(std::signbit(simulation->_parameterCaches[1]));
        QCOMPARE(simulation->_parameterInputCaches[1], 3.5f);
        QCOMPARE(simulation->_currentRemainTime, 0.017f);
        QCOMPARE(simulation->_currentRigOutputs[0].outputs[0], 0.37f);
        QCOMPARE(simulation->_previousRigOutputs[0].outputs[0], -0.42f);
        QCOMPARE(particle.Position.X, 0.25f);
        QCOMPARE(particle.Position.Y, 0.75f);
        QCOMPARE(particle.Velocity.X, 0.33f);
        QCOMPARE(particle.Velocity.Y, -0.44f);
        simulation->CommitParameterDefinitionChange(pending);
        QCOMPARE(simulation->_parameterCaches[0], 1.0f);
    }
    void defaultChangesKeepCacheBitsAndColdCachesStayCold()
    {
        auto before = model(), next = model();
        State(next->GetModel()).defaults[0] = 0.5f;
        auto simulation = physics();
        CubismPhysics::ParameterDefinitionState pending;
        QVERIFY(simulation->PrepareParameterDefinitionChange(*before, *next, pending));
        simulation->CommitParameterDefinitionChange(pending);
        QCOMPARE(simulation->_parameterCaches.size(), 0u);
        QCOMPARE(simulation->_parameterInputCaches.size(), 0u);
        simulation->_parameterCaches.append(3.0f);
        simulation->_parameterInputCaches.append(-0.0f);
        QVERIFY(simulation->PrepareParameterDefinitionChange(*before, *next, pending));
        simulation->CommitParameterDefinitionChange(pending);
        QCOMPARE(simulation->_parameterCaches[0], 3.0f);
        QVERIFY(std::signbit(simulation->_parameterInputCaches[0]));
        simulation->_parameterInputCaches[0] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!simulation->PrepareParameterDefinitionChange(*before, *next, pending));
        QCOMPARE(simulation->_parameterCaches[0], 3.0f);
    }
    void finiteExtremeNormalizationUsesTheRangeMidpoint_data()
    {
        QTest::addColumn<float>("minimum");
        QTest::addColumn<float>("maximum");
        QTest::addColumn<float>("value");
        QTest::addColumn<float>("expected");
        const float limit = std::numeric_limits<float>::max();
        QTest::newRow("wide-min") << -limit << limit << -limit << -1.0f;
        QTest::newRow("wide-mid") << -limit << limit << 0.0f << 0.0f;
        QTest::newRow("wide-max") << -limit << limit << limit << 1.0f;
        QTest::newRow("wide-positive") << -limit << limit << limit * 0.5f << 0.5f;
        QTest::newRow("wide-negative") << -limit << limit << -limit * 0.5f << -0.5f;
        QTest::newRow("same-sign-mid") << 1.0e38f << 3.0e38f << 2.0e38f << 0.0f;
        QTest::newRow("constant") << limit << limit << limit << 0.0f;
        QTest::newRow("ordinary-mid-default-ignored") << -1.0f << 3.0f << 1.0f << 0.0f;
        QTest::newRow("ordinary-quarter") << -1.0f << 3.0f << 2.0f << 0.5f;
    }
    void finiteExtremeNormalizationUsesTheRangeMidpoint()
    {
        QFETCH(float, minimum); QFETCH(float, maximum); QFETCH(float, value); QFETCH(float, expected);
        auto simulation = physics();
        auto& input = simulation->_physicsRig->Inputs[0];
        auto& settings = simulation->_physicsRig->Settings[0];
        for (int reflection : {0, 1}) {
            CubismVector2 translation;
            float angle = 0;
            input.GetNormalizedParameterValue(&translation, &angle, value, minimum, maximum, minimum,
                &settings.NormalizationPosition, &settings.NormalizationAngle, reflection, 1.0f);
            QVERIFY(std::isfinite(translation.X));
            QVERIFY(std::abs(translation.X - (reflection ? expected : -expected)) < 0.000001f);
        }
    }
    void evaluateReadsTheReplacementRange()
    {
        auto source = model(), next = model();
        State(source->GetModel()).maximum[0] = 3.0f;
        source->SetParameterValue(0, 1.0f);
        next->SetParameterValue(0, 1.0f);
        auto original = physics(), replacement = physics();
        original->Stabilization(source.get());
        replacement->Stabilization(next.get());
        QCOMPARE(original->_physicsRig->Particles[0].Position.X, 0.0f);
        QCOMPARE(replacement->_physicsRig->Particles[0].Position.X, 1.0f);
        QVERIFY(std::isfinite(next->GetParameterValue(1)));
    }
};

QTEST_GUILESS_MAIN(PhysicsParameterDefinitionTests)
#include "physics_parameter_definition_tests.moc"
