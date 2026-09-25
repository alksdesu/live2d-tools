#include "live2dstruct/tree/c2dtree.h"
#include <QtTest>
#include <memory>
#include <type_traits>

using namespace live2dstruct;

namespace {
std::unique_ptr<C2DTree> loadTree(const QString& name)
{
    CReader reader(QString(TEST_RESOURCES) + QLatin1Char('/') + name + QLatin1Char('/') + name + ".moc3");
    CSectionTable table(reader);
    return table.IsValid() ? std::make_unique<C2DTree>(table) : nullptr;
}

bool near(const std::vector<float>& actual, const std::vector<float>& expected)
{
    if (actual.size() != expected.size()) return false;
    for (size_t index = 0; index < actual.size(); ++index)
        if (!std::isfinite(actual[index]) || std::abs(actual[index] - expected[index]) > 0.00002f) return false;
    return true;
}

void linkNodes(C2DTreeNode* parent, C2DTreeNode* child)
{
    parent->AddChild(child);
    child->AddFather(parent);
}
}

class DeformerTreeTests : public QObject
{
    Q_OBJECT
private slots:
    void nestedRotationsKeepLocalKeyformsAndComposeAllParents()
    {
        static_assert(!std::is_copy_constructible_v<C2DTree>);
        auto tree = loadTree("Mark");
        QVERIFY(tree);
        std::vector<CRotationNode*> rotations;
        for (auto* node : tree->GetWarpKeys())
            if (auto* rotation = dynamic_cast<CRotationNode*>(node)) rotations.push_back(rotation);
        QVERIFY(rotations.size() >= 2);
        QVERIFY(!tree->getArtTree().isEmpty());
        auto* mesh = dynamic_cast<CArtMeshNode*>(tree->getArtTree().first());
        QVERIFY(mesh);
        auto* parent = rotations[0];
        auto* child = rotations[1];
        for (C2DTreeNode* node : QList<C2DTreeNode*>{parent, child, mesh}) {
            node->getFathers().clear();
            node->getChilds().clear();
        }
        tree->getDeformRoot() = {parent};
        linkNodes(parent, child);
        linkNodes(child, mesh);
        parent->GetKeyFormS() = {CRotationNodeKeyform(1, 90 - parent->GetBaseAngles(), 0, 0, 1, false, false)};
        child->GetKeyFormS() = {CRotationNodeKeyform(1, -child->GetBaseAngles(), 1, 0, 1, false, false)};
        mesh->GetKeyFormS() = {CArtMeshKeyForm(1, 0, {1, 0, 0, 1})};
        for (int repeat = 0; repeat < 10; ++repeat) {
            QCOMPARE(tree->GetWarpKeys().size(), size_t(2));
            QVERIFY(near(child->GetCurKeyForm().GetPosition(), {1, 0}));
            QVERIFY(near(child->GetCanvasPositions(), {0, 1}));
            QVERIFY(near(mesh->GetCurKeyForm().GetPosition(), {1, 0, 0, 1}));
            QVERIFY(near(mesh->GetCanvasPositions(), {0, 2, -1, 1}));
            std::vector<float> restored;
            QVERIFY(mesh->MapFromCanvas(mesh->GetCanvasPositions(), restored));
            QVERIFY(near(restored, {1, 0, 0, 1}));
        }
        parent->GetKeyFormS()[0].Setangles(180 - parent->GetBaseAngles());
        tree->GetWarpKeys();
        QVERIFY(near(mesh->GetCanvasPositions(), {-2, 0, -1, -1}));
        QVERIFY(near(mesh->GetKeyFormS()[0].GetPosition(), {1, 0, 0, 1}));
    }

    void rotationAboveWarpComposesWithoutChangingControlPoints()
    {
        auto tree = loadTree("Mark");
        QVERIFY(tree);
        CRotationNode* rotation = nullptr;
        CWarpNode* warp = nullptr;
        for (auto* node : tree->GetWarpKeys()) {
            if (!rotation) rotation = dynamic_cast<CRotationNode*>(node);
            if (!warp) warp = dynamic_cast<CWarpNode*>(node);
        }
        QVERIFY(rotation && warp);
        QVERIFY(warp->GetColumns() > 0 && warp->GetRows() > 0);
        auto* mesh = dynamic_cast<CArtMeshNode*>(tree->getArtTree().first());
        QVERIFY(mesh);
        for (C2DTreeNode* node : QList<C2DTreeNode*>{rotation, warp, mesh}) {
            node->getFathers().clear();
            node->getChilds().clear();
        }
        tree->getDeformRoot() = {rotation};
        linkNodes(rotation, warp);
        linkNodes(warp, mesh);
        rotation->GetKeyFormS() = {CRotationNodeKeyform(1, 90 - rotation->GetBaseAngles(), 0, 0, 2, false, false)};
        std::vector<float> control;
        for (int row = 0; row <= warp->GetRows(); ++row) {
            for (int column = 0; column <= warp->GetColumns(); ++column) {
                control.push_back(1.0f + 2.0f * column / warp->GetColumns());
                control.push_back(2.0f + 3.0f * row / warp->GetRows());
            }
        }
        warp->GetKeyFormS() = {CWarpNodeKeyform(1, control)};
        mesh->GetKeyFormS() = {CArtMeshKeyForm(1, 0, {0.25f, 0.75f})};
        tree->GetWarpKeys();
        QVERIFY(near(warp->GetCurKeyForm().GetPosition(), control));
        QVERIFY(near(mesh->GetCanvasPositions(), {-8.5f, 3.0f}));
        std::vector<float> restored;
        QVERIFY(mesh->MapFromCanvas(mesh->GetCanvasPositions(), restored));
        QVERIFY(near(restored, {0.25f, 0.75f}));
    }

    void sourceLifetimeAndRepeatedUpdates_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }

    void sourceLifetimeAndRepeatedUpdates()
    {
        QFETCH(QString, name);
        auto tree = loadTree(name);
        QVERIFY(tree);
        const auto nodes = tree->GetWarpKeys();
        QVERIFY(!nodes.empty());
        std::vector<std::vector<float>> before;
        for (auto* node : nodes) {
            const auto& positions = node->GetCanvasPositions();
            auto* warp = dynamic_cast<CWarpNode*>(node);
            if (warp) QCOMPARE(positions.size(), static_cast<size_t>(warp->GetVertex_counts()) * 2);
            else QCOMPARE(positions.size(), size_t(2));
            before.push_back(positions);
        }
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto updated = tree->GetWarpKeys();
            QCOMPARE(updated.size(), nodes.size());
            for (size_t index = 0; index < nodes.size(); ++index) {
                QCOMPARE(updated[index], nodes[index]);
                QVERIFY(near(updated[index]->GetCanvasPositions(), before[index]));
            }
        }
    }
};

QTEST_APPLESS_MAIN(DeformerTreeTests)
#include "deformer_tree_tests.moc"
