#include "live2dstruct/tree/c2dtree.h"
#include <QtTest>
#include <memory>
#include <limits>
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

std::unique_ptr<C2DTree> rotationTree(const QVector<int>& parents, float scale = 1.0f)
{
    CReader reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    CSectionTable table(reader);
    if (!table.IsValid() || table.deformers.empty() || table.rotation_deformers.empty() ||
        table.rotation_deformer_keyforms.empty()) return {};
    const CDeformers prototype = *table.deformers.front();
    qDeleteAll(table.deformers);
    table.deformers.clear();
    qDeleteAll(table.parts);
    table.parts.clear();
    qDeleteAll(table.art_meshes);
    table.art_meshes.clear();
    qDeleteAll(table.parameters);
    table.parameters.clear();
    auto* source = table.rotation_deformers.front();
    source->base_angles = 0;
    source->keyform_binding_sources_indices = -1;
    source->keyform_sources_begin_indices = 0;
    source->keyform_sources_counts = 1;
    auto* form = table.rotation_deformer_keyforms.front();
    form->opacities = 1;
    form->angles = 0;
    form->origin_x = form->origin_y = 0;
    form->scales = scale;
    form->is_reflect_x = form->is_reflect_y = false;
    for (int index = 0; index < parents.size(); ++index) {
        auto node = std::make_unique<CDeformers>(prototype);
        node->ids = QStringLiteral("R%1").arg(index);
        node->keyform_binding_sources_indices = -1;
        node->parent_part_indices = -1;
        node->parent_deformer_indices = parents[index];
        node->types = 1;
        node->specific_sources_indices = 0;
        table.deformers.push_back(node.release());
    }
    return std::make_unique<C2DTree>(table);
}
}

class DeformerTreeTests : public QObject
{
    Q_OBJECT
private slots:
    void deepAndWideIdentityTreesKeepEveryNode_data()
    {
        QTest::addColumn<bool>("deep");
        QTest::newRow("deep") << true;
        QTest::newRow("wide") << false;
    }

    void deepAndWideIdentityTreesKeepEveryNode()
    {
        QFETCH(bool, deep);
        constexpr int count = 30000;
        QVector<int> parents(count, 0);
        parents[0] = -1;
        if (deep) for (int index = 1; index < count; ++index) parents[index] = index - 1;
        auto tree = rotationTree(parents);
        QVERIFY(tree);
        auto nodes = tree->GetWarpKeys();
        QCOMPARE(nodes.size(), size_t(count));
        for (int index = 0; index < count; ++index) {
            QCOMPARE(nodes[index]->getName(), QStringLiteral("R%1").arg(index));
            QVERIFY(near(nodes[index]->GetCanvasPositions(), {0, 0}));
        }
        auto* root = dynamic_cast<CRotationNode*>(nodes.front());
        QVERIFY(root);
        root->GetKeyFormS()[0] = CRotationNodeKeyform(1, 90, 2, 3, 1, false, false);
        for (int repeat = 0; repeat < 3; ++repeat) {
            nodes = tree->GetWarpKeys();
            QCOMPARE(nodes.size(), size_t(count));
            for (auto* node : nodes) QVERIFY(near(node->GetCanvasPositions(), {2, 3}));
            std::vector<float> world, restored;
            QVERIFY(nodes.back()->MapToCanvas({1.25f, -2.5f}, world));
            QVERIFY(near(world, {4.5f, 4.25f}));
            QVERIFY(nodes.back()->MapFromCanvas(world, restored));
            QVERIFY(near(restored, {1.25f, -2.5f}));
        }
        tree.reset();
    }

    void invalidParentComponentsDoNotDiscardOtherRoots()
    {
        auto tree = rotationTree({-1, 0, 3, 2, 3, -1, 9, 6});
        QVERIFY(tree);
        const auto nodes = tree->GetWarpKeys();
        QCOMPARE(nodes.size(), size_t(3));
        QCOMPARE(nodes[0]->getName(), QStringLiteral("R0"));
        QCOMPARE(nodes[1]->getName(), QStringLiteral("R1"));
        QCOMPARE(nodes[2]->getName(), QStringLiteral("R5"));
        for (auto* node : nodes) QVERIFY(near(node->GetCanvasPositions(), {0, 0}));
    }

