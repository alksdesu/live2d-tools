#include "live2dstruct/cmeshtopology.h"

#include <QtTest>

using live2dstruct::CMeshTopology;

class MeshTopologyTests : public QObject
{
    Q_OBJECT
private slots:
    void validationKeepsExistingFormatLimits()
    {
        QVERIFY(CMeshTopology::IsValid({0, 1, 2}, 3));
        QVERIFY(CMeshTopology::IsValid({0, 32767, 1}, 32768));
        QVERIFY(CMeshTopology::IsValid({0, 0, 1, 0, 0, 1}, 3));
        QVERIFY(!CMeshTopology::IsValid({}, 3));
        QVERIFY(!CMeshTopology::IsValid({0, 1}, 3));
        QVERIFY(!CMeshTopology::IsValid({0, 1, 3}, 3));
        QVERIFY(!CMeshTopology::IsValid({0, 1, 32768}, 65536));
        QVERIFY(!CMeshTopology::IsValid({0, 1, 2}, 0));
        QVERIFY(!CMeshTopology::IsValid({0, 1, 2}, -1));
        QVector<quint16> indices(3000000, 0);
        QVERIFY(CMeshTopology::IsValid(indices, 1));
        indices.append({0, 0, 0});
        QVERIFY(!CMeshTopology::IsValid(indices, 1));
    }

    void mergeRemovesNonadjacentDuplicates()
    {
        const QVector<quint16> input{0, 2, 3, 0, 3, 4, 1, 2, 3};
        QVector<quint16> output;
        QVERIFY(CMeshTopology::MergeVertices(input, {1, 0}, 5, &output));
        QCOMPARE(output, (QVector<quint16>{0, 2, 3, 0, 3, 4}));
        QCOMPARE(input, (QVector<quint16>{0, 2, 3, 0, 3, 4, 1, 2, 3}));
    }

    void mergeKeepsFirstWindingAndCyclicOrder()
    {
        const QVector<quint16> input{2, 3, 1, 0, 3, 4, 3, 0, 2, 3, 2, 0, 2, 0, 3};
        QVector<quint16> output;
        QVERIFY(CMeshTopology::MergeVertices(input, {0, 1}, 5, &output));
        QCOMPARE(output, (QVector<quint16>{2, 3, 0, 0, 3, 4, 3, 2, 0}));
        QVERIFY(CMeshTopology::IsValid(output, 5));
    }

    void cleanupSharesOrientedDeduplication()
    {
        QVector<quint16> output;
        QVERIFY(CMeshTopology::CleanTriangles({2, 3, 0, 1, 1, 2, 0, 2, 3, 3, 0, 2, 0, 3, 2}, 4, &output));
        QCOMPARE(output, (QVector<quint16>{2, 3, 0, 0, 3, 2}));
        QVector<quint16> repeated = output;
        QVERIFY(CMeshTopology::CleanTriangles(repeated, 4, &repeated));
        QCOMPARE(repeated, output);
        QVERIFY(CMeshTopology::MergeVertices(repeated, {1, 2}, 4, &repeated));
        QCOMPARE(repeated, (QVector<quint16>{1, 3, 0, 0, 3, 1}));
    }

    void invalidInputsKeepOutputUntouched()
    {
        const QVector<quint16> initial{7, 8, 9};
        QVector<quint16> output = initial;
        for (const auto& selection : {QSet<int>{}, QSet<int>{0}, QSet<int>{-1, 0}, QSet<int>{0, 5}, QSet<int>{0, 32768}}) {
            QVERIFY(!CMeshTopology::MergeVertices({0, 2, 3}, selection, 5, &output));
            QCOMPARE(output, initial);
        }
        QVERIFY(!CMeshTopology::MergeVertices({0, 2, 3}, {0, 1}, 5, nullptr));
        QVERIFY(!CMeshTopology::CleanTriangles({0, 2, 3}, 5, nullptr));
        for (const auto& invalid : {QVector<quint16>{}, QVector<quint16>{0, 1}, QVector<quint16>{0, 1, 5},
                                    QVector<quint16>{0, 1, 32768}}) {
            QVERIFY(!CMeshTopology::MergeVertices(invalid, {0, 1}, 5, &output));
            QCOMPARE(output, initial);
            QVERIFY(!CMeshTopology::CleanTriangles(invalid, 5, &output));
            QCOMPARE(output, initial);
        }
        QVERIFY(!CMeshTopology::MergeVertices({0, 1, 2}, {0, 1}, 3, &output));
        QCOMPARE(output, initial);
        QVERIFY(!CMeshTopology::CleanTriangles({0, 0, 2}, 3, &output));
        QCOMPARE(output, initial);
    }

    void mergeCanReturnUnchangedTopology()
    {
        const QVector<quint16> input{0, 2, 3, 0, 3, 4};
        QVector<quint16> output;
        QVERIFY(CMeshTopology::MergeVertices(input, {0, 1}, 5, &output));
        QCOMPARE(output, input);
        QVERIFY(CMeshTopology::MergeVertices(input, {5, 6}, 7, &output));
        QCOMPARE(output, input);
    }
};

QTEST_GUILESS_MAIN(MeshTopologyTests)
#include "mesh_topology_tests.moc"
