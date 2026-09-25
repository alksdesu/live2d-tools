#include "framework/Model/CubismModel.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include "cubism_model_fixture.h"
#include "framework/Physics/CubismPhysics.hpp"
#include "framework/Utils/CubismJson.hpp"

#include <QtTest>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
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
    static ModelOwner model()
    {
        auto data = std::make_unique<GetterState>();
        void* handle = allocate(sizeof(CubismModelFixture::ModelHandle), Core::csmAlignofModel);
        auto* core = CubismModelFixture::AttachModel(handle, data.get());
        coreStates.emplace(handle, std::move(data));
        ModelOwner result(new CubismModel(core), [](CubismModel* value) { delete value; });
        result->Initialize();
        return result;
    }
    static PhysicsOwner physics()
    {
        const QByteArray json = R"({"Version":3,"Meta":{"PhysicsSettingCount":1,"TotalInputCount":1,"TotalOutputCount":1,"VertexCount":2,"Fps":30,"EffectiveForces":{"Gravity":{"X":0,"Y":-1},"Wind":{"X":0,"Y":0}}},"PhysicsSettings":[{"Id":"one","Input":[{"Source":{"Target":"Parameter","Id":"ParamA"},"Weight":100,"Type":"X","Reflect":true}],"Output":[{"Destination":{"Target":"Parameter","Id":"ParamB"},"VertexIndex":1,"Scale":1,"Weight":100,"Type":"X","Reflect":false}],"Vertices":[{"Position":{"X":0,"Y":0},"Mobility":1,"Delay":1,"Acceleration":1,"Radius":0},{"Position":{"X":0,"Y":1},"Mobility":0.8,"Delay":0.2,"Acceleration":1,"Radius":1}],"Normalization":{"Position":{"Minimum":-1,"Maximum":1,"Default":0},"Angle":{"Minimum":-30,"Maximum":30,"Default":0}}}]})";
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
    void migrationOnlyChangesAffectedAbsoluteCachesAfterCommit()
    {
        auto before = model(), next = model();
        State(before->GetModel()).minimum[0] = -2;
        State(before->GetModel()).maximum[0] = 2;
        auto simulation = physics();
        simulation->_parameterCaches.PushBack(1.5f);
        simulation->_parameterCaches.PushBack(-0.0f);
        simulation->_parameterInputCaches.PushBack(-1.5f);
        simulation->_parameterInputCaches.PushBack(3.5f);
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
        QCOMPARE(simulation->_parameterCaches.GetSize(), 0u);
        QCOMPARE(simulation->_parameterInputCaches.GetSize(), 0u);
        simulation->_parameterCaches.PushBack(3.0f);
        simulation->_parameterInputCaches.PushBack(-0.0f);
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
