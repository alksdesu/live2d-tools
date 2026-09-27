#include "live2dstruct/cboundsspatial.h"
#include "live2dstruct/cmesheditplanner.h"
#include "opengl/cuvgeometry.h"
#include <QMap>
#include <QtTest>
#include <cmath>
#include <limits>
#include <random>
#include <set>

using namespace live2dstruct;
using Planner = CMeshEditPlanner;
namespace {
Planner::Input square(bool clockwise = false)
{
    Planner::Input input{"Mesh", {0, 0, 4, 0, 4, 4, 0, 4}, {0, 1, 2, 0, 2, 3}, {}};
    if (clockwise) for (int i = 0; i < input.indices.size(); i += 3) std::swap(input.indices[i + 1], input.indices[i + 2]);
    return input;
}
Planner::Input ring()
{
    return {"Ring", {-2, -2, 2, -2, 2, 2, -2, 2, -1, -1, 1, -1, 1, 1, -1, 1},
            {0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7}, {}};
}
Planner::Input grid(int width, int height)
{
    Planner::Input input; input.artMeshId = "Grid";
    for (int y = 0; y <= height; ++y) for (int x = 0; x <= width; ++x) input.positions += QVector<float>{float(x), float(y)};
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        const quint16 a = y * (width + 1) + x, b = a + 1, c = b + width + 1, d = a + width + 1;
        input.indices += QVector<quint16>{a, b, c, a, c, d};
    }
    return input;
}
QVector<float> positions(const Planner::Input& input, const Planner::Result& result)
{
    QVector<float> values;
    for (const auto& source : result.edit.vertices) for (int axis = 0; axis < 2; ++axis) {
        double value = 0, total = 0;
        for (double weight : source.weights) total += weight;
        for (int i = 0; i < source.indices.size(); ++i) value += double(input.positions[source.indices[i] * 2 + axis]) * (source.weights[i] / total);
        values.append(float(value));
    }
    return values;
}
Planner::Input applied(const Planner::Input& input, const Planner::Result& result)
{
    return {input.artMeshId, positions(input, result), result.edit.indices, result.protectedEdges};
}
bool hasEdge(const QVector<quint16>& indices, int a, int b)
{
    for (int i = 0; i < indices.size(); i += 3) for (int side = 0; side < 3; ++side) {
        const int x = indices[i + side], y = indices[i + (side + 1) % 3];
        if ((a == x && b == y) || (a == y && b == x)) return true;
    }
    return false;
}
double area(const QVector<float>& xy, const QVector<quint16>& indices)
{
    double sum = 0;
    for (int i = 0; i < indices.size(); i += 3) {
        const int a = indices[i] * 2, b = indices[i + 1] * 2, c = indices[i + 2] * 2;
        sum += ((double(xy[b]) - xy[a]) * (double(xy[c + 1]) - xy[a + 1]) -
                (double(xy[b + 1]) - xy[a + 1]) * (double(xy[c]) - xy[a])) * .5;
    }
    return sum;
}
bool contains(const Planner::Input& input, QPointF point)
{
    const auto p = [&](int i) { return QPointF(input.positions[i * 2], input.positions[i * 2 + 1]); };
    for (int i = 0; i < input.indices.size(); i += 3)
        if (CUvGeometry::ContainsTriangle(point, p(input.indices[i]), p(input.indices[i + 1]), p(input.indices[i + 2]))) return true;
    return false;
}
QMap<quint64, int> edgeCounts(const QVector<quint16>& indices)
{
    QMap<quint64, int> result;
    for (int i = 0; i < indices.size(); i += 3) for (int j = 0; j < 3; ++j) {
        const int a = indices[i + j], b = indices[i + (j + 1) % 3];
        ++result[(quint64(std::min(a, b)) << 32) | quint32(std::max(a, b))];
    }
    return result;
}
}

class MeshEditPlannerTests : public QObject
{
    Q_OBJECT
private slots:
    void spatialPairsMatchIndependentExhaustiveOracle()
    {
        using Box = CBoundsSpatial::Box;
        std::mt19937 random(13891);
        QVector<Box> boxes;
        for (int i = 0; i < 400; ++i) {
            const double x = int(random() % 100) - 50, y = int(random() % 100) - 50;
            boxes.append({ x, x + int(random() % 15), y, y + int(random() % 15) });
        }
        const double huge = std::numeric_limits<double>::max();
        boxes.append({ -huge, huge, -huge, huge });
        boxes.append({ 0, 0, -huge, huge });
        boxes.append({ -huge, huge, 0, 0 });
        for (bool interior : { false, true }) {
            std::set<std::pair<int, int>> expected, actual;
            for (int i = 0; i < boxes.size(); ++i)
                for (int j = i + 1; j < boxes.size(); ++j) {
                    const auto a = boxes[i], b = boxes[j];
                    const double width = std::min(a.right, b.right) - std::max(a.left, b.left);
                    const double height = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
                    if (interior ? width > 0 && height > 0 : width >= 0 && height >= 0)
                        expected.emplace(i, j);
                }
            qint64 work = 0;
            const auto spend = [&] { ++work; };
            const CBoundsSpatial index(boxes, spend);
            index.pairs(interior, spend, [&](int i, int j) { QVERIFY(actual.emplace(i, j).second); });
            QCOMPARE(actual, expected);
            QVERIFY(work < 100000);
        }
    }

    void spatialQueryFreezesInputsAndHonorsBudget()
    {
        QVector<CBoundsSpatial::Box> boxes { { 0, 0, 0, 0 }, { 1, 1, 1, 1 }, { 2, 2, 2, 2 } };
        int work = 0;
        const CBoundsSpatial index(
            boxes,
            [&] {
                boxes.clear();
                ++work;
            },
            CBoundsSpatial::Usage::Queries);
        QVector<int> result;
        index.query({ 0, 1, 0, 1 }, [&] { ++work; }, [&](int i) { result.append(i); });
        std::sort(result.begin(), result.end());
        QCOMPARE(result, (QVector<int> { 0, 1 }));
        QVERIFY(work > 0);
        struct Cancelled
        {
        };
        QVERIFY_EXCEPTION_THROWN(index.pairs(false, [] { throw Cancelled { }; }, [](int, int) { }), Cancelled);
        QVERIFY_EXCEPTION_THROWN(
            CBoundsSpatial((QVector<CBoundsSpatial::Box> { { 1, 0, 0, 0 } }), [] { }), std::invalid_argument);
    }

