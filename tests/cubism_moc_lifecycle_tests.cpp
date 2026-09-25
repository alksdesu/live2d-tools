#include "framework/Model/CubismMoc.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Utils/CubismDebug.hpp"

#include <QtTest>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <unordered_map>

#if !defined(CSM_DEBUG) || defined(NDEBUG)
#error CubismMoc lifecycle tests require CSM_DEBUG and enabled assertions
#endif

using namespace Live2D::Cubism::Framework;
namespace Core = Live2D::Cubism::Core;

namespace {
struct ControlledRuntime
{
    std::unordered_map<void*, std::size_t> allocations;
    bool failAlignedAllocation = false;
    bool failObjectAllocation = false;
    bool consistent = true;
    bool revive = true;
    bool initialize = true;
    bool throwOnModelInitialize = false;
    unsigned int modelSize = 128;
    int allocationCalls = 0;
    int alignedAllocationCalls = 0;
    int alignedDeallocations = 0;
    int consistencyCalls = 0;
    int reviveCalls = 0;
    int versionCalls = 0;
    int modelSizeCalls = 0;
    int modelInitializeCalls = 0;
    int wrapperInitializations = 0;
    int wrapperDestructions = 0;
    QByteArray copiedBytes;
};

ControlledRuntime runtime;
constexpr std::array<csmByte, 8> input{{'M', 'O', 'C', '3', 3, 0, 7, 255}};

void* allocate(std::size_t size, std::size_t alignment)
{
    ++runtime.allocationCalls;
    if (alignment) {
        ++runtime.alignedAllocationCalls;
        if (runtime.failAlignedAllocation) {
            runtime.failAlignedAllocation = false;
            return nullptr;
        }
    } else if (runtime.failObjectAllocation) {
        runtime.failObjectAllocation = false;
        throw std::bad_alloc();
    }
    void* memory = alignment ? ::operator new(size, std::align_val_t(alignment), std::nothrow) : std::malloc(size);
    if (!memory) throw std::bad_alloc();
    runtime.allocations.emplace(memory, alignment);
    return memory;
}

void release(void* memory, bool aligned)
{
    if (!memory) return;
    const auto found = runtime.allocations.find(memory);
    if (found == runtime.allocations.end() || (found->second != 0) != aligned)
        qFatal("Invalid allocation ownership in CubismMoc lifecycle test");
    const std::size_t alignment = found->second;
    runtime.allocations.erase(found);
    if (aligned) {
        ++runtime.alignedDeallocations;
        ::operator delete(memory, std::align_val_t(alignment));
    } else std::free(memory);
}

void checkAllocation(const void* memory, std::size_t alignment)
{
    const auto found = runtime.allocations.find(const_cast<void*>(memory));
    if (found == runtime.allocations.end() || found->second != alignment ||
        reinterpret_cast<std::uintptr_t>(memory) % alignment != 0)
        qFatal("Core test double received an invalid aligned buffer");
}

using MocOwner = std::unique_ptr<CubismMoc, decltype(&CubismMoc::Delete)>;
MocOwner createMoc(bool checkConsistency = false)
{
    return MocOwner(CubismMoc::Create(input.data(), input.size(), checkConsistency), &CubismMoc::Delete);
}

struct ModelDeleter
{
    CubismMoc* moc;
    void operator()(CubismModel* model) const { moc->DeleteModel(model); }
};
using ModelOwner = std::unique_ptr<CubismModel, ModelDeleter>;
}

void* operator new(csmSizeType size, CubismAllocationTag) { return allocate(size, 0); }
void operator delete(void* memory, CubismAllocationTag) { release(memory, false); }

namespace Live2D { namespace Cubism { namespace Framework {
void* CubismFramework::Allocate(csmSizeType size) { return allocate(size, 0); }
void CubismFramework::Deallocate(void* memory) { release(memory, false); }
void* CubismFramework::AllocateAligned(csmSizeType size, csmUint32 alignment) { return allocate(size, alignment); }
void CubismFramework::DeallocateAligned(void* memory) { release(memory, true); }
void Utils::CubismDebug::Print(CubismFramework::Option::LogLevel, const csmChar*, ...) {}

// Core and model doubles isolate the real CubismMoc ownership paths.
CubismModel::CubismModel(Core::csmModel* model) : _model(model) {}
CubismModel::~CubismModel()
{
    ++runtime.wrapperDestructions;
    CSM_FREE_ALLIGNED(_model);
}
void CubismModel::Initialize()
{
    ++runtime.wrapperInitializations;
    if (runtime.throwOnModelInitialize) throw std::runtime_error("controlled model initialization failure");
}
}}}

