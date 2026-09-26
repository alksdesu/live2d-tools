#include "framework/Model/CubismModel.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "framework/Utils/CubismDebug.hpp"
#include "cubism_model_fixture.h"

#include <QtTest>
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
    if (found == allocations.end()) qFatal("Unknown SDK allocation in model state test");
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
    if (coreStates.erase(memory) != 1) qFatal("Unknown Core allocation in model state test");
    release(memory);
}
CubismIdManager* CubismFramework::GetIdManager() { return idManager.get(); }
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}
}}}


class ModelRuntimeStateTests : public QObject
{
    Q_OBJECT
    using ModelOwner = std::unique_ptr<CubismModel, void (*)(CubismModel*)>;

    static ModelOwner create(bool empty = false, int drawables = 2, const std::function<void(GetterState&)>& configure = {})
    {
        auto data = std::make_unique<GetterState>();
        data->drawableCount = drawables;
        if (configure) configure(*data);
        if (empty) data->parameterCount = data->partCount = data->drawableCount = 0;
        void* handle = allocate(sizeof(CubismModelFixture::ModelHandle), Core::csmAlignofModel);
        auto* model = CubismModelFixture::AttachModel(handle, data.get());
        coreStates.emplace(handle, std::move(data));
        ModelOwner result(new CubismModel(model),
                          [](CubismModel* model) { delete model; });
        result->Initialize();
        return result;
    }

    static void verifyColor(const Rendering::CubismRenderer::CubismTextureColor& color,
                            float r, float g, float b, float a)
    {
        QCOMPARE(color.R, r);
        QCOMPARE(color.G, g);
        QCOMPARE(color.B, b);
        QCOMPARE(color.A, a);
    }

private slots:
    void init()
    {
        QVERIFY(allocations.empty());
        QVERIFY(coreStates.empty());
        rejectSdkAllocation = false;
        idManager = std::make_unique<CubismIdManager>();
    }

    void cleanup()
    {
        rejectSdkAllocation = false;
        idManager.reset();
        QVERIFY(coreStates.empty());
        QVERIFY(allocations.empty());
    }

    void structureReordersValuesAndPreservesSparseSavedState()
    {
        auto source = create();
        source->_savedParameters = {0.2f};
        source->SetParameterValue(0, 0.7f);
        source->SetParameterValue(1, 0.8f);
        const auto* virtualId = idManager->GetId("Virtual");
        const int oldVirtual = source->GetParameterIndex(virtualId);
        source->SetParameterValue(oldVirtual, -0.0f);
        auto next = create(false, 2, [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamC", "ParamB", "ParamA"}); });
        QString error;
        QVERIFY(!next->CopyRuntimeStateFrom(*source, &error));
        QVERIFY(!next->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowParameterDefinitionEdits, &error));
        QVERIFY2(next->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowParameterStructureEdits, &error), qPrintable(error));
        QCOMPARE(next->GetParameterValue(0), 0.4f);
        QCOMPARE(next->GetParameterValue(1), 0.8f);
        QCOMPARE(next->GetParameterValue(2), 0.7f);
        QCOMPARE(next->_savedPresence, QVector<quint8>({1, 0, 1}));
        QCOMPARE(next->_savedParameters, QVector<float>({0.4f, 0, 0.2f}));
        QCOMPARE(next->GetParameterIndex(virtualId), 3);
        QVERIFY(std::signbit(next->GetParameterValue(3)));
        QCOMPARE(source->GetParameterIndex(virtualId), 2);
        next->SetParameterValue(1, 0.9f);
        next->LoadParameters();
        QCOMPARE(next->GetParameterValue(0), 0.4f);
        QCOMPARE(next->GetParameterValue(1), 0.9f);
        QCOMPARE(next->GetParameterValue(2), 0.2f);
        CubismModel::RuntimeValueSnapshot captured;
        QVERIFY(next->CaptureRuntimeValues(captured));
        next->SaveParameters();
        QVERIFY(next->_savedPresence.isEmpty());
        QVERIFY(next->RestoreRuntimeValues(captured));
        QCOMPARE(next->_savedPresence, QVector<quint8>({1, 0, 1}));
        QCOMPARE(source->_savedParameters, QVector<float>({0.2f}));
    }

