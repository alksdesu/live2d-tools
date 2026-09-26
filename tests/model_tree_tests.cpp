#include "ui/cmodeltreepanel.h"
#include "ui/cmodelhierarchymodel.h"
#include "ui/cmodelhierarchyview.h"
#include "ui/cinspectorpanel.h"
#include "ui/para/ccidjson.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include "deformer_snapshot_fixture.h"
#include <QtTest>
#include <QSignalSpy>
#include <QAccessible>
#include <QCheckBox>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QSet>
#include <QSignalBlocker>
#include <QComboBox>
#include <QLineEdit>
#include <QItemSelectionModel>
#include <QMenu>
#include <QTimer>
#include <algorithm>

namespace {
using Visibility = QMap<QString, bool>;
CModelHierarchyModel* hierarchy(CModelTreePanel& panel, const char* name = "partHierarchy")
{
    auto* view = panel.findChild<CModelHierarchyView*>(name);
    return view ? qobject_cast<CModelHierarchyModel*>(view->model()) : nullptr;
}

void checkMirrors(CModelTreePanel& panel)
{
    const auto state = panel.VisibilityState();
    for (auto* view : panel.findChildren<CModelHierarchyView*>()) {
        const auto* model = qobject_cast<CModelHierarchyModel*>(view->model());
        for (const auto& index : model->Indices())
            QCOMPARE(index.data(Qt::CheckStateRole).toInt(), state.value(CModelHierarchyModel::Key(index)) ? int(Qt::Checked) : int(Qt::Unchecked));
    }
}

bool chooseMenuAction(CModelHierarchyView* view, const QModelIndex& index, const QString& actionText)
{
    bool chosen = false;
    QTimer closer;
    closer.setSingleShot(true);
    QObject::connect(&closer, &QTimer::timeout, view, [] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) menu->close();
    });
    closer.start(3000);
    QTimer selectAction;
    selectAction.setSingleShot(true);
    QObject::connect(&selectAction, &QTimer::timeout, view, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) return;
        for (auto* action : menu->actions()) if (action->text() == actionText) {
            menu->setActiveAction(action);
            QTest::keyClick(menu, Qt::Key_Return);
            chosen = true;
            return;
        }
        menu->close();
    });
    selectAction.start(0);
    return QMetaObject::invokeMethod(view, "customContextMenuRequested", Q_ARG(QPoint, view->visualRect(index).center())) && chosen;
}

QByteArray rootMeshesBytes()
{
    live2dstruct::CReader reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    live2dstruct::CSectionTable table(reader);
    if (!table.IsValid()) return {};
    auto bytes = reader.m_byte;
    const auto* offsets = table.sectionOffsetTable->art_meshes;
    for (int index = 0; index < table.art_meshes.size(); ++index) {
        DeformerSnapshotData::put(bytes, offsets->parent_part_indices + index * 4, -1);
        DeformerSnapshotData::put(bytes, offsets->parent_deformer_indices + index * 4, -1);
    }
    return bytes;
}

bool clickItem(CModelHierarchyView* view, const QModelIndex& index, Qt::KeyboardModifiers modifiers = {})
{
    view->scrollTo(index);
    QCoreApplication::processEvents();
    const auto rect = view->visualRect(index).intersected(view->viewport()->rect());
    if (rect.isEmpty()) return false;
    QTest::mouseClick(view->viewport(), Qt::LeftButton, modifiers, QPoint(rect.right() - 3, rect.center().y()));
    return true;
}

QByteArray deepTreeBytes(int count, bool deformers)
{
    using namespace DeformerSnapshotData;
    Fixture fixture;
    if (!fixture.table.IsValid()) return {};
    auto& bytes = fixture.bytes;
    if (!deformers) {
        for (int index = 0; index < 32; ++index) fixture.count(index, 0);
        fixture.count(0, count);
    } else fixture.count(1, count);
    const auto column = [&](int slot, int width, const auto& value) {
        const int offset = bytes.size();
        put(bytes, slot, offset);
        bytes.append(QByteArray(count * width, '\0'));
        for (int row = 0; row < count; ++row) value(offset + row * width, row);
    };
    column(deformers ? 0x6c : 0x4c, 64, [&](int offset, int row) {
        bytes.replace(offset, 64, (QByteArray(deformers ? "D" : "P") + QByteArray::number(row)).leftJustified(64, '\0'));
    });
    const auto numbers = [&](int slot, const auto& value) { column(slot, 4, [&](int offset, int row) { put(bytes, offset, value(row)); }); };
    if (deformers) {
        numbers(0x68, [](int) { return 0; });
        numbers(0x70, [](int) { return -1; });
        for (int slot : {0x74, 0x78, 0x84}) numbers(slot, [](int) { return 1; });
        numbers(0x7c, [](int) { return -1; });
        numbers(0x80, [](int row) { return row - 1; });
        numbers(0x88, [](int) { return 0; });
        fixture.column(fixture.table.sectionOffsetTable->art_meshes->parent_deformer_indices, 0, count - 1);
    } else {
        put(bytes, 0x48, 0);
        numbers(0x50, [](int) { return -1; });
        for (int slot : {0x54, 0x58}) numbers(slot, [](int) { return 0; });
        for (int slot : {0x5c, 0x60}) numbers(slot, [](int) { return 1; });
        numbers(0x64, [](int row) { return row - 1; });
    }
    return bytes;
}
}

