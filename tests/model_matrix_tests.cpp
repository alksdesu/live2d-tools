#include "framework/Math/CubismModelMatrix.hpp"

#include <QtTest>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>

using namespace Live2D::Cubism::Framework;

namespace {
enum class AllocationFailure { None, ReturnNull, Throw };
std::unordered_set<void*> allocations;
size_t allocationAttempts = 0;
AllocationFailure nextAllocationFailure = AllocationFailure::None;

void* allocate(csmSizeType size)
{
    ++allocationAttempts;
    const auto failure = nextAllocationFailure;
    nextAllocationFailure = AllocationFailure::None;
    if (failure == AllocationFailure::ReturnNull) return nullptr;
    if (failure == AllocationFailure::Throw) throw std::bad_alloc();
    void* address = std::malloc(size);
    if (!address) throw std::bad_alloc();
    try {
        allocations.insert(address);
    } catch (...) {
        std::free(address);
        throw;
    }
    return address;
}

void deallocate(void* address)
{
    if (!address) return;
    if (allocations.erase(address) != 1) qFatal("Unknown SDK allocation released");
    std::free(address);
}

void* allocateObject(csmSizeType size)
{
    void* address = allocate(size);
    if (!address) throw std::bad_alloc();
    return address;
}

class FailNextAllocation
{
public:
    explicit FailNextAllocation(AllocationFailure failure) { nextAllocationFailure = failure; }
    ~FailNextAllocation() { nextAllocationFailure = AllocationFailure::None; }
};

QByteArray stringBytes(const csmString& value)
{
    return QByteArray(value.GetRawString(), value.GetLength());
}

class AccessibleString : public csmString
{
public:
    using csmString::csmString;
    using csmString::Copy;
    using csmString::Initialize;
};
}

namespace Live2D { namespace Cubism { namespace Framework {
#ifdef CSM_DEBUG_MEMORY_LEAKING
void* CubismFramework::Allocate(csmSizeType size, const csmChar*, csmInt32) { return allocate(size); }
void CubismFramework::Deallocate(void* address, const csmChar*, csmInt32) { deallocate(address); }
#else
void* CubismFramework::Allocate(csmSizeType size) { return allocate(size); }
void CubismFramework::Deallocate(void* address) { deallocate(address); }
#endif
}}}

#ifdef CSM_DEBUG_MEMORY_LEAKING
void* operator new(csmSizeType size, CubismAllocationTag, const csmChar*, csmInt32) { return allocateObject(size); }
void operator delete(void* address, CubismAllocationTag, const csmChar*, csmInt32) { deallocate(address); }
#else
void* operator new(csmSizeType size, CubismAllocationTag) { return allocateObject(size); }
void operator delete(void* address, CubismAllocationTag) { deallocate(address); }
#endif

namespace {
std::array<float, 16> snapshot(CubismModelMatrix& matrix)
{
    std::array<float, 16> values;
    std::copy_n(matrix.GetArray(), values.size(), values.begin());
    return values;
}
}

class ModelMatrixTests : public QObject
{
    Q_OBJECT
private slots:
    void cleanup()
    {
        nextAllocationFailure = AllocationFailure::None;
        QCOMPARE(allocations.size(), size_t(0));
    }