    void nonidentityChainsKeepPerLayerOverflowAndInverseChecks()
    {
        QVector<int> parents(256);
        for (int index = 0; index < parents.size(); ++index) parents[index] = index - 1;
        auto tree = rotationTree(parents, 2.0f);
        QVERIFY(tree);
        const auto nodes = tree->GetWarpKeys();
        QCOMPARE(nodes.size(), size_t(256));
        std::vector<float> values{7, 9};
        QVERIFY(nodes.back()->MapToCanvas({0, 0}, values));
        QVERIFY(near(values, {0, 0}));
        values = {7, 9};
        QVERIFY(!nodes.back()->MapToCanvas({std::numeric_limits<float>::max(), 0}, values));
        QVERIFY(near(values, {7, 9}));
        auto* root = dynamic_cast<CRotationNode*>(nodes.front());
        QVERIFY(root);
        root->GetKeyFormS()[0].Setscales(0);
        tree->GetWarpKeys();
        QVERIFY(!nodes.back()->MapFromCanvas({0, 0}, values));
        QVERIFY(near(values, {7, 9}));
    }

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
        child->GetKeyFormS().push_back(child->GetKeyFormS().front());
        tree->GetWarpKeys();
        QVERIFY(child->GetCanvasPositions().empty());
        QVERIFY(mesh->GetCanvasPositions().empty());
        std::vector<float> preserved{7, 9};
        QVERIFY(!mesh->MapFromCanvas({-2, 0}, preserved));
        QVERIFY((preserved == std::vector<float>{7, 9}));
        child->GetKeyFormS().resize(1);
        tree->GetWarpKeys();
        QVERIFY(near(mesh->GetCanvasPositions(), {-2, 0, -1, -1}));
    }

    void zeroBindingParametersDoNotShiftWankoMouthKeyforms()
    {
        auto tree = loadTree("Wanko");
        QVERIFY(tree);
        CArtMeshNode* face = nullptr;
        for (auto* node : tree->getArtTree())
            if (node->getName() == QLatin1String("D_FACE_01")) face = dynamic_cast<CArtMeshNode*>(node);
        QVERIFY(face);
        QCOMPARE(face->GetKeyFormS().size(), size_t(2));
        CParameterNode* mouth = nullptr;
        for (auto* node : face->getFathers()) {
            if (auto* parameter = dynamic_cast<CParameterNode*>(node); parameter && parameter->GetType() == 0) {
                QVERIFY(!mouth);
                mouth = parameter;
            }
        }
        QVERIFY(mouth);
        QCOMPARE(mouth->getName(), QStringLiteral("PARAM_MOUTH_OPEN_Y"));
        QVERIFY((mouth->GetKeys() == std::vector<float>{0, 1}));
        for (int index : {1, 0, 1}) {
            tree->SetParaValue(mouth->getName(), static_cast<float>(index));
            tree->GetWarpKeys();
            QVERIFY(face->GetCurKeyForm().GetPosition() == face->GetKeyFormS()[index].GetPosition());
            QVERIFY(!face->GetCanvasPositions().empty());
        }
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

    void fileBindingIndicesIdentifyTheCorrectParameterAndKeys_data()
    {
        sourceLifetimeAndRepeatedUpdates_data();
    }

    void fileBindingIndicesIdentifyTheCorrectParameterAndKeys()
    {
        QFETCH(QString, name);
        CReader reader(QString(TEST_RESOURCES) + QLatin1Char('/') + name + QLatin1Char('/') + name + ".moc3");
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        C2DTree tree(table);
        QHash<QString, C2DTreeNode*> deformers;
        for (auto* node : tree.GetWarpKeys()) deformers.insert(node->getName(), node);
        QHash<QString, C2DTreeNode*> meshes;
        for (auto* node : tree.getArtTree()) meshes.insert(node->getName(), node);
        QVector<const CParameters*> owners(table.parameter_bindings.size(), nullptr);
        for (const auto* parameter : table.parameters) {
            for (int index = 0; index < parameter->parameter_binding_sources_counts; ++index)
                owners[parameter->parameter_binding_sources_begin_indices + index] = parameter;
        }
        const auto check = [&](C2DTreeNode* node, int bindingIndex) {
            QVERIFY(node);
            std::vector<CParameterNode*> axes;
            for (auto* parent : node->getFathers()) {
                if (auto* parameter = dynamic_cast<CParameterNode*>(parent); parameter && parameter->GetType() == 0)
                    axes.push_back(parameter);
            }
            std::vector<int> expectedBindings;
            if (bindingIndex >= 0) {
                QVERIFY(bindingIndex < table.keyform_bindings.size());
                const auto* binding = table.keyform_bindings[bindingIndex];
                for (int offset = 0; offset < binding->parameter_binding_index_sources_counts; ++offset) {
                    const int source = table.parameter_binding_indices[
                        binding->parameter_binding_index_sources_begin_indices + offset]->binding_sources_indices;
                    QVERIFY(source >= 0 && source < owners.size());
                    QVERIFY(owners[source]);
                    if (owners[source]->parameter_types == 0) expectedBindings.push_back(source);
                }
            }
            QCOMPARE(axes.size(), expectedBindings.size());
            for (size_t axis = 0; axis < axes.size(); ++axis) {
                const int source = expectedBindings[axis];
                QCOMPARE(axes[axis]->getName(), owners[source]->ids);
                const auto* binding = table.parameter_bindings[source];
                std::vector<float> expected;
                for (int index = 0; index < binding->keys_sources_counts; ++index)
                    expected.push_back(table.keys->values[binding->keys_sources_begin_indices + index]);
                QVERIFY(axes[axis]->GetKeys() == expected);
            }
        };
        for (const auto* deformer : table.deformers)
            check(deformers.value(deformer->ids, nullptr), deformer->keyform_binding_sources_indices);
        for (const auto* mesh : table.art_meshes)
            check(meshes.value(mesh->ids, nullptr), mesh->keyform_binding_sources_indices);
        for (auto* node : tree.getArtTree()) {
            auto* mesh = dynamic_cast<CArtMeshNode*>(node);
            QVERIFY(mesh);
            QVERIFY2(!mesh->GetCanvasPositions().empty(), qPrintable(mesh->getName()));
            QCOMPARE(mesh->GetCanvasPositions().size(), mesh->GetKeyFormS().front().GetPosition().size());
        }
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