class ModelTreeTests : public QObject
{
    Q_OBJECT
private slots:
    void realMouseMultiSelectionKeepsInspectorAndLegacyConsumersConsistent()
    {
        live2dstruct::CModelDocument document;
        const auto bytes = rootMeshesBytes();
        QVERIFY(!bytes.isEmpty());
        QVERIFY(document.LoadBytes(bytes, "root-selection-fixture"));
        QVERIFY(document.Drawables().size() >= 3);
        CModelTreePanel panel;
        QVERIFY(panel.BuildTreeBytes(bytes, nullptr));
        CInspectorPanel inspector;
        inspector.SetDocument(&document);
        connect(&panel, &CModelTreePanel::ObjectsSelected, &inspector, &CInspectorPanel::SetSelections);
        connect(&panel, &CModelTreePanel::ObjectSelected, &inspector, &CInspectorPanel::SetSelection);
        connect(&panel, &CModelTreePanel::MeshSelected, &panel, &CModelTreePanel::HighlightMesh);
        panel.resize(480, 500);
        panel.show();
        auto* view = panel.findChild<CModelHierarchyView*>("partHierarchy");
        auto* model = hierarchy(panel);
        QVERIFY(view && model);
        const QString first = document.Drawables()[0].id, second = document.Drawables()[1].id, third = document.Drawables()[2].id;
        const auto a = model->IndexForObject(first, "artmesh");
        const auto b = model->IndexForObject(second, "artmesh");
        const auto c = model->IndexForObject(third, "artmesh");
        QVERIFY(a.isValid() && b.isValid() && c.isValid());
        QVERIFY(clickItem(view, a));
        QCOMPARE(inspector.GetSelectionId(), first);
        QSignalSpy single(&panel, &CModelTreePanel::ObjectSelected);
        QSignalSpy mesh(&panel, &CModelTreePanel::MeshSelected);
        QSignalSpy batch(&panel, &CModelTreePanel::ObjectsSelected);
        QSignalSpy overlays(&panel, &CModelTreePanel::ShowDeformerMeshes);
        QVERIFY(clickItem(view, b, Qt::ControlModifier));
        QCOMPARE(view->selectionModel()->selectedRows().size(), 2);
        QVERIFY(view->selectionModel()->isSelected(a) && view->selectionModel()->isSelected(b));
        QVERIFY(inspector.GetSelectionId().isEmpty());
        QVERIFY(inspector.GetSelectionType().isEmpty());
        QCOMPARE(single.size(), 0);
        QCOMPARE(mesh.size(), 0);
        QCOMPARE(batch.size(), 1);
        const auto selectedIds = batch.last()[0].toStringList();
        QCOMPARE(QSet<QString>(selectedIds.cbegin(), selectedIds.cend()), (QSet<QString>{first, second}));
        QCOMPARE(overlays.size(), 1);
        QCOMPARE(overlays.last()[0].toStringList(), (QStringList{first, second}));
        auto* visibility = inspector.findChild<QCheckBox*>();
        QVERIFY(visibility && visibility->isEnabled());

        QVERIFY(clickItem(view, a));
        single.clear(); mesh.clear(); batch.clear(); overlays.clear();
        QVERIFY(clickItem(view, c, Qt::ShiftModifier));
        QCOMPARE(view->selectionModel()->selectedRows().size(), 3);
        QVERIFY(view->selectionModel()->isSelected(a) && view->selectionModel()->isSelected(b) && view->selectionModel()->isSelected(c));
        QVERIFY(inspector.GetSelectionId().isEmpty());
        QCOMPARE(single.size(), 0);
        QCOMPARE(mesh.size(), 0);
        QCOMPARE(batch.size(), 1);
        QCOMPARE(overlays.size(), 1);
        QCOMPARE(overlays.last()[0].toStringList(), (QStringList{first, second, third}));
        overlays.clear();
        QVERIFY(clickItem(view, c, Qt::ControlModifier));
        QCOMPARE(view->selectionModel()->selectedRows().size(), 2);
        QVERIFY(inspector.GetSelectionId().isEmpty());
        QCOMPARE(single.size(), 0);
        QCOMPARE(mesh.size(), 0);
        QCOMPARE(overlays.size(), 1);
        QCOMPARE(overlays.last()[0].toStringList(), (QStringList{first, second}));
        overlays.clear();
        QVERIFY(clickItem(view, a, Qt::ControlModifier));
        QCOMPARE(view->selectionModel()->selectedRows(), QModelIndexList{b});
        QCOMPARE(inspector.GetSelectionId(), second);
        QCOMPARE(single.size(), 0);
        QCOMPARE(mesh.size(), 0);
        QCOMPARE(overlays.size(), 1);
        QCOMPARE(overlays.last()[0].toStringList(), QStringList{second});
        QVERIFY(!document.CanUndoEdit());
    }

