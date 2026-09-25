#include "edit/ctriangl.h"

#include <QtTest>
#include <cmath>
#include <limits>

class TrianglMeshTests : public QObject
{
    Q_OBJECT

private slots:
    void picksDisconnectedTrianglesWithUnorderedVertices()
    {
        const float positions[] = {0, 0, 4, 0, 2, 0, 6, 0, 0, 2, 4, 2};
        const short indices[] = {0, 2, 4, 1, 5, 3};
        CTrianglMesh mesh(6, indices, 12, positions, positions);
        QVERIFY(mesh.GetContentPoint({0.5, 0.5}));
        QVERIFY(mesh.GetContentPoint({4.5, 0.5}));
        QVERIFY(!mesh.GetContentPoint({3, 0.5}));
        QVERIFY(!mesh.GetContentPoint({1.5, 1.5}));
        QVERIFY(mesh.GetContentPoint({0, 0}));
        QVERIFY(mesh.GetContentPoint({1, 1}));
    }

    void leavesMeshHolesUnpickable()
    {
        const float positions[] = {-2, -2, 2, -2, 2, 2, -2, 2, -1, -1, 1, -1, 1, 1, -1, 1};
        const short indices[] = {0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
                                 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
        CTrianglMesh mesh(24, indices, 16, positions, positions);
        for (const QPointF& point : {QPointF(0, -1.5), QPointF(1.5, 0), QPointF(0, 1.5), QPointF(-1.5, 0)})
            QVERIFY(mesh.GetContentPoint(point));
        QVERIFY(!mesh.GetContentPoint({0, 0}));
        QVERIFY(!mesh.GetContentPoint({0.9, 0.9}));
        QVERIFY(!mesh.GetContentPoint({3, 0}));
        QVERIFY(mesh.GetContentPoint({1, 0}));
    }

    void topologyChangesRefreshPickingAndBuffers()
    {
        const float positions[] = {0, 0, 2, 0, 2, 2, 0, 2};
        const short indices[] = {0, 1, 2};
        CTrianglMesh mesh(3, indices, 8, positions, positions);
        QVERIFY(mesh.GetContentPoint({1.5, 0.5}));
        QVERIFY(!mesh.GetContentPoint({0.5, 1.5}));
        QVERIFY(mesh.SetIndices({0, 2, 3}));
        QVERIFY(!mesh.GetContentPoint({1.5, 0.5}));
        QVERIFY(mesh.GetContentPoint({0.5, 1.5}));
        QCOMPARE(mesh.GetEBOSize(), 3);
        QCOMPARE(mesh.GetEBOIndex()[1], short(2));
        QCOMPARE(mesh.GetEdge().size(), size_t(3));
        QCOMPARE(mesh.GetEdge()[0]->start, 0);
        QCOMPARE(mesh.GetEdge()[0]->end, 2);
        QVERIFY(!mesh.SetIndices({0, 4, 2}));
        QVERIFY(!mesh.SetIndices({0, 1}));
        QVERIFY(!mesh.SetIndices({}));
        QVERIFY(mesh.GetContentPoint({0.5, 1.5}));
        QCOMPARE(mesh.GetEBOIndex()[2], short(3));
    }

    void positionChangesRefreshPickingAndBounds()
    {
        const float positions[] = {0, 0, 2, 0, 0, 2};
        const short indices[] = {0, 1, 2};
        CTrianglMesh mesh(3, indices, 6, positions, positions);
        QVERIFY(mesh.SetPositions({4, 4, 6, 4, 4, 6}));
        QCOMPARE(mesh.GetRectF(), QRectF(4, 4, 2, 2));
        QVERIFY(!mesh.GetContentPoint({0.5, 0.5}));
        QVERIFY(mesh.GetContentPoint({4.5, 4.5}));
        for (CPoint* point : mesh.GetCpoint()) point->SetIsSelect(true);
        mesh.MoveSelected({2, -2});
        QCOMPARE(mesh.GetRectF(), QRectF(6, 2, 2, 2));
        QVERIFY(!mesh.GetContentPoint({4.5, 4.5}));
        QVERIFY(mesh.GetContentPoint({6.5, 2.5}));
        QCOMPARE(mesh.GetPos()[0], 6.0f);
        const auto before = mesh.Positions();
        QVERIFY(!mesh.SetPositions({0, 0}));
        QVERIFY(!mesh.SetPositions({0, 0, 2, 0, 0, std::numeric_limits<float>::infinity()}));
        QCOMPARE(mesh.Positions(), before);
    }

    void invalidConstructorInputsLeaveAnEmptyMesh()
    {
        const float positions[] = {0, 0, 2, 0, 0, 2};
        const float invalid[] = {0, 0, 2, 0, 0, std::numeric_limits<float>::quiet_NaN()};
        const short indices[] = {0, 1, 2};
        const short negative[] = {0, -1, 2};
        const short outOfRange[] = {0, 1, 3};
        struct Input { int count; const short* indices; int size; const float* uv; const float* position; };
        const Input inputs[] = {
            {-1, indices, 6, positions, positions}, {2, indices, 6, positions, positions},
            {3000003, indices, 6, positions, positions}, {3, nullptr, 6, positions, positions},
            {3, indices, -2, positions, positions}, {3, indices, 5, positions, positions},
            {3, indices, 2000002, positions, positions}, {3, indices, 6, nullptr, positions},
            {3, indices, 6, positions, nullptr}, {3, negative, 6, positions, positions},
            {3, outOfRange, 6, positions, positions}, {3, indices, 6, invalid, positions},
            {3, indices, 6, positions, invalid}, {3, indices, 0, nullptr, nullptr},
            {0, nullptr, 0, nullptr, nullptr}
        };
        for (const Input& input : inputs) {
            CTrianglMesh mesh(input.count, input.indices, input.size, input.uv, input.position);
            QVERIFY(mesh.Positions().isEmpty());
            QCOMPARE(mesh.GetEBOSize(), 0);
            QVERIFY(mesh.GetEdge().empty());
            QVERIFY(!mesh.GetContentPoint({0, 0}));
            QVERIFY(!mesh.AddPoint({0, 0}));
            QVERIFY(!mesh.SetIndices({0, 1, 2}));
            mesh.MoveSelected({1, 1});
        }
    }

    void rejectsUnsupportedIndicesWithoutDroppingValidTopology()
    {
        QVector<float> positions(65538, 0);
        positions[2] = 2;
        positions[65535] = 2;
        const short indices[] = {0, 1, 32767};
        CTrianglMesh mesh(3, indices, positions.size(), positions.data(), positions.data());
        QVERIFY(mesh.GetContentPoint({0.5, 0.5}));
        QVERIFY(!mesh.SetIndices({0, 1, 32768}));
        QVERIFY(!mesh.AddPoint({0.5, 0.5}));
        QCOMPARE(mesh.GetEBOIndex()[2], short(32767));
        QVERIFY(mesh.GetContentPoint({0.5, 0.5}));
    }

    void ignoresDegenerateTrianglesAndNonfiniteQueries()
    {
        const float positions[] = {0, 0, 1, 1, 2, 2, 0, 2};
        const short indices[] = {0, 1, 2, 0, 0, 3};
        CTrianglMesh mesh(6, indices, 8, positions, positions);
        QVERIFY(!mesh.GetContentPoint({1, 1}));
        QVERIFY(!mesh.GetContentPoint({0, 1}));
        QVERIFY(!mesh.AddPoint({0.5, 0.5}));
        QVERIFY(mesh.SetIndices({0, 2, 3}));
        QVERIFY(mesh.GetContentPoint({0.5, 1}));
        QVERIFY(!mesh.GetContentPoint({std::numeric_limits<double>::infinity(), 1}));
        QVERIFY(!mesh.GetContentPoint({0, std::numeric_limits<double>::quiet_NaN()}));
        QVERIFY(!mesh.GetContentPoint({std::numeric_limits<double>::max(), 1}));
        QVERIFY(!mesh.AddPoint({std::numeric_limits<float>::infinity(), 1}));
    }

    void boundaryInsertionLeavesAdjacentTrianglesIntact()
    {
        const float positions[] = {0, 0, 2, 0, 2, 2, 0, 2};
        const short indices[] = {0, 1, 2, 0, 2, 3};
        CTrianglMesh mesh(6, indices, 8, positions, positions);
        QVERIFY(mesh.GetContentPoint({1, 1}));
        QVERIFY(!mesh.AddPoint({1, 1}));
        QCOMPARE(mesh.GetWolrd().size(), size_t(4));
        QCOMPARE(mesh.GetEBOSize(), 6);
        for (int i = 0; i < 6; ++i) QCOMPARE(mesh.GetEBOIndex()[i], indices[i]);
        QVERIFY(mesh.GetContentPoint({0.5, 1.5}));
        QVERIFY(mesh.GetContentPoint({1.5, 0.5}));
    }

    void finiteExtremeTrianglesRemainPickable()
    {
        const short indices[] = {0, 1, 2};
        const float uv[] = {0, 0, 1, 0, 0, 1};
        for (float scale : {std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min() * 16}) {
            const float positions[] = {0, 0, scale, 0, 0, scale};
            CTrianglMesh mesh(3, indices, 6, uv, positions);
            QVERIFY(mesh.GetContentPoint({double(scale) / 4, double(scale) / 4}));
            QVERIFY(!mesh.GetContentPoint({double(scale) * 0.75, double(scale) * 0.75}));
            QVERIFY(mesh.AddPoint({scale / 4, scale / 4}));
            QCOMPARE(mesh.GetUV()[6], 0.25f);
            QCOMPARE(mesh.GetUV()[7], 0.25f);
        }
        const float huge = std::numeric_limits<float>::max();
        const float positions[] = {-huge, -huge, huge, -huge, -huge, huge};
        CTrianglMesh mesh(3, indices, 6, uv, positions);
        QVERIFY(std::isfinite(mesh.GetRectF().width()));
        QVERIFY(mesh.GetContentPoint({-huge / 4, -huge / 4}));
        QVERIFY(!mesh.GetContentPoint({huge / 4, huge / 4}));
    }

    void nearestPointAndMovementAvoidFloatOverflow()
    {
        const float huge = std::numeric_limits<float>::max();
        const float positions[] = {huge, 0, 0, 0, 0, 1};
        const short indices[] = {0, 1, 2};
        CTrianglMesh mesh(3, indices, 6, positions, positions);
        QVERIFY(mesh.SelectNearestPoint({-huge, 0}, huge));
        QVERIFY(!mesh.GetWolrd()[0]->GetIsSelect());
        QVERIFY(mesh.GetWolrd()[1]->GetIsSelect() || mesh.GetWolrd()[2]->GetIsSelect());
        QVERIFY(!mesh.SelectNearestPoint({-huge, 0}, huge / 2));
        for (CPoint* point : mesh.GetCpoint()) point->SetIsSelect(true);
        const auto before = mesh.Positions();
        mesh.MoveSelected({huge, 1});
        QCOMPARE(mesh.Positions(), before);
        mesh.MoveSelected({std::numeric_limits<float>::quiet_NaN(), 0});
        QCOMPARE(mesh.Positions(), before);
        QVERIFY(!mesh.SelectNearestPoint({std::numeric_limits<float>::infinity(), 0}, 1));
        QVERIFY(!mesh.SelectNearestPoint({0, 0}, -1));
        mesh.GetCpoint()[0]->SetPoint({std::numeric_limits<float>::infinity(), 0});
        QCOMPARE(mesh.Positions(), before);
    }

    void interiorInsertionInterpolatesUvAndPreservesWinding()
    {
        const float positions[] = {0, 0, 4, 0, 0, 4};
        const float uv[] = {-1, 2, 3, 4, 7, -2};
        for (bool clockwise : {false, true}) {
            const short indices[] = {0, short(clockwise ? 2 : 1), short(clockwise ? 1 : 2)};
            CTrianglMesh mesh(3, indices, 6, uv, positions);
            QVERIFY(mesh.AddPoint({1, 1}));
            QCOMPARE(mesh.GetWolrd().size(), size_t(4));
            QCOMPARE(mesh.GetEBOSize(), 9);
            QCOMPARE(mesh.GetEdge().size(), size_t(9));
            QCOMPARE(mesh.GetUV()[6], 2.0f);
            QCOMPARE(mesh.GetUV()[7], 1.5f);
            QCOMPARE(mesh.GetPos()[6], 1.0f);
            QCOMPARE(mesh.GetPos()[7], 1.0f);
            for (int i = 0; i < mesh.GetEBOSize(); i += 3) {
                const QVector2D a = mesh.GetWolrd()[mesh.GetEBOIndex()[i]]->GetPoint();
                const QVector2D b = mesh.GetWolrd()[mesh.GetEBOIndex()[i + 1]]->GetPoint();
                const QVector2D c = mesh.GetWolrd()[mesh.GetEBOIndex()[i + 2]]->GetPoint();
                const float area = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
                QVERIFY(clockwise ? area < 0 : area > 0);
            }
            QVERIFY(mesh.GetContentPoint({0.5, 0.5}));
            QVERIFY(mesh.GetContentPoint({3, 0.5}));
            QVERIFY(mesh.GetContentPoint({0.5, 3}));
            const auto before = mesh.Positions();
            for (const QVector2D& point : {QVector2D(1, 1), QVector2D(2, 0), QVector2D(0, 0), QVector2D(3, 3)})
                QVERIFY(!mesh.AddPoint(point));
            QCOMPARE(mesh.Positions(), before);
            QCOMPARE(mesh.GetEBOSize(), 9);
        }
    }
};

QTEST_APPLESS_MAIN(TrianglMeshTests)
#include "triangl_mesh_tests.moc"