CMeshMap::CMeshMap() = default;

namespace Live2D { namespace Cubism { namespace Core {
int csmCallingConvention csmHasMocConsistency(void* address, unsigned int size)
{
    checkAllocation(address, csmAlignofMoc);
    ++runtime.consistencyCalls;
    runtime.copiedBytes = QByteArray(static_cast<const char*>(address), size);
    return runtime.consistent ? 1 : 0;
}
csmMoc* csmCallingConvention csmReviveMocInPlace(void* address, unsigned int size)
{
    checkAllocation(address, csmAlignofMoc);
    ++runtime.reviveCalls;
    runtime.copiedBytes = QByteArray(static_cast<const char*>(address), size);
    return runtime.revive ? static_cast<csmMoc*>(address) : nullptr;
}
csmMocVersion csmCallingConvention csmGetMocVersion(const void* address, unsigned int)
{
    checkAllocation(address, csmAlignofMoc);
    ++runtime.versionCalls;
    return csmMocVersion_40;
}
csmMocVersion csmCallingConvention csmGetLatestMocVersion() { return csmMocVersion_50; }
unsigned int csmCallingConvention csmGetSizeofModel(const csmMoc* moc)
{
    checkAllocation(moc, csmAlignofMoc);
    ++runtime.modelSizeCalls;
    return runtime.modelSize;
}
csmModel* csmCallingConvention csmInitializeModelInPlace(const csmMoc* moc, void* address, unsigned int size)
{
    checkAllocation(moc, csmAlignofMoc);
    checkAllocation(address, csmAlignofModel);
    if (size != runtime.modelSize) qFatal("Unexpected model size in Core test double");
    ++runtime.modelInitializeCalls;
    return runtime.initialize ? static_cast<csmModel*>(address) : nullptr;
}
}}}