    void rejectedMouseSelectionKeepsInspectorCurrentAndAccessibleFocus()
    {
        live2dstruct::CModelDocument document;
        const auto bytes = rootMeshesBytes();
        QVERIFY(document.LoadBytes(bytes, "root-rollback-fixture"));
        CModelTreePanel panel;
        QVERIFY(panel.BuildTreeBytes(bytes, nullptr));
        CInspectorPanel inspector;
        inspector.SetDocument(&document);
        const QString accepted = document.Drawables()[0].id, rejected = document.Drawables()[1].id;
        int rejections = 0;
        connect(&panel, &CModelTreePanel::ObjectsSelected, &inspector, [&](const QStringList& ids, const QStringList& types) {
            if (ids.contains(rejected)) {
                ++rejections;
                const QSignalBlocker blocker(&panel);
                panel.HighlightObject(accepted, "artmesh");
                return;
            }
            inspector.SetSelections(ids, types);
        });
        connect(&panel, &CModelTreePanel::ObjectSelected, &inspector, &CInspectorPanel::SetSelection);
        connect(&panel, &CModelTreePanel::MeshSelected, &panel, &CModelTreePanel::HighlightMesh);
        panel.resize(480, 500);
        panel.show();
        auto* view = panel.findChild<CModelHierarchyView*>("partHierarchy");
        auto* model = hierarchy(panel);
        QVERIFY(view && model);
        const auto oldIndex = model->IndexForObject(accepted, "artmesh"), newIndex = model->IndexForObject(rejected, "artmesh");
        QVERIFY(clickItem(view, oldIndex));
        QCOMPARE(inspector.GetSelectionId(), accepted);
        QSignalSpy singles(&panel, &CModelTreePanel::ObjectSelected);
        QSignalSpy meshes(&panel, &CModelTreePanel::MeshSelected);
        QSignalSpy overlays(&panel, &CModelTreePanel::ShowDeformerMeshes);
        QVERIFY(clickItem(view, newIndex));
        QCOMPARE(rejections, 1);
        QCOMPARE(view->currentIndex(), oldIndex);
        QCOMPARE(view->selectionModel()->selectedRows(), QModelIndexList{oldIndex});
        QCOMPARE(inspector.GetSelectionId(), accepted);
        QCOMPARE(singles.size(), 0);
        QCOMPARE(meshes.size(), 0);
        QCOMPARE(overlays.size(), 0);
        auto* tree = QAccessible::queryAccessibleInterface(view);
        QVERIFY(tree && tree->focusChild());
        QCOMPARE(tree->focusChild()->text(QAccessible::Name), accepted);
        QVERIFY(tree->focusChild()->state().selected);
        QCOMPARE(tree->selectionInterface()->selectedItemCount(), 1);
        QVERIFY(!document.CanUndoEdit());
    }

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

    void cdiReparseOnlyReplacesNamesAndSourceAfterSuccess()
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
        const QByteArray secondBytes = "{\r\n\"Parts\":[{\"Id\":\"PartB\",\"Name\":\"B\"}],\r\n\"Parameters\":[{\"Id\":\"ParamB\",\"Name\":\"PB\"}],\"CombinedParameters\":[[\"ParamB\"]]}\r\n";
        QCOMPARE(second.write(secondBytes), secondBytes.size());
        second.close();

