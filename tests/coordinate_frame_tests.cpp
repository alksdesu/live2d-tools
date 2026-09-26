#include "live2dstruct/coor/ccoordinateframe.h"
#include "live2dstruct/coor/crotationcoortype.h"
#include "live2dstruct/coor/cwarpcoortype.h"

#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

using namespace live2dstruct;

namespace {
std::unique_ptr<CCoorTypeObject> Rotation(float angle, float x = 0, float y = 0,
                                        float scale = 1, bool reflectX = false, bool reflectY = false)
{
    CRotationNodeKeyform key(1, angle, x, y, scale, reflectX, reflectY);
    return std::make_unique<CRotationCoorType>(0, key);
}

bool Near(const std::vector<float>& actual, const std::vector<float>& expected)
{
    if (actual.size() != expected.size()) return false;
    for (size_t index = 0; index < actual.size(); ++index)
        if (!std::isfinite(actual[index]) ||
            std::abs(actual[index] - expected[index]) > 0.00002f * std::max(1.0f, std::abs(expected[index]))) return false;
    return true;
}

QByteArray Bytes(const std::vector<float>& coordinates)
{
    return QByteArray(reinterpret_cast<const char*>(coordinates.data()), coordinates.size() * sizeof(float));
}

class CountedIdentity : public CCoorTypeObject
{
public:
    explicit CountedIdentity(std::shared_ptr<size_t> destroyed) : m_destroyed(std::move(destroyed)) {}
    ~CountedIdentity() override { ++*m_destroyed; }
    bool transformLocalToCanvas(float* source, float* destination, int count, int offset, int stride) override
    {
        for (int point = 0; point < count; ++point) {
            const int index = offset + point * stride;
            const float x = source[index], y = source[index + 1];
            destination[index] = x;
            destination[index + 1] = y;
        }
        return true;
    }
    bool transformCanvasToLocal(float* source, float* destination, int count) override
    {
        return transformLocalToCanvas(source, destination, count, 0, 2);
    }
private:
    std::shared_ptr<size_t> m_destroyed;
};
}