class CubismMocLifecycleTests : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        QVERIFY(runtime.allocations.empty());
        runtime = ControlledRuntime{};
    }

    void cleanup()
    {
        QVERIFY2(runtime.allocations.empty(), "CubismMoc left tracked allocations alive");
    }

    void nullAndEmptyInputsNeverReachAllocatorOrCore()
    {
        QVERIFY(!CubismMoc::Create(nullptr, input.size(), true));
        QVERIFY(!CubismMoc::Create(input.data(), 0, false));
        QVERIFY(!CubismMoc::HasMocConsistencyFromUnrevivedMoc(nullptr, input.size()));
        QVERIFY(!CubismMoc::HasMocConsistencyFromUnrevivedMoc(input.data(), 0));
        QVERIFY(!CubismMoc::HasMocConsistency(nullptr, input.size()));
        QVERIFY(!CubismMoc::HasMocConsistency(const_cast<csmByte*>(input.data()), 0));
        CubismMoc::Delete(nullptr);
        QCOMPARE(runtime.allocationCalls, 0);
        QCOMPARE(runtime.consistencyCalls, 0);
        QCOMPARE(runtime.reviveCalls, 0);
    }

    void mocAllocationFailureNeverReachesCore()
    {
        runtime.failAlignedAllocation = true;
        QVERIFY(!createMoc(true));
        QCOMPARE(runtime.alignedAllocationCalls, 1);
        QCOMPARE(runtime.consistencyCalls, 0);
        QCOMPARE(runtime.reviveCalls, 0);
        QCOMPARE(runtime.versionCalls, 0);
    }

    void inconsistentMocReleasesCopiedBuffer()
    {
        runtime.consistent = false;
        QVERIFY(!createMoc(true));
        QCOMPARE(runtime.consistencyCalls, 1);
        QCOMPARE(runtime.reviveCalls, 0);
        QCOMPARE(runtime.alignedDeallocations, 1);
        QCOMPARE(runtime.copiedBytes, QByteArray(reinterpret_cast<const char*>(input.data()), input.size()));
    }

    void revivalFailureReleasesCopiedBuffer_data()
    {
        QTest::addColumn<bool>("checkConsistency");
        QTest::newRow("checked") << true;
        QTest::newRow("unchecked") << false;
    }

    void revivalFailureReleasesCopiedBuffer()
    {
        QFETCH(bool, checkConsistency);
        runtime.revive = false;
        QVERIFY(!createMoc(checkConsistency));
        QCOMPARE(runtime.consistencyCalls, int(checkConsistency));
        QCOMPARE(runtime.reviveCalls, 1);
        QCOMPARE(runtime.versionCalls, 0);
        QCOMPARE(runtime.alignedDeallocations, 1);
    }

    void consistencyProbeAlwaysReleasesItsBuffer_data()
    {
        QTest::addColumn<bool>("consistent");
        QTest::newRow("valid") << true;
        QTest::newRow("invalid") << false;
    }

    void consistencyProbeAlwaysReleasesItsBuffer()
    {
        QFETCH(bool, consistent);
        runtime.consistent = consistent;
        QCOMPARE(CubismMoc::HasMocConsistencyFromUnrevivedMoc(input.data(), input.size()), consistent);
        QCOMPARE(runtime.consistencyCalls, 1);
        QCOMPARE(runtime.alignedDeallocations, 1);
        QCOMPARE(runtime.reviveCalls, 0);
        QCOMPARE(runtime.copiedBytes, QByteArray(reinterpret_cast<const char*>(input.data()), input.size()));
    }

    void consistencyAllocationFailureNeverReachesCore()
    {
        runtime.failAlignedAllocation = true;
        QVERIFY(!CubismMoc::HasMocConsistencyFromUnrevivedMoc(input.data(), input.size()));
        QCOMPARE(runtime.alignedAllocationCalls, 1);
        QCOMPARE(runtime.consistencyCalls, 0);
    }

    void failedModelCreationLeavesMocReusable_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("zero-size") << 0;
        QTest::newRow("allocation") << 1;
        QTest::newRow("initialize") << 2;
    }

    void failedModelCreationLeavesMocReusable()
    {
        QFETCH(int, failure);
        auto moc = createMoc();
        QVERIFY(moc);
        QCOMPARE(moc->GetMocVersion(), Core::csmMocVersion_40);
        const auto baseline = runtime.allocations.size();
        const int allocationsBefore = runtime.alignedAllocationCalls;
        if (failure == 0) runtime.modelSize = 0;
        if (failure == 1) runtime.failAlignedAllocation = true;
        if (failure == 2) runtime.initialize = false;
        QVERIFY(!moc->CreateModel());
        QCOMPARE(runtime.allocations.size(), baseline);
        QCOMPARE(runtime.modelInitializeCalls, failure == 2 ? 1 : 0);
        QCOMPARE(runtime.alignedAllocationCalls - allocationsBefore, failure == 0 ? 0 : 1);
        moc->DeleteModel(nullptr);
        runtime.modelSize = 128;
        runtime.initialize = true;
        ModelOwner model(moc->CreateModel(), ModelDeleter{moc.get()});
        QVERIFY(model);
        QCOMPARE(runtime.wrapperInitializations, 1);
        model.reset();
        QCOMPARE(runtime.allocations.size(), baseline);
        QCOMPARE(runtime.wrapperDestructions, 1);
    }

    void multipleModelsAndNullDeletionPreserveOwnerCount()
    {
        auto moc = createMoc(true);
        QVERIFY(moc);
        moc->DeleteModel(nullptr);
        ModelOwner first(moc->CreateModel(), ModelDeleter{moc.get()});
        ModelOwner second(moc->CreateModel(), ModelDeleter{moc.get()});
        QVERIFY(first && second);
        QVERIFY(first.get() != second.get());
        moc->DeleteModel(nullptr);
        second.reset();
        first.reset();
        moc->DeleteModel(nullptr);
        QCOMPARE(runtime.wrapperInitializations, 2);
        QCOMPARE(runtime.wrapperDestructions, 2);
        moc.reset();
        QCOMPARE(runtime.alignedDeallocations, 3);
    }

    void wrapperAllocationExceptionsReleaseCandidateBuffers()
    {
        runtime.failObjectAllocation = true;
        QVERIFY_THROWS_EXCEPTION(std::bad_alloc, createMoc());
        QVERIFY(runtime.allocations.empty());
        auto moc = createMoc();
        QVERIFY(moc);
        const auto baseline = runtime.allocations.size();
        runtime.failObjectAllocation = true;
        QVERIFY_THROWS_EXCEPTION(std::bad_alloc, moc->CreateModel());
        QCOMPARE(runtime.allocations.size(), baseline);
        QCOMPARE(runtime.wrapperInitializations, 0);
    }

    void wrapperInitializationExceptionReleasesModelAndAllowsRetry()
    {
        auto moc = createMoc();
        QVERIFY(moc);
        const auto baseline = runtime.allocations.size();
        runtime.throwOnModelInitialize = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, moc->CreateModel());
        QCOMPARE(runtime.allocations.size(), baseline);
        QCOMPARE(runtime.wrapperDestructions, 1);
        runtime.throwOnModelInitialize = false;
        ModelOwner model(moc->CreateModel(), ModelDeleter{moc.get()});
        QVERIFY(model);
    }
};

QTEST_APPLESS_MAIN(CubismMocLifecycleTests)
#include "cubism_moc_lifecycle_tests.moc"