    void spatialContactsAndNestedIntervalsRemainVisible()
    {
        using Box = CBoundsSpatial::Box;
        const QVector<Box> boxes { { 0, 2, 0, 2 }, { 2, 4, 2, 4 }, { 2, 2, 2, 2 },
            { -100, 100, 1, 1 }, { 1, 1, -100, 100 }, { -4, 4, -4, 4 }, { -.5, .5, -.5, .5 } };
        const CBoundsSpatial index(boxes, [] { }, CBoundsSpatial::Usage::Queries);
        for (bool interior : { false, true }) {
            std::set<std::pair<int, int>> pairs;
            index.pairs(interior, [] { }, [&](int i, int j) { QVERIFY(pairs.emplace(i, j).second); });
            for (int i = 0; i < boxes.size(); ++i)
                for (int j = i + 1; j < boxes.size(); ++j) {
                    const auto a = boxes[i], b = boxes[j];
                    const double width = std::min(a.right, b.right) - std::max(a.left, b.left);
                    const double height = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
                    const bool expected = interior ? width > 0 && height > 0 : width >= 0 && height >= 0;
                    QCOMPARE(pairs.count({ i, j }) != 0, expected);
                }
        }
        for (const auto query : boxes) {
            QSet<int> actual;
            index.query(query, [] { }, [&](int i) { QVERIFY(!actual.contains(i)); actual.insert(i); });
            for (int i = 0; i < boxes.size(); ++i) {
                const auto box = boxes[i];
                const bool expected = std::max(query.left, box.left) <= std::min(query.right, box.right)
                    && std::max(query.top, box.top) <= std::min(query.bottom, box.bottom);
                QCOMPARE(actual.contains(i), expected);
            }
        }
    }

    void pathologicalOverlapStillFailsWithinWorkBudget()
    {
        Planner::Input input;
        input.artMeshId = "ParallelStrips";
        for (int i = 0; i < 2500; ++i) {
            const int first = input.positions.size() / 2;
            const float y = i * 2;
            input.positions.append({ 0, y, 1, y + 20000, 1, y + 20000.5f, 0, y + .5f });
            input.indices.append({ quint16(first), quint16(first + 1), quint16(first + 2),
                quint16(first), quint16(first + 2), quint16(first + 3) });
        }
        Planner::Result output;
        output.edit.artMeshId = "sentinel";
        output.edit.indices = { 9, 8, 7 };
        Planner::Diagnostic diagnostic;
        QVERIFY(!Planner::Connect(input, 0, 1, Planner::CrossingPolicy::RejectProtected, output, &diagnostic));
        QCOMPARE(diagnostic.code, Planner::Code::LimitExceeded);
        QCOMPARE(output.edit.artMeshId, QString("sentinel"));
        QCOMPARE(output.edit.indices, (QVector<quint16> { 9, 8, 7 }));
    }

    void fullUnsignedGridSupportsLocalEditing()
    {
        const auto input = grid(255, 255);
        QCOMPARE(input.positions.size() / 2, 65536);
        QCOMPARE(input.indices.size(), 390150);
        QSet<int> used;
        for (quint16 vertex : input.indices)
            used.insert(vertex);
        QCOMPARE(used.size(), 65536);
        Planner::Result connected, removed, inserted;
        Planner::Diagnostic diagnostic;
        QVERIFY2(
            Planner::Connect(input, 65279, 65534, Planner::CrossingPolicy::RejectProtected, connected, &diagnostic),
            qPrintable(diagnostic.message));
        QVERIFY(hasEdge(connected.edit.indices, 65279, 65534));
        QVERIFY(!hasEdge(connected.edit.indices, 65278, 65535));
        QCOMPARE(connected.edit.vertices.size(), 65536);
        QVERIFY2(Planner::Delete(input, { 65021 }, Planner::BoundaryPolicy::PreserveBoundary, removed, &diagnostic),
            qPrintable(diagnostic.message));
        QCOMPARE(removed.edit.vertices.size(), 65535);
        QCOMPARE(removed.edit.oldToNew[65021], -1);
        QCOMPARE(removed.edit.oldToNew[65535], 65534);
        const auto reduced = applied(input, removed);
        QVERIFY2(Planner::Insert(reduced, { 253, 253 }, -1, inserted, &diagnostic), qPrintable(diagnostic.message));
        QCOMPARE(inserted.edit.vertices.size(), 65536);
        QCOMPARE(inserted.insertedVertices, (QVector<int> { 65535 }));
        QVERIFY(inserted.edit.indices.contains(65535));
        const auto before = inserted.edit.indices;
        QVERIFY(!Planner::Insert(input, { .25, .5 }, -1, inserted, &diagnostic));
        QCOMPARE(diagnostic.code, Planner::Code::LimitExceeded);
        QCOMPARE(inserted.edit.indices, before);
    }

