#include "ui/cmodeltreepanel.h"
#include "ui/cinspectorpanel.h"
#include "ui/para/ccidjson.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <QComboBox>
#include <algorithm>

namespace {
QList<QTreeWidgetItem*> treeItems(CModelTreePanel& panel)
{
    QList<QTreeWidgetItem*> items;
    for (QTreeWidget* tree : panel.findChildren<QTreeWidget*>())
        for (QTreeWidgetItemIterator it(tree); *it; ++it) items.append(*it);
    return items;
}

QMap<QTreeWidgetItem*, Qt::CheckState> checkStates(CModelTreePanel& panel)
{
    QMap<QTreeWidgetItem*, Qt::CheckState> states;
    for (auto* item : treeItems(panel)) states.insert(item, item->checkState(0));
    return states;
}
}

class ModelTreeTests : public QObject
{
    Q_OBJECT
private slots:
    void meshHighlightPreservesAcceptedKeyformAcrossBothTrees()
    {
        live2dstruct::CModelDocument document;
        const QString path = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        QVERIFY(document.Load(path));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable){ return drawable.vertexCount && drawable.keyforms.size() > 1; });
        QVERIFY(mesh != document.Drawables().cend());
        CModelTreePanel tree;
        tree.BuildTree(path, nullptr);
        CInspectorPanel inspector;
        inspector.SetDocument(&document);
        int activations = 0;
        inspector.SetKeyformHandler([&](const QString&, int, QString*) { ++activations; return true; });
        connect(&tree, &CModelTreePanel::ObjectSelected, &inspector, &CInspectorPanel::SetSelection);
        connect(&tree, &CModelTreePanel::ObjectsSelected, &inspector, &CInspectorPanel::SetSelections);
        QSignalSpy selections(&tree, &CModelTreePanel::ObjectsSelected);
        QSignalSpy objectSelections(&tree, &CModelTreePanel::ObjectSelected);
        tree.HighlightMesh(mesh->id);
        QCOMPARE(selections.size(), 1);
        QCOMPARE(selections.first(), (QList<QVariant>{QStringList{mesh->id}, QStringList{QStringLiteral("artmesh")}}));
        QCOMPARE(objectSelections.size(), 0);
        QCOMPARE(inspector.GetSelectionId(), mesh->id);
        QCOMPARE(inspector.GetActiveKeyformIndex(), 0);
        QCOMPARE(activations, 0);
        auto* combo = inspector.findChild<QComboBox*>("keyformSelector");
        QVERIFY(combo);
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(inspector.GetActiveKeyformIndex(), 1);
        QCOMPARE(activations, 1);
        selections.clear();
        tree.HighlightMesh(mesh->id);
        QCOMPARE(selections.size(), 0);
        QCOMPARE(inspector.GetSelectionId(), mesh->id);
        QCOMPARE(inspector.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(activations, 1);
        const auto other = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [&mesh](const auto& value){ return value.id != mesh->id && value.vertexCount && !value.keyforms.isEmpty(); });
        QVERIFY(other != document.Drawables().cend());
        tree.HighlightMesh(other->id);
        QCOMPARE(selections.size(), 1);
        QCOMPARE(selections.first(), (QList<QVariant>{QStringList{other->id}, QStringList{QStringLiteral("artmesh")}}));
        QCOMPARE(inspector.GetSelectionId(), other->id);
        QCOMPARE(inspector.GetActiveKeyformIndex(), 0);
        QCOMPARE(activations, 1);
        selections.clear();
        tree.HighlightMesh({});
        QCOMPARE(selections.size(), 1);
        QCOMPARE(selections.first(), (QList<QVariant>{QStringList{}, QStringList{}}));
        QVERIFY(inspector.GetSelectionId().isEmpty());
        QCOMPARE(inspector.GetActiveKeyformIndex(), -1);
        tree.HighlightMesh({});
        QCOMPARE(selections.size(), 1);
        QVERIFY(!document.CanUndoEdit());
    }

    void programmaticVisibilityChangesOnlyTheSelectedObject_data()
    {
        QTest::addColumn<QString>("type");
        for (const char* type : {"part", "warp", "rotation", "artmesh"})
            QTest::newRow(type) << QString::fromLatin1(type);
    }

    void programmaticVisibilityChangesOnlyTheSelectedObject()
    {
        QFETCH(QString, type);
        CModelTreePanel panel;
        panel.BuildTree(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", nullptr);
        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        QTreeWidgetItem* target = nullptr;
        for (auto* item : treeItems(panel)) {
            if (item->data(0, Qt::UserRole + 1).toString() == type &&
                (type == "artmesh" || item->childCount() > 0)) {
                target = item;
                break;
            }
        }
        QVERIFY(target);
        const QString name = target->data(0, Qt::UserRole).toString();
        QList<QTreeWidgetItem*> targets;
        for (auto* item : treeItems(panel)) {
            if (item->data(0, Qt::UserRole).toString() == name &&
                item->data(0, Qt::UserRole + 1).toString() == type) targets.append(item);
        }
        QCOMPARE(targets.size(), type == "artmesh" ? 1 : 2);
        for (auto* tree : trees) {
            const QSignalBlocker blocker(tree);
            int index = 0;
            for (QTreeWidgetItemIterator it(tree); *it; ++it) {
                const bool visible = targets.contains(*it) || index++ % 2 != 0;
                (*it)->setCheckState(0, visible ? Qt::Checked : Qt::Unchecked);
            }
        }
        const auto initial = checkStates(panel);
        QSignalSpy changes(&panel, &CModelTreePanel::VisibilityChanged);
        QSignalSpy firstTreeChanges(trees[0], &QTreeWidget::itemChanged);
        QSignalSpy secondTreeChanges(trees[1], &QTreeWidget::itemChanged);
        trees[0]->blockSignals(true);
        for (bool visible : {false, true}) {
            panel.SetVisibility(name, type, visible);
            QCOMPARE(changes.size(), 1);
            QCOMPARE(changes[0], (QList<QVariant>{name, type, visible}));
            changes.clear();
            for (auto it = initial.cbegin(); it != initial.cend(); ++it) {
                const auto expected = targets.contains(it.key()) ? (visible ? Qt::Checked : Qt::Unchecked) : it.value();
                QCOMPARE(it.key()->checkState(0), expected);
            }
            QVERIFY(trees[0]->signalsBlocked());
            QVERIFY(!trees[1]->signalsBlocked());
            QCOMPARE(firstTreeChanges.size(), 0);
            QCOMPARE(secondTreeChanges.size(), 0);
        }
        QCOMPARE(checkStates(panel), initial);
        panel.SetVisibility(name, "unknown-kind", false);
        panel.SetVisibility("missing-object", type, false);
        panel.SetVisibility({}, type, false);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(checkStates(panel), initial);
    }

    void matchingIdsInDifferentObjectTypesStayIndependent()
    {
        const QString source = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CReader reader(source);
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QVERIFY(!table.parts.isEmpty());
        QVERIFY(!table.art_meshes.isEmpty());
        int warp = -1, rotation = -1;
        for (int index = 0; index < table.deformers.size(); ++index) {
            if (warp < 0 && table.deformers[index]->types == 0) warp = index;
            if (rotation < 0 && table.deformers[index]->types == 1) rotation = index;
        }
        QVERIFY(warp >= 0);
        QVERIFY(rotation >= 0);
        const int meshParent = table.art_meshes.first()->parent_deformer_indices;
        QVERIFY(meshParent >= 0 && meshParent < table.deformers.size());
        if (table.deformers[meshParent]->types == 0) warp = meshParent;
        else rotation = meshParent;
        QByteArray bytes = reader.m_byte;
        const QString sharedName = "SharedObjectId";
        const QByteArray id = sharedName.toUtf8().leftJustified(64, '\0');
        const auto& offsets = *table.sectionOffsetTable;
        for (int offset : {offsets.parts->ids, offsets.art_meshes->ids,
                           offsets.deformers->ids + warp * 64, offsets.deformers->ids + rotation * 64}) {
            QVERIFY(offset >= 0 && offset <= bytes.size() - 64);
            bytes.replace(offset, 64, id);
        }
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("shared-ids.moc3");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        CModelTreePanel panel;
        panel.BuildTree(path, nullptr);
        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        for (auto* tree : trees) {
            const QSignalBlocker blocker(tree);
            for (QTreeWidgetItemIterator it(tree); *it; ++it) (*it)->setCheckState(0, Qt::Checked);
        }
        const auto initial = checkStates(panel);
        QSignalSpy changes(&panel, &CModelTreePanel::VisibilityChanged);
        for (const QString& type : {QStringLiteral("part"), QStringLiteral("artmesh"),
                                    QStringLiteral("warp"), QStringLiteral("rotation")}) {
            int targets = 0;
            for (auto* item : treeItems(panel))
                if (item->data(0, Qt::UserRole).toString() == sharedName &&
                    item->data(0, Qt::UserRole + 1).toString() == type) ++targets;
            QCOMPARE(targets, type == "artmesh" ? 1 : 2);
            panel.SetVisibility(sharedName, type, false);
            QCOMPARE(changes.size(), 1);
            QCOMPARE(changes.takeFirst(), (QList<QVariant>{sharedName, type, false}));
            for (auto* item : treeItems(panel)) {
                const bool matches = item->data(0, Qt::UserRole).toString() == sharedName &&
                                     item->data(0, Qt::UserRole + 1).toString() == type;
                QCOMPARE(item->checkState(0), matches ? Qt::Unchecked : Qt::Checked);
            }
            panel.SetVisibility(sharedName, type, true);
            QCOMPARE(changes.size(), 1);
            QCOMPARE(changes.takeFirst(), (QList<QVariant>{sharedName, type, true}));
            QCOMPARE(checkStates(panel), initial);
        }

        QTreeWidgetItem* mesh = nullptr;
        QTreeWidgetItem* partOnly = nullptr;
        for (auto* item : treeItems(panel)) {
            const QString type = item->data(0, Qt::UserRole + 1).toString();
            const QString id = item->data(0, Qt::UserRole).toString();
            if (type == "artmesh" && id == sharedName) mesh = item;
            if (type == "part" && id != sharedName) partOnly = item;
        }
        QVERIFY(mesh);
        QVERIFY(mesh->parent());
        QVERIFY(partOnly);
        QCOMPARE(mesh->parent()->data(0, Qt::UserRole).toString(), sharedName);
        const QString parentType = mesh->parent()->data(0, Qt::UserRole + 1).toString();
        QTreeWidget* partTree = mesh->treeWidget();
        QTreeWidget* deformerTree = trees[0] == partTree ? trees[1] : trees[0];
        QSignalSpy selections(&panel, &CModelTreePanel::ObjectsSelected);
        panel.HighlightMesh(sharedName);
        QCOMPARE(partTree->selectedItems(), (QList<QTreeWidgetItem*>{mesh}));
        QCOMPARE(deformerTree->selectedItems().size(), 1);
        auto* selectedParent = deformerTree->selectedItems().first();
        QCOMPARE(selectedParent->data(0, Qt::UserRole).toString(), sharedName);
        QCOMPARE(selectedParent->data(0, Qt::UserRole + 1).toString(), parentType);
        QCOMPARE(selections.size(), 1);
        QCOMPARE(selections.first(), (QList<QVariant>{QStringList{sharedName}, QStringList{"artmesh"}}));
        selections.clear();
        panel.HighlightMesh(partOnly->data(0, Qt::UserRole).toString());
        QCOMPARE(partTree->selectedItems(), (QList<QTreeWidgetItem*>{mesh}));
        QCOMPARE(deformerTree->selectedItems(), (QList<QTreeWidgetItem*>{selectedParent}));
        QCOMPARE(selections.size(), 0);
        panel.HighlightMesh({});
        QVERIFY(partTree->selectedItems().isEmpty());
        QVERIFY(deformerTree->selectedItems().isEmpty());
        QCOMPARE(selections.size(), 1);
        QVERIFY(selections.last()[0].toStringList().isEmpty());
        QVERIFY(selections.last()[1].toStringList().isEmpty());
        QCOMPARE(checkStates(panel), initial);
        QVERIFY(changes.isEmpty());
    }

    void treeCheckChangesStillApplyToWholeSubtree_data()
    {
        QTest::addColumn<int>("treeIndex");
        QTest::newRow("parts") << 0;
        QTest::newRow("deformers") << 1;
    }

    void treeCheckChangesStillApplyToWholeSubtree()
    {
        QFETCH(int, treeIndex);
        CModelTreePanel panel;
        panel.BuildTree(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", nullptr);
        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        QTreeWidgetItem* parent = nullptr;
        for (QTreeWidgetItemIterator it(trees[treeIndex]); *it; ++it) {
            if ((*it)->childCount() > 0) { parent = *it; break; }
        }
        QVERIFY(parent);
        QSet<QTreeWidgetItem*> subtree;
        for (auto* item : treeItems(panel)) {
            for (auto* ancestor = item; ancestor; ancestor = ancestor->parent()) {
                if (ancestor == parent) { subtree.insert(item); break; }
            }
        }
        QVERIFY(subtree.size() > 1);
        {
            const QSignalBlocker blocker(trees[treeIndex]);
            for (auto* item : subtree) item->setCheckState(0, Qt::Checked);
        }
        const auto initial = checkStates(panel);
        QSignalSpy changes(&panel, &CModelTreePanel::VisibilityChanged);
        for (Qt::CheckState state : {Qt::Unchecked, Qt::Checked}) {
            parent->setCheckState(0, state);
            QCOMPARE(changes.size(), subtree.size());
            QSet<QString> emitted;
            for (const auto& event : changes) {
                QCOMPARE(event[2].toBool(), state == Qt::Checked);
                emitted.insert(event[1].toString() + '|' + event[0].toString());
            }
            for (auto* item : subtree) {
                QVERIFY(emitted.contains(item->data(0, Qt::UserRole + 1).toString() + '|' + item->data(0, Qt::UserRole).toString()));
                QCOMPARE(item->checkState(0), state);
            }
            for (auto it = initial.cbegin(); it != initial.cend(); ++it)
                if (!subtree.contains(it.key())) QCOMPARE(it.key()->checkState(0), it.value());
            changes.clear();
        }
    }

    void reparseCdiClearsPreviousState()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString firstPath = temp.filePath(QStringLiteral("first.cdi3.json"));
        const QString secondPath = temp.filePath(QStringLiteral("second.cdi3.json"));
        QFile first(firstPath);
        QVERIFY(first.open(QIODevice::WriteOnly));
        QVERIFY(first.write(R"({"Parts":[{"Id":"PartA","Name":"A"}],"ParameterGroups":[{"Id":"GroupA","Name":"GA"}],"Parameters":[{"Id":"ParamA","GroupId":"GroupA","Name":"PA"}]})") > 0);
        first.close();
        QFile second(secondPath);
        QVERIFY(second.open(QIODevice::WriteOnly));
        QVERIFY(second.write(R"({"Parts":[{"Id":"PartB","Name":"B"}],"Parameters":[{"Id":"ParamB","Name":"PB"}]})") > 0);
        second.close();

        CCidJson cid;
        QString error;
        QVERIFY(cid.Parse(firstPath, &error));
        QCOMPARE(cid.getPartName(QStringLiteral("PartA")), QStringLiteral("A"));
        QVERIFY(cid.Parse(secondPath, &error));
        QVERIFY(cid.getPartName(QStringLiteral("PartA")).isEmpty());
        QCOMPARE(cid.getPartName(QStringLiteral("PartB")), QStringLiteral("B"));
        QVERIFY(cid.getGroups().size() <= 1);
        const QString partialPath = temp.filePath(QStringLiteral("partial.cdi3.json"));
        QFile partial(partialPath);
        QVERIFY(partial.open(QIODevice::WriteOnly));
        QVERIFY(partial.write(R"({"Parts":[{"Id":"PartC","Name":"C"}],"ParameterGroups":"invalid"})") > 0);
        partial.close();
        QVERIFY(!cid.Parse(partialPath, &error));
        QVERIFY(cid.getPartName(QStringLiteral("PartC")).isEmpty());
        QVERIFY(cid.getPartName(QStringLiteral("PartB")).isEmpty());
        QVERIFY(!cid.Parse(temp.filePath(QStringLiteral("broken.json")), &error));
        QVERIFY(cid.getPartName(QStringLiteral("PartB")).isEmpty());
    }

    void mirrorsInitialVisibilityAndClearsSelection()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));

        CModelTreePanel panel;
        QSignalSpy selections(&panel, &CModelTreePanel::ObjectsSelected);
        panel.BuildTree(mocPath, nullptr);

        const auto expected = [&document](const QString& type, const QString& id, bool* found) {
            *found = true;
            if (type == QLatin1String("part")) {
                for (const auto& object : document.Parts()) if (object.id == id) return object.visible;
            } else if (type == QLatin1String("warp") || type == QLatin1String("rotation")) {
                for (const auto& object : document.Deformers()) if (object.id == id) return object.visible;
            } else if (type == QLatin1String("artmesh")) {
                for (const auto& object : document.Drawables()) if (object.id == id) return object.visible;
            }
            *found = false;
            return false;
        };

        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        int itemCount = 0;
        for (QTreeWidget* tree : trees) {
            const auto visit = [&](QTreeWidgetItem* item, auto&& visitRef) -> void {
                const QString type = item->data(0, Qt::UserRole + 1).toString();
                const QString id = item->data(0, Qt::UserRole).toString();
                bool found = false;
                const bool visible = expected(type, id, &found);
                QVERIFY(found);
                QCOMPARE(item->checkState(0), visible ? Qt::Checked : Qt::Unchecked);
                ++itemCount;
                for (int i = 0; i < item->childCount(); ++i) visitRef(item->child(i), visitRef);
            };
            for (int i = 0; i < tree->topLevelItemCount(); ++i) visit(tree->topLevelItem(i), visit);
        }
        QVERIFY(itemCount > 0);

        QTreeWidgetItem* firstItem = trees.first()->topLevelItem(0);
        QVERIFY(firstItem);
        const QString firstKey = firstItem->data(0, Qt::UserRole + 1).toString() + QLatin1Char('|') +
            firstItem->data(0, Qt::UserRole).toString();
        panel.ApplyLockedState({{firstKey, true}});
        QVERIFY(panel.LockedState().contains(firstKey));
        QVERIFY(!(firstItem->flags() & Qt::ItemIsUserCheckable));
        panel.ApplyLockedState({});
        QVERIFY(panel.LockedState().isEmpty());
        QVERIFY(firstItem->flags() & Qt::ItemIsUserCheckable);

        QTreeWidget* partTree = trees.first();
        QVERIFY(partTree->topLevelItemCount() > 0);
        partTree->topLevelItem(0)->setSelected(true);
        QVERIFY(!selections.isEmpty());
        partTree->clearSelection();
        QVERIFY(!selections.isEmpty());
        const QList<QVariant> last = selections.last();
        QVERIFY(last.size() == 2);
        QVERIFY(last.at(0).toStringList().isEmpty());
        QVERIFY(last.at(1).toStringList().isEmpty());

        QSignalSpy visibilityChanges(&panel, &CModelTreePanel::VisibilityChanged);
        QTreeWidgetItem* deformer = nullptr;
        const auto findDeformer = [&](QTreeWidgetItem* item, auto&& findRef) -> void {
            const QString type = item->data(0, Qt::UserRole + 1).toString();
            if (!deformer && (type == QLatin1String("warp") || type == QLatin1String("rotation"))) {
                deformer = item;
                return;
            }
            for (int i = 0; i < item->childCount(); ++i) findRef(item->child(i), findRef);
        };
        for (QTreeWidget* tree : trees) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) findDeformer(tree->topLevelItem(i), findDeformer);
            if (deformer) break;
        }
        QVERIFY(deformer);
        deformer->setCheckState(0, Qt::Unchecked);
        bool sawDeformerEvent = false;
        for (const auto& event : visibilityChanges) {
            if (event.size() >= 2 && (event.at(1).toString() == QLatin1String("warp") ||
                                      event.at(1).toString() == QLatin1String("rotation"))) {
                sawDeformerEvent = true;
                break;
            }
        }
        QVERIFY(sawDeformerEvent);
    }
};

QTEST_MAIN(ModelTreeTests)
#include "model_tree_tests.moc"