class CoordinateFrameTests : public QObject
{
    Q_OBJECT
private slots:
    void preparedFramesRejectInvalidInputsAtomically()
    {
        auto existing = std::make_shared<CCoordinateFrame>(Rotation(0, 3, 4));
        std::shared_ptr<const CCoordinateFrame> result = existing;
        CCoordinateFrame::RotationBasis basis{{7, 8}, {1, 2}, {3, 4}};
        CRotationNodeKeyform key(1, 0, 0.25f, 0.4f, 1, false, false);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!CCoordinateFrame::CreateRotation(nan, key, {}, result, basis));
        QCOMPARE(result.get(), existing.get());
        QCOMPARE(basis.origin, QPointF(7, 8));
        QCOMPARE(basis.xAxis, QPointF(1, 2));
        QCOMPARE(basis.yAxis, QPointF(3, 4));
        auto overflow = std::make_shared<CCoordinateFrame>(Rotation(0, 0, 0, std::numeric_limits<float>::max()));
        key.Setscales(2);
        QVERIFY(!CCoordinateFrame::CreateRotation(0, key, overflow, result, basis));
        QCOMPARE(result.get(), existing.get());
        QCOMPARE(basis.origin, QPointF(7, 8));
        QVERIFY(!CCoordinateFrame::CreateWarp(1, 1, {0, 0, 1, 0}, false, {}, result));
        QCOMPARE(result.get(), existing.get());
        auto invalid = std::make_shared<CCoordinateFrame>(Rotation(nan));
        key.Setscales(1);
        QVERIFY(!CCoordinateFrame::CreateRotation(0, key, invalid, result, basis));
        QCOMPARE(result.get(), existing.get());
        QCOMPARE(basis.origin, QPointF(7, 8));
    }

    void parentRotationAppliesAfterChildOrigin()
    {
        auto parent = std::make_shared<CCoordinateFrame>(Rotation(90));
        const CCoordinateFrame child(Rotation(0, 1, 0), parent);
        const std::vector<float> local{1, 0, 0, 0};
        std::vector<float> canvas, restored;
        QVERIFY(child.MapToCanvas(local, canvas));
        QVERIFY(Near(canvas, {0, 2, 0, 1}));
        QVERIFY(child.MapFromCanvas(canvas, restored));
        QVERIFY(Near(restored, local));
        parent.reset();
        QVERIFY(child.MapToCanvas(local, canvas));
        QVERIFY(Near(canvas, {0, 2, 0, 1}));
    }

    void reflectionAndNegativeScaleComposeInOrder()
    {
        auto root = std::make_shared<CCoordinateFrame>(Rotation(90, 10, -5, 2, true, false));
        auto middle = std::make_shared<CCoordinateFrame>(Rotation(0, 1, 3, -1, false, true), root);
        const CCoordinateFrame leaf(Rotation(-90, -2, 1, 0.5f, true, true), middle);
        const std::vector<float> local{4, -3, -2, 5};
        std::vector<float> canvas, restored;
        QVERIFY(leaf.MapToCanvas(local, canvas));
        QVERIFY(Near(canvas, {-2, -8, 4, -16}));
        QVERIFY(leaf.MapFromCanvas(canvas, restored));
        QVERIFY(Near(restored, local));
    }

    void warpAndRotationsComposeAndInvert()
    {
        for (bool quad : {false, true}) {
            std::vector<float> controls{0, 0, 2, 0, 0, 2, 3, 3};
            auto root = std::make_shared<CCoordinateFrame>(Rotation(90, 10, -4));
            auto warp = std::make_shared<CCoordinateFrame>(std::make_unique<CWarpCoorType>(1, 1, controls, quad), root);
            const CCoordinateFrame child(Rotation(0, 0.25f, 0.25f, 0.5f), warp);
            const std::vector<float> local{0, 0.5f, 1, 0};
            const std::vector<float> expected = quad ? std::vector<float>{8.875f, -3.375f, 9.3125f, -2.3125f}
                                                     : std::vector<float>{9, -3.5f, 9.5f, -2.5f};
            std::vector<float> canvas, restored;
            QVERIFY(child.MapToCanvas(local, canvas));
            QVERIFY(Near(canvas, expected));
            QVERIFY(child.MapFromCanvas(canvas, restored));
            QVERIFY(Near(restored, local));
        }
    }

    void nullTransformIsIdentityWithStrictInputValidation()
    {
        const CCoordinateFrame identity(nullptr);
        const std::vector<float> valid{1, -2, 3, 4};
        std::vector<float> output{7, 9};
        QVERIFY(identity.MapToCanvas(valid, output));
        QVERIFY(output == valid);
        QVERIFY(identity.MapFromCanvas(output, output));
        QVERIFY(output == valid);
        QVERIFY(identity.MapToCanvas({}, output));
        QVERIFY(output.empty());
        QVERIFY(identity.MapFromCanvas({}, output));
        const float infinity = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::vector<std::vector<float>> invalid{{1}, {1, 2, 3}, {nan, 0}, {0, infinity},
                                                     std::vector<float>(2000002, 0)};
        for (const auto& input : invalid) {
            output = {7, 9};
            QVERIFY(!identity.MapToCanvas(input, output));
            QVERIFY((output == std::vector<float>{7, 9}));
            QVERIFY(!identity.MapFromCanvas(input, output));
            QVERIFY((output == std::vector<float>{7, 9}));
            auto inPlace = input;
            const QByteArray before = Bytes(inPlace);
            QVERIFY(!identity.MapToCanvas(inPlace, inPlace));
            QCOMPARE(Bytes(inPlace), before);
            QVERIFY(!identity.MapFromCanvas(inPlace, inPlace));
            QCOMPARE(Bytes(inPlace), before);
        }
        std::vector<float> limit(2000000, 0);
        QVERIFY(identity.MapToCanvas(limit, output));
        QVERIFY(output == limit);
    }

    void identityChildRetainsParentTransform()
    {
        auto parent = std::make_shared<CCoordinateFrame>(Rotation(90, 2, 3));
        const CCoordinateFrame child(nullptr, parent);
        std::vector<float> coordinates{1, 0};
        QVERIFY(child.MapToCanvas(coordinates, coordinates));
        QVERIFY(Near(coordinates, {2, 4}));
        QVERIFY(child.MapFromCanvas(coordinates, coordinates));
        QVERIFY(Near(coordinates, {1, 0}));
    }

    void failingAncestorAndInverseKeepDestinationUntouched()
    {
        const float infinity = std::numeric_limits<float>::infinity();
        auto invalidParent = std::make_shared<CCoordinateFrame>(Rotation(infinity));
        const CCoordinateFrame validChild(Rotation(90, 1, 2), invalidParent);
        const std::vector<float> source{1, 0, 2, 3};
        std::vector<float> output{7, 9};
        QVERIFY(!validChild.MapToCanvas(source, output));
        QVERIFY((output == std::vector<float>{7, 9}));
        auto inPlace = source;
        QVERIFY(!validChild.MapToCanvas(inPlace, inPlace));
        QVERIFY(inPlace == source);
        auto parent = std::make_shared<CCoordinateFrame>(Rotation(90, 5, 7));
        const CCoordinateFrame collapsed(Rotation(0, 1, 2, 0), parent);
        QVERIFY(!collapsed.MapFromCanvas(source, output));
        QVERIFY((output == std::vector<float>{7, 9}));
        QVERIFY(!collapsed.MapFromCanvas(inPlace, inPlace));
        QVERIFY(inPlace == source);
        auto enlargingParent = std::make_shared<CCoordinateFrame>(Rotation(0, 0, 0, 2));
        const CCoordinateFrame tooLarge(Rotation(0, 0, 0, std::numeric_limits<float>::max()), enlargingParent);
        QVERIFY(!tooLarge.MapToCanvas({0, 0, 1, 0}, output));
        QVERIFY((output == std::vector<float>{7, 9}));
    }

    void repeatedMappingsNeverAccumulate()
    {
        auto parent = std::make_shared<CCoordinateFrame>(Rotation(30, 3, 4, 0.5f));
        const CCoordinateFrame child(Rotation(-45, 1, 2, 3, true, false), parent);
        const std::vector<float> source{1, 2, 3, 4};
        std::vector<float> expected;
        QVERIFY(child.MapToCanvas(source, expected));
        for (int repeat = 0; repeat < 20; ++repeat) {
            std::vector<float> output;
            QVERIFY(child.MapToCanvas(source, output));
            QVERIFY(output == expected);
            QVERIFY(child.MapFromCanvas(output, output));
            QVERIFY(Near(output, source));
        }
        QVERIFY((source == std::vector<float>{1, 2, 3, 4}));
    }

    void sharedParentsAreOwnedOnceAndSurviveOtherBranches()
    {
        auto destroyed = std::make_shared<size_t>(0);
        auto parent = std::make_shared<CCoordinateFrame>(std::make_unique<CountedIdentity>(destroyed));
        auto first = std::make_shared<CCoordinateFrame>(Rotation(0, 1, 0), parent);
        auto second = std::make_shared<CCoordinateFrame>(Rotation(0, 0, 1), parent);
        parent.reset();
        QCOMPARE(*destroyed, size_t(0));
        std::vector<float> output;
        QVERIFY(first->MapToCanvas({1, 2}, output));
        QVERIFY(Near(output, {2, 2}));
        first.reset();
        QCOMPARE(*destroyed, size_t(0));
        QVERIFY(second->MapToCanvas({1, 2}, output));
        QVERIFY(Near(output, {1, 3}));
        second.reset();
        QCOMPARE(*destroyed, size_t(1));
    }

    void deepChainsMapAndDestroyWithoutRecursion()
    {
        constexpr size_t depth = 100000;
        auto destroyed = std::make_shared<size_t>(0);
        std::shared_ptr<const CCoordinateFrame> frame;
        for (size_t index = 0; index < depth; ++index)
            frame = std::make_shared<CCoordinateFrame>(std::make_unique<CountedIdentity>(destroyed), frame);
        QCOMPARE(*destroyed, size_t(0));
        std::vector<float> coordinates{1, 2, 3, 4};
        QVERIFY(frame->MapToCanvas(coordinates, coordinates));
        QVERIFY((coordinates == std::vector<float>{1, 2, 3, 4}));
        QVERIFY(frame->MapFromCanvas(coordinates, coordinates));
        QVERIFY((coordinates == std::vector<float>{1, 2, 3, 4}));
        frame.reset();
        QCOMPARE(*destroyed, depth);
    }
};

QTEST_GUILESS_MAIN(CoordinateFrameTests)
#include "coordinate_frame_tests.moc"
