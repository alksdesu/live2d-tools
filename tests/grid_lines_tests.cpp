#include "edit/cgridlines.h"

#include <QtTest>
#include <algorithm>
#include <limits>
#include <set>

class GridLinesTests : public QObject
{
    Q_OBJECT

    static std::set<std::pair<quint32, quint32>> edges(const std::vector<quint32>& indices)
    {
        std::set<std::pair<quint32, quint32>> result;
        for (std::size_t index = 0; index + 1 < indices.size(); index += 2)
            result.emplace(std::min(indices[index], indices[index + 1]),
                           std::max(indices[index], indices[index + 1]));
        return result;
    }

private slots:
    void rectangularGridContainsOnlyUniqueHorizontalAndVerticalEdges()
    {
        const std::vector<float> positions{0, 0, 1, 0, 2, 0,
                                           0, 1, 1, 1, 2, 1,
                                           0, 2, 1, 2, 2, 2,
                                           0, 3, 1, 3, 2, 3};
        const std::set<std::pair<quint32, quint32>> expected{
            {0, 1}, {1, 2}, {3, 4}, {4, 5}, {6, 7}, {7, 8}, {9, 10}, {10, 11},
            {0, 3}, {1, 4}, {2, 5}, {3, 6}, {4, 7}, {5, 8}, {6, 9}, {7, 10}, {8, 11}
        };
        CGridLines grid;
        QVERIFY(grid.SetGrid(2, 3, positions));
        QCOMPARE(grid.Indices().size(), std::size_t(34));
        QVERIFY(edges(grid.Indices()) == expected);
        QVERIFY(grid.Positions() == positions);
        std::vector<int> degree(12, 0);
        for (std::size_t index = 0; index < grid.Indices().size(); index += 2) {
            const quint32 a = grid.Indices()[index];
            const quint32 b = grid.Indices()[index + 1];
            QVERIFY(a < 12 && b < 12 && a != b);
            const float dx = std::abs(positions[a * 2] - positions[b * 2]);
            const float dy = std::abs(positions[a * 2 + 1] - positions[b * 2 + 1]);
            QCOMPARE(dx + dy, 1.0f);
            ++degree[a];
            ++degree[b];
        }
        QVERIFY(degree == std::vector<int>({2, 3, 2, 3, 4, 3, 3, 4, 3, 2, 3, 2}));
    }

    void geometryUpdatesCopyCoordinatesAndRebuildChangedDimensions()
    {
        std::vector<float> positions(24, 0);
        CGridLines grid;
        QVERIFY(grid.SetGrid(2, 3, positions));
        const auto oldIndices = grid.Indices();
        positions[0] = 12;
        QCOMPARE(grid.Positions()[0], 0.0f);
        QVERIFY(grid.SetGrid(2, 3, positions));
        QCOMPARE(grid.Positions()[0], 12.0f);
        QVERIFY(grid.Indices() == oldIndices);
        QVERIFY(grid.SetGrid(3, 2, positions));
        QVERIFY(grid.Indices() != oldIndices);
        const auto updated = edges(grid.Indices());
        QVERIFY(updated.count({0, 4}) == 1);
        QVERIFY(updated.count({0, 3}) == 0);
        QVERIFY(grid.SetGrid(3, 2, grid.Positions()));
        QCOMPARE(grid.Positions()[0], 12.0f);
        grid.Clear();
        QVERIFY(grid.Positions().empty());
        QVERIFY(grid.Indices().empty());
        QVERIFY(grid.SetGrid(2, 3, positions));
        QVERIFY(grid.Indices() == oldIndices);
    }

    void invalidInputsClearPreviousGeometry()
    {
        const std::vector<float> valid{0, 0, 1, 0, 0, 1, 1, 1};
        const std::pair<int, int> invalidDimensions[] = {
            {0, 1}, {1, 0}, {-1, 1}, {1, -1},
            {std::numeric_limits<int>::max(), 1}, {1, std::numeric_limits<int>::max()},
            {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()},
            {std::numeric_limits<int>::min(), std::numeric_limits<int>::min()},
            {1000, 1000}, {499999, 2}
        };
        CGridLines grid;
        for (const auto& dimensions : invalidDimensions) {
            QVERIFY(grid.SetGrid(1, 1, valid));
            QVERIFY(!grid.SetGrid(dimensions.first, dimensions.second, valid));
            QVERIFY(grid.Positions().empty());
            QVERIFY(grid.Indices().empty());
        }
        for (const auto& invalid : {std::vector<float>{}, std::vector<float>(7, 0), std::vector<float>(10, 0)}) {
            QVERIFY(grid.SetGrid(1, 1, valid));
            QVERIFY(!grid.SetGrid(1, 1, invalid));
            QVERIFY(grid.Positions().empty());
            QVERIFY(grid.Indices().empty());
        }
        for (float invalid : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            auto positions = valid;
            positions.back() = invalid;
            QVERIFY(grid.SetGrid(1, 1, valid));
            QVERIFY(!grid.SetGrid(1, 1, positions));
            QVERIFY(grid.Positions().empty());
            QVERIFY(grid.Indices().empty());
        }
        QVERIFY(grid.SetGrid(1, 1, valid));
        QVERIFY(!grid.SetGrid(2, 2, grid.Positions()));
        QVERIFY(grid.Positions().empty());
        QVERIFY(grid.Indices().empty());
    }

    void largeGridsKeepFullWidthIndicesAndEnforcePointLimit()
    {
        CGridLines grid;
        std::vector<float> positions(2000000, 0);
        QVERIFY(grid.SetGrid(999, 999, positions));
        QCOMPARE(grid.Indices().size(), std::size_t(3996000));
        QCOMPARE(*std::max_element(grid.Indices().begin(), grid.Indices().end()), quint32(999999));
        QVERIFY(std::all_of(grid.Indices().begin(), grid.Indices().end(), [](quint32 index) { return index < 1000000; }));
        QVERIFY(std::find(grid.Indices().begin(), grid.Indices().end(), quint32(65536)) != grid.Indices().end());
        positions.resize(2002000);
        QVERIFY(!grid.SetGrid(1000, 999, positions));
        QVERIFY(grid.Positions().empty());
        QVERIFY(grid.Indices().empty());
    }

    void finiteCoordinatesRetainTheirOriginalValues()
    {
        const float maximum = std::numeric_limits<float>::max();
        const float minimum = std::numeric_limits<float>::denorm_min();
        const std::vector<float> positions{-maximum, maximum, minimum, -minimum, 0, 0, maximum, -maximum};
        CGridLines grid;
        QVERIFY(grid.SetGrid(1, 1, positions));
        QVERIFY(grid.Positions() == positions);
        QCOMPARE(grid.Indices().size(), std::size_t(8));
        const std::set<std::pair<quint32, quint32>> perimeter{{0, 1}, {0, 2}, {1, 3}, {2, 3}};
        QVERIFY(edges(grid.Indices()) == perimeter);
    }
};

QTEST_APPLESS_MAIN(GridLinesTests)
#include "grid_lines_tests.moc"