        CCidJson cid;
        QString error;
        QVERIFY(cid.SourcePath().isEmpty()); QVERIFY(cid.SourceDigest().isEmpty());
        QVERIFY(cid.Parse(firstPath, &error));
        QCOMPARE(cid.SourcePath(), firstPath);
        QVERIFY(first.open(QIODevice::ReadOnly));
        QCOMPARE(cid.SourceDigest(), QCryptographicHash::hash(first.readAll(), QCryptographicHash::Sha256)); first.close();
        const auto firstDigest = cid.SourceDigest();
        QCOMPARE(cid.getPartName(QStringLiteral("PartA")), QStringLiteral("A"));
        QVERIFY(cid.Parse(QDir::current().relativeFilePath(secondPath), &error));
        QCOMPARE(cid.SourcePath(), secondPath);
        const auto secondDigest = QCryptographicHash::hash(secondBytes, QCryptographicHash::Sha256);
        QCOMPARE(cid.SourceDigest(), secondDigest); QVERIFY(secondDigest != firstDigest);
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
        QCOMPARE(cid.getPartName(QStringLiteral("PartB")), QStringLiteral("B"));
        QCOMPARE(cid.getParaName(QStringLiteral("ParamB")), QStringLiteral("PB"));
        QVERIFY(cid.isCombinedParameter(QStringLiteral("ParamB")));
        QCOMPARE(cid.SourcePath(), secondPath); QCOMPARE(cid.SourceDigest(), secondDigest);
        QVERIFY(!cid.Parse(temp.filePath(QStringLiteral("broken.json")), &error));
        QCOMPARE(cid.getPartName(QStringLiteral("PartB")), QStringLiteral("B"));
        QCOMPARE(cid.SourcePath(), secondPath); QCOMPARE(cid.SourceDigest(), secondDigest);
        QVERIFY(QFile::remove(secondPath));
        QVERIFY(!cid.Parse(secondPath, &error));
        QCOMPARE(cid.SourcePath(), secondPath); QCOMPARE(cid.SourceDigest(), secondDigest);
        QCOMPARE(cid.getPartName(QStringLiteral("PartB")), QStringLiteral("B"));
    }

    void programmaticVisibilityChangesOnlyTheSelectedObject_data()
    {
        QTest::addColumn<QString>("type");
        for (const char* type : {"part", "warp", "rotation", "artmesh"}) QTest::newRow(type) << QString::fromLatin1(type);
    }

    void programmaticVisibilityChangesOnlyTheSelectedObject()
    {
        QFETCH(QString, type);
        CModelTreePanel panel;
        panel.BuildTree(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", nullptr);
        auto* model = hierarchy(panel);
        QVERIFY(model);
        QModelIndex target;
        auto states = panel.VisibilityState();
        int counter = 0;
        for (auto state = states.begin(); state != states.end(); ++state) state.value() = ++counter % 2;
        for (const auto& index : model->Indices())
            if (index.data(Qt::UserRole + 1).toString() == type && (type == "artmesh" || model->rowCount(index))) { target = index; break; }
        QVERIFY(target.isValid());
        const auto key = CModelHierarchyModel::Key(target);
        states[key] = true;
        panel.ApplyVisibilityState(states);
        QSignalSpy changes(&panel, &CModelTreePanel::VisibilityChanged);
        QSignalSpy batches(&panel, &CModelTreePanel::VisibilityBatchChanged);
        for (bool visible : {false, true}) {
            panel.SetVisibility(target.data(Qt::UserRole).toString(), type, visible);
            states[key] = visible;
            QCOMPARE(panel.VisibilityState(), states);
            QCOMPARE(changes.size(), 1);
            QCOMPARE(batches.size(), 1);
            QCOMPARE(qvariant_cast<Visibility>(batches.takeFirst().first()), (Visibility{{key, visible}}));
            changes.clear();
            checkMirrors(panel);
        }
        panel.SetVisibility(target.data(Qt::UserRole).toString(), "unknown", false);
        panel.SetVisibility("unknown", type, false);
        panel.SetVisibility({}, type, false);
        QCOMPARE(panel.VisibilityState(), states);
        QVERIFY(changes.isEmpty());
        QVERIFY(batches.isEmpty());
    }

    void matchingIdsInDifferentObjectTypesStayIndependent()
    {
        using namespace live2dstruct;
        CReader reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int warp = -1, rotation = -1;
        for (int index = 0; index < table.deformers.size(); ++index) {
            if (warp < 0 && table.deformers[index]->types == 0) warp = index;
            if (rotation < 0 && table.deformers[index]->types == 1) rotation = index;
        }
        QVERIFY(warp >= 0 && rotation >= 0);
        auto bytes = reader.m_byte;
        const QString name = "SharedObjectId";
        const auto& offsets = *table.sectionOffsetTable;
        for (int offset : {offsets.parts->ids, offsets.art_meshes->ids,
                           offsets.deformers->ids + warp * 64, offsets.deformers->ids + rotation * 64})
            bytes.replace(offset, 64, name.toUtf8().leftJustified(64, '\0'));
        CModelTreePanel panel;
        QVERIFY(panel.BuildTreeBytes(bytes, nullptr));
        auto* model = hierarchy(panel);
        auto states = panel.VisibilityState();
        for (auto state = states.begin(); state != states.end(); ++state) state.value() = true;
        panel.ApplyVisibilityState(states);
        for (const auto& type : {QStringLiteral("part"), QStringLiteral("artmesh"), QStringLiteral("warp"), QStringLiteral("rotation")}) {
            const auto index = model->IndexForObject(name, type);
            QVERIFY(index.isValid());
            panel.SetVisibility(name, type, false);
            auto expected = states;
            expected[type + '|' + name] = false;
            QCOMPARE(panel.VisibilityState(), expected);
            checkMirrors(panel);
            panel.SetVisibility(name, type, true);
        }
        QSignalSpy selections(&panel, &CModelTreePanel::ObjectsSelected);
        panel.HighlightMesh(name);
        auto* view = panel.findChild<CModelHierarchyView*>("partHierarchy");
        QCOMPARE(view->selectionModel()->selectedRows(), (QModelIndexList{model->IndexForObject(name, "artmesh")}));
        QCOMPARE(selections.size(), 1);
        panel.HighlightMesh("missing");
        QCOMPARE(selections.size(), 1);
        panel.HighlightMesh({});
        QCOMPARE(selections.size(), 2);
        for (auto* tree : panel.findChildren<CModelHierarchyView*>()) QVERIFY(tree->selectionModel()->selectedRows().isEmpty());
    }

    void treeCheckChangesStillApplyToWholeSubtree_data()
    {
        QTest::addColumn<QString>("viewName");
        QTest::newRow("parts") << QStringLiteral("partHierarchy");
        QTest::newRow("deformers") << QStringLiteral("deformerHierarchy");
    }

    void treeCheckChangesStillApplyToWholeSubtree()
    {
        QFETCH(QString, viewName);
        CModelTreePanel panel;
        panel.BuildTree(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", nullptr);
        auto* model = hierarchy(panel, viewName.toUtf8().constData());
        QVERIFY(model);
        QModelIndex parent;
        for (const auto& index : model->Indices()) if (model->rowCount(index)) { parent = index; break; }
        QVERIFY(parent.isValid());
        const auto keys = model->SubtreeKeys(parent);
        QVERIFY(keys.size() > 1);
        auto expected = panel.VisibilityState();
        for (const auto& key : keys) expected[key] = true;
        panel.ApplyVisibilityState(expected);
        QSignalSpy changes(&panel, &CModelTreePanel::VisibilityChanged);
        QSignalSpy batches(&panel, &CModelTreePanel::VisibilityBatchChanged);
        for (bool visible : {false, true}) {
            QVERIFY(model->setData(parent, visible ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole));
            for (const auto& key : keys) expected[key] = visible;
            QCOMPARE(panel.VisibilityState(), expected);
            QCOMPARE(changes.size(), keys.size());
            QCOMPARE(batches.size(), 1);
            QCOMPARE(qvariant_cast<Visibility>(batches.first().first()).size(), keys.size());
            changes.clear(); batches.clear();
            checkMirrors(panel);
        }
    }

    void locksNeverChangeVisibilityOrEmitVisibility()
    {
        CModelTreePanel panel;
        panel.BuildTree(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", nullptr);
        auto state = panel.VisibilityState();
        int counter = 0;
        for (auto entry = state.begin(); entry != state.end(); ++entry) entry.value() = ++counter % 2;
        panel.ApplyVisibilityState(state);
        QSignalSpy visibility(&panel, &CModelTreePanel::VisibilityChanged);
        QSignalSpy batches(&panel, &CModelTreePanel::VisibilityBatchChanged);
        auto* model = hierarchy(panel);
        const auto root = model->index(0, 0);
        const auto key = CModelHierarchyModel::Key(root);
        for (bool locked : {true, false, true}) {
            panel.ApplyLockedState({{key, locked}});
            QCOMPARE(panel.VisibilityState(), state);
            QCOMPARE(panel.LockedState().contains(key), locked);
            QCOMPARE(bool(model->flags(root) & Qt::ItemIsUserCheckable), !locked);
            checkMirrors(panel);
        }
        panel.ApplyLockedState({});
        QVERIFY(panel.LockedState().isEmpty());
        QVERIFY(visibility.isEmpty());
        QVERIFY(batches.isEmpty());
    }

    void initialVisibilityAndMappingsMatchSource_data()
    {
        QTest::addColumn<QString>("sample");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }

    void initialVisibilityAndMappingsMatchSource()
    {
        QFETCH(QString, sample);
        using namespace live2dstruct;
        CReader reader(QString(TEST_RESOURCES) + '/' + sample + '/' + sample + ".moc3");
        CSectionTable source(reader);
        QVERIFY(source.IsValid());
        CModelTreePanel panel;
        QVERIFY(panel.BuildTreeBytes(reader.m_byte, nullptr));
        auto* model = hierarchy(panel);
        auto* ownership = hierarchy(panel, "deformerHierarchy");
        QCOMPARE(model->Indices().size(), source.parts.size() + source.deformers.size() + source.art_meshes.size());
        QCOMPARE(ownership->Indices().size(), source.parts.size() + source.deformers.size());
        Visibility expected;
        for (const auto* part : source.parts) expected.insert("part|" + part->ids, part->is_visible);
        for (const auto* mesh : source.art_meshes) expected.insert("artmesh|" + mesh->ids, mesh->is_visible);
        for (int deformer = 0; deformer < source.deformers.size(); ++deformer) {
            const auto* value = source.deformers[deformer];
            const QString type = value->types == 0 ? "warp" : "rotation";
            expected.insert(type + '|' + value->ids, value->is_visible);
            const auto index = model->IndexForObject(value->ids, type);
            QVERIFY(index.isValid());
            QSet<QString> meshes;
            QSet<int> bindings;
            for (int parent = deformer; parent >= 0; parent = source.deformers[parent]->parent_deformer_indices)
                bindings.insert(source.deformers[parent]->keyform_binding_sources_indices);
            for (const auto* mesh : source.art_meshes) {
                for (int parent = mesh->parent_deformer_indices; parent >= 0;
                     parent = source.deformers[parent]->parent_deformer_indices) {
                    if (parent == deformer) {
                        meshes.insert(mesh->ids);
                        bindings.insert(mesh->keyform_binding_sources_indices);
                        break;
                    }
                }
            }
            const auto actualMeshes = model->MeshesForSelection(index);
            QCOMPARE(QSet<QString>(actualMeshes.cbegin(), actualMeshes.cend()), meshes);
            QCOMPARE(actualMeshes.size(), meshes.size());
            const auto info = model->ParametersForSelection(index);
            QCOMPARE(info.ids.size(), info.minimums.size());
            QCOMPARE(info.ids.size(), info.maximums.size());
            QCOMPARE(info.ids.size(), info.defaults.size());
            QCOMPARE(QSet<QString>(info.ids.cbegin(), info.ids.cend()).size(), info.ids.size());
            QSet<QString> expectedParameters;
            for (int binding : bindings) {
                if (binding < 0) continue;
                const auto* keyform = source.keyform_bindings[binding];
                const int end = keyform->parameter_binding_index_sources_begin_indices + keyform->parameter_binding_index_sources_counts;
                for (int reference = keyform->parameter_binding_index_sources_begin_indices; reference < end; ++reference) {
                    const int parameterBinding = source.parameter_binding_indices[reference]->binding_sources_indices;
                    const live2dstruct::CParameters* owner = nullptr;
                    for (const auto* parameter : source.parameters)
                        if (parameterBinding >= parameter->parameter_binding_sources_begin_indices &&
                            parameterBinding < parameter->parameter_binding_sources_begin_indices + parameter->parameter_binding_sources_counts)
                            owner = parameter;
                    if (owner) expectedParameters.insert(owner->ids);
                }
            }
            QCOMPARE(QSet<QString>(info.ids.cbegin(), info.ids.cend()), expectedParameters);
            for (int parameter = 0; parameter < info.ids.size(); ++parameter) {
                const auto found = std::find_if(source.parameters.cbegin(), source.parameters.cend(),
                    [&](const auto* entry) { return entry->ids == info.ids[parameter]; });
                QVERIFY(found != source.parameters.cend());
                QCOMPARE(info.minimums[parameter], (*found)->min_values);
                QCOMPARE(info.maximums[parameter], (*found)->max_values);
                QCOMPARE(info.defaults[parameter], (*found)->default_values);
            }
        }
        QCOMPARE(panel.VisibilityState(), expected);
        checkMirrors(panel);
    }

    void batchRejectionRestoresMirrorsWithoutLegacyFalseEvents()
    {
        CModelTreePanel panel;
        panel.BuildTree(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", nullptr);
        auto before = panel.VisibilityState();
        for (auto entry = before.begin(); entry != before.end(); ++entry) entry.value() = true;
        panel.ApplyVisibilityState(before);
        auto* model = hierarchy(panel);
        connect(&panel, &CModelTreePanel::VisibilityBatchChanged, &panel, [&](const Visibility&) {
            const QSignalBlocker blocker(&panel);
            panel.ApplyVisibilityState(before);
        });
        QSignalSpy legacy(&panel, &CModelTreePanel::VisibilityChanged);
        const auto root = model->index(0, 0);
        QVERIFY(root.isValid());
        QVERIFY(model->setData(root, Qt::Unchecked, Qt::CheckStateRole));
        QCOMPARE(panel.VisibilityState(), before);
        QVERIFY(legacy.isEmpty());
        checkMirrors(panel);
    }

    void lockMenuPublishesOneBatchWithAtomicRollback_data()
    {
        QTest::addColumn<bool>("reject");
        QTest::newRow("accept") << false;
        QTest::newRow("reject") << true;
    }

    void lockMenuPublishesOneBatchWithAtomicRollback()
    {
        QFETCH(bool, reject);
        CModelTreePanel panel;
        panel.BuildTree(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", nullptr);
        auto* model = hierarchy(panel);
        const auto items = model->Indices();
        QVERIFY(items.size() >= 2);
        auto* view = panel.findChild<CModelHierarchyView*>("partHierarchy");
        panel.resize(540, 480);
        panel.show();
        view->scrollTo(items[0]);
        QItemSelection selected;
        selected.select(items[0], items[0]);
        selected.select(items[1], items[1]);
        view->selectionModel()->select(selected, QItemSelectionModel::ClearAndSelect);
        QCoreApplication::processEvents();
        const auto visibility = panel.VisibilityState();
        const auto before = panel.LockedState();
        if (reject) connect(&panel, &CModelTreePanel::LocksBatchChanged, &panel,
                            [&](const Visibility&) { panel.ApplyLockedState(before); });
        QSignalSpy batches(&panel, &CModelTreePanel::LocksBatchChanged);
        QSignalSpy legacy(&panel, &CModelTreePanel::ObjectLockChanged);
        QSignalSpy changedVisibility(&panel, &CModelTreePanel::VisibilityBatchChanged);
        QVERIFY(chooseMenuAction(view, items[0], QStringLiteral("锁定选中对象")));
        QCOMPARE(batches.size(), 1);
        const Visibility expected{{CModelHierarchyModel::Key(items[0]), true}, {CModelHierarchyModel::Key(items[1]), true}};
        QCOMPARE(qvariant_cast<Visibility>(batches.first().first()), expected);
        QCOMPARE(legacy.size(), reject ? 0 : 2);
        QCOMPARE(panel.LockedState(), reject ? before : expected);
        QCOMPARE(panel.VisibilityState(), visibility);
        QVERIFY(changedVisibility.isEmpty());
        for (auto* tree : panel.findChildren<CModelHierarchyView*>()) {
            auto* linked = qobject_cast<CModelHierarchyModel*>(tree->model());
            for (auto value = expected.cbegin(); value != expected.cend(); ++value) {
                const auto index = linked->IndexForKey(value.key());
                if (index.isValid()) QCOMPARE(index.data(Qt::UserRole + 2).toBool(), !reject);
            }
        }
    }

    void soloKeepsSelectedDescendantsAndAncestorsConsistent()
    {
        CModelTreePanel panel;
        panel.BuildTree(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", nullptr);
        auto* model = hierarchy(panel);
        QModelIndex selected;
        for (const auto& index : model->Indices())
            if (index.parent().isValid() && model->rowCount(index)) { selected = index; break; }
        QVERIFY(selected.isValid());
        QSet<QString> expected;
        for (const auto& key : model->SubtreeKeys(selected)) expected.insert(key);
        for (auto parent = selected.parent(); parent.isValid(); parent = parent.parent()) expected.insert(CModelHierarchyModel::Key(parent));
        const auto state = model->SoloVisibility({CModelHierarchyModel::Key(selected)});
        for (auto value = state.cbegin(); value != state.cend(); ++value) QCOMPARE(value.value(), expected.contains(value.key()));
        auto* view = panel.findChild<CModelHierarchyView*>("partHierarchy");
        panel.resize(540, 480);
        panel.show();
        view->scrollTo(selected);
        view->selectionModel()->select(selected, QItemSelectionModel::ClearAndSelect);
        QCoreApplication::processEvents();
        QSignalSpy batches(&panel, &CModelTreePanel::VisibilityBatchChanged);
        QSignalSpy changes(&panel, &CModelTreePanel::VisibilityChanged);
        QVERIFY(chooseMenuAction(view, selected, QStringLiteral("独显选中对象")));
        QCOMPARE(panel.VisibilityState(), state);
        QCOMPARE(batches.size(), 1);
        for (const auto& change : changes) QCOMPARE(change[2].toBool(), state.value(change[1].toString() + '|' + change[0].toString()));
        checkMirrors(panel);
    }

    void rootlessHierarchyAndOnDemandMappingsAreComplete()
    {
        DeformerSnapshotData::Fixture fixture;
        CModelTreePanel panel;
        QString error;
        QVERIFY2(panel.BuildTreeBytes(fixture.bytes, nullptr, &error), qPrintable(error));
        auto* model = hierarchy(panel);
        auto* deformers = hierarchy(panel, "deformerHierarchy");
        QCOMPARE(model->Indices().size(), 4);
        QCOMPARE(deformers->rowCount(), 3);
        const auto root = model->IndexForObject("Root", "rotation");
        const auto warp = model->IndexForObject("Warp", "warp");
        const auto child = model->IndexForObject("Child", "rotation");
        const auto mesh = model->IndexForObject("Mesh", "artmesh");
        QVERIFY(root.isValid() && !root.parent().isValid());
        QCOMPARE(warp.parent(), root);
        QCOMPARE(child.parent(), warp);
        QCOMPARE(mesh.parent(), child);
        QCOMPARE(model->MeshesForSelection(root), QStringList{"Mesh"});
        QCOMPARE(deformers->MeshesForSelection(deformers->IndexForObject("Warp", "warp")), QStringList{"Mesh"});
        QCOMPARE(model->ParametersForSelection(root).ids, QStringList{"P"});
        fixture.column(fixture.table.sectionOffsetTable->art_meshes->parent_deformer_indices, 0, -1);
        QVERIFY(panel.BuildTreeBytes(fixture.bytes, nullptr));
        QCOMPARE(model->rowCount(), 2);
        const auto unparented = model->IndexForObject("Mesh", "artmesh");
        QVERIFY(unparented.isValid());
        QVERIFY(!unparented.parent().isValid());
        panel.HighlightMesh("Mesh");
        auto* meshView = panel.findChild<CModelHierarchyView*>("partHierarchy");
        auto* deformerView = panel.findChild<CModelHierarchyView*>("deformerHierarchy");
        QCOMPARE(meshView->selectionModel()->selectedRows(), QModelIndexList{unparented});
        QVERIFY(deformerView->selectionModel()->selectedRows().isEmpty());
    }

    void forwardParentReferencesKeepOriginalSiblingOrder()
    {
        auto bytes = deepTreeBytes(4, false);
        const int offset = DeformerSnapshotData::integer(bytes, 0x64);
        for (int index = 0; index < 4; ++index) DeformerSnapshotData::put(bytes, offset + index * 4, index == 3 ? -1 : 3);
        CModelTreePanel panel;
        QString error;
        QVERIFY2(panel.BuildTreeBytes(bytes, nullptr, &error), qPrintable(error));
        for (auto* view : panel.findChildren<CModelHierarchyView*>()) {
            auto* model = qobject_cast<CModelHierarchyModel*>(view->model());
            QCOMPARE(model->rowCount(), 1);
            const auto root = model->index(0, 0);
            QCOMPARE(root.data(Qt::UserRole).toString(), QStringLiteral("P3"));
            QCOMPARE(model->rowCount(root), 3);
            for (int row = 0; row < 3; ++row) {
                const auto child = model->index(row, 0, root);
                QCOMPARE(child.parent(), root);
                QCOMPARE(child.data(Qt::UserRole).toString(), QStringLiteral("P") + QString::number(row));
            }
        }
    }

    void deepLegalTreesKeepHierarchyAndSurviveInteraction_data()
    {
        QTest::addColumn<bool>("deformers");
        QTest::newRow("parts") << false;
        QTest::newRow("deformers") << true;
    }

    void deepLegalTreesKeepHierarchyAndSurviveInteraction()
    {
        QFETCH(bool, deformers);
        constexpr int count = 12000;
        const auto bytes = deepTreeBytes(count, deformers);
        QVERIFY(!bytes.isEmpty());
        auto panel = std::make_unique<CModelTreePanel>();
        QString error;
        QVERIFY2(panel->BuildTreeBytes(bytes, nullptr, &error), qPrintable(error));
        auto* model = hierarchy(*panel);
        QCOMPARE(model->Indices().size(), count + (deformers ? 1 : 0));
        const QString id = QString(deformers ? "D" : "P") + QString::number(count - 1);
        const QString type = deformers ? "rotation" : "part";
        const auto last = model->IndexForObject(id, type);
        int depth = 0;
        for (auto index = last; index.isValid(); index = index.parent()) ++depth;
        QCOMPARE(depth, count);
        auto* view = panel->findChild<CModelHierarchyView*>("partHierarchy");
        panel->resize(540, 480);
        panel->show();
        panel->HighlightObject(id, type);
        QCOMPARE(view->selectionModel()->selectedRows(), QModelIndexList{last});
        view->expandToDepth(count);
        QCoreApplication::processEvents();
        QVERIFY(!view->grab().isNull());
        auto* search = panel->findChild<QLineEdit*>();
        search->setText(id);
        QCoreApplication::processEvents();
        QVERIFY(!view->visualRect(last).isEmpty());
        search->clear();
        const auto initial = panel->VisibilityState();
        panel->SetVisibility(id, type, false);
        QCOMPARE(panel->VisibilityState().value(type + '|' + id), false);
        panel->ApplyLockedState({{type + '|' + id, true}});
        QCOMPARE(panel->VisibilityState().value(type + '|' + id), false);
        panel->ApplyVisibilityState(initial);
        QVERIFY(panel->BuildTreeBytes(bytes, nullptr, &error));
        const auto rebuilt = model->IndexForObject(id, type);
        QCOMPARE(view->selectionModel()->selectedRows(), QModelIndexList{rebuilt});
        QVERIFY(view->isExpanded(rebuilt.parent()));
        QVERIFY(!panel->BuildTreeBytes("bad moc", nullptr, &error));
        QCOMPARE(model->IndexForObject(id, type), rebuilt);
        panel.reset();
    }
};

QTEST_MAIN(ModelTreeTests)
#include "model_tree_tests.moc"
