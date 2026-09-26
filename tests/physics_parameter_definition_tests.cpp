#include "framework/Model/CubismModel.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include "cubism_model_fixture.h"
#include "framework/Physics/CubismPhysics.hpp"
#include "framework/Model/CubismParameterReferences.hpp"
#include "framework/Utils/CubismJson.hpp"

#include <QtTest>
#include <QCryptographicHash>
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