    void stringHashesKeepReverseOrderAndCharacterSignedness_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("signedHash");
        QTest::addColumn<int>("unsignedHash");
        QTest::newRow("empty") << QByteArray() << 0 << 0;
        QTest::newRow("height") << QByteArray("height") << -874766983 << -874766983;
        QTest::newRow("width") << QByteArray("width") << 99601414 << 99601414;
        QTest::newRow("center-x") << QByteArray("center_x") << 457910002 << 457910002;
        QTest::newRow("center-y") << QByteArray("center_y") << -2094246959 << -2094246959;
        QTest::newRow("long-ascii") << QByteArray("Live2D_Long_Parameter_Name_0123456789_").repeated(4)
                                    << -307271928 << -307271928;
        QTest::newRow("high-bytes") << QByteArray::fromHex("80ff7fc3a9") << -82041690 << 162014374;
        QTest::newRow("signed-byte-sentinel") << QByteArray::fromHex("ff") << -2 << 255;
        QTest::newRow("ascii-sentinel") << QByteArray::fromHex("3a2b342d223920") << -2 << -2;
    }

    void stringHashesKeepReverseOrderAndCharacterSignedness()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(int, signedHash);
        QFETCH(int, unsignedHash);
        const csmInt32 expected = std::numeric_limits<csmChar>::is_signed ? signedHash : unsignedHash;
        csmString direct(bytes.constData());
        csmString counted(bytes.constData(), static_cast<csmInt32>(bytes.size()));
        csmString copied(counted);
        csmString assigned("previous");
        assigned = direct;
        for (csmString* value : {&direct, &counted, &copied, &assigned}) {
            QCOMPARE(value->GetHashcode(), expected);
            QCOMPARE(value->GetLength(), bytes.size());
            QCOMPARE(QByteArray(value->GetRawString(), value->GetLength()), bytes);
            QVERIFY(*value == counted);
        }
        assigned = bytes.constData();
        QCOMPARE(assigned.GetHashcode(), expected);
        QVERIFY(assigned == direct);
    }

    void stringMutationKeepsHashAndComparisonConsistent()
    {
        csmString value("center");
        value.Append("_x", 2);
        QCOMPARE(value.GetHashcode(), csmInt32(457910002));
        QVERIFY(value == csmString("center_x"));
        value += csmString("_suffix");
        QCOMPARE(value.GetHashcode(), csmInt32(-972957854));
        value.Append(80, '#');
        QCOMPARE(value.GetLength(), 95);
        QCOMPARE(value.GetHashcode(), csmInt32(-993761182));
        const QByteArray grown = QByteArray("center_x_suffix") + QByteArray(80, '#');
        QVERIFY(value == csmString(grown.constData()));
        value = "height";
        QCOMPARE(value.GetHashcode(), csmInt32(-874766983));
        value += "!";
        QCOMPARE(value.GetHashcode(), csmInt32(-1651916582));
        QVERIFY(value == csmString("height!"));
        value.Clear();
        QCOMPARE(value.GetLength(), 0);
        QCOMPARE(value.GetHashcode(), csmInt32(0));
        QCOMPARE(value.GetRawString()[0], '\0');
        value.Append("height", 6);
        QCOMPARE(value.GetHashcode(), csmInt32(-874766983));
        csmString x = csmString("center_") + csmString("x");
        csmString y = csmString("center_") + "y";
        QCOMPARE(x.GetHashcode(), csmInt32(457910002));
        QCOMPARE(y.GetHashcode(), csmInt32(-2094246959));
        QVERIFY(!(x == y));
        x.Append(80, '_');
        y.Append(80, '_');
        QCOMPARE(x.GetHashcode(), csmInt32(-765435406));
        QCOMPARE(y.GetHashcode(), csmInt32(977374929));
        QVERIFY(!(x == y));
    }

    void equalHashStringsStillHaveDistinctMapKeys()
    {
        csmString first("aA"), second("BB");
        QCOMPARE(first.GetHashcode(), csmInt32(2112));
        QCOMPARE(second.GetHashcode(), csmInt32(2112));
        QVERIFY(!(first == second));
        csmMap<csmString, int> values;
        values[first] = 17;
        values[second] = 23;
        QCOMPARE(values[csmString("aA")], 17);
        QCOMPARE(values[csmString("BB")], 23);
        const QByteArray sentinelBytes = QByteArray::fromHex("3a2b342d223920");
        csmString sentinel(sentinelBytes.constData());
        QCOMPARE(sentinel.GetHashcode(), csmInt32(-2));
        values[sentinel] = 31;
        QCOMPARE(values[csmString(sentinelBytes.constData())], 31);
    }

    void stringStoragePreservesTheSmallBufferBoundary()
    {
        for (int length : {62, 63, 64}) {
            const QByteArray bytes(length, 'x');
            const size_t before = allocations.size();
            {
                csmString value(bytes.constData());
                QCOMPARE(stringBytes(value), bytes);
                QCOMPARE(allocations.size(), before + (length >= 63 ? 1 : 0));
                const char* pointer = value.GetRawString();
                QCOMPARE(allocations.count(const_cast<char*>(pointer)), size_t(length >= 63 ? 1 : 0));
                value = "short";
                QCOMPARE(stringBytes(value), QByteArray("short"));
                QCOMPARE(allocations.size(), before);
                value = bytes.constData();
                QCOMPARE(stringBytes(value), bytes);
                value.Clear();
                QCOMPARE(allocations.size(), before);
                value.Append(bytes.constData(), length);
                QCOMPARE(stringBytes(value), bytes);
            }
            QCOMPARE(allocations.size(), before);
        }
        csmString value(QByteArray(62, 'a').constData());
        value += "b";
        QCOMPARE(stringBytes(value), QByteArray(62, 'a') + "b");
        QCOMPARE(allocations.size(), size_t(1));
        value += "c";
        QCOMPARE(stringBytes(value), QByteArray(62, 'a') + "bc");
        QCOMPARE(allocations.size(), size_t(1));
    }

    void stringAssignmentAndAppendSupportOwnStorageAliases()
    {
        for (int length : {0, 1, 31, 62, 63, 64, 100}) {
            const QByteArray bytes = QByteArray("0123456789").repeated(10).left(length);
            csmString value(bytes.constData());
            const char* pointer = value.GetRawString();
            const size_t attempts = allocationAttempts;
            value = value;
            QVERIFY(value.GetRawString() == pointer);
            QCOMPARE(allocationAttempts, attempts);
            QCOMPARE(stringBytes(value), bytes);
            value = value.GetRawString();
            QCOMPARE(stringBytes(value), bytes);
            for (int offset : {0, std::min(1, length), length / 2, length}) {
                value = bytes.constData();
                value = value.GetRawString() + offset;
                QCOMPARE(stringBytes(value), bytes.mid(offset));
                value = bytes.constData();
                value += value.GetRawString() + offset;
                QCOMPARE(stringBytes(value), bytes + bytes.mid(offset));
                value = bytes.constData();
                value.Append(value.GetRawString() + offset, length - offset);
                QCOMPARE(stringBytes(value), bytes + bytes.mid(offset));
            }
            value = bytes.constData();
            value += value;
            QCOMPARE(stringBytes(value), bytes + bytes);
            csmString expected((bytes + bytes).constData());
            QCOMPARE(value.GetHashcode(), expected.GetHashcode());
            QVERIFY(value == expected);
        }
    }

    void stringConcatenationOverloadsAgreeAtBoundariesAndForUtf8()
    {
        const QByteArray utf8 = QByteArray::fromHex("e6a8a1e59e8be88c83e59bb4f09f8ea8");
        for (const QByteArray& prefix : {QByteArray(), QByteArray(61, 'p'), QByteArray(62, 'p'),
                                       QByteArray(63, 'p'), QByteArray(64, 'p'), utf8.repeated(5)}) {
            for (const QByteArray& suffix : {QByteArray(), QByteArray("!"), QByteArray("!?"), utf8}) {
                const QByteArray expected = prefix + suffix;
                csmString left(prefix.constData()), right(suffix.constData());
                csmString plusString = left + right;
                csmString plusRaw = left + suffix.constData();
                csmString appendString(left), appendRaw(left), appendCounted(left);
                appendString += right;
                appendRaw += suffix.constData();
                appendCounted.Append(suffix.constData(), static_cast<csmInt32>(suffix.size()));
                for (csmString* value : {&plusString, &plusRaw, &appendString, &appendRaw, &appendCounted}) {
                    QCOMPARE(stringBytes(*value), expected);
                    QCOMPARE(value->GetRawString()[value->GetLength()], '\0');
                    QCOMPARE(value->GetHashcode(), plusString.GetHashcode());
                    QVERIFY(*value == plusString);
                }
                QCOMPARE(stringBytes(left), prefix);
                QCOMPARE(stringBytes(right), suffix);
            }
        }
    }

    void stringAdoptionOwnsShortAndEmptyBuffersUntilRelease()
    {
        for (int length : {0, 1, 62, 63, 64}) {
            for (bool explicitClear : {false, true}) {
                const QByteArray bytes(length, 'a');
                const size_t before = allocations.size();
                char* buffer = static_cast<char*>(allocate(static_cast<size_t>(length) + 1));
                std::memcpy(buffer, bytes.constData(), static_cast<size_t>(length) + 1);
                buffer[length] = '?';
                {
                    const size_t attempts = allocationAttempts;
                    csmString adopted(buffer, length, true);
                    QVERIFY(adopted.GetRawString() == buffer);
                    QCOMPARE(allocationAttempts, attempts);
                    QCOMPARE(stringBytes(adopted), bytes);
                    QCOMPARE(adopted.GetRawString()[length], '\0');
                    csmString copied(adopted);
                    QCOMPARE(stringBytes(copied), bytes);
                    QCOMPARE(copied.GetHashcode(), adopted.GetHashcode());
                    QVERIFY(copied.GetRawString() != buffer);
                    if (explicitClear) {
                        adopted.Clear();
                        QCOMPARE(allocations.count(buffer), size_t(0));
                        QCOMPARE(adopted.GetLength(), 0);
                        adopted.Append("reused", 6);
                        QCOMPARE(stringBytes(adopted), QByteArray("reused"));
                    } else {
                        QCOMPARE(allocations.count(buffer), size_t(1));
                    }
                }
                QCOMPARE(allocations.size(), before);
            }
            QByteArray source(length, 'b');
            csmString copied(source.constData(), length, false);
            QVERIFY(copied.GetRawString() != source.constData());
            source.fill('c');
            QCOMPARE(stringBytes(copied), QByteArray(length, 'b'));
        }
    }

    void stringReinitializationKeepsOwnershipOfItsStartingPointer()
    {
        for (int length : {0, 1, 62, 63, 64, 100}) {
            const QByteArray bytes(length, 'a');
            for (bool adopt : {false, true}) {
                char* buffer = nullptr;
                if (adopt) {
                    buffer = static_cast<char*>(allocate(static_cast<size_t>(length) + 1));
                    std::memcpy(buffer, bytes.constData(), static_cast<size_t>(length) + 1);
                }
                AccessibleString value(adopt ? buffer : bytes.constData(), length, adopt);
                const char* pointer = value.GetRawString();
                const size_t owned = allocations.size(), attempts = allocationAttempts;
                const_cast<char*>(pointer)[length] = '?';
                value.Initialize(pointer, length, true);
                QVERIFY(value.GetRawString() == pointer);
                QCOMPARE(stringBytes(value), bytes);
                QCOMPARE(value.GetRawString()[length], '\0');
                QCOMPARE(allocations.size(), owned);
                QCOMPARE(allocationAttempts, attempts);
                {
                    csmString expected(bytes.constData());
                    QCOMPARE(value.GetHashcode(), expected.GetHashcode());
                }
                value.Initialize(pointer, length / 2, true);
                QVERIFY(value.GetRawString() == pointer);
                QCOMPARE(stringBytes(value), bytes.left(length / 2));
                QCOMPARE(value.GetRawString()[length / 2], '\0');
                QCOMPARE(allocations.size(), owned);
                {
                    csmString expected(bytes.left(length / 2).constData());
                    QCOMPARE(value.GetHashcode(), expected.GetHashcode());
                }
                value.Initialize(pointer, 0, true);
                QVERIFY(value.GetRawString() == pointer);
                QCOMPARE(value.GetLength(), 0);
                QCOMPARE(value.GetHashcode(), csmInt32(0));
                QCOMPARE(allocations.size(), owned);
                value.Clear();
                QCOMPARE(allocations.size(), size_t(0));
            }
        }
    }

    void stringReinitializationRejectsInteriorPointersAndUnprovenGrowth()
    {
        for (int length : {0, 1, 62, 63, 64, 100}) {
            const QByteArray bytes(length, 'a');
            for (bool adopt : {false, true}) {
                char* buffer = nullptr;
                if (adopt) {
                    buffer = static_cast<char*>(allocate(static_cast<size_t>(length) + 1));
                    std::memcpy(buffer, bytes.constData(), static_cast<size_t>(length) + 1);
                }
                AccessibleString value(adopt ? buffer : bytes.constData(), length, adopt);
                const char* pointer = value.GetRawString();
                const csmInt32 hash = value.GetHashcode();
                const size_t owned = allocations.size(), attempts = allocationAttempts;
                for (int offset : {1, length / 2, length}) {
                    if (offset <= 0 || offset > length) continue;
                    QVERIFY_EXCEPTION_THROWN(value.Initialize(pointer + offset, length - offset, true),
                                             std::invalid_argument);
                }
                for (csmInt32 invalid : {-1, length + 1, std::numeric_limits<csmInt32>::max() - 1,
                                         std::numeric_limits<csmInt32>::max()}) {
                    QVERIFY_EXCEPTION_THROWN(value.Initialize(pointer, invalid, true), std::length_error);
                }
                QVERIFY(value.GetRawString() == pointer);
                QCOMPARE(stringBytes(value), bytes);
                QCOMPARE(value.GetHashcode(), hash);
                QCOMPARE(allocations.size(), owned);
                QCOMPARE(allocationAttempts, attempts);
                value.Initialize(pointer + length / 2, length - length / 2, false);
                QCOMPARE(stringBytes(value), bytes.mid(length / 2));
            }
        }
        AccessibleString grown("short");
        const char* inlineBuffer = grown.GetRawString();
        grown.Append(100, 'a');
        const QByteArray bytes = stringBytes(grown);
        QVERIFY_EXCEPTION_THROWN(grown.Initialize(inlineBuffer, 5, true), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(grown.Initialize(inlineBuffer + 1, 4, true), std::invalid_argument);
        QCOMPARE(stringBytes(grown), bytes);
    }

    void stringEmptyCopiesReleaseExistingStorage()
    {
        for (int length : {0, 62, 63, 64}) {
            AccessibleString value(QByteArray(length, 'v').constData());
            value.Copy(nullptr, 0);
            QCOMPARE(value.GetLength(), 0);
            QCOMPARE(value.GetHashcode(), csmInt32(0));
            QCOMPARE(value.GetRawString()[0], '\0');
            QCOMPARE(allocations.size(), size_t(0));
            value.Append(64, 'r');
            value.Copy(value.GetRawString() + value.GetLength(), 0);
            QCOMPARE(stringBytes(value), QByteArray());
            QCOMPARE(allocations.size(), size_t(0));
        }
        csmString empty;
        csmString value(QByteArray(100, 'v').constData());
        value = empty;
        QCOMPARE(stringBytes(value), QByteArray());
        value = QByteArray(100, 'v').constData();
        value = "";
        QCOMPARE(stringBytes(value), QByteArray());
        csmString counted(nullptr, 0), adopted(nullptr, 0, true);
        QCOMPARE(counted.GetLength(), 0);
        QCOMPARE(adopted.GetLength(), 0);
    }

    void stringAllocationFailurePreservesValuesAndOwnership()
    {
        const QByteArray initial(100, 'i'), replacement(120, 'r');
        for (AllocationFailure failure : {AllocationFailure::ReturnNull, AllocationFailure::Throw}) {
            for (int operation = 0; operation < 12; ++operation) {
                csmString value(initial.constData()), source(replacement.constData());
                const char* pointer = value.GetRawString();
                const csmInt32 hash = value.GetHashcode();
                const size_t before = allocations.size(), attempts = allocationAttempts;
                bool threw = false;
                {
                    FailNextAllocation fail(failure);
                    try {
                        switch (operation) {
                        case 0: value = replacement.constData(); break;
                        case 1: value = source; break;
                        case 2: value = value.GetRawString(); break;
                        case 3: value = value.GetRawString() + 1; break;
                        case 4: value += value; break;
                        case 5: value += value.GetRawString() + 1; break;
                        case 6: value.Append(value.GetRawString() + 1, 99); break;
                        case 7: value.Append(1, 'x'); break;
                        case 8: { csmString result = value + source; break; }
                        case 9: { csmString result = value + replacement.constData(); break; }
                        case 10: { csmString result(value); break; }
                        case 11: { csmString result(replacement.constData()); break; }
                        }
                    } catch (const std::bad_alloc&) {
                        threw = true;
                    }
                }
                QVERIFY2(threw, qPrintable(QString("operation %1 did not report allocation failure").arg(operation)));
                QCOMPARE(allocationAttempts, attempts + 1);
                QCOMPARE(allocations.size(), before);
                QVERIFY(value.GetRawString() == pointer);
                QCOMPARE(stringBytes(value), initial);
                QCOMPARE(value.GetHashcode(), hash);
                QCOMPARE(stringBytes(source), replacement);
                value += "!";
                QCOMPARE(stringBytes(value), initial + "!");
            }
            csmString small(QByteArray(62, 's').constData());
            const char* pointer = small.GetRawString();
            {
                FailNextAllocation fail(failure);
                QVERIFY_EXCEPTION_THROWN(small += "!", std::bad_alloc);
            }
            QVERIFY(small.GetRawString() == pointer);
            QCOMPARE(stringBytes(small), QByteArray(62, 's'));
            QCOMPARE(allocations.size(), size_t(0));
        }
    }

    void stringInvalidLengthsFailBeforeAllocationAndLeaveValuesIntact()
    {
        const csmInt32 maximum = std::numeric_limits<csmInt32>::max();
        csmString value("unchanged");
        const size_t attempts = allocationAttempts;
        for (csmInt32 invalid : {-1, maximum}) {
            QVERIFY_EXCEPTION_THROWN(csmString("x", invalid), std::length_error);
            QVERIFY_EXCEPTION_THROWN(csmString("x", invalid, true), std::length_error);
            QVERIFY_EXCEPTION_THROWN(value.Append("x", invalid), std::length_error);
            QVERIFY_EXCEPTION_THROWN(value.Append(invalid, 'x'), std::length_error);
        }
        QVERIFY_EXCEPTION_THROWN(value.Append("x", maximum - 1), std::length_error);
        QVERIFY_EXCEPTION_THROWN(value.Append(maximum - 1, 'x'), std::length_error);
        QVERIFY_EXCEPTION_THROWN(csmString(nullptr), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(csmString(nullptr, 1), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(csmString(nullptr, 1, true), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(value = static_cast<const char*>(nullptr), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(value.Append(nullptr, 1), std::invalid_argument);
        QCOMPARE(allocationAttempts, attempts);
        QCOMPARE(stringBytes(value), QByteArray("unchanged"));
    }

    void modelSizesMatchFourSampleCanvases_data()
    {
        QTest::addColumn<float>("width");
        QTest::addColumn<float>("height");
        QTest::addColumn<float>("ppu");
        QTest::addColumn<float>("unitWidth");
        QTest::addColumn<float>("unitHeight");
        QTest::addColumn<float>("scale");
        QTest::newRow("Mark") << 2400.0f << 3000.0f << 2400.0f << 1.0f << 1.25f << 1.6f;
        QTest::newRow("Rice") << 4500.0f << 3000.0f << 3000.0f << 1.5f << 1.0f << 2.0f;
        QTest::newRow("Natori") << 4000.0f << 7000.0f << 4000.0f << 1.0f << 1.75f << float(8.0 / 7.0);
        QTest::newRow("Wanko") << 1200.0f << 1200.0f << 1200.0f << 1.0f << 1.0f << 2.0f;
    }

    void modelSizesMatchFourSampleCanvases()
    {
        QFETCH(float, width);
        QFETCH(float, height);
        QFETCH(float, ppu);
        QFETCH(float, unitWidth);
        QFETCH(float, unitHeight);
        QFETCH(float, scale);
        float actualWidth = 99, actualHeight = 101;
        QVERIFY(CubismModelMatrix::TryComputeModelSize(width, height, ppu, actualWidth, actualHeight));
        QCOMPARE(actualWidth, unitWidth);
        QCOMPARE(actualHeight, unitHeight);
        CubismModelMatrix matrix(actualWidth, actualHeight);
        QVERIFY(matrix.IsValid());
        QCOMPARE(matrix.GetScaleX(), scale);
        QCOMPARE(matrix.GetScaleY(), scale);
        QCOMPARE(matrix.GetTranslateX(), 0.0f);
        QCOMPARE(matrix.GetTranslateY(), 0.0f);
    }

    void invalidSizeInputsLeaveBothOutputsUntouched()
    {
        const float maximum = std::numeric_limits<float>::max();
        const float tiny = std::numeric_limits<float>::denorm_min();
        const float infinity = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::array<std::array<float, 3>, 15> cases{{
            {0, 1, 1}, {1, 0, 1}, {-1, 1, 1}, {1, -1, 1}, {1, 1, 0}, {1, 1, -1},
            {nan, 1, 1}, {1, nan, 1}, {1, 1, nan}, {infinity, 1, 1}, {1, infinity, 1},
            {1, 1, infinity}, {1, 1, tiny}, {tiny, 1, maximum}, {1, tiny, 1}
        }};
        for (const auto& input : cases) {
            float width = 7, height = 9;
            QVERIFY(!CubismModelMatrix::TryComputeModelSize(input[0], input[1], input[2], width, height));
            QCOMPARE(width, 7.0f);
            QCOMPARE(height, 9.0f);
        }
        float width = 7, height = 9;
        QVERIFY(!CubismModelMatrix::TryComputeModelSize(1, 1, maximum, width, height));
        QCOMPARE(width, 7.0f);
        QCOMPARE(height, 9.0f);
    }

    void extremeFiniteAndSubnormalSizesRemainValid()
    {
        const float maximum = std::numeric_limits<float>::max();
        const float tiny = std::numeric_limits<float>::denorm_min();
        const float validSubnormalHeight = std::numeric_limits<float>::min() * 0.75f;
        const std::array<std::array<float, 3>, 6> cases{{
            {maximum, maximum, maximum}, {tiny, tiny, tiny}, {tiny, 1, 1},
            {1, validSubnormalHeight, 1}, {1, 4, maximum}, {maximum, maximum, 1}
        }};
        for (const auto& input : cases) {
            float width = 0, height = 0;
            QVERIFY(CubismModelMatrix::TryComputeModelSize(input[0], input[1], input[2], width, height));
            QVERIFY(std::isfinite(width) && width > 0);
            QVERIFY(std::isfinite(height) && height > 0);
            CubismModelMatrix matrix(width, height);
            QVERIFY(matrix.IsValid());
            QVERIFY(std::isfinite(matrix.GetScaleX()) && matrix.GetScaleX() != 0);
        }
        CubismModelMatrix wide(maximum, 1);
        wide.SetWidth(1);
        QVERIFY(wide.IsValid());
        QCOMPARE(std::fpclassify(wide.GetScaleX()), FP_SUBNORMAL);
        const auto before = snapshot(wide);
        wide.SetWidth(tiny);
        QVERIFY(snapshot(wide) == before);
    }

    void invalidConstructionNeverDividesByZero()
    {
        CubismModelMatrix empty;
        QVERIFY(!empty.IsValid());
        const auto initial = snapshot(empty);
        empty.SetWidth(2);
        empty.SetHeight(2);
        empty.SetPosition(4, 5);
        empty.SetCenterPosition(4, 5);
        QVERIFY(snapshot(empty) == initial);
        csmMap<csmString, float> layout;
        layout["height"] = 2;
        QVERIFY(!empty.TrySetupFromLayout(layout));
        for (float invalid : {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            CubismModelMatrix badWidth(invalid, 1);
            CubismModelMatrix badHeight(1, invalid);
            QVERIFY(!badWidth.IsValid() && !badHeight.IsValid());
            for (float entry : snapshot(badWidth)) QVERIFY(std::isfinite(entry));
            for (float entry : snapshot(badHeight)) QVERIFY(std::isfinite(entry));
        }
    }

    void settersAndLayoutUseTheSamePlacementSemantics()
    {
        CubismModelMatrix direct(4, 2);
        direct.SetWidth(8);
        direct.SetPosition(3, 5);
        QCOMPARE(direct.GetScaleX(), 2.0f);
        QCOMPARE(direct.GetTranslateX(), 3.0f);
        QCOMPARE(direct.GetTranslateY(), 5.0f);
        direct.SetCenterPosition(10, 20);
        QCOMPARE(direct.GetTranslateX(), 6.0f);
        QCOMPARE(direct.GetTranslateY(), 18.0f);
        direct.Right(10);
        direct.Bottom(20);
        QCOMPARE(direct.GetTranslateX(), 2.0f);
        QCOMPARE(direct.GetTranslateY(), 16.0f);
        direct.Left(4);
        direct.Top(6);
        direct.SetX(7);
        direct.SetY(9);
        direct.CenterX(10);
        direct.CenterY(20);
        CubismModelMatrix fromLayout(4, 2);
        csmMap<csmString, float> layout;
        layout["center_x"] = 10;
        layout["center_y"] = 20;
        layout["width"] = 8;
        QVERIFY(fromLayout.TrySetupFromLayout(layout));
        QVERIFY(snapshot(fromLayout) == snapshot(direct));
        CubismModelMatrix legacy(4, 2);
        legacy.SetupFromLayout(layout);
        QVERIFY(snapshot(legacy) == snapshot(direct));
    }

    void twoSizeKeysKeepInsertionOrderAndAllowReflection()
    {
        CubismModelMatrix heightLast(4, 2);
        csmMap<csmString, float> layout;
        layout["center_x"] = 10;
        layout["width"] = 8;
        layout["height"] = 6;
        layout["right"] = 20;
        QVERIFY(heightLast.TrySetupFromLayout(layout));
        QCOMPARE(heightLast.GetScaleX(), 3.0f);
        QCOMPARE(heightLast.GetTranslateX(), 8.0f);
        CubismModelMatrix widthLast(4, 2);
        csmMap<csmString, float> reversed;
        reversed["height"] = 6;
        reversed["width"] = -8;
        reversed["center_x"] = 10;
        reversed["bottom"] = 20;
        QVERIFY(widthLast.TrySetupFromLayout(reversed));
        QCOMPARE(widthLast.GetScaleX(), -2.0f);
        QCOMPARE(widthLast.GetScaleY(), -2.0f);
        QCOMPARE(widthLast.GetTranslateX(), 14.0f);
        QCOMPARE(widthLast.GetTranslateY(), 24.0f);
        QVERIFY(widthLast.IsValid());
        widthLast.SetHeight(-6);
        QCOMPARE(widthLast.GetScaleX(), -3.0f);
        QVERIFY(widthLast.IsValid());
    }

    void invalidLayoutFieldsDoNotCommitEarlierChanges()
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float infinity = std::numeric_limits<float>::infinity();
        for (const char* key : {"width", "height", "x", "y", "center_x", "center_y", "top", "bottom", "left", "right"}) {
            for (float invalid : {nan, infinity, -infinity}) {
                CubismModelMatrix matrix(4, 2);
                matrix.SetPosition(5, 7);
                const auto before = snapshot(matrix);
                csmMap<csmString, float> layout;
                layout["width"] = 8;
                layout["x"] = 13;
                layout[key] = invalid;
                QVERIFY(!matrix.TrySetupFromLayout(layout));
                QVERIFY(snapshot(matrix) == before);
                matrix.SetupFromLayout(layout);
                QVERIFY(snapshot(matrix) == before);
            }
        }
        CubismModelMatrix matrix(4, 2);
        const auto before = snapshot(matrix);
        for (const char* key : {"width", "height"}) {
            csmMap<csmString, float> layout;
            layout[key] = 0;
            layout[std::strcmp(key, "width") == 0 ? "height" : "width"] = 6;
            QVERIFY(!matrix.TrySetupFromLayout(layout));
            QVERIFY(snapshot(matrix) == before);
        }
        csmMap<csmString, float> ignored;
        ignored["unknown"] = nan;
        QVERIFY(matrix.TrySetupFromLayout(ignored));
        QVERIFY(snapshot(matrix) == before);
        CubismModelMatrix tiny(1, std::numeric_limits<float>::min());
        const auto tinyBefore = snapshot(tiny);
        csmMap<csmString, float> overflow;
        overflow["x"] = 3;
        overflow["height"] = std::numeric_limits<float>::max();
        QVERIFY(!tiny.TrySetupFromLayout(overflow));
        QVERIFY(snapshot(tiny) == tinyBefore);
        const float maximum = std::numeric_limits<float>::max();
        CubismModelMatrix large(maximum, maximum);
        const auto largeBefore = snapshot(large);
        csmMap<csmString, float> invalidLastPosition;
        invalidLastPosition["height"] = maximum;
        invalidLastPosition["x"] = 13;
        invalidLastPosition["right"] = -maximum;
        QVERIFY(!large.TrySetupFromLayout(invalidLastPosition));
        QVERIFY(snapshot(large) == largeBefore);
    }

    void allPositionSettersRejectInvalidValuesAtomically()
    {
        using Setter = void (CubismModelMatrix::*)(float);
        const Setter setters[]{&CubismModelMatrix::SetX, &CubismModelMatrix::SetY,
            &CubismModelMatrix::Left, &CubismModelMatrix::Top, &CubismModelMatrix::Right,
            &CubismModelMatrix::Bottom, &CubismModelMatrix::CenterX, &CubismModelMatrix::CenterY};
        CubismModelMatrix matrix(4, 2);
        matrix.SetPosition(5, 7);
        const auto before = snapshot(matrix);
        for (float invalid : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            for (Setter setter : setters) {
                (matrix.*setter)(invalid);
                QVERIFY(snapshot(matrix) == before);
            }
            matrix.SetPosition(11, invalid);
            QVERIFY(snapshot(matrix) == before);
            matrix.SetPosition(invalid, 11);
            QVERIFY(snapshot(matrix) == before);
            matrix.SetCenterPosition(11, invalid);
            QVERIFY(snapshot(matrix) == before);
            matrix.SetCenterPosition(invalid, 11);
            QVERIFY(snapshot(matrix) == before);
            matrix.SetWidth(invalid);
            matrix.SetHeight(invalid);
            QVERIFY(snapshot(matrix) == before);
        }
        matrix.SetWidth(0);
        matrix.SetHeight(0);
        QVERIFY(snapshot(matrix) == before);
    }

    void finiteLargeIntermediateExtentsDoNotOverflow()
    {
        const float maximum = std::numeric_limits<float>::max();
        CubismModelMatrix matrix(maximum, maximum);
        matrix.Scale(2, 2);
        QVERIFY(matrix.IsValid());
        matrix.SetCenterPosition(maximum, maximum);
        QCOMPARE(matrix.GetTranslateX(), 0.0f);
        QCOMPARE(matrix.GetTranslateY(), 0.0f);
        matrix.Right(maximum);
        matrix.Bottom(maximum);
        QCOMPARE(matrix.GetTranslateX(), -maximum);
        QCOMPARE(matrix.GetTranslateY(), -maximum);
        matrix.Scale(4, 4);
        matrix.SetPosition(12, 13);
        const auto before = snapshot(matrix);
        matrix.SetCenterPosition(maximum, -maximum);
        QVERIFY(snapshot(matrix) == before);
        matrix.Right(maximum);
        matrix.Bottom(maximum);
        QVERIFY(snapshot(matrix) == before);
        matrix.SetCenterPosition(maximum, maximum);
        QCOMPARE(matrix.GetTranslateX(), -maximum);
        QCOMPARE(matrix.GetTranslateY(), -maximum);
        QVERIFY(matrix.IsValid());
    }

    void validityRejectsNonfiniteMatricesAndZeroScale()
    {
        for (int index = 0; index < 16; ++index) {
            CubismModelMatrix matrix(4, 2);
            matrix.GetArray()[index] = std::numeric_limits<float>::quiet_NaN();
            QVERIFY(!matrix.IsValid());
        }
        CubismModelMatrix matrix(4, 2);
        matrix.Scale(0, 1);
        QVERIFY(!matrix.IsValid());
        matrix.Scale(1, 0);
        QVERIFY(!matrix.IsValid());
        matrix.Scale(-1, -1);
        QVERIFY(matrix.IsValid());
    }
};

QTEST_APPLESS_MAIN(ModelMatrixTests)
#include "model_matrix_tests.moc"