    void fullReconstructionCoversTheConvexHull_data()
    {
        QTest::addColumn<Planner::Input>("input");
        QTest::addColumn<double>("expectedArea");
        QTest::addColumn<bool>("boundaryChanged");
        QTest::newRow("square") << square() << 16.0 << false;
        QTest::newRow("clockwise") << square(true) << -16.0 << false;
        QTest::newRow("fills-hole") << ring() << 16.0 << true;
        QTest::newRow("joins-components") << Planner::Input{"Pieces", {0, 0, 1, 0, 0, 1, 3, 0, 4, 0, 4, 1}, {0, 1, 2, 3, 4, 5}, {}} << 4.0 << true;
        QTest::newRow("fills-concavity") << Planner::Input{"Notch", {0, 0, 4, 0, 4, 1, 1, 1, 1, 4, 0, 4}, {0, 1, 3, 1, 2, 3, 0, 3, 5, 3, 4, 5}, {}} << 11.5 << true;
        auto isolated = square(); isolated.positions += QVector<float>{6, 2};
        QTest::newRow("outside-isolated-point") << isolated << 20.0 << true;
        auto interior = square(); interior.positions += QVector<float>{1, 1, 2, 2, 3, 3, 2, 0, 4, 2, 2, 4, 0, 2};
        QTest::newRow("interior-and-collinear-boundary-points") << interior << 16.0 << true;
        auto empty = interior; empty.indices.clear();
        QTest::newRow("empty-old-topology") << empty << 16.0 << true;
        empty.indices = {99, 0, 1, 0};
        QTest::newRow("invalid-old-topology") << empty << 16.0 << true;
        empty.indices = {99, 0, 1, 0, 0, 1, 0, 2, 1, 0};
        QTest::newRow("first-valid-old-face-is-clockwise") << empty << -16.0 << true;
    }
    void fullReconstructionCoversTheConvexHull()
    {
        QFETCH(Planner::Input, input); QFETCH(double, expectedArea); QFETCH(bool, boundaryChanged);
        const auto originalPositions = input.positions; const auto originalIndices = input.indices;
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::TriangulateVertices(input, result, &error), qPrintable(error.message));
        QCOMPARE(error.code, Planner::Code::None); QCOMPARE(result.edit.artMeshId, input.artMeshId);
        QCOMPARE(result.boundaryChanged, boundaryChanged); QCOMPARE(area(input.positions, result.edit.indices), expectedArea);
        const int vertices = input.positions.size() / 2;
        QCOMPARE(result.edit.vertices.size(), vertices); QCOMPARE(result.edit.oldToNew.size(), vertices);
        QSet<int> used;
        for (int index : result.edit.indices) used.insert(index);
        QCOMPARE(used.size(), vertices); QVERIFY(result.insertedVertices.isEmpty()); QVERIFY(result.protectedEdges.isEmpty());
        for (int index = 0; index < vertices; ++index) {
            QCOMPARE(result.edit.oldToNew[index], index);
            QCOMPARE(result.edit.vertices[index].indices, QVector<int>{index});
            QCOMPARE(result.edit.vertices[index].weights, QVector<double>{1});
        }
        QCOMPARE(positions(input, result), originalPositions);
        for (auto count : edgeCounts(result.edit.indices)) QVERIFY(count == 1 || count == 2);
        const auto p = [&](int v) { return QPointF(input.positions[v * 2], input.positions[v * 2 + 1]); };
        for (int i = 0; i < result.edit.indices.size(); i += 3)
            QCOMPARE(CUvGeometry::Orientation(p(result.edit.indices[i]), p(result.edit.indices[i + 1]), p(result.edit.indices[i + 2])), expectedArea > 0 ? 1 : -1);
        Planner::Result unchanged;
        QVERIFY2(Planner::TriangulateVertices(applied(input, result), unchanged, &error), qPrintable(error.message));
        QCOMPARE(unchanged.edit.indices, result.edit.indices); QVERIFY(!unchanged.boundaryChanged); QVERIFY(unchanged.affectedTriangles.isEmpty());
        QCOMPARE(input.positions, originalPositions); QCOMPARE(input.indices, originalIndices);
    }
    void fullReconstructionIsDeterministicForCocircularPoints()
    {
        Planner::Input input{"Circle", {5, 0, 3, 4, 0, 5, -3, 4, -5, 0, -3, -4, 0, -5, 3, -4, 0, 0}, {}, {}};
        Planner::Result first; Planner::Diagnostic error;
        QVERIFY2(Planner::TriangulateVertices(input, first, &error), qPrintable(error.message));
        QCOMPARE(area(input.positions, first.edit.indices), 70.0);
        for (int attempt = 0; attempt < 8; ++attempt) {
            input.indices = attempt % 2 ? QVector<quint16>{99, 99, 99} : QVector<quint16>{0, 1, 2};
            Planner::Result next;
            QVERIFY2(Planner::TriangulateVertices(input, next, &error), qPrintable(error.message));
            QCOMPARE(next.edit.indices, first.edit.indices);
        }
        auto cocircular = square(); cocircular.indices = {0, 1, 3, 1, 2, 3};
        QVERIFY2(Planner::TriangulateVertices(cocircular, first, &error), qPrintable(error.message));
        QVERIFY(hasEdge(first.edit.indices, 0, 2)); QVERIFY(!hasEdge(first.edit.indices, 1, 3));
    }
    void fullReconstructionDoesNotInheritOldTopologyDefects()
    {
        auto input = square(); input.indices = {0, 1, 2, 2, 1, 0, 0, 0, 2, 1, 2, 99};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::TriangulateVertices(input, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.indices, square().indices); QCOMPARE(area(input.positions, result.edit.indices), 16.0);
        auto safe = ring(); Planner::Result optimized;
        QVERIFY2(Planner::Retriangulate(safe, {0, 1, 2, 3, 4, 5, 6, 7}, optimized, &error), qPrintable(error.message));
        QCOMPARE(area(safe.positions, optimized.edit.indices), 12.0); QVERIFY(!contains(applied(safe, optimized), {0, 0}));
        QVERIFY2(Planner::TriangulateVertices(safe, result, &error), qPrintable(error.message));
        QCOMPARE(area(safe.positions, result.edit.indices), 16.0); QVERIFY(contains(applied(safe, result), {0, 0}));
    }
    void fullReconstructionRejectsInvalidPointsAtomically_data()
    {
        QTest::addColumn<int>("mutation");
        const QStringList names{"empty-id", "nul-id", "empty", "two-points", "odd-coordinates", "nan", "inf", "duplicate", "signed-zero-duplicate", "collinear", "protected", "too-many-vertices"};
        for (int index = 0; index < names.size(); ++index) QTest::newRow(qPrintable(names[index])) << index;
    }
    void fullReconstructionRejectsInvalidPointsAtomically()
    {
        QFETCH(int, mutation); auto input = square();
        switch (mutation) {
        case 0: input.artMeshId.clear(); break;
        case 1: input.artMeshId.append(QChar::Null); break;
        case 2: input.positions.clear(); break;
        case 3: input.positions.resize(4); break;
        case 4: input.positions.append(1); break;
        case 5: input.positions[0] = std::numeric_limits<float>::quiet_NaN(); break;
        case 6: input.positions[1] = -std::numeric_limits<float>::infinity(); break;
        case 7: input.positions += QVector<float>{4, 0}; break;
        case 8: input.positions += QVector<float>{-0.0f, 0.0f}; break;
        case 9: input.positions = {0, 0, 1, 1, 2, 2, -1, -1}; break;
        case 10: input.protectedEdges = {{0, 1}}; break;
        case 11: input.positions.resize((CMocMeshStructureBuilder::MaximumVertices + 1) * 2); break;
        }
        Planner::Result result; result.edit.artMeshId = "keep"; result.edit.indices = {1, 2, 3};
        result.edit.oldToNew = {-1, 0}; result.selectedVertices = {2}; result.insertedVertices = {3};
        result.affectedTriangles = {4}; result.boundaryChanged = true;
        Planner::Diagnostic error;
        QVERIFY(!Planner::TriangulateVertices(input, result, &error));
        QVERIFY(error.code != Planner::Code::None); QVERIFY(!error.message.isEmpty());
        QCOMPARE(result.edit.artMeshId, QString("keep")); QCOMPARE(result.edit.indices, QVector<quint16>({1, 2, 3}));
        QCOMPARE(result.edit.oldToNew, QVector<int>({-1, 0})); QCOMPARE(result.selectedVertices, QVector<int>{2});
        QCOMPARE(result.insertedVertices, QVector<int>{3}); QCOMPARE(result.affectedTriangles, QVector<int>{4}); QVERIFY(result.boundaryChanged);
        if (mutation == 7 || mutation == 8) { QCOMPARE(error.code, Planner::Code::AmbiguousLocation); QCOMPARE(error.vertices.size(), 2); }
        if (mutation == 9) QCOMPARE(error.code, Planner::Code::Degenerate);
        if (mutation == 10) QCOMPARE(error.code, Planner::Code::ProtectedEdge);
    }
    void fullReconstructionHandlesFiniteFloatExtremes_data()
    {
        QTest::addColumn<float>("value");
        QTest::newRow("smallest-positive") << std::numeric_limits<float>::denorm_min();
        QTest::newRow("smallest-normal") << std::numeric_limits<float>::min();
        QTest::newRow("largest") << std::numeric_limits<float>::max();
    }
    void fullReconstructionHandlesFiniteFloatExtremes()
    {
        QFETCH(float, value);
        Planner::Input input{"Extremes", {-value, -value, value, -value, value, value, -value, value, 0, 0}, {}, {}};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::TriangulateVertices(input, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.vertices.size(), 5); QCOMPARE(result.edit.indices.size(), 12);
        QCOMPARE(positions(input, result), input.positions);
        for (int corner = 0; corner < 4; ++corner) QVERIFY(hasEdge(result.edit.indices, corner, 4));
        QCOMPARE(area(input.positions, result.edit.indices), double(value) * double(value) * 4);
    }
    void fullReconstructionBudgetFailureKeepsOutput()
    {
        Planner::Input input; input.artMeshId = "LargeHull";
        for (int index = 0; index < 4096; ++index) input.positions += QVector<float>{float(index), float(index * index)};
        Planner::Result result; result.edit.artMeshId = "keep"; result.edit.indices = {0, 2, 1};
        Planner::Diagnostic error;
        QVERIFY(!Planner::TriangulateVertices(input, result, &error));
        QCOMPARE(error.code, Planner::Code::LimitExceeded); QCOMPARE(result.edit.artMeshId, QString("keep"));
        QCOMPARE(result.edit.indices, QVector<quint16>({0, 2, 1}));
    }
    void fullReconstructionRetainsThinAndTranslatedHulls_data()
    {
        QTest::addColumn<Planner::Input>("input");
        const float maximum = std::numeric_limits<float>::max();
        const float previous = std::nextafter(maximum, 0.0f);
        const float tiny = std::numeric_limits<float>::denorm_min();
        QTest::newRow("thin-full-range") << Planner::Input{"Thin", {0, 0, maximum, 0, maximum, tiny, 0, tiny, 1, 0}, {}, {}};
        QTest::newRow("translated-near-maximum") << Planner::Input{"Translated", {previous, previous, maximum, previous, maximum, maximum, previous, maximum}, {}, {}};
    }
    void fullReconstructionRetainsThinAndTranslatedHulls()
    {
        QFETCH(Planner::Input, input);
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::TriangulateVertices(input, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.vertices.size(), input.positions.size() / 2);
        const QVector<quint16> hull{0, 1, 2, 0, 2, 3};
        QCOMPARE(area(input.positions, result.edit.indices), area(input.positions, hull));
        QSet<int> referenced;
        for (int vertex : result.edit.indices) referenced.insert(vertex);
        QCOMPARE(referenced.size(), input.positions.size() / 2);
        for (int vertex = 0; vertex < input.positions.size() / 2; ++vertex)
            QCOMPARE(result.edit.vertices[vertex].indices, QVector<int>{vertex});
    }
    void insertion_data()
    {
        QTest::addColumn<bool>("clockwise"); QTest::addColumn<QPointF>("point"); QTest::addColumn<int>("faces"); QTest::addColumn<QVector<int>>("source"); QTest::addColumn<QVector<double>>("weights");
        for (bool clockwise : {false, true}) {
            const QByteArray suffix = clockwise ? "-cw" : "-ccw";
            QTest::newRow(("face" + suffix).constData()) << clockwise << QPointF(3, 1) << 4 << QVector<int>{0, 1, 2} << QVector<double>{.25, .5, .25};
            QTest::newRow(("shared-edge" + suffix).constData()) << clockwise << QPointF(2, 2) << 4 << QVector<int>{0, 2} << QVector<double>{.5, .5};
            QTest::newRow(("boundary" + suffix).constData()) << clockwise << QPointF(2, 0) << 3 << QVector<int>{0, 1} << QVector<double>{.5, .5};
        }
    }
    void insertion()
    {
        QFETCH(bool, clockwise); QFETCH(QPointF, point); QFETCH(int, faces); QFETCH(QVector<int>, source); QFETCH(QVector<double>, weights);
        const auto input = square(clockwise); Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Insert(input, point, -1, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.vertices.size(), 5); QCOMPARE(result.edit.indices.size(), faces * 3);
        QCOMPARE(result.edit.vertices.last().indices, source); QCOMPARE(result.edit.vertices.last().weights, weights);
        QCOMPARE(result.edit.oldToNew, QVector<int>({0, 1, 2, 3})); QCOMPARE(result.insertedVertices, QVector<int>{4}); QCOMPARE(result.selectedVertices, QVector<int>{4});
        QCOMPARE(positions(input, result).mid(8), QVector<float>({float(point.x()), float(point.y())}));
        QCOMPARE(area(positions(input, result), result.edit.indices), clockwise ? -16.0 : 16.0);
        for (auto count : edgeCounts(result.edit.indices)) QVERIFY(count == 1 || count == 2);
        for (int i = 0; i < 4; ++i) { QCOMPARE(result.edit.vertices[i].indices, QVector<int>{i}); QCOMPARE(result.edit.vertices[i].weights, QVector<double>{1}); }
        if (point == QPointF(2, 2)) { QVERIFY(!hasEdge(result.edit.indices, 0, 2)); QCOMPARE(result.affectedTriangles, QVector<int>({0, 1})); }
        else QCOMPARE(result.affectedTriangles, QVector<int>{0});
    }
    void insertionOnProtectedEdgeSplitsItsConstraint()
    {
        auto input = square(); input.protectedEdges = {{0, 2}}; Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Insert(input, {2, 2}, 0, result, &error), qPrintable(error.message));
        QCOMPARE(result.protectedEdges, QVector<Planner::Edge>({{0, 4}, {2, 4}}));
        Planner::Result remeshed; QVERIFY2(Planner::Retriangulate(applied(input, result), {0, 1, 2, 3}, remeshed, &error), qPrintable(error.message));
        QVERIFY(hasEdge(remeshed.edit.indices, 0, 4)); QVERIFY(hasEdge(remeshed.edit.indices, 2, 4));
    }
    void existingVertexIsAnExactNoopAndSeedIsChecked()
    {
        const auto input = square(); Planner::Result result; Planner::Diagnostic error;
        QVERIFY(Planner::Insert(input, {4, 0}, 0, result, &error)); QCOMPARE(result.edit.indices, input.indices); QVERIFY(result.insertedVertices.isEmpty()); QCOMPARE(result.selectedVertices, QVector<int>{1});
        result.edit.artMeshId = "keep";
        QVERIFY(!Planner::Insert(input, {3, 1}, 1, result, &error)); QCOMPARE(error.code, Planner::Code::InvalidSelection); QCOMPARE(result.edit.artMeshId, QString("keep"));
        QVERIFY(!Planner::Insert(input, {5, 5}, -1, result, &error)); QCOMPARE(error.code, Planner::Code::OutsideMesh);
    }
    void deletesInteriorConnectedVertexAndPreservesBoundary()
    {
        auto input = square(); input.positions += QVector<float>{2, 2}; input.indices = {0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Delete(input, {4}, Planner::BoundaryPolicy::PreserveBoundary, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.oldToNew, QVector<int>({0, 1, 2, 3, -1})); QCOMPARE(result.edit.vertices.size(), 4); QCOMPARE(result.edit.indices.size(), 6);
        QCOMPARE(area(positions(input, result), result.edit.indices), 16.0); QVERIFY(!result.boundaryChanged);
        QCOMPARE(result.affectedTriangles, QVector<int>({0, 1, 2, 3}));
        for (const auto e : {Planner::Edge{0, 1}, {1, 2}, {2, 3}, {3, 0}}) QVERIFY(hasEdge(result.edit.indices, e.a, e.b));
    }
    void deletesInteriorWithConcaveCavity()
    {
        Planner::Input input{"Concave", {0, 0, 4, 0, 4, 4, 2, 2, 0, 4, 2, 1},
            {0, 1, 5, 1, 2, 5, 2, 3, 5, 3, 4, 5, 4, 0, 5}, {}};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Delete(input, {5}, Planner::BoundaryPolicy::PreserveBoundary, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.indices.size(), 9); QCOMPARE(area(positions(input, result), result.edit.indices), 12.0);
        QVERIFY(!contains(applied(input, result), {2, 3.5}));
    }
    void boundaryDeletionRequiresExplicitPolicy()
    {
        const auto input = square(); Planner::Result result; result.edit.artMeshId = "keep"; Planner::Diagnostic error;
        QVERIFY(!Planner::Delete(input, {1}, Planner::BoundaryPolicy::PreserveBoundary, result, &error)); QCOMPARE(error.code, Planner::Code::BoundaryChange); QCOMPARE(result.edit.artMeshId, QString("keep"));
        QVERIFY2(Planner::Delete(input, {1}, Planner::BoundaryPolicy::ReplaceWithChord, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.oldToNew, QVector<int>({0, -1, 1, 2})); QCOMPARE(result.edit.indices.size(), 3); QVERIFY(result.boundaryChanged); QCOMPARE(area(positions(input, result), result.edit.indices), 8.0);
    }
    void collinearBoundaryDeletionPreservesShape()
    {
        Planner::Input input{"Mesh", {0, 0, 2, 0, 4, 0, 4, 4, 0, 4}, {0, 1, 4, 1, 3, 4, 1, 2, 3}, {}};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Delete(input, {1}, Planner::BoundaryPolicy::PreserveBoundary, result, &error), qPrintable(error.message));
        QVERIFY(!result.boundaryChanged); QCOMPARE(area(positions(input, result), result.edit.indices), 16.0); QCOMPARE(result.edit.indices.size(), 6);
    }
    void isolatedDeletionAndDisconnectedComponents()
    {
        auto input = square(); input.positions += QVector<float>{10, 10, 20, 0, 22, 0, 20, 2}; input.indices += QVector<quint16>{5, 6, 7};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Delete(input, {4}, Planner::BoundaryPolicy::PreserveBoundary, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.oldToNew, QVector<int>({0, 1, 2, 3, -1, 4, 5, 6})); QCOMPARE(result.edit.indices.mid(6), QVector<quint16>({4, 5, 6})); QVERIFY(result.affectedTriangles.isEmpty());
        QVERIFY2(Planner::Insert(input, {3, 1}, 0, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.indices.mid(0, 6), QVector<quint16>({0, 2, 3, 5, 6, 7}));
        result.edit.artMeshId = "keep"; QVERIFY(!Planner::Connect(input, 1, 5, Planner::CrossingPolicy::RejectProtected, result, &error)); QCOMPARE(result.edit.artMeshId, QString("keep"));
    }
    void holesRemainEmptyAndCannotBeBridged()
    {
        const auto input = ring(); Planner::Result result; Planner::Diagnostic error;
        QVERIFY(!Planner::Insert(input, {0, 0}, -1, result, &error)); QCOMPARE(error.code, Planner::Code::OutsideMesh);
        QVERIFY(!Planner::Connect(input, 4, 6, Planner::CrossingPolicy::RejectProtected, result, &error));
        QVERIFY2(Planner::Retriangulate(input, {0, 1, 2, 3, 4, 5, 6, 7}, result, &error), qPrintable(error.message));
        QCOMPARE(area(positions(input, result), result.edit.indices), 12.0); QVERIFY(!contains(applied(input, result), {0, 0}));
        for (const auto e : {Planner::Edge{4, 5}, {5, 6}, {6, 7}, {7, 4}}) QVERIFY(hasEdge(result.edit.indices, e.a, e.b));
    }
    void connectedCorridorCannotFillAConcaveBoundaryGap()
    {
        Planner::Input input{"Notch", {-3, 1, -2, 2, 0, 0, 2, 2, 3, 1, 0, -2},
            {0, 1, 5, 1, 2, 5, 2, 3, 5, 3, 4, 5}, {}};
        Planner::Result result; result.edit.artMeshId = "keep"; Planner::Diagnostic error;
        QVERIFY(!Planner::Connect(input, 0, 4, Planner::CrossingPolicy::RejectProtected, result, &error));
        QCOMPARE(error.code, Planner::Code::OutsideMesh); QCOMPARE(result.edit.artMeshId, QString("keep"));
    }
    void connectCrossesOrdinaryEdgesWithoutAddingVertices()
    {
        for (bool clockwise : {false, true}) {
            const auto input = square(clockwise); Planner::Result result; Planner::Diagnostic error;
            QVERIFY2(Planner::Connect(input, 1, 3, Planner::CrossingPolicy::RejectProtected, result, &error), qPrintable(error.message));
            QCOMPARE(result.edit.vertices.size(), 4); QVERIFY(hasEdge(result.edit.indices, 1, 3)); QVERIFY(!hasEdge(result.edit.indices, 0, 2));
            QCOMPARE(result.protectedEdges, QVector<Planner::Edge>({{1, 3}})); QCOMPARE(area(positions(input, result), result.edit.indices), clockwise ? -16.0 : 16.0);
        }
    }
    void connectProtectedCrossingIsExplicitAndWeighted()
    {
        auto input = square(); input.protectedEdges = {{0, 2}}; Planner::Result result; result.edit.artMeshId = "keep"; Planner::Diagnostic error;
        QVERIFY(!Planner::Connect(input, 1, 3, Planner::CrossingPolicy::RejectProtected, result, &error)); QCOMPARE(error.code, Planner::Code::ProtectedEdge); QCOMPARE(result.edit.artMeshId, QString("keep"));
        QVERIFY2(Planner::Connect(input, 1, 3, Planner::CrossingPolicy::SplitProtected, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.vertices.size(), 5); QCOMPARE(result.edit.indices.size(), 12); QCOMPARE(result.edit.vertices[4].indices, QVector<int>({0, 2})); QCOMPARE(result.edit.vertices[4].weights, QVector<double>({.5, .5}));
        for (int v = 0; v < 4; ++v) QVERIFY(hasEdge(result.edit.indices, v, 4));
        QCOMPARE(result.protectedEdges.size(), 4); QCOMPARE(result.selectedVertices, QVector<int>({1, 3, 4}));
    }
    void connectUsesExistingCollinearVertexChain()
    {
        auto input = square(); input.positions += QVector<float>{2, 2}; input.indices = {0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Connect(input, 1, 3, Planner::CrossingPolicy::RejectProtected, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.indices, input.indices); QCOMPARE(result.protectedEdges.size(), 2); QCOMPARE(result.selectedVertices, QVector<int>({1, 3, 4})); QVERIFY(result.insertedVertices.isEmpty());
    }
    void connectEmbedsAnExistingIsolatedVertexWithoutChangingItsSource()
    {
        auto input = square(); input.positions += QVector<float>{3, 1}; Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Insert(input, {3, 1}, -1, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.indices, input.indices); QCOMPARE(result.edit.vertices.size(), 5); QCOMPARE(result.selectedVertices, QVector<int>{4});
        QVERIFY2(Planner::Connect(input, 4, 3, Planner::CrossingPolicy::RejectProtected, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.vertices.size(), 5); QCOMPARE(result.edit.vertices[4].indices, QVector<int>{4}); QCOMPARE(result.edit.vertices[4].weights, QVector<double>{1}); QVERIFY(result.insertedVertices.isEmpty()); QVERIFY(hasEdge(result.edit.indices, 4, 3));
    }
    void localRetriangulationKeepsUnselectedFacesAndProtectedEdges()
    {
        Planner::Input input{"Mesh", {0, 0, 4, 0, 3, 2, 0, 4, 8, 0, 10, 0, 8, 2}, {0, 1, 3, 1, 2, 3, 4, 5, 6}, {}};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Retriangulate(input, {0, 1}, result, &error), qPrintable(error.message));
        QVERIFY(hasEdge(result.edit.indices, 0, 2)); QVERIFY(!hasEdge(result.edit.indices, 1, 3)); QCOMPARE(result.edit.indices.mid(6), input.indices.mid(6)); QCOMPARE(result.affectedTriangles, QVector<int>({0, 1}));
        const auto first = result.edit.indices;
        QVERIFY2(Planner::Retriangulate(applied(input, result), {0, 1}, result, &error), qPrintable(error.message)); QCOMPARE(result.edit.indices, first); QVERIFY(result.affectedTriangles.isEmpty());
        input.protectedEdges = {{1, 3}};
        QVERIFY2(Planner::Retriangulate(input, {0, 1}, result, &error), qPrintable(error.message)); QCOMPARE(result.edit.indices, input.indices);
        input.protectedEdges.clear(); QVERIFY(Planner::Retriangulate(input, {0}, result, &error)); QCOMPARE(result.edit.indices, input.indices);
    }
    void multiFaceCorridorAndAdjacentDeletion()
    {
        const auto input = grid(4, 3); Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Connect(input, 1, 18, Planner::CrossingPolicy::RejectProtected, result, &error), qPrintable(error.message));
        QVERIFY(hasEdge(result.edit.indices, 1, 18)); QCOMPARE(result.edit.vertices.size(), 20); QCOMPARE(area(positions(input, result), result.edit.indices), 12.0);
        QVERIFY(result.affectedTriangles.size() > 2);
        QVERIFY2(Planner::Delete(input, {6, 7, 11, 12}, Planner::BoundaryPolicy::PreserveBoundary, result, &error), qPrintable(error.message));
        QCOMPARE(result.edit.vertices.size(), 16); QCOMPARE(area(positions(input, result), result.edit.indices), 12.0);
        for (int index : {6, 7, 11, 12}) QCOMPARE(result.edit.oldToNew[index], -1);
        for (auto count : edgeCounts(result.edit.indices)) QVERIFY(count <= 2);
    }
    void everyGridVertexPairCanBeConnectedWithoutChangingTheDomain()
    {
        const auto input = grid(4, 3); Planner::Diagnostic error;
        for (int first = 0; first < 20; ++first) for (int second = first + 1; second < 20; ++second) {
            Planner::Result result;
            QVERIFY2(Planner::Connect(input, first, second, Planner::CrossingPolicy::RejectProtected, result, &error),
                     qPrintable(QString("%1 -> %2: %3").arg(first).arg(second).arg(error.message)));
            QCOMPARE(result.edit.vertices.size(), 20); QCOMPARE(area(positions(input, result), result.edit.indices), 12.0);
            QVERIFY(result.selectedVertices.contains(first)); QVERIFY(result.selectedVertices.contains(second));
            for (const auto& e : result.protectedEdges) QVERIFY(hasEdge(result.edit.indices, e.a, e.b));
        }
    }
    void boundaryHoleVertexCanBeRemovedWithAnExplicitContourChange()
    {
        const auto input = ring(); Planner::Result result; Planner::Diagnostic error;
        QVERIFY(!Planner::Delete(input, {4}, Planner::BoundaryPolicy::PreserveBoundary, result, &error)); QCOMPARE(error.code, Planner::Code::BoundaryChange);
        QVERIFY2(Planner::Delete(input, {4}, Planner::BoundaryPolicy::ReplaceWithChord, result, &error), qPrintable(error.message));
        QVERIFY(result.boundaryChanged); QCOMPARE(area(positions(input, result), result.edit.indices), 14.0);
        QVERIFY(!contains(applied(input, result), {.5, .5}));
        const auto before = result.edit.indices;
        QVERIFY(!Planner::Delete(input, {4, 5, 6, 7}, Planner::BoundaryPolicy::ReplaceWithChord, result, &error)); QCOMPARE(result.edit.indices, before);
    }
    void repeatedLocalOperationsRemainValidAndDeterministic()
    {
        auto input = grid(7, 5); Planner::Result result, again; Planner::Diagnostic error;
        for (int operation = 0; operation < 8; ++operation) {
            const int face = operation * 7;
            const int a = input.indices[face * 3], b = input.indices[face * 3 + 1], c = input.indices[face * 3 + 2];
            const QPointF point((double(input.positions[a * 2]) + input.positions[b * 2] + input.positions[c * 2]) / 3,
                                (double(input.positions[a * 2 + 1]) + input.positions[b * 2 + 1] + input.positions[c * 2 + 1]) / 3);
            QVERIFY2(Planner::Insert(input, point, face, result, &error), qPrintable(error.message));
            QVERIFY2(Planner::Insert(input, point, face, again, &error), qPrintable(error.message)); QCOMPARE(again.edit.indices, result.edit.indices);
            input = applied(input, result);
        }
        QVector<int> selection; for (int i = 0; i < input.indices.size() / 3; ++i) selection.append(i);
        QVERIFY2(Planner::Retriangulate(input, selection, result, &error), qPrintable(error.message));
        const auto output = applied(input, result); QCOMPARE(area(output.positions, output.indices), 35.0);
        QVERIFY2(Planner::Retriangulate(output, selection, again, &error), qPrintable(error.message)); QCOMPARE(again.edit.indices, result.edit.indices);
    }
    void constraintsArePreservedAcrossSeveralCrossings()
    {
        auto input = grid(4, 2); input.protectedEdges = {{1, 6}, {6, 11}, {3, 8}, {8, 13}};
        input.positions += QVector<float>{.25f, .75f, 3.75f, 1.25f};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Connect(input, 15, 16, Planner::CrossingPolicy::SplitProtected, result, &error), qPrintable(error.message));
        QCOMPARE(result.insertedVertices.size(), 2); QCOMPARE(result.selectedVertices.size(), 5);
        QVERIFY(result.selectedVertices.contains(7));
        QCOMPARE(area(positions(input, result), result.edit.indices), 8.0);
        for (const auto& e : result.protectedEdges) QVERIFY(hasEdge(result.edit.indices, e.a, e.b));
        for (int index : result.insertedVertices) {
            QCOMPARE(result.edit.vertices[index].indices.size(), 2);
            double sum = 0; for (double weight : result.edit.vertices[index].weights) { QVERIFY(weight >= 0); sum += weight; } QCOMPARE(sum, 1.0);
        }
    }
    void invalidInputsRejectAtomically_data()
    {
        QTest::addColumn<int>("mutation"); for (int i = 0; i < 13; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidInputsRejectAtomically()
    {
        QFETCH(int, mutation); auto input = square();
        switch (mutation) {
        case 0: input.artMeshId.clear(); break;
        case 1: input.positions[0] = std::numeric_limits<float>::quiet_NaN(); break;
        case 2: input.indices[0] = 99; break;
        case 3: input.indices.removeLast(); break;
        case 4: input.indices = {0, 1, 2, 2, 1, 0}; break;
        case 5: input.indices[1] = 0; break;
        case 6: input.positions[5] = 0; break;
        case 7: input.indices = {0, 1, 2, 0, 3, 2}; break;
        case 8: input.positions += QVector<float>{2, -2}; input.indices += QVector<quint16>{0, 2, 4}; break;
        case 9: input.positions += QVector<float>{-2, 0, 0, -2}; input.indices += QVector<quint16>{0, 4, 5}; break;
        case 10: input.positions += QVector<float>{1, 1, 2, 1, 1, 2}; input.indices += QVector<quint16>{4, 5, 6}; break;
        case 11: input.protectedEdges = {{1, 3}}; break;
        case 12: input.protectedEdges = {{1, 99}}; break;
        }
        const auto before = input.indices; Planner::Result result; result.edit.artMeshId = "keep"; result.edit.indices = {1, 2, 3}; Planner::Diagnostic error;
        QVERIFY(!Planner::Insert(input, {3, 1}, -1, result, &error)); QVERIFY(error.code != Planner::Code::None); QVERIFY(!error.message.isEmpty()); QCOMPARE(result.edit.artMeshId, QString("keep")); QCOMPARE(result.edit.indices, QVector<quint16>({1, 2, 3})); QCOMPARE(input.indices, before);
    }
    void invalidOperationsAndLateFailureKeepResult()
    {
        const auto input = square(); Planner::Result result; result.edit.artMeshId = "keep"; Planner::Diagnostic error;
        QVERIFY(!Planner::Insert(input, {std::numeric_limits<double>::infinity(), 0}, -1, result, &error));
        QVERIFY(!Planner::Delete(input, {0, 1, 2, 3}, Planner::BoundaryPolicy::ReplaceWithChord, result, &error));
        QVERIFY(!Planner::Delete(input, {0}, Planner::BoundaryPolicy(99), result, &error));
        QVERIFY(!Planner::Connect(input, 0, 0, Planner::CrossingPolicy::RejectProtected, result, &error));
        QVERIFY(!Planner::Connect(input, 0, 99, Planner::CrossingPolicy::RejectProtected, result, &error));
        QVERIFY(!Planner::Retriangulate(input, {99}, result, &error)); QVERIFY(!Planner::Retriangulate(input, {}, result, &error));
        QCOMPARE(result.edit.artMeshId, QString("keep"));
        auto constrained = input; constrained.protectedEdges = {{0, 1}};
        QVERIFY(!Planner::Delete(constrained, {0}, Planner::BoundaryPolicy::ReplaceWithChord, result, &error)); QCOMPARE(error.code, Planner::Code::ProtectedEdge);
    }
    void finiteExtremeReferenceCoordinates_data()
    {
        QTest::addColumn<float>("scale"); QTest::newRow("large") << std::numeric_limits<float>::max(); QTest::newRow("small") << std::numeric_limits<float>::denorm_min() * 16;
    }
    void finiteExtremeReferenceCoordinates()
    {
        QFETCH(float, scale); Planner::Input input{"Mesh", {0, 0, scale, 0, scale, scale, 0, scale}, {0, 1, 2, 0, 2, 3}, {}};
        Planner::Result result; Planner::Diagnostic error;
        QVERIFY2(Planner::Insert(input, {double(scale) * .5, double(scale) * .5}, -1, result, &error), qPrintable(error.message)); QCOMPARE(result.edit.vertices.last().weights, QVector<double>({.5, .5}));
        QVERIFY2(Planner::Retriangulate(input, {0, 1}, result, &error), qPrintable(error.message)); QCOMPARE(result.edit.indices, input.indices);
        QVERIFY2(Planner::Connect(input, 1, 3, Planner::CrossingPolicy::RejectProtected, result, &error), qPrintable(error.message)); QVERIFY(hasEdge(result.edit.indices, 1, 3));
    }
    void roundsTinyInsertionWithoutCreatingDegenerateTriangles()
    {
        const float tiny = std::numeric_limits<float>::denorm_min(); Planner::Input input{"Mesh", {0, 0, tiny, 0, 0, tiny}, {0, 1, 2}, {}};
        Planner::Result result; result.edit.artMeshId = "keep"; Planner::Diagnostic error;
        QVERIFY(!Planner::Insert(input, {double(tiny) / 4, double(tiny) / 4}, -1, result, &error)); QCOMPARE(error.code, Planner::Code::PrecisionLoss); QCOMPARE(result.edit.artMeshId, QString("keep"));
    }
    void resourceLimitsRejectWithoutPublishingPartialResults()
    {
        auto input = square(); input.positions.resize((CMocMeshStructureBuilder::MaximumVertices + 1) * 2);
        Planner::Result result; result.edit.artMeshId = "keep"; Planner::Diagnostic error;
        QVERIFY(!Planner::Insert(input, {1, 1}, -1, result, &error)); QCOMPARE(result.edit.artMeshId, QString("keep"));
        input = {"Fan", {0, 0}, {}, {}};
        constexpr int count = 4096;
        for (int i = 0; i < count; ++i) {
            const double angle = i * 6.2831853071795864769 / count;
            input.positions.append(float(std::cos(angle))); input.positions.append(float(std::sin(angle)));
            input.indices += QVector<quint16>{0, quint16(i + 1), quint16((i + 1) % count + 1)};
        }
        QVERIFY(!Planner::Retriangulate(input, {0}, result, &error)); QCOMPARE(error.code, Planner::Code::LimitExceeded); QCOMPARE(result.edit.artMeshId, QString("keep"));
    }
};
QTEST_GUILESS_MAIN(MeshEditPlannerTests)
#include "mesh_edit_planner_tests.moc"
