#include "live2dstruct/cparentgraph.h"

#include <QtTest>
#include <QSet>
#include <limits>

using live2dstruct::CParentGraph;

class ParentGraphTests : public QObject
{
    Q_OBJECT
private slots:
    void unorderedForestsPreserveStableChildren()
    {
        const QVector<int> parents{3, 3, -1, 2, -1, 1, 2, 4};
        CParentGraph graph(parents);
        QVERIFY(graph.IsValid());
        QCOMPARE(graph.Parents(), parents);
        QCOMPARE(graph.Roots(), (QVector<int>{2, 4}));
        QCOMPARE(graph.TopologicalOrder(), (QVector<int>{2, 4, 3, 6, 7, 0, 1, 5}));
        QCOMPARE(graph.FirstChild()[2], 3);
        QCOMPARE(graph.NextSibling()[3], 6);
        QCOMPARE(graph.FirstChild()[3], 0);
        QCOMPARE(graph.NextSibling()[0], 1);
        QVERIFY(graph.IsAncestor(2, 5));
        QVERIFY(graph.IsAncestor(3, 5));
        QVERIFY(graph.IsAncestor(1, 5));
        QVERIFY(!graph.IsAncestor(5, 2));
        QVERIFY(!graph.IsAncestor(3, 3));
        QVERIFY(!graph.IsAncestor(4, 5));
        QVERIFY(!graph.IsAncestor(-1, 5));
        QVERIFY(!graph.IsAncestor(2, parents.size()));
    }

    void errorsClassifyComponentsWithoutDiscardingValidRoots()
    {
        const QVector<int> parents{-1, 0, 9, 2, 5, 4, 5, 7, -2};
        CParentGraph graph;
        QVERIFY(!graph.Analyze(parents));
        QCOMPARE(graph.Parents(), parents);
        using Error = CParentGraph::Error;
        QCOMPARE(graph.Errors(), (QVector<Error>{Error::None, Error::None, Error::InvalidParent,
            Error::InvalidAncestor, Error::Cycle, Error::Cycle, Error::InvalidAncestor,
            Error::Cycle, Error::InvalidParent}));
        QCOMPARE(graph.Roots(), QVector<int>{0});
        QCOMPARE(graph.TopologicalOrder(), (QVector<int>{0, 1}));
        QVERIFY(graph.IsAncestor(0, 1));
        for (int index = 2; index < parents.size(); ++index) {
            QVERIFY(!graph.IsValid(index));
            QCOMPARE(graph.FirstChild()[index], -1);
            QCOMPARE(graph.NextSibling()[index], -1);
            QVERIFY(!graph.IsAncestor(index, 6));
        }
        QCOMPARE(graph.ErrorFor(-1), Error::InvalidParent);
        QCOMPARE(graph.ErrorFor(std::numeric_limits<int>::max()), Error::InvalidParent);
        QVERIFY(graph.Analyze({-1, 0}));
        QCOMPARE(graph.Errors(), (QVector<Error>{Error::None, Error::None}));
        QVERIFY(graph.Analyze({}));
        QVERIFY(graph.Roots().isEmpty());
        QVERIFY(graph.TopologicalOrder().isEmpty());
    }

    void deepAndWideForestsHaveCompleteTopology_data()
    {
        QTest::addColumn<bool>("deep");
        QTest::newRow("deep") << true;
        QTest::newRow("wide") << false;
    }

    void deepAndWideForestsHaveCompleteTopology()
    {
        QFETCH(bool, deep);
        constexpr int count = 100000;
        QVector<int> parents(count, 0);
        parents[0] = -1;
        if (deep) for (int index = 1; index < count; ++index) parents[index] = index - 1;
        CParentGraph graph(parents);
        QVERIFY(graph.IsValid());
        QCOMPARE(graph.TopologicalOrder().size(), count);
        QCOMPARE(graph.Roots(), QVector<int>{0});
        for (int index = 0; index < count; ++index) {
            QCOMPARE(graph.TopologicalOrder()[index], index);
            QVERIFY(graph.IsValid(index));
        }
        QVERIFY(graph.IsAncestor(0, count - 1));
        QCOMPARE(graph.IsAncestor(1, count - 1), deep);
        CParentGraph copy = graph;
        parents[count / 2] = count / 2;
        QVERIFY(!graph.Analyze(parents));
        QVERIFY(copy.IsValid());
        QCOMPARE(graph.ErrorFor(count / 2), CParentGraph::Error::Cycle);
        QCOMPARE(graph.ErrorFor(count - 1), deep ? CParentGraph::Error::InvalidAncestor : CParentGraph::Error::None);
        QVERIFY(copy.IsAncestor(0, count - 1));
    }

    void exhaustiveSmallGraphsMatchParentWalking()
    {
        constexpr int count = 4;
        for (int encoded = 0; encoded < 1296; ++encoded) {
            QVector<int> parents;
            int value = encoded;
            for (int index = 0; index < count; ++index) { parents.append(value % 6 - 1); value /= 6; }
            CParentGraph graph(parents);
            int validCount = 0;
            for (int start = 0; start < count; ++start) {
                QSet<int> seen;
                int current = start;
                while (current >= 0 && current < count && !seen.contains(current)) {
                    seen.insert(current);
                    current = parents[current];
                }
                const bool valid = current == -1;
                QCOMPARE(graph.IsValid(start), valid);
                if (valid) ++validCount;
                for (int ancestor = 0; ancestor < count; ++ancestor)
                    QCOMPARE(graph.IsAncestor(ancestor, start), valid && ancestor != start && seen.contains(ancestor));
            }
            QCOMPARE(graph.TopologicalOrder().size(), validCount);
            QCOMPARE(graph.IsValid(), validCount == count);
        }
    }
};

QTEST_APPLESS_MAIN(ParentGraphTests)
#include "parent_graph_tests.moc"