    void structuralHistorySeedsSurviveVirtualWritesAndSameIdRecreation()
    {
        const auto policy = CubismModelUpdatePolicy::AllowParameterStructureEdits;
        auto original = create();
        original->_savedParameters = {0.2f};
        original->SetParameterValue(0, 0.7f);
        CubismModel::RuntimeValueSnapshot values;
        QVERIFY(original->CaptureRuntimeValues(values));
        ParameterStructureSeed saved;
        saved.mode = ParameterStructureSeed::Mode::Values;
        saved.runtime = values.Parameter(0);
        auto deleted = create(false, 2, [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); });
        QVERIFY(deleted->CopyRuntimeStateFrom(*original, policy));
        const auto* a = idManager->GetId("ParamA");
        QCOMPARE(deleted->GetParameterIndex(a), 1);
        deleted->SetParameterValue(a, 0.99f);
        auto recreated = create();
        QVERIFY(recreated->CopyRuntimeStateFrom(*deleted, policy, {{QStringLiteral("ParamA"), ParameterStructureSeed{}}}));
        QCOMPARE(recreated->GetParameterValue(0), 0.0f);
        QCOMPARE(recreated->_savedParameters, QVector<float>({0.0f}));
        auto removedAgain = create(false, 2, [](GetterState& state) { CubismModelFixture::SetParameterLayout(state, {"ParamB"}); });
        QVERIFY(removedAgain->CopyRuntimeStateFrom(*recreated, policy));
        auto restored = create();
        QVERIFY(restored->CopyRuntimeStateFrom(*removedAgain, policy, {{QStringLiteral("ParamA"), saved}}));
        QCOMPARE(restored->GetParameterValue(0), 0.7f);
        QCOMPARE(restored->_savedParameters, QVector<float>({0.2f}));
        QVERIFY(!restored->_notExistParameterId.contains(a));
        QVERIFY(!restored->_retiredParameters.contains(QStringLiteral("ParamA")));
        restored->LoadParameters();
        QCOMPARE(restored->GetParameterValue(0), 0.2f);
        QCOMPARE(restored->GetParameterValue(1), 0.25f);
    }

    void structuralKeysCanChangeWithoutWeakeningOtherPolicies()
    {
        auto source = create(), candidate = create();
        State(candidate->GetModel()).keys[0] = {-0.5f, 0.2f, 0.75f};
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source));
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowParameterDefinitionEdits));
        QVERIFY(candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowParameterStructureEdits));
        State(candidate->GetModel()).keys[0][1] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowParameterStructureEdits));
    }

    void invalidStructuralSeedsAndPresenceDoNotPartiallyWrite()
    {
        auto source = create();
        auto candidate = create(false, 2, [](GetterState& state) { state.parameterCount = 3; });
        const auto before = State(candidate->GetModel()).values;
        QString error;
        for (int kind = 0; kind < 4; ++kind) {
            ParameterStructureSeed seed;
            seed.mode = ParameterStructureSeed::Mode::Values;
            seed.runtime = {0.5f, 0.2f, 0, -1, 1};
            QString id = QStringLiteral("ParamC");
            if (kind == 0) id = QStringLiteral("Missing");
            if (kind == 1) id = QStringLiteral("ParamA");
            if (kind == 2) seed.runtime.type = 1;
            if (kind == 3) seed.runtime.saved = std::numeric_limits<float>::quiet_NaN();
            QVERIFY(!candidate->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowParameterStructureEdits, {{id, seed}}, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(State(candidate->GetModel()).values, before);
        }
        CubismModel::RuntimeValueSnapshot state;
        QVERIFY(source->CaptureRuntimeValues(state));
        state.savedParameters = {0.25f};
        state.savedPresence = {2};
        QVERIFY(!source->RestoreRuntimeValues(state, &error));
        state.savedPresence = {1, 0};
        QVERIFY(!source->RestoreRuntimeValues(state, &error));
        QVERIFY(source->_savedParameters.isEmpty());
    }

    void staticSchemaAllowsRuntimeAndGeometryDifferences()
    {
        auto source = create();
        auto candidate = create();
        auto& data = State(candidate->GetModel());
        data.positions[0][0] = {100, -20};
        data.values[0] = 0.8f;
        data.partOpacities[0] = 0.2f;
        data.drawableOpacities[0] = 0.4f;
        data.drawOrders[0] = 99;
        data.renderOrders[0] = -7;
        data.multiply[0].X = 0.5f;
        data.screen[0].Z = 0.6f;
        data.dynamicFlags[0] = 0;
        QString error = QStringLiteral("old");
        QVERIFY2(candidate->IsCompatibleForKeyformUpdate(*source, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(candidate->HasFiniteDrawableState(&error));
        QVERIFY(candidate->CopyRuntimeStateFrom(*source, &error));
        QCOMPARE(data.positions[0][0].X, 100.0f);
        QCOMPARE(data.values[0], 0.0f);
        QCOMPARE(data.drawableOpacities[0], 0.4f);
        QCOMPARE(data.drawOrders[0], 99);
        QCOMPARE(data.updates, 0);
    }

    void clippingPolicyAllowsOnlyClippingChangesAndCopiesRuntimeState()
    {
        auto source = create(false, 3);
        auto candidate = create(false, 3);
        auto& old = State(source->GetModel());
        auto& next = State(candidate->GetModel());
        old.maskCounts.fill(0);
        next.maskCounts = {0, 0, 2};
        next.masks[2] = {0, 1};
        next.flags[2] ^= Core::csmIsInvertedMask;
        source->SetParameterValue(0, 0.3f);
        source->SaveParameters();
        source->SetParameterValue(0, 0.7f);
        source->SetPartOpacity(0, 0.4f);
        source->SetModelOpacity(0.6f);
        QString error;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, &error));
        QVERIFY(!candidate->CopyRuntimeStateFrom(*source, &error));
        QCOMPARE(candidate->GetParameterValue(0), 0.0f);
        QVERIFY2(candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowClippingEdits, &error), qPrintable(error));
        QVERIFY(candidate->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowClippingEdits, &error));
        QCOMPARE(candidate->GetParameterValue(0), 0.7f);
        QCOMPARE(candidate->GetPartOpacity(0), 0.4f);
        QCOMPARE(candidate->GetModelOpacity(), 0.6f);
        candidate->LoadParameters();
        QCOMPARE(candidate->GetParameterValue(0), 0.3f);
        QCOMPARE(next.maskCounts[2], 2);
        QVERIFY(candidate->GetDrawableInvertedMask(2));
        QCOMPARE(old.maskCounts[2], 0);
        QVERIFY(!source->GetDrawableInvertedMask(2));
        QCOMPARE(next.updates, 0);
        next.flags[2] ^= Core::csmIsDoubleSided;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowClippingEdits));
        next.flags[2] ^= Core::csmIsDoubleSided;
        next.flags[2] ^= 0x80;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowClippingEdits));
    }

    void invalidClippingCandidatesKeepRuntimeValues_data()
    {
        QTest::addColumn<QString>("invalid");
        for (const char* value : {"negative", "range", "null-list", "null-array", "null-counts", "negative-count", "large-count"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }

    void invalidClippingCandidatesKeepRuntimeValues()
    {
        QFETCH(QString, invalid);
        auto source = create(false, 3), candidate = create(false, 3);
        auto& data = State(candidate->GetModel());
        data.maskCounts[2] = 2;
        data.masks[2] = {0, 1};
        source->SetParameterValue(0, 0.8f);
        candidate->SetParameterValue(0, -0.5f);
        if (invalid == "negative") data.masks[2][1] = -1;
        if (invalid == "range") data.masks[2][1] = 3;
        if (invalid == "null-list") data.maskPointers[2] = nullptr;
        if (invalid == "null-array") data.nullGetters.insert("csmGetDrawableMasks");
        if (invalid == "null-counts") data.nullGetters.insert("csmGetDrawableMaskCounts");
        if (invalid == "negative-count") data.maskCounts[2] = -1;
        if (invalid == "large-count") data.maskCounts[2] = std::numeric_limits<int>::max();
        QString error;
        QVERIFY(!candidate->HasValidClippingDefinitions(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!candidate->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowClippingEdits, &error));
        QCOMPARE(candidate->GetParameterValue(0), -0.5f);
        QCOMPARE(source->GetParameterValue(0), 0.8f);
    }

    void existingMaskListsCanBeRemovedAndRestored_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("self") << 0;
        QTest::newRow("duplicate") << 1;
        QTest::newRow("duplicates-beyond-drawable-count") << 2;
    }

    void existingMaskListsCanBeRemovedAndRestored()
    {
        QFETCH(int, kind);
        const std::array<int, 4> repeated{0, 0, 0, 0};
        auto source = create(false, 3), cleared = create(false, 3), restored = create(false, 3);
        auto& before = State(source->GetModel());
        auto& after = State(restored->GetModel());
        const int count = kind == 0 ? 1 : kind == 1 ? 2 : 4;
        before.maskCounts[2] = after.maskCounts[2] = count;
        before.masks[2] = after.masks[2] = kind == 0 ? std::array<int, 2>{2, 0} : std::array<int, 2>{0, 0};
        if (kind == 2) before.maskPointers[2] = after.maskPointers[2] = repeated.data();
        source->SetParameterValue(0, 0.3f);
        source->SaveParameters();
        source->SetParameterValue(0, 0.8f);
        QVERIFY(restored->HasValidClippingDefinitions());
        QCOMPARE(restored->IsCompatibleForKeyformUpdate(*source), kind != 2);
        QVERIFY(!cleared->CopyRuntimeStateFrom(*source));
        QVERIFY(cleared->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowClippingEdits));
        QCOMPARE(cleared->GetParameterValue(0), 0.8f);
        QCOMPARE(cleared->GetDrawableMaskCounts()[2], 0);
        QCOMPARE(source->GetDrawableMaskCounts()[2], count);
        QVERIFY(!restored->CopyRuntimeStateFrom(*cleared));
        QVERIFY(restored->CopyRuntimeStateFrom(*cleared, CubismModelUpdatePolicy::AllowClippingEdits));
        QCOMPARE(restored->GetDrawableMaskCounts()[2], count);
        for (int index = 0; index < count; ++index)
            QCOMPARE(restored->GetDrawableMasks()[2][index], source->GetDrawableMasks()[2][index]);
        restored->LoadParameters();
        QCOMPARE(restored->GetParameterValue(0), 0.3f);
        after.flags[2] ^= Core::csmIsInvertedMask;
        QVERIFY(!restored->IsCompatibleForKeyformUpdate(*source));
        QVERIFY(restored->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowClippingEdits));
        QCOMPARE(restored->GetParameterValue(0), 0.8f);
    }

    void validMutualMaskReferencesAreNotTreatedAsRecursiveDependencies()
    {
        auto source = create(), candidate = create();
        auto& data = State(candidate->GetModel());
        data.maskCounts = {1, 1};
        data.masks[0][0] = 1;
        data.masks[1][0] = 0;
        QVERIFY(candidate->HasValidClippingDefinitions());
        QVERIFY(candidate->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowClippingEdits));
        QVERIFY(!candidate->CopyRuntimeStateFrom(*source, static_cast<CubismModelUpdatePolicy>(99)));
    }

    void incompatibleDefinitions_data()
    {
        QTest::addColumn<int>("mutation");
        const QStringList names{QStringLiteral("parameter-count"), QStringLiteral("part-count"), QStringLiteral("drawable-count"),
            QStringLiteral("parameter-id"), QStringLiteral("part-id"), QStringLiteral("drawable-id"),
            QStringLiteral("minimum"), QStringLiteral("maximum"), QStringLiteral("default"), QStringLiteral("type"),
            QStringLiteral("key-count"), QStringLiteral("key-value"), QStringLiteral("canvas-width"), QStringLiteral("canvas-height"),
            QStringLiteral("canvas-origin-x"), QStringLiteral("canvas-origin-y"), QStringLiteral("canvas-ppu"),
            QStringLiteral("part-parent"), QStringLiteral("drawable-parent"), QStringLiteral("texture"), QStringLiteral("flags"),
            QStringLiteral("vertex-count"), QStringLiteral("index-count"), QStringLiteral("index-value"), QStringLiteral("uv-x"),
            QStringLiteral("uv-y"), QStringLiteral("mask-count"), QStringLiteral("mask-value"), QStringLiteral("negative-count"),
            QStringLiteral("huge-count"), QStringLiteral("negative-keys"), QStringLiteral("huge-indices"),
            QStringLiteral("nan-uv"), QStringLiteral("nan-default"), QStringLiteral("nan-key"), QStringLiteral("nan-origin"),
            QStringLiteral("null-keys"), QStringLiteral("null-uvs"), QStringLiteral("null-indices"), QStringLiteral("null-masks")};
        for (int i = 0; i < names.size(); ++i) QTest::newRow(qPrintable(names[i])) << i;
    }

    void incompatibleDefinitions()
    {
        QFETCH(int, mutation);
        auto source = create();
        auto candidate = create();
        auto& data = State(candidate->GetModel());
        const float nan = std::numeric_limits<float>::quiet_NaN();
        switch (mutation) {
        case 0: data.parameterCount = 1; break;
        case 1: data.partCount = 1; break;
        case 2: data.drawableCount = 1; break;
        case 3: std::swap(data.parameterIds[0], data.parameterIds[1]); break;
        case 4: std::swap(data.partIds[0], data.partIds[1]); break;
        case 5: std::swap(data.drawableIds[0], data.drawableIds[1]); break;
        case 6: data.minimum[0] = -2; break;
        case 7: data.maximum[0] = 2; break;
        case 8: data.defaults[0] = 0.5f; break;
        case 9: data.types[0] = Core::csmParameterType_BlendShape; break;
        case 10: data.keyCounts[0] = 2; break;
        case 11: data.keys[0][1] = 0.5f; break;
        case 12: data.size.X += 1; break;
        case 13: data.size.Y += 1; break;
        case 14: data.origin.X += 1; break;
        case 15: data.origin.Y += 1; break;
        case 16: data.ppu += 1; break;
        case 17: data.partParents[1] = -1; break;
        case 18: data.drawableParents[1] = 0; break;
        case 19: data.textures[1] = 0; break;
        case 20: data.flags[1] ^= Core::csmIsDoubleSided; break;
        case 21: data.vertexCounts[0] = 3; break;
        case 22: data.indexCounts[0] = 3; break;
        case 23: data.indices[0][2] = 3; break;
        case 24: data.uvs[0][0].X = 0.25f; break;
        case 25: data.uvs[0][0].Y = 0.25f; break;
        case 26: data.maskCounts[1] = 0; break;
        case 27: data.masks[1][0] = 1; break;
        case 28: data.drawableCount = -1; break;
        case 29: data.parameterCount = std::numeric_limits<int>::max(); break;
        case 30: data.keyCounts[0] = -1; break;
        case 31: data.indexCounts[0] = std::numeric_limits<int>::max(); break;
        case 32: data.uvs[0][0].X = nan; break;
        case 33: data.defaults[0] = nan; break;
        case 34: data.keys[0][0] = nan; break;
        case 35: data.origin.X = nan; break;
        case 36: data.keyPointers[0] = nullptr; break;
        case 37: data.uvPointers[0] = nullptr; break;
        case 38: data.indexPointers[0] = nullptr; break;
        case 39: data.maskPointers[1] = nullptr; break;
        }
        QString error;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, &error));
        QVERIFY(!error.isEmpty());
        const auto values = data.values;
        QVERIFY(!candidate->CopyRuntimeStateFrom(*source, &error));
        if (mutation != 26 && mutation != 27)
            QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowClippingEdits, &error));
        QCOMPARE(data.values, values);
        QCOMPARE(State(source->GetModel()).values[0], 0.0f);
    }

    void rawTopologyIsComparedBeforeOverrides()
    {
        auto source = create();
        auto candidate = create();
        const QVector<quint16> changed{0, 1, 3, 1, 2, 3};
        QVERIFY(source->_drawableIndexOverrides.Set(0, 4, changed));
        QCOMPARE(source->GetDrawableVertexIndices(0)[2], quint16(3));
        QVERIFY(candidate->IsCompatibleForKeyformUpdate(*source));
        auto& data = State(candidate->GetModel());
        std::copy(changed.cbegin(), changed.cend(), data.indices[0].begin());
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source));
    }

    void identicalInvalidDefinitionsAreRejected_data()
    {
        QTest::addColumn<int>("mutation");
        const QStringList names{QStringLiteral("default-outside-range"), QStringLiteral("invalid-parent"),
            QStringLiteral("invalid-mask"), QStringLiteral("invalid-index"), QStringLiteral("nontriangle-indices"),
            QStringLiteral("zero-ppu"), QStringLiteral("negative-canvas"), QStringLiteral("negative-texture"),
            QStringLiteral("oversized-key-count"), QStringLiteral("missing-identifiers"),
            QStringLiteral("duplicate-parameter-id"), QStringLiteral("duplicate-part-id"),
            QStringLiteral("duplicate-drawable-id")};
        for (int i = 0; i < names.size(); ++i) QTest::newRow(qPrintable(names[i])) << i;
    }

    void identicalInvalidDefinitionsAreRejected()
    {
        QFETCH(int, mutation);
        auto source = create();
        auto candidate = create();
        for (auto* model : {source.get(), candidate.get()}) {
            auto& data = State(model->GetModel());
            switch (mutation) {
            case 0: data.defaults[0] = 5; break;
            case 1: data.partParents[1] = 1; break;
            case 2: data.masks[1][0] = 2; break;
            case 3: data.indices[1][5] = 4; break;
            case 4: data.indexCounts[1] = 5; break;
            case 5: data.ppu = 0; break;
            case 6: data.size.X = -1; break;
            case 7: data.textures[1] = -1; break;
            case 8: data.keyCounts[1] = std::numeric_limits<int>::max(); break;
            case 9: data.parameterIds[0] = nullptr; break;
            case 10: data.parameterIds[1] = data.parameterIds[0]; break;
            case 11: data.partIds[1] = data.partIds[0]; break;
            case 12: data.drawableIds[1] = data.drawableIds[0]; break;
            }
        }
        QString error;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, &error));
        QVERIFY(!error.isEmpty());
    }

    void sourceGeometryMayBeRepairedByCandidate()
    {
        auto source = create();
        auto candidate = create();
        State(source->GetModel()).positions[1][3].X = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!source->HasFiniteDrawableState());
        QVERIFY(candidate->IsCompatibleForKeyformUpdate(*source));
        QVERIFY(candidate->CopyRuntimeStateFrom(*source));
        candidate->Update();
        QVERIFY(candidate->HasFiniteDrawableState());
        QCOMPARE(State(candidate->GetModel()).updates, 1);
    }

    void copiesRawStateAndKeepsCoreAndMeshOwnership()
    {
        auto source = create();
        auto candidate = create();
        source->SetParameterValue(0, 0.2f);
        source->SetParameterValue(1, 0.3f);
        source->SaveParameters();
        source->SetParameterValue(0, 0.7f);
        source->SetParameterValue(1, 0.8f);
        source->SetPartOpacity(0, 0.4f);
        source->SetPartOpacity(1, 0.6f);
        source->SetModelOpacity(0.9f);
        source->SetMultiplyColor(0, 0.1f, 0.2f, 0.3f, 0.4f);
        source->SetScreenColor(1, 0.5f, 0.6f, 0.7f, 0.8f);
        source->SetPartMultiplyColor(1, 0.9f, 0.8f, 0.7f, 0.6f);
        source->SetPartScreenColor(0, 0.4f, 0.3f, 0.2f, 0.1f);
        source->SetOverwriteFlagForDrawableMultiplyColors(1, true);
        source->SetOverwriteFlagForDrawableScreenColors(0, true);
        source->SetOverwriteFlagForModelScreenColors(true);
        source->SetOverwriteFlagForModelCullings(true);
        source->SetDrawableCulling(0, 1);
        source->SetDrawableCulling(1, 0);
        source->SetOverwriteFlagForDrawableCullings(1, true);
        const auto virtualParameter = idManager->GetId("VirtualParameter");
        const auto virtualPart = idManager->GetId("VirtualPart");
        const auto secondVirtualParameter = idManager->GetId("AnotherVirtualParameter");
        const auto secondVirtualPart = idManager->GetId("AnotherVirtualPart");
        QCOMPARE(source->GetParameterIndex(virtualParameter), 2);
        QCOMPARE(source->GetParameterIndex(secondVirtualParameter), 3);
        QCOMPARE(source->GetPartIndex(virtualPart), 2);
        QCOMPARE(source->GetPartIndex(secondVirtualPart), 3);
        source->SetParameterValue(virtualParameter, 4.5f);
        source->SetParameterValue(secondVirtualParameter, -2.5f);
        source->SetPartOpacity(virtualPart, 0.15f);
        source->SetPartOpacity(secondVirtualPart, 0.25f);
        const QVector<quint16> changed{0, 1, 3, 1, 2, 3};
        QVERIFY(source->_drawableIndexOverrides.Set(0, 4, changed));
        source->InitMesh();
        auto* sourceMesh = source->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA"));
        QVERIFY(sourceMesh);
        const auto* candidateCore = candidate->GetModel();
        const auto* candidateValues = candidate->_parameterValues;
        const auto* candidateIds = candidate->_parameterIds.GetPtr();
        const int allocationsBefore = sdkAllocationCalls;
        rejectSdkAllocation = true;
        QVERIFY(candidate->CopyRuntimeStateFrom(*source));
        rejectSdkAllocation = false;
        QCOMPARE(sdkAllocationCalls, allocationsBefore);
        QCOMPARE(candidate->GetModel(), candidateCore);
        QCOMPARE(candidate->_parameterValues, candidateValues);
        QCOMPARE(candidate->_parameterIds.GetPtr(), candidateIds);
        QVERIFY(candidate->GetMeshMap()->GetMapMesh().isEmpty());
        QCOMPARE(candidate->GetParameterValue(0), 0.7f);
        QCOMPARE(candidate->GetParameterValue(1), 0.8f);
        QCOMPARE(candidate->_savedParameters, QVector<float>({0.2f, 0.3f}));
        QCOMPARE(candidate->GetPartOpacity(0), 0.4f);
        QCOMPARE(candidate->GetPartOpacity(1), 0.6f);
        QCOMPARE(candidate->GetModelOpacity(), 0.9f);
        verifyColor(candidate->_userMultiplyColors[0].Color, 0.1f, 0.2f, 0.3f, 0.4f);
        QVERIFY(!candidate->_userMultiplyColors[0].IsOverwritten);
        QVERIFY(candidate->_userMultiplyColors[1].IsOverwritten);
        verifyColor(candidate->_userScreenColors[1].Color, 0.5f, 0.6f, 0.7f, 0.8f);
        QVERIFY(!candidate->_userScreenColors[1].IsOverwritten);
        QVERIFY(candidate->_userScreenColors[0].IsOverwritten);
        verifyColor(candidate->GetPartMultiplyColor(1), 0.9f, 0.8f, 0.7f, 0.6f);
        verifyColor(candidate->GetPartScreenColor(0), 0.4f, 0.3f, 0.2f, 0.1f);
        QVERIFY(!candidate->GetOverwriteFlagForModelMultiplyColors());
        QVERIFY(candidate->GetOverwriteFlagForModelScreenColors());
        QVERIFY(candidate->GetOverwriteFlagForModelCullings());
        QVERIFY(!candidate->GetOverwriteFlagForDrawableCullings(0));
        QVERIFY(candidate->GetOverwriteFlagForDrawableCullings(1));
        QCOMPARE(candidate->_userCullings[0].IsCulling, 1);
        QCOMPARE(candidate->_userCullings[1].IsCulling, 0);
        QCOMPARE(candidate->GetParameterIndex(virtualParameter), 2);
        QCOMPARE(candidate->GetParameterIndex(secondVirtualParameter), 3);
        QCOMPARE(candidate->GetPartIndex(virtualPart), 2);
        QCOMPARE(candidate->GetPartIndex(secondVirtualPart), 3);
        QCOMPARE(candidate->GetParameterValue(virtualParameter), 4.5f);
        QCOMPARE(candidate->GetPartOpacity(virtualPart), 0.15f);
        QCOMPARE(candidate->GetParameterIndex(idManager->GetId("ThirdVirtualParameter")), 4);
        QCOMPARE(candidate->GetPartIndex(idManager->GetId("ThirdVirtualPart")), 4);
        QCOMPARE(source->_notExistParameterId.size(), 2);
        QCOMPARE(source->_notExistPartId.size(), 2);
        QCOMPARE(candidate->GetDrawableIndicesRevision(), source->GetDrawableIndicesRevision());
        QCOMPARE(candidate->GetDrawableVertexIndices(0)[2], quint16(3));
        candidate->LoadParameters();
        QCOMPARE(candidate->GetParameterValue(0), 0.2f);
        QCOMPARE(candidate->GetParameterValue(1), 0.3f);
        QCOMPARE(source->GetParameterValue(0), 0.7f);
        candidate->SetParameterValue(virtualParameter, 12.0f);
        candidate->SetPartOpacity(virtualPart, 0.45f);
        QCOMPARE(source->GetParameterValue(virtualParameter), 4.5f);
        QCOMPARE(source->GetPartOpacity(virtualPart), 0.15f);
        candidate->SaveParameters();
        QCOMPARE(source->_savedParameters[0], 0.2f);
        candidate->ClearDrawableIndicesOverrides();
        QVERIFY(source->HasDrawableIndicesOverrides());
        QCOMPARE(source->GetMeshMap()->GetDrawMesh(QStringLiteral("MeshA")), sourceMesh);
        source.reset();
        QCOMPARE(candidate->GetParameterValue(virtualParameter), 12.0f);
        QCOMPARE(candidate->GetPartOpacity(virtualPart), 0.45f);
    }

    void copiesPartFlagsWithoutPropagatingOverRawDrawableColors()
    {
        auto source = create();
        auto candidate = create();
        source->SetPartMultiplyColor(0, 0.2f, 0.3f, 0.4f, 0.5f);
        source->SetPartScreenColor(1, 0.6f, 0.7f, 0.8f, 0.9f);
        source->SetOverwriteColorForPartMultiplyColors(0, true);
        source->SetOverwriteColorForPartScreenColors(1, true);
        source->SetMultiplyColor(0, 0.9f, 0.8f, 0.7f, 0.6f);
        source->SetScreenColor(1, 0.4f, 0.3f, 0.2f, 0.1f);
        source->SetOverwriteFlagForDrawableMultiplyColors(0, false);
        QVERIFY(candidate->CopyRuntimeStateFrom(*source));
        QVERIFY(candidate->GetOverwriteColorForPartMultiplyColors(0));
        QVERIFY(candidate->GetOverwriteColorForPartScreenColors(1));
        QVERIFY(!candidate->GetOverwriteFlagForDrawableMultiplyColors(0));
        verifyColor(candidate->_userMultiplyColors[0].Color, 0.9f, 0.8f, 0.7f, 0.6f);
        verifyColor(candidate->_userScreenColors[1].Color, 0.4f, 0.3f, 0.2f, 0.1f);
        candidate->SetPartMultiplyColor(0, 1, 0, 0, 1);
        verifyColor(source->_userMultiplyColors[0].Color, 0.9f, 0.8f, 0.7f, 0.6f);
    }

    void savedParameterLengthAndSelfCopyRemainExact()
    {
        auto source = create();
        auto candidate = create();
        candidate->SaveParameters();
        QVERIFY(candidate->CopyRuntimeStateFrom(*source));
        QVERIFY(candidate->_savedParameters.isEmpty());
        source->_savedParameters = {0.33f};
        source->SetParameterValue(0, 0.55f);
        source->SetParameterValue(1, 0.66f);
        QVERIFY(candidate->CopyRuntimeStateFrom(*source));
        candidate->LoadParameters();
        QCOMPARE(candidate->GetParameterValue(0), 0.33f);
        QCOMPARE(candidate->GetParameterValue(1), 0.66f);
        QVERIFY(candidate->CopyRuntimeStateFrom(*candidate));
        QCOMPARE(candidate->_savedParameters, QVector<float>({0.33f}));
        candidate->SaveParameters();
        QCOMPARE(candidate->_savedParameters, QVector<float>({0.33f, 0.66f}));
        QCOMPARE(source->_savedParameters, QVector<float>({0.33f}));
    }

    void invalidRuntimeStateIsRejectedAtomically_data()
    {
        QTest::addColumn<int>("mutation");
        const QStringList names{QStringLiteral("current"), QStringLiteral("saved"), QStringLiteral("part-opacity"),
            QStringLiteral("model-opacity"), QStringLiteral("multiply"), QStringLiteral("screen"),
            QStringLiteral("part-multiply"), QStringLiteral("part-screen"), QStringLiteral("virtual-value"),
            QStringLiteral("virtual-opacity"), QStringLiteral("virtual-index"), QStringLiteral("saved-length"),
            QStringLiteral("override-size"), QStringLiteral("virtual-collision")};
        for (int i = 0; i < names.size(); ++i) QTest::newRow(qPrintable(names[i])) << i;
    }

    void invalidRuntimeStateIsRejectedAtomically()
    {
        QFETCH(int, mutation);
        auto source = create();
        auto candidate = create();
        source->SaveParameters();
        const auto id = idManager->GetId("Virtual");
        source->SetParameterValue(id, 2.5f);
        source->SetPartOpacity(id, 0.5f);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        switch (mutation) {
        case 0: source->_parameterValues[1] = nan; break;
        case 1: source->_savedParameters[1] = nan; break;
        case 2: source->_partOpacities[1] = nan; break;
        case 3: source->SetModelOpacity(nan); break;
        case 4: source->SetMultiplyColor(1, nan, 0, 0, 1); break;
        case 5: source->SetScreenColor(1, 0, nan, 0, 1); break;
        case 6: source->SetPartMultiplyColor(1, 0, 0, nan, 1); break;
        case 7: source->SetPartScreenColor(1, 0, 0, 0, nan); break;
        case 8: source->SetParameterValue(id, nan); break;
        case 9: source->SetPartOpacity(id, nan); break;
        case 10: source->_notExistParameterId[id] = -1; break;
        case 11: source->_savedParameters.append(0); break;
        case 12: source->_userScreenColors.Clear(); break;
        case 13: {
            const auto other = idManager->GetId("OtherVirtual");
            source->SetParameterValue(other, 3.5f);
            source->_notExistParameterId[other] = source->_notExistParameterId[id];
            break;
        }
        }
        candidate->SetParameterValue(0, -0.5f);
        candidate->SaveParameters();
        candidate->SetModelOpacity(0.75f);
        candidate->SetParameterValue(id, -3.5f);
        const auto saved = candidate->_savedParameters;
        const auto virtuals = candidate->_notExistParameterValues;
        QString error;
        QVERIFY(!candidate->CopyRuntimeStateFrom(*source, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(candidate->GetParameterValue(0), -0.5f);
        QCOMPARE(candidate->GetModelOpacity(), 0.75f);
        QCOMPARE(candidate->_savedParameters, saved);
        QCOMPARE(candidate->_notExistParameterValues, virtuals);
        QVERIFY(!candidate->HasDrawableIndicesOverrides());
    }

    void finiteDrawableState_data()
    {
        QTest::addColumn<int>("mutation");
        for (int i = 0; i < 10; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }

    void finiteDrawableState()
    {
        QFETCH(int, mutation);
        auto model = create();
        QVERIFY(model->HasFiniteDrawableState());
        auto& data = State(model->GetModel());
        const float bad = mutation % 2 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
        switch (mutation) {
        case 0: data.positions[1][3].X = bad; break;
        case 1: data.positions[1][3].Y = bad; break;
        case 2: data.uvs[1][3].X = bad; break;
        case 3: data.uvs[1][3].Y = bad; break;
        case 4: data.drawableOpacities[1] = bad; break;
        case 5: data.multiply[1].W = bad; break;
        case 6: data.screen[1].Z = bad; break;
        case 7: data.positionPointers[1] = nullptr; break;
        case 8: data.vertexCounts[1] = -1; break;
        case 9: data.vertexCounts[1] = std::numeric_limits<int>::max(); break;
        }
        QString error;
        QVERIFY(!model->HasFiniteDrawableState(&error));
        QVERIFY(!error.isEmpty());
    }

    void missingCoreArraysAreRejected_data()
    {
        QTest::addColumn<QString>("getter");
        QTest::addColumn<bool>("staticDefinition");
        const QStringList definitionGetters{QStringLiteral("csmGetParameterIds"), QStringLiteral("csmGetPartIds"),
            QStringLiteral("csmGetDrawableIds"), QStringLiteral("csmGetParameterTypes"),
            QStringLiteral("csmGetParameterMinimumValues"), QStringLiteral("csmGetParameterMaximumValues"),
            QStringLiteral("csmGetParameterDefaultValues"), QStringLiteral("csmGetParameterKeyCounts"),
            QStringLiteral("csmGetParameterKeyValues"), QStringLiteral("csmGetPartParentPartIndices"),
            QStringLiteral("csmGetDrawableParentPartIndices"), QStringLiteral("csmGetDrawableVertexCounts"),
            QStringLiteral("csmGetDrawableIndexCounts"), QStringLiteral("csmGetDrawableMaskCounts"),
            QStringLiteral("csmGetDrawableTextureIndices"), QStringLiteral("csmGetDrawableConstantFlags"),
            QStringLiteral("csmGetDrawableVertexUvs"), QStringLiteral("csmGetDrawableIndices"),
            QStringLiteral("csmGetDrawableMasks")};
        for (const auto& getter : definitionGetters) QTest::newRow(qPrintable(getter)) << getter << true;
        const QStringList dynamicGetters{QStringLiteral("csmGetDrawableVertexPositions"),
            QStringLiteral("csmGetDrawableOpacities"), QStringLiteral("csmGetDrawableMultiplyColors"),
            QStringLiteral("csmGetDrawableScreenColors")};
        for (const auto& getter : dynamicGetters) QTest::newRow(qPrintable(getter)) << getter << false;
    }

    void missingCoreArraysAreRejected()
    {
        QFETCH(QString, getter);
        QFETCH(bool, staticDefinition);
        auto source = create();
        auto candidate = create();
        State(candidate->GetModel()).nullGetters.insert(getter);
        QString error;
        if (staticDefinition) QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, &error));
        else QVERIFY(!candidate->HasFiniteDrawableState(&error));
        QVERIFY(!error.isEmpty());
    }

    void emptyModelsAndZeroLengthArraysAreSafe()
    {
        auto source = create(true);
        auto candidate = create(true);
        auto& data = State(candidate->GetModel());
        data.nullGetters = {QStringLiteral("csmGetDrawableVertexUvs"), QStringLiteral("csmGetDrawableVertexPositions"),
                            QStringLiteral("csmGetDrawableOpacities"), QStringLiteral("csmGetDrawableMultiplyColors"),
                            QStringLiteral("csmGetDrawableScreenColors"), QStringLiteral("csmGetParameterKeyValues")};
        QVERIFY(candidate->IsCompatibleForKeyformUpdate(*source));
        QVERIFY(candidate->CopyRuntimeStateFrom(*source));
        QVERIFY(candidate->HasFiniteDrawableState());
    }

    void finiteBoundaryCoordinatesArePreserved()
    {
        auto source = create();
        auto candidate = create();
        auto& data = State(candidate->GetModel());
        data.positions[1][3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min()};
        QVERIFY(candidate->IsCompatibleForKeyformUpdate(*source));
        QVERIFY(candidate->CopyRuntimeStateFrom(*source));
        QVERIFY(candidate->HasFiniteDrawableState());
        QCOMPARE(data.positions[1][3].X, std::numeric_limits<float>::max());
        QCOMPARE(data.positions[1][3].Y, std::numeric_limits<float>::denorm_min());
    }

    void valueSnapshotOutlivesSourceAndRestoresOnlyRuntimeValues()
    {
        CubismModel::RuntimeValueSnapshot entry;
        {
            auto source = create();
            source->SetParameterValue(0, 0.2f);
            source->SetParameterValue(1, 0.3f);
            source->SaveParameters();
            source->SetParameterValue(0, 0.7f);
            source->SetParameterValue(1, 0.8f);
            source->SetPartOpacity(0, 0.4f);
            source->SetPartOpacity(1, 0.6f);
            source->SetModelOpacity(0.45f);
            source->SetParameterValue(idManager->GetId("VirtualParameter"), 2.5f);
            source->SetPartOpacity(idManager->GetId("VirtualPart"), 0.15f);
            QVERIFY(source->CaptureRuntimeValues(entry));
            QCOMPARE(entry.Parameters(), (QMap<QString, float>{{"ParamA", 0.7f}, {"ParamB", 0.8f}}));
            QCOMPARE(entry.savedParameters, QVector<float>({0.2f, 0.3f}));
        }
        QVERIFY(coreStates.empty());
        auto candidate = create();
        auto& data = State(candidate->GetModel());
        data.positions[0][0] = {123, 456};
        candidate->SetMultiplyColor(0, 0.1f, 0.2f, 0.3f, 0.4f);
        candidate->SetParameterValue(idManager->GetId("ExtraVirtual"), 99);
        candidate->SetPartOpacity(idManager->GetId("ExtraVirtualPart"), 0.8f);
        QVERIFY(candidate->RestoreRuntimeValues(entry));
        QCOMPARE(candidate->GetParameterValue(0), 0.7f);
        QCOMPARE(candidate->GetParameterValue(1), 0.8f);
        QCOMPARE(candidate->_savedParameters, QVector<float>({0.2f, 0.3f}));
        QCOMPARE(candidate->GetParameterValue(idManager->GetId("VirtualParameter")), 2.5f);
        QCOMPARE(candidate->GetPartOpacity(idManager->GetId("VirtualPart")), 0.15f);
        QCOMPARE(candidate->_notExistParameterId.size(), 1);
        QCOMPARE(candidate->_notExistPartId.size(), 1);
        QCOMPARE(candidate->GetPartOpacity(0), 0.4f);
        QCOMPARE(candidate->GetPartOpacity(1), 0.6f);
        QCOMPARE(candidate->GetModelOpacity(), 0.45f);
        QCOMPARE(data.positions[0][0].X, 123.0f);
        QCOMPARE(data.positions[0][0].Y, 456.0f);
        verifyColor(candidate->_userMultiplyColors[0].Color, 0.1f, 0.2f, 0.3f, 0.4f);
        QCOMPARE(data.updates, 0);
        candidate->LoadParameters();
        QCOMPARE(candidate->GetParameterValue(0), 0.2f);
        QCOMPARE(candidate->GetParameterValue(1), 0.3f);
        QCOMPARE(entry.Parameters().value("ParamA"), 0.7f);
    }

    void exactPoseRequiresAllRealParametersAndNeverSaves()
    {
        auto model = create();
        model->SetParameterValue(0, 0.2f);
        model->SetParameterValue(1, 0.3f);
        model->SaveParameters();
        const auto virtualId = idManager->GetId("VirtualParameter");
        model->SetParameterValue(virtualId, 7.5f);
        QVERIFY(model->ApplyParameterPose({{"ParamA", -1.0f}, {"ParamB", 2.0f}}));
        QCOMPARE(model->GetParameterValue(0), -1.0f);
        QCOMPARE(model->GetParameterValue(1), 2.0f);
        QCOMPARE(model->GetParameterValue(virtualId), 7.5f);
        QCOMPARE(model->_savedParameters, QVector<float>({0.2f, 0.3f}));
        const QVector<QMap<QString, float>> invalid{
            {}, {{"ParamA", 0}}, {{"ParamA", 0}, {"Unknown", 0}},
            {{"ParamA", 0}, {"ParamB", 0}, {"VirtualParameter", 0}},
            {{"ParamA", -1.01f}, {"ParamB", 0}}, {{"ParamA", 0}, {"ParamB", 2.01f}},
            {{"ParamA", 0}, {"ParamB", std::numeric_limits<float>::quiet_NaN()}},
            {{"ParamA", std::numeric_limits<float>::infinity()}, {"ParamB", 0}}
        };
        for (const auto& pose : invalid) {
            QString error;
            QVERIFY(!model->ApplyParameterPose(pose, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(model->GetParameterValue(0), -1.0f);
            QCOMPARE(model->GetParameterValue(1), 2.0f);
            QCOMPARE(model->_savedParameters, QVector<float>({0.2f, 0.3f}));
        }
        model->LoadParameters();
        QCOMPARE(model->GetParameterValue(0), 0.2f);
        QCOMPARE(model->GetParameterValue(1), 0.3f);
        QCOMPARE(State(model->GetModel()).updates, 0);
    }

    void valueSnapshotPreservesSavedPrefixAndFiniteOvershoot()
    {
        auto model = create();
        State(model->GetModel()).values = {5.0f, -8.0f};
        model->_savedParameters = {4.0f};
        CubismModel::RuntimeValueSnapshot snapshot;
        QVERIFY(model->CaptureRuntimeValues(snapshot));
        QVERIFY(model->ApplyParameterPose({{"ParamA", 0}, {"ParamB", 0}}));
        QVERIFY(model->RestoreRuntimeValues(snapshot));
        QCOMPARE(model->GetParameterValue(0), 5.0f);
        QCOMPARE(model->GetParameterValue(1), -8.0f);
        QCOMPARE(model->_savedParameters, QVector<float>({4.0f}));
        model->LoadParameters();
        QCOMPARE(model->GetParameterValue(0), 4.0f);
        QCOMPARE(model->GetParameterValue(1), -8.0f);
    }

    void invalidSnapshotRestoreIsAtomic_data()
    {
        QTest::addColumn<int>("failure");
        for (int failure = 0; failure < 12; ++failure)
            QTest::newRow(qPrintable(QString::number(failure))) << failure;
    }

    void invalidSnapshotRestoreIsAtomic()
    {
        QFETCH(int, failure);
        auto model = create();
        model->SaveParameters();
        CubismModel::RuntimeValueSnapshot snapshot;
        QVERIFY(model->CaptureRuntimeValues(snapshot));
        const float nan = std::numeric_limits<float>::quiet_NaN();
        switch (failure) {
        case 0: snapshot.valid = false; break;
        case 1: snapshot.parameterIds.swapItemsAt(0, 1); break;
        case 2: snapshot.partIds[0] = "Other"; break;
        case 3: snapshot.minimums[0] = -99; break;
        case 4: snapshot.parameterValues[1] = nan; break;
        case 5: snapshot.partOpacities[1] = nan; break;
        case 6: snapshot.savedParameters.append(0); break;
        case 7: snapshot.modelOpacity = nan; break;
        case 8: snapshot.virtualParameterIds.insert("Extra", 99); snapshot.virtualParameterValues.insert(99, 1); break;
        case 9: snapshot.virtualParameterIds.insert("ParamA", 2); snapshot.virtualParameterValues.insert(2, 1); break;
        case 10: snapshot.virtualPartIds.insert("Extra", 2); snapshot.virtualPartOpacities.insert(2, nan); break;
        case 11: snapshot.parameterValues.clear(); break;
        }
        const auto values = State(model->GetModel()).values;
        const auto parts = State(model->GetModel()).partOpacities;
        const auto saved = model->_savedParameters;
        const auto opacity = model->GetModelOpacity();
        QString error;
        QVERIFY(!model->RestoreRuntimeValues(snapshot, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(State(model->GetModel()).values == values);
        QVERIFY(State(model->GetModel()).partOpacities == parts);
        QCOMPARE(model->_savedParameters, saved);
        QCOMPARE(model->GetModelOpacity(), opacity);
        QVERIFY(model->_notExistParameterId.isEmpty());
        QVERIFY(model->_notExistPartId.isEmpty());
    }

    void parameterDefinitionPolicyIsExplicitAndComposable()
    {
        auto source = create();
        auto candidate = create();
        auto& next = State(candidate->GetModel());
        next.minimum[0] = -2.0f;
        next.maximum[0] = 3.0f;
        next.defaults[0] = 0.5f;
        QString error;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, &error));
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowClippingEdits, &error));
        QVERIFY2(candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowParameterDefinitionEdits, &error), qPrintable(error));
        next.flags[0] ^= Core::csmIsInvertedMask;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, CubismModelUpdatePolicy::AllowParameterDefinitionEdits, &error));
        const auto combined = CubismModelUpdatePolicy::AllowParameterDefinitionEdits | CubismModelUpdatePolicy::AllowClippingEdits;
        QVERIFY(candidate->IsCompatibleForKeyformUpdate(*source, combined, &error));
        next.types[0] = Core::csmParameterType_BlendShape;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, combined, &error));
        next.types[0] = Core::csmParameterType_Normal;
        next.keys[0][1] = 0.2f;
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, combined, &error));
        next.keys[0][1] = 0.0f;
        next.minimum[0] = -0.5f;
        QVERIFY(candidate->IsCompatibleForKeyformUpdate(*source, combined, &error));
        QCOMPARE(next.keys[0][0], -1.0f);
        next.minimum[0] = -2.0f;
        next.maximum[0] = std::numeric_limits<float>::infinity();
        QVERIFY(!candidate->IsCompatibleForKeyformUpdate(*source, combined, &error));
    }

    void narrowedRangeKeepsOutlyingBindingKeys()
    {
        auto source = create(), candidate = create();
        auto& old = State(source->GetModel());
        auto& next = State(candidate->GetModel());
        old.minimum[0] = -30.0f;
        old.maximum[0] = 30.0f;
        old.keys[0] = {{-30.0f, 0.0f, 30.0f}};
        next.minimum[0] = -5.0f;
        next.maximum[0] = 5.0f;
        next.keys[0] = old.keys[0];
        source->SetParameterValue(0, 25.0f);
        source->SaveParameters();
        source->SetParameterValue(0, -26.0f);
        QString error;
        QVERIFY2(candidate->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowParameterDefinitionEdits, &error), qPrintable(error));
        QCOMPARE(candidate->GetParameterValue(0), -5.0f);
        QCOMPARE(candidate->_savedParameters[0], 5.0f);
        QCOMPARE(next.keys[0][0], -30.0f);
        QCOMPARE(next.keys[0][1], 0.0f);
        QCOMPARE(next.keys[0][2], 30.0f);
    }

    void changedRangesClampCurrentAndSavedSeparately()
    {
        auto source = create();
        auto candidate = create();
        auto& previous = State(source->GetModel());
        previous.minimum[0] = -2.0f;
        previous.maximum[0] = 2.0f;
        previous.values = {{1.75f, -0.0f}};
        source->_savedParameters = {-1.5f, 3.5f};
        previous.partOpacities[0] = 0.37f;
        source->SetModelOpacity(0.42f);
        source->SetParameterValue(idManager->GetId("VirtualRange"), 5.0f);
        QString error;
        QVERIFY2(candidate->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowParameterDefinitionEdits, &error), qPrintable(error));
        QCOMPARE(candidate->GetParameterValue(0), 1.0f);
        QVERIFY(std::signbit(candidate->GetParameterValue(1)));
        QCOMPARE(candidate->_savedParameters, QVector<float>({-1.0f, 3.5f}));
        QCOMPARE(candidate->GetPartOpacity(0), 0.37f);
        QCOMPARE(candidate->GetModelOpacity(), 0.42f);
        QCOMPARE(candidate->GetParameterValue(idManager->GetId("VirtualRange")), 5.0f);
        QCOMPARE(previous.values[0], 1.75f);
        QCOMPARE(source->_savedParameters[0], -1.5f);
        auto defaultOnly = create();
        State(defaultOnly->GetModel()).minimum[0] = -2.0f;
        State(defaultOnly->GetModel()).maximum[0] = 2.0f;
        State(defaultOnly->GetModel()).defaults[0] = 1.0f;
        QVERIFY(defaultOnly->CopyRuntimeStateFrom(*source, CubismModelUpdatePolicy::AllowParameterDefinitionEdits));
        QCOMPARE(defaultOnly->GetParameterValue(0), 1.75f);
        QCOMPARE(defaultOnly->_savedParameters[0], -1.5f);
        QVERIFY(std::signbit(defaultOnly->GetParameterValue(1)));
    }

    void runtimeSnapshotCanRebaseWithoutLosingIdentityOrUnchangedValues()
    {
        auto source = create();
        auto candidate = create();
        State(source->GetModel()).minimum[0] = -2.0f;
        State(source->GetModel()).maximum[0] = 2.0f;
        State(source->GetModel()).values = {{1.5f, -0.0f}};
        source->_savedParameters = {-1.5f, 4.0f};
        CubismModel::RuntimeValueSnapshot original, migrated;
        QVERIFY(source->CaptureRuntimeValues(original));
        QVERIFY(candidate->RebaseRuntimeValues(original, migrated));
        QCOMPARE(original.parameterValues[0], 1.5f);
        QCOMPARE(migrated.parameterValues[0], 1.0f);
        QCOMPARE(migrated.savedParameters, QVector<float>({-1.0f, 4.0f}));
        QVERIFY(std::signbit(migrated.parameterValues[1]));
        QCOMPARE(migrated.minimums, QVector<float>({-1.0f, -2.0f}));
        QCOMPARE(migrated.parameterTypes, original.parameterTypes);
        QVERIFY(!candidate->RestoreRuntimeValues(original));
        QVERIFY(candidate->RestoreRuntimeValues(migrated));
        const auto previous = migrated.Parameters();
        auto invalid = original;
        invalid.parameterTypes[0] = int(Core::csmParameterType_BlendShape);
        QVERIFY(!candidate->RebaseRuntimeValues(invalid, migrated));
        QCOMPARE(migrated.Parameters(), previous);
        QVERIFY(!candidate->RestoreRuntimeValues(invalid));
    }

    void failedCapturePreservesPreviousSnapshot()
    {
        auto model = create();
        CubismModel::RuntimeValueSnapshot snapshot;
        QVERIFY(model->CaptureRuntimeValues(snapshot));
        const auto original = snapshot.Parameters();
        State(model->GetModel()).values[1] = std::numeric_limits<float>::infinity();
        QString error;
        QVERIFY(!model->CaptureRuntimeValues(snapshot, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(snapshot.Parameters(), original);
        auto empty = create(true);
        QVERIFY(empty->ApplyParameterPose({}));
        QVERIFY(empty->CaptureRuntimeValues(snapshot));
        QVERIFY(snapshot.Parameters().isEmpty());
        QVERIFY(empty->RestoreRuntimeValues(snapshot));
    }
};

QTEST_GUILESS_MAIN(ModelRuntimeStateTests)
#include "model_runtime_state_tests.moc"
