#include "live2dstruct/cmesheditplanner.h"
#include "opengl/cuvgeometry.h"
#include <QtTest>
#include <QMap>
#include <cmath>
#include <limits>

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
