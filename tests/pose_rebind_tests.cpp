#include "framework/Effect/CubismPose.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Utils/CubismDebug.hpp"

#include <QtTest>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <unordered_map>

using namespace Live2D::Cubism::Framework;

namespace {
struct ModelState
{
    std::array<float, 3> parameters{};
    std::array<float, 3> opacities{};
    int lookups = 0;
    int parameterWrites = 0;
    int opacityWrites = 0;
};

std::unordered_map<const CubismModel*, ModelState> models;
std::unique_ptr<CubismIdManager> ids;

int indexOf(CubismIdHandle id)
{
    const QByteArray value(id->GetString().GetRawString());
    if (value == "PartA") return 0;
    if (value == "PartB") return 1;
    if (value == "PartLink") return 2;
    qFatal("Unexpected Pose test ID");
}

using PoseOwner = std::unique_ptr<CubismPose, decltype(&CubismPose::Delete)>;
PoseOwner createPose()
{
    const QByteArray json = R"({"FadeInTime":1,"Groups":[[{"Id":"PartA","Link":["PartLink"]},{"Id":"PartB"}]]})";
    return {CubismPose::Create(reinterpret_cast<const csmByte*>(json.constData()), json.size()), &CubismPose::Delete};
}

struct ModelDeleter
{
    CubismMoc* moc;
    void operator()(CubismModel* model) const { moc->DeleteModel(model); }
};
using ModelOwner = std::unique_ptr<CubismModel, ModelDeleter>;
}

void* operator new(csmSizeType size, CubismAllocationTag)
{
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory, CubismAllocationTag) { std::free(memory); }

namespace Live2D { namespace Cubism { namespace Framework {
void* CubismFramework::Allocate(csmSizeType size) { return ::operator new(size, GlobalTag); }
void CubismFramework::Deallocate(void* memory) { std::free(memory); }
CubismIdManager* CubismFramework::GetIdManager() { return ids.get(); }
void CubismFramework::Initialize()
{
    Utils::Value::StaticInitializeNotForClientCall();
    ids = std::make_unique<CubismIdManager>();
}
void CubismFramework::Dispose()
{
    ids.reset();
    Utils::Value::StaticReleaseNotForClientCall();
}
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}

// Controlled model methods exercise the real Pose without Cubism Core.
CubismMoc::CubismMoc(Core::csmMoc* moc) : _moc(moc), _modelCount(0), _mocVersion(0) {}
CubismMoc::~CubismMoc() { Q_ASSERT(_modelCount == 0); }
CubismMoc* CubismMoc::Create(const csmByte*, csmSizeInt, csmBool) { return new CubismMoc(nullptr); }
void CubismMoc::Delete(CubismMoc* moc) { delete moc; }
CubismModel* CubismMoc::CreateModel()
{
    auto* model = new CubismModel(nullptr);
    models.emplace(model, ModelState{});
    ++_modelCount;
    return model;
}
void CubismMoc::DeleteModel(CubismModel* model)
{
    if (!model) return;
    if (models.erase(model) != 1) qFatal("Unknown controlled model");
    --_modelCount;
    delete model;
}
CubismModel::CubismModel(Core::csmModel* model) : _model(model) {}
CubismModel::~CubismModel() = default;
csmInt32 CubismModel::GetParameterIndex(CubismIdHandle id)
{
    ++models.at(this).lookups;
    return indexOf(id);
}
csmInt32 CubismModel::GetPartIndex(CubismIdHandle id)
{
    ++models.at(this).lookups;
    return indexOf(id);
}
csmFloat32 CubismModel::GetParameterValue(csmInt32 index) { return models.at(this).parameters.at(index); }
csmFloat32 CubismModel::GetPartOpacity(csmInt32 index) { return models.at(this).opacities.at(index); }
void CubismModel::SetParameterValue(csmInt32 index, csmFloat32 value, csmFloat32 weight)
{
    auto& model = models.at(this);
    model.parameters.at(index) = model.parameters.at(index) * (1 - weight) + value * weight;
    ++model.parameterWrites;
}
void CubismModel::SetPartOpacity(csmInt32 index, csmFloat32 value)
{
    auto& model = models.at(this);
    model.opacities.at(index) = value;
    ++model.opacityWrites;
}
}}}

CMeshMap::CMeshMap() = default;

class PoseRebindTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { CubismFramework::Initialize(); }
    void cleanup() { QVERIFY(models.empty()); }
    void cleanupTestCase() { CubismFramework::Dispose(); }

    void boundPoseContinuesItsFadeWithoutResettingReplacement()
    {
        std::unique_ptr<CubismMoc, decltype(&CubismMoc::Delete)> moc(CubismMoc::Create(nullptr, 0), &CubismMoc::Delete);
        ModelOwner previous(moc->CreateModel(), ModelDeleter{moc.get()});
        ModelOwner replacement(moc->CreateModel(), ModelDeleter{moc.get()});
        auto pose = createPose();
        QVERIFY(pose);
        pose->UpdateParameters(previous.get(), 0);
        models.at(previous.get()) = {{0, 1, 0.4f}, {0.8f, 0.35f, 0.8f}};
        models.at(replacement.get()) = models.at(previous.get());
        const auto before = models.at(previous.get());
        QVERIFY(pose->CanRebindModel(previous.get()));
        QVERIFY(pose->RebindModel(previous.get(), replacement.get()));
        QVERIFY(!pose->CanRebindModel(previous.get()));
        QVERIFY(pose->CanRebindModel(replacement.get()));
        QCOMPARE(models.at(previous.get()).parameters, before.parameters);
        QCOMPARE(models.at(previous.get()).opacities, before.opacities);
        QCOMPARE(models.at(replacement.get()).parameters, before.parameters);
        QCOMPARE(models.at(replacement.get()).opacities, before.opacities);
        QCOMPARE(models.at(replacement.get()).lookups, 0);
        QCOMPARE(models.at(replacement.get()).parameterWrites, 0);
        QCOMPARE(models.at(replacement.get()).opacityWrites, 0);
        pose->UpdateParameters(replacement.get(), 0.1f);
        const auto& after = models.at(replacement.get());
        QCOMPARE(after.lookups, 0);
        QCOMPARE(after.parameterWrites, 0);
        QCOMPARE(after.parameters, before.parameters);
        QVERIFY(std::abs(after.opacities[1] - 0.45f) < 0.00001f);
        QCOMPARE(after.opacities[2], after.opacities[0]);
        QCOMPARE(models.at(previous.get()).parameters, before.parameters);
        QCOMPARE(models.at(previous.get()).opacities, before.opacities);
        QCOMPARE(models.at(previous.get()).lookups, 0);
        QCOMPARE(models.at(previous.get()).parameterWrites, 0);
        QCOMPARE(models.at(previous.get()).opacityWrites, 0);
        QVERIFY(pose->RebindModel(replacement.get(), replacement.get()));
        QCOMPARE(after.lookups, 0);
    }

    void coldPoseRemainsUnboundUntilItsFirstUpdate()
    {
        std::unique_ptr<CubismMoc, decltype(&CubismMoc::Delete)> moc(CubismMoc::Create(nullptr, 0), &CubismMoc::Delete);
        ModelOwner previous(moc->CreateModel(), ModelDeleter{moc.get()});
        ModelOwner replacement(moc->CreateModel(), ModelDeleter{moc.get()});
        auto pose = createPose();
        QVERIFY(pose);
        models.at(replacement.get()) = {{0, 1, 0.4f}, {0.8f, 0.35f, 0.8f}};
        QVERIFY(pose->CanRebindModel(previous.get()));
        QVERIFY(pose->RebindModel(previous.get(), replacement.get()));
        QVERIFY(pose->CanRebindModel(previous.get()));
        QCOMPARE(models.at(replacement.get()).lookups, 0);
        QCOMPARE(models.at(replacement.get()).parameterWrites, 0);
        pose->UpdateParameters(replacement.get(), 0);
        QVERIFY(models.at(replacement.get()).lookups > 0);
        QVERIFY(models.at(replacement.get()).parameterWrites > 0);
        QCOMPARE(models.at(replacement.get()).parameters[0], 1.0f);
        QCOMPARE(models.at(replacement.get()).parameters[1], 0.0f);
        QCOMPARE(models.at(replacement.get()).opacities[0], 1.0f);
        QCOMPARE(models.at(replacement.get()).opacities[1], 0.0f);
    }

    void foreignAndNullBindingsFailWithoutChangingTheActivePose()
    {
        std::unique_ptr<CubismMoc, decltype(&CubismMoc::Delete)> moc(CubismMoc::Create(nullptr, 0), &CubismMoc::Delete);
        ModelOwner previous(moc->CreateModel(), ModelDeleter{moc.get()});
        ModelOwner replacement(moc->CreateModel(), ModelDeleter{moc.get()});
        ModelOwner foreign(moc->CreateModel(), ModelDeleter{moc.get()});
        auto pose = createPose();
        QVERIFY(pose);
        pose->UpdateParameters(previous.get(), 0);
        models.at(previous.get()) = {{0, 1, 0.4f}, {0.8f, 0.35f, 0.8f}};
        QVERIFY(!pose->CanRebindModel(foreign.get()));
        QVERIFY(!pose->CanRebindModel(nullptr));
        QVERIFY(!pose->RebindModel(foreign.get(), replacement.get()));
        QVERIFY(!pose->RebindModel(nullptr, replacement.get()));
        QVERIFY(!pose->RebindModel(previous.get(), nullptr));
        QVERIFY(pose->CanRebindModel(previous.get()));
        QCOMPARE(models.at(previous.get()).lookups, 0);
        QCOMPARE(models.at(previous.get()).parameterWrites, 0);
        QCOMPARE(models.at(previous.get()).opacityWrites, 0);
        pose->UpdateParameters(previous.get(), 0.1f);
        QCOMPARE(models.at(previous.get()).lookups, 0);
        QCOMPARE(models.at(previous.get()).parameters[0], 0.0f);
        QVERIFY(std::abs(models.at(previous.get()).opacities[1] - 0.45f) < 0.00001f);
        QCOMPARE(models.at(replacement.get()).parameterWrites, 0);
        QCOMPARE(models.at(replacement.get()).opacityWrites, 0);
        QCOMPARE(models.at(foreign.get()).parameterWrites, 0);
        QCOMPARE(models.at(foreign.get()).opacityWrites, 0);
    }
};

QTEST_GUILESS_MAIN(PoseRebindTests)
#include "pose_rebind_tests.moc"
