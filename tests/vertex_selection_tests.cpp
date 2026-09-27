#include "live2dstruct/cvertexselection.h"
#include "std/cexportcontrol.h"
#include <QScopeGuard>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using Selection = live2dstruct::CVertexSelection;
namespace {
Selection::Geometry chain()
{
    return { { { 0, 0 }, { 1, 0 }, { 3, 0 }, { 4, 0 }, { 0, 0 } }, { { 0, 1 }, { 1, 2 }, { 2, 3 } } };
}
Selection::Soft soft(Selection::Distance distance = Selection::Distance::AlongEdges)
{
    return { true, 4, distance, Selection::Falloff::Smooth };
}
QByteArray bytes(const QVector<float>& values)
{
    return { reinterpret_cast<const char*>(values.constData()), values.size() * qsizetype(sizeof(float)) };
}
void compareWeights(const QVector<double>& actual, const QVector<double>& expected)
{
    QCOMPARE(actual.size(), expected.size());
    for (int index = 0; index < actual.size(); ++index)
        QVERIFY(std::abs(actual[index] - expected[index]) <= 1e-12);
}
}
class VertexSelectionTests : public QObject
{
    Q_OBJECT
private slots:
    void hardAndPaintedModesAreExplicit()
    {
        const Selection::State hard { { 0, 2 }, { } }, blank, zeroPaint { { }, QVector<double> { 0, 0, 0 } };
        QVERIFY(Selection::Validate(hard, 3));
        QVERIFY(Selection::Validate(blank, 3));
        QVERIFY(Selection::Validate(zeroPaint, 3));
        QVERIFY(blank != zeroPaint);
        QVERIFY(Selection::Validate({ { }, QVector<double> { } }, 0));
        QVERIFY(!Selection::Validate({ { 0 }, QVector<double> { .5 } }, 1));
        QVERIFY(!Selection::Validate({ { 0, 0 }, { } }, 2));
        QVERIFY(!Selection::Validate({ { }, QVector<double> { .5 } }, 2));
        for (double invalid :
            { -1., 1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
            QVERIFY(!Selection::Validate({ { }, QVector<double> { invalid } }, 1));
    }
    void remapKeepsIdentityAndNewPaintedPointsStartAtZero()
    {
        Selection::State state { { }, QVector<double> { .2, .7, .1 } }, result;
        QVERIFY(Selection::Remap(state, 3, 4, { 2, -1, 0 }, result));
        QCOMPARE(*result.painted, QVector<double>({ .1, 0, .2, 0 }));
        QVERIFY(result.hard.isEmpty());
        QVERIFY(Selection::Remap({ { 0, 2 }, { } }, 3, 4, { 2, -1, 0 }, result));
        QCOMPARE(result.hard, QVector<int>({ 0, 2 }));
        QVERIFY(!result.painted);
        const auto saved = result;
        QVERIFY(!Selection::Remap(state, 3, 4, { 2, 2, 0 }, result));
        QCOMPARE(result, saved);
        QVERIFY(!Selection::Remap(state, 3, 4, { 2, -2, 0 }, result));
        QCOMPARE(result, saved);
        QVERIFY(Selection::Remap(state, 3, 3, { 2, 0, 1 }, state));
        QCOMPARE(*state.painted, QVector<double>({ .7, .1, .2 }));
    }
    void explicitHardSelectionClearsPaintEvenWhenBothHardSetsAreEmpty()
    {
        Selection::State state { { }, QVector<double> { .2, .5, 0 } }, result;
        QVERIFY(Selection::SelectVertices(3, { }, state, Selection::Combine::Replace, result));
        QVERIFY(result.hard.isEmpty());
        QVERIFY(!result.painted);
        QVERIFY(Selection::SelectVertices(3, { 2, 0 }, state, Selection::Combine::Add, result));
        QCOMPARE(result.hard, QVector<int>({ 0, 2 }));
        QVERIFY(!result.painted);
        QVERIFY(Selection::SelectVertices(3, { 0 }, result, Selection::Combine::Subtract, result));
        QCOMPARE(result.hard, QVector<int> { 2 });
        const auto saved = result;
        QVERIFY(!Selection::SelectVertices(3, { 1, 1 }, result, Selection::Combine::Add, result));
        QCOMPARE(result, saved);
    }
    void nonconvexLassoAndInclusiveBoundaryAreIndependentOfDirection()
    {
        const QVector<QPointF> points { { 3, .5 }, { 3, 3 }, { .5, 3 }, { 1, 2 }, { 0, 0 }, { 5, 0 } };
        QVector<QPointF> polygon { { 0, 0 }, { 4, 0 }, { 4, 1 }, { 1, 1 }, { 1, 4 }, { 0, 4 } };
        Selection::State result;
        QString error;
        QVERIFY2(Selection::SelectPolygon(points, polygon, { }, Selection::Combine::Replace, result, &error),
            qPrintable(error));
        QCOMPARE(result.hard, QVector<int>({ 0, 2, 3, 4 }));
        std::reverse(polygon.begin(), polygon.end());
        polygon.append(polygon.last());
        polygon.append(polygon.first());
        Selection::State reverse;
        QVERIFY(Selection::SelectPolygon(points, polygon, { }, Selection::Combine::Replace, reverse));
        QCOMPARE(reverse, result);
        QVERIFY(Selection::SelectPolygon(points, polygon, { { 1 }, { } }, Selection::Combine::Add, reverse));
        QCOMPARE(reverse.hard, QVector<int>({ 0, 1, 2, 3, 4 }));
        QVERIFY(Selection::SelectPolygon(points, polygon, { { 0, 1, 2 }, { } }, Selection::Combine::Subtract, reverse));
        QCOMPARE(reverse.hard, QVector<int> { 1 });
    }
    void selfIntersectingLassoUsesEvenOddRatherThanSignedArea()
    {
        const QVector<QPointF> points { { 1, 1 }, { 2, 1 }, { 2, 3 }, { .5, 2 }, { 2, 2 } };
        Selection::State result;
        QVERIFY(Selection::SelectPolygon(
            points, { { 0, 0 }, { 4, 4 }, { 0, 4 }, { 4, 0 } }, { }, Selection::Combine::Replace, result));
        QCOMPARE(result.hard, QVector<int>({ 0, 1, 2, 4 }));
        const auto saved = result;
        QVERIFY(!Selection::SelectPolygon(
            points, { { 0, 0 }, { 1, 0 }, { 2, 0 } }, { }, Selection::Combine::Replace, result));
        QCOMPARE(result, saved);
        QVERIFY(!Selection::SelectPolygon(points, { { 0, 0 }, { 0, 0 } }, { }, Selection::Combine::Replace, result));
        QCOMPARE(result, saved);
    }
    void analyticGraphDistancesAndSmoothWeights()
    {
        Selection::Influence result;
        QString error;
        QVERIFY2(Selection::ComputeInfluence(chain(), { { 0 }, { } }, soft(), result, &error), qPrintable(error));
        QCOMPARE(result.source, Selection::Source::Distance);
        compareWeights(result.weights, { 1, .84375, .15625, 0, 0 });
        QCOMPARE(result.affected, QVector<int>({ 0, 1, 2 }));
        auto config = soft();
        config.falloff = Selection::Falloff::Linear;
        QVERIFY(Selection::ComputeInfluence(chain(), { { 0 }, { } }, config, result));
        compareWeights(result.weights, { 1, .75, .25, 0, 0 });
        QVERIFY(Selection::ComputeInfluence(chain(), { { 0, 3 }, { } }, soft(), result));
        compareWeights(result.weights, { 1, .84375, .84375, 1, 0 });
    }
    void distanceModesDistinguishFoldsAndDisconnectedCoincidentVertices()
    {
        Selection::Geometry geometry { { { 0, 0 }, { 10, 0 }, { 10, 1 }, { 0, 1 }, { 0, 0 } },
            { { 0, 1 }, { 1, 2 }, { 2, 3 } } };
        auto config = soft();
        config.radius = 2;
        Selection::Influence result;
        QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        compareWeights(result.weights, { 1, 0, 0, 0, 0 });
        config.distance = Selection::Distance::ConnectedEuclidean;
        QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        compareWeights(result.weights, { 1, 0, 0, .5, 0 });
        config.distance = Selection::Distance::AllEuclidean;
        QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        compareWeights(result.weights, { 1, 0, 0, .5, 1 });
    }
    void zeroEdgesSeedsAndRadiusHaveDefinedResults()
    {
        Selection::Geometry geometry { { { 0, 0 }, { 0, 0 }, { .5, 0 } }, { { 0, 1 }, { 1, 2 }, { 1, 0 }, { 0, 0 } } };
        auto config = soft();
        config.radius = 1;
        Selection::Influence result;
        QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        compareWeights(result.weights, { 1, 1, .5 });
        config.radius = 0;
        QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        compareWeights(result.weights, { 1, 0, 0 });
        config.enabled = false;
        QVERIFY(Selection::ComputeInfluence(geometry, { { 0, 2 }, { } }, config, result));
        compareWeights(result.weights, { 1, 0, 1 });
        QCOMPARE(result.source, Selection::Source::Hard);
        config.enabled = true;
        QVERIFY(Selection::ComputeInfluence(geometry, { }, config, result));
        compareWeights(result.weights, { 0, 0, 0 });
        QVERIFY(Selection::ComputeInfluence(geometry, { { }, QVector<double> { 0, 0, 0 } }, config, result));
        QCOMPARE(result.source, Selection::Source::Painted);
        QVERIFY(result.affected.isEmpty());
    }
    void sortedEdgesAndHashSeedsGiveTheSameFieldAndBudget()
    {
        const auto restore = qScopeGuard([] { QHashSeed::resetRandomGlobalSeed(); });
        auto geometry = chain();
        Selection::Influence baseline, changed;
        Selection::Limits limits;
        QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, soft(), baseline));
        std::reverse(geometry.edges.begin(), geometry.edges.end());
        for (auto& edge : geometry.edges)
            std::swap(edge.a, edge.b);
        for (int attempt = 0; attempt < 4; ++attempt) {
            QHashSeed::resetRandomGlobalSeed();
            QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, soft(), changed));
            QCOMPARE(changed.weights, baseline.weights);
            QCOMPARE(changed.operations, baseline.operations);
            limits.maximumOperations = baseline.operations;
            QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, soft(), changed, limits));
            --limits.maximumOperations;
            changed.affected = { 123 };
            QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, soft(), changed, limits));
            QCOMPARE(changed.affected, QVector<int> { 123 });
        }
    }
    void hardSeedOrderDoesNotChangeFieldAndBrushCancelPreservesInput()
    {
        Selection::Influence first, second;
        for (auto metric : { Selection::Distance::AlongEdges, Selection::Distance::ConnectedEuclidean,
                 Selection::Distance::AllEuclidean }) {
            QVERIFY(Selection::ComputeInfluence(chain(), { { 0, 3 }, { } }, soft(metric), first));
            QVERIFY(Selection::ComputeInfluence(chain(), { { 3, 0 }, { } }, soft(metric), second));
            QCOMPARE(first.weights, second.weights);
            QCOMPARE(first.operations, second.operations);
        }
        const Selection::State before { { 2, 0 }, { } };
        Selection::Stroke stroke;
        Selection::State state;
        QVERIFY(Selection::BeginBrush(
            chain(), before, { }, { .1, .5, 1 }, Selection::Combine::Add, { 3, 0 }, stroke, state));
        QCOMPARE(state, before);
        QVERIFY(Selection::CancelBrush(stroke, state));
        QCOMPARE(state, before);
    }
    void triangleAdjacencyIgnoresOrientationAndDuplicateEdges()
    {
        QVector<Selection::Edge> result;
        QVERIFY(Selection::EdgesFromTriangles(4, { 0, 1, 2, 2, 1, 0, 0, 0, 3 }, result));
        QCOMPARE(result, QVector<Selection::Edge>({ { 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 } }));
        const auto saved = result;
        QVERIFY(!Selection::EdgesFromTriangles(3, { 0, 1, 3 }, result));
        QCOMPARE(result, saved);
        QVERIFY(!Selection::EdgesFromTriangles(3, { 0, 1 }, result));
        QCOMPARE(result, saved);
    }
    void translationPreservesZeroWeightAndZeroDeltaBits()
    {
        const QVector<float> original { -0.f, std::numeric_limits<float>::denorm_min(), 3, 4, -0.f, 8 };
        QVector<float> result;
        QVERIFY(Selection::Translate(original, { 0, .5, 1 }, { 2, -4 }, result));
        QCOMPARE(bytes(result.mid(0, 2)), bytes(original.mid(0, 2)));
        QCOMPARE(result[2], 4.f);
        QCOMPARE(result[3], 2.f);
        QCOMPARE(result[4], 2.f);
        QCOMPARE(result[5], 4.f);
        QVERIFY(Selection::Translate(original, { 1, .5, 1 }, { 0, 0 }, result));
        QCOMPARE(bytes(result), bytes(original));
        QVERIFY(Selection::Translate(original, { 1, .5, 1 }, { 0, 1 }, result));
        QCOMPARE(bytes(result.mid(0, 1)), bytes(original.mid(0, 1)));
        QVERIFY(Selection::Translate({ 0, 0 }, { 1e-300 }, { 1e300, 0 }, result));
        QCOMPARE(result, QVector<float>({ 1, 0 }));
    }
    void translationRejectsAllInvalidInputBeforePublishing()
    {
        QVector<float> result { 123, 456 };
        const auto saved = result;
        QString error;
        QVERIFY(!Selection::Translate({ 0, 0, 1, 1 }, { 1, -1 }, { 1, 0 }, result, &error));
        QCOMPARE(result, saved);
        QVERIFY(!Selection::Translate({ 0, 0, 1 }, { 1 }, { }, result));
        QCOMPARE(result, saved);
        QVERIFY(!Selection::Translate({ 0, std::numeric_limits<float>::quiet_NaN() }, { 0 }, { }, result));
        QCOMPARE(result, saved);
        QVERIFY(!Selection::Translate({ 0, 0 }, { 1 }, { std::numeric_limits<double>::max(), 0 }, result));
        QCOMPARE(result, saved);
        QVERIFY(!Selection::Translate({ 0, 0 }, { std::numeric_limits<double>::infinity() }, { }, result));
        QCOMPARE(result, saved);
    }
    void sweptBrushUsesSegmentDistanceAndNotEventCount()
    {
        const Selection::Geometry geometry { { { 0, 0 }, { 5, 0 }, { 10, 0 }, { 5, .5 }, { 5, 2 } }, { } };
        Selection::Brush brush { 1, .25, 0 };
        Selection::Stroke coarse, dense;
        Selection::State a, b;
        QVERIFY(Selection::BeginBrush(geometry, { }, { }, brush, Selection::Combine::Replace, { 0, 0 }, coarse, a));
        QVERIFY(Selection::AppendBrush(coarse, { 10, 0 }, coarse, a));
        compareWeights(*a.painted, { .25, .25, .25, .125, 0 });
        QVERIFY(Selection::BeginBrush(geometry, { }, { }, brush, Selection::Combine::Replace, { 0, 0 }, dense, b));
        for (int i = 1; i <= 100; ++i)
            QVERIFY(Selection::AppendBrush(dense, { i / 10., 0 }, dense, b));
        compareWeights(*a.painted, *b.painted);
        QVERIFY(Selection::AppendBrush(dense, { 0, 0 }, dense, b));
        compareWeights(*a.painted, *b.painted);
        QVERIFY(Selection::AppendBrush(dense, { 0, 0 }, dense, b));
        compareWeights(*a.painted, *b.painted);
    }
    void diagonalBrushMatchesPerpendicularDistanceOracle()
    {
        Selection::Geometry geometry { { { 4, 5 }, { 5, 5 }, { 5, 9 } }, { } };
        Selection::Stroke coarse, dense;
        Selection::State one, many;
        const Selection::Brush brush { 2, .8, 0 };
        QVERIFY(Selection::BeginBrush(geometry, { }, { }, brush, Selection::Combine::Replace, { }, coarse, one));
        QVERIFY(Selection::AppendBrush(coarse, { 10, 10 }, coarse, one));
        const double u = 1 - 1 / (2 * std::sqrt(2.0));
        compareWeights(*one.painted, { .8 * u * u * (3 - 2 * u), .8, 0 });
        QVERIFY(Selection::BeginBrush(geometry, { }, { }, brush, Selection::Combine::Replace, { }, dense, many));
        for (int index = 1; index <= 37; ++index) {
            const double value = double(index) * 10 / 37;
            QVERIFY(Selection::AppendBrush(dense, { value, value }, dense, many));
        }
        compareWeights(*one.painted, *many.painted);
    }
    void brushStrengthAccumulatesAcrossStrokesOnly()
    {
        Selection::Geometry geometry { { { 0, 0 }, { 10, 0 } }, { } };
        Selection::Brush brush { 1, .25, 1 };
        Selection::Stroke stroke;
        Selection::State result;
        QVERIFY(Selection::BeginBrush(geometry, { }, { }, brush, Selection::Combine::Add, { }, stroke, result));
        QCOMPARE(*result.painted, QVector<double>({ .25, 0 }));
        QVERIFY(result.hard.isEmpty());
        QVERIFY(Selection::AppendBrush(stroke, { }, stroke, result));
        QCOMPARE(*result.painted, QVector<double>({ .25, 0 }));
        QVERIFY(Selection::BeginBrush(geometry, result, { }, brush, Selection::Combine::Add, { }, stroke, result));
        QCOMPARE(*result.painted, QVector<double>({ .5, 0 }));
        QVERIFY(Selection::BeginBrush(geometry, result, { }, brush, Selection::Combine::Subtract, { }, stroke, result));
        QCOMPARE(*result.painted, QVector<double>({ .25, 0 }));
        QVERIFY(Selection::BeginBrush(geometry, result, { }, brush, Selection::Combine::Subtract, { }, stroke, result));
        QCOMPARE(*result.painted, QVector<double>({ 0, 0 }));
        QVERIFY(Selection::BeginBrush(geometry, { { }, QVector<double> { .8, .7 } }, { }, brush,
            Selection::Combine::Replace, { }, stroke, result));
        QCOMPARE(*result.painted, QVector<double>({ .25, 0 }));
    }
    void brushCancelAndNoopRestoreTheOriginalRepresentation()
    {
        auto geometry = chain();
        const Selection::State before { { 0 }, { } };
        Selection::Stroke stroke;
        Selection::State result, restored;
        Selection::Brush brush { 1, .5, .5 };
        QVERIFY(
            Selection::BeginBrush(geometry, before, soft(), brush, Selection::Combine::Add, { 3, 0 }, stroke, result));
        QVERIFY(result.painted);
        QVERIFY(result.hard.isEmpty());
        QVERIFY(Selection::CancelBrush(stroke, restored));
        QCOMPARE(restored, before);
        brush.strength = 0;
        QVERIFY(Selection::BeginBrush(
            geometry, before, soft(), brush, Selection::Combine::Replace, { 3, 0 }, stroke, result));
        QCOMPARE(result, before);
        brush.strength = .5;
        QVERIFY(Selection::BeginBrush(
            geometry, before, soft(), brush, Selection::Combine::Add, { 100, 100 }, stroke, result));
        QCOMPARE(result, before);
    }
    void hardBrushIncludesItsRadiusBoundaryWithoutDivisionByZero()
    {
        Selection::Geometry geometry { { { 0, 0 }, { 1, 0 }, { 1.01, 0 } }, { } };
        Selection::Stroke stroke;
        Selection::State result;
        QVERIFY(
            Selection::BeginBrush(geometry, { }, { }, { 1, 1, 1 }, Selection::Combine::Replace, { }, stroke, result));
        compareWeights(*result.painted, { 1, 1, 0 });
        QVERIFY(
            Selection::BeginBrush(geometry, { }, { }, { 1, 1, 0 }, Selection::Combine::Replace, { }, stroke, result));
        compareWeights(*result.painted, { 1, 0, 0 });
    }
    void strokeSnapshotsAreImmutableAndFailureIsAtomic()
    {
        Selection::Geometry geometry { { { 0, 0 }, { 10, 0 } }, { } };
        Selection::Limits limits;
        limits.maximumStrokeSegments = 1;
        Selection::Stroke initial, advanced;
        Selection::State before, after;
        QString error;
        QVERIFY(Selection::BeginBrush(
            geometry, { }, { }, { 1, .5, 1 }, Selection::Combine::Replace, { }, initial, before, limits));
        QVERIFY(Selection::AppendBrush(initial, { 10, 0 }, advanced, after));
        Selection::State read;
        QVERIFY(Selection::BrushState(initial, read));
        QCOMPARE(read, before);
        const auto saved = after;
        QVERIFY(!Selection::AppendBrush(advanced, { 20, 0 }, advanced, after, &error));
        QCOMPARE(after, saved);
        QVERIFY(Selection::BrushState(advanced, read));
        QCOMPARE(read, saved);
        QVERIFY(!Selection::AppendBrush(initial, { std::numeric_limits<double>::infinity(), 0 }, advanced, after));
        QCOMPARE(after, saved);
    }
    void callbackMutationCannotChangeFrozenGeometryOrSettings()
    {
        auto geometry = chain();
        auto config = soft();
        Selection::State state { { 0 }, { } };
        Selection::Limits limits;
        Selection::Influence result;
        auto* rawPoints = geometry.points.data();
        auto* rawEdges = geometry.edges.data();
        auto* rawHard = state.hard.data();
        CExportControl control;
        bool changed = false;
        control.SetHandler([&](const auto&, QString*) {
            if (!changed) {
                changed = true;
                rawPoints[1] = { 100, 100 };
                rawEdges[0] = { -1, 999 };
                rawHard[0] = 4;
                config.radius = 0;
                limits.maximumOperations = 1;
            }
            return true;
        });
        QVERIFY(Selection::ComputeInfluence(geometry, state, config, result, limits, nullptr, &control));
        compareWeights(result.weights, { 1, .84375, .15625, 0, 0 });
    }
    void brushBaselineIsOwnedBeforeTheFirstCallback()
    {
        Selection::Geometry geometry { { { 0, 0 }, { 10, 0 } }, { } };
        Selection::State state { { }, QVector<double> { .2, .6 } };
        auto brush = Selection::Brush { 1, .25, 1 };
        auto* rawPoints = geometry.points.data();
        auto* rawPaint = state.painted->data();
        Selection::Stroke stroke;
        Selection::State result;
        CExportControl control;
        bool changed = false;
        control.SetHandler([&](const auto&, QString*) {
            if (!changed) {
                changed = true;
                rawPoints[0] = { 100, 100 };
                rawPaint[0] = .9;
                brush.strength = 1;
            }
            return true;
        });
        QVERIFY(Selection::BeginBrush(
            geometry, state, { }, brush, Selection::Combine::Add, { }, stroke, result, nullptr, &control));
        compareWeights(*result.painted, { .45, .6 });
        QVERIFY(Selection::CancelBrush(stroke, result));
        compareWeights(*result.painted, { .2, .6 });
    }
    void budgetsAndCancellationKeepPublishedResults()
    {
        auto geometry = chain();
        Selection::Influence result;
        result.weights = { 123 };
        Selection::Limits limits;
        QString error;
        limits.maximumOperations = 1;
        QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, soft(), result, limits, &error));
        QCOMPARE(result.weights, QVector<double> { 123 });
        limits = { };
        limits.maximumBytes = 1;
        QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, soft(), result, limits, &error));
        QCOMPARE(result.weights, QVector<double> { 123 });
        CExportControl control;
        int calls = 0;
        control.SetHandler([&](const auto&, QString*) { return ++calls < 5; });
        QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, soft(), result, &error, &control));
        QCOMPARE(result.weights, QVector<double> { 123 });
        Selection::State selected { { 4 }, { } };
        limits = { };
        limits.maximumPathPoints = 3;
        QVERIFY(!Selection::SelectPolygon(geometry.points, { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, { },
            Selection::Combine::Replace, selected, limits));
        QCOMPARE(selected.hard, QVector<int> { 4 });
    }
    void brushCancellationDuringAppendDoesNotAdvanceStroke()
    {
        Selection::Geometry geometry { { { 0, 0 }, { 5, 0 }, { 10, 0 } }, { } };
        Selection::Stroke stroke;
        Selection::State current, original;
        QVERIFY(
            Selection::BeginBrush(geometry, { }, { }, { 1, .5, 1 }, Selection::Combine::Replace, { }, stroke, current));
        original = current;
        CExportControl control;
        int calls = 0;
        control.SetHandler([&](const auto&, QString*) { return ++calls < 3; });
        QVERIFY(!Selection::AppendBrush(stroke, { 10, 0 }, stroke, current, nullptr, &control));
        QCOMPARE(current, original);
        QVERIFY(Selection::BrushState(stroke, current));
        QCOMPARE(current, original);
        QVERIFY(Selection::AppendBrush(stroke, { 10, 0 }, stroke, current));
        compareWeights(*current.painted, { .5, .5, .5 });
    }
    void lassoOwnsItsPolygonBeforeAnyCallback()
    {
        QVector<QPointF> points { { 1, 1 }, { 10, 10 } }, polygon { { 0, 0 }, { 2, 0 }, { 2, 2 }, { 0, 2 } };
        auto* pointData = points.data();
        auto* pathData = polygon.data();
        Selection::State result;
        CExportControl control;
        bool changed = false;
        control.SetHandler([&](const auto&, QString*) {
            if (!changed) {
                changed = true;
                pointData[0] = { 100, 100 };
                pathData[1] = { -1, -1 };
            }
            return true;
        });
        QVERIFY(Selection::SelectPolygon(points, polygon, { }, Selection::Combine::Replace, result, nullptr, &control));
        QCOMPARE(result.hard, QVector<int> { 0 });
    }
    void emptyPaintedTopologyRemapsToZeroWeights()
    {
        Selection::State state { { }, QVector<double> { } }, result;
        QVERIFY(Selection::Remap(state, 0, 3, { }, result));
        QVERIFY(result.painted);
        QCOMPARE(*result.painted, QVector<double>({ 0, 0, 0 }));
        QVERIFY(Selection::Remap(result, 3, 0, { -1, -1, -1 }, result));
        QVERIFY(result.painted);
        QVERIFY(result.painted->isEmpty());
    }
    void largeGraphStopsAtRadiusAndRespectsMemoryLimits()
    {
        Selection::Geometry geometry;
        for (int index = 0; index < 20000; ++index) {
            geometry.points.append({ double(index), 0 });
            if (index)
                geometry.edges.append({ index - 1, index });
        }
        Selection::Influence result;
        auto config = soft();
        config.radius = 10;
        QVERIFY(Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        QCOMPARE(result.affected.size(), 10);
        QCOMPARE(result.affected.last(), 9);
        QCOMPARE(result.weights[10], 0.0);
        QCOMPARE(result.weights.last(), 0.0);
        const auto saved = result.weights;
        Selection::Limits limits;
        limits.maximumVertices = 1000;
        QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result, limits));
        QCOMPARE(result.weights, saved);
        limits = { };
        limits.maximumEdges = 100;
        QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result, limits));
        QCOMPARE(result.weights, saved);
    }
    void euclideanWeightsMatchAnExhaustiveIndependentOracle()
    {
        Selection::Geometry geometry;
        Selection::State state;
        for (int index = 0; index < 101; ++index) {
            geometry.points.append({ double((index % 17) * 2), double((index * 13) % 31) });
            if (index % 9 == 0)
                state.hard.append(index);
        }
        auto config = soft(Selection::Distance::AllEuclidean);
        config.radius = 9;
        Selection::Influence result;
        QVERIFY(Selection::ComputeInfluence(geometry, state, config, result));
        for (int index = 0; index < geometry.points.size(); ++index) {
            double distance = config.radius;
            for (int seed : state.hard) {
                const auto d = geometry.points[index] - geometry.points[seed];
                distance = std::min(distance, std::sqrt(d.x() * d.x() + d.y() * d.y()));
            }
            const double t = distance / config.radius;
            QVERIFY(std::abs(result.weights[index] - (1 - 3 * t * t + 2 * t * t * t)) < 1e-12);
        }
    }
    void manyEuclideanSeedsDoNotRequireEveryPointSeedPair()
    {
        Selection::Geometry geometry;
        Selection::State state;
        for (int index = 0; index < 20000; ++index) {
            geometry.points.append({ double(index), 0 });
            if (index % 2 == 0)
                state.hard.append(index);
        }
        auto config = soft(Selection::Distance::AllEuclidean);
        config.radius = 2;
        Selection::Limits limits;
        limits.maximumOperations = 1000000;
        Selection::Influence result;
        QString error;
        QVERIFY2(Selection::ComputeInfluence(geometry, state, config, result, limits, &error), qPrintable(error));
        QCOMPARE(result.weights[0], 1.0);
        QCOMPARE(result.weights[1], .5);
        QCOMPARE(result.weights.last(), .5);
        QVERIFY(result.operations < limits.maximumOperations);
    }
    void invalidGeometryAndEnumsAreRejected()
    {
        auto geometry = chain();
        Selection::Influence result;
        result.weights = { 123 };
        auto config = soft();
        geometry.edges.append({ 0, 999 });
        QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        QCOMPARE(result.weights, QVector<double> { 123 });
        geometry = chain();
        geometry.points[2] = { std::numeric_limits<double>::quiet_NaN(), 0 };
        QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        geometry = chain();
        config.distance = Selection::Distance(99);
        QVERIFY(!Selection::ComputeInfluence(geometry, { { 0 }, { } }, config, result));
        Selection::Settings settings;
        settings.brush.radius = 0;
        QVERIFY(!Selection::ValidateSettings(settings));
        settings = { };
        settings.brush.hardness = 1.01;
        QVERIFY(!Selection::ValidateSettings(settings));
    }
    void toolSettingsRoundtripStrictlyWithoutAuthorState()
    {
        Selection::Settings settings;
        settings.tool = Selection::Tool::Brush;
        settings.combine = Selection::Combine::Subtract;
        settings.soft = soft(Selection::Distance::ConnectedEuclidean);
        settings.brush = { 23.5, .25, .9 };
        const auto json = Selection::ToJson(settings);
        Selection::Settings result;
        QVERIFY(Selection::FromJson(json, result));
        QCOMPARE(result, settings);
        QVERIFY(!json.contains("painted"));
        for (int invalid = 0; invalid < 5; ++invalid) {
            auto changed = json;
            if (invalid == 0)
                changed["version"] = 2;
            if (invalid == 1)
                changed["extra"] = true;
            if (invalid == 2)
                changed["tool"] = 99;
            if (invalid == 3) {
                auto value = changed["brush"].toObject();
                value["strength"] = "0.5";
                changed["brush"] = value;
            }
            if (invalid == 4) {
                auto value = changed["soft"].toObject();
                value["radius"] = -1;
                changed["soft"] = value;
            }
            QVERIFY(!Selection::FromJson(changed, result));
            QCOMPARE(result, settings);
        }
    }
};
QTEST_GUILESS_MAIN(VertexSelectionTests)
#include "vertex_selection_tests.moc"
