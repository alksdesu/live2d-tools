#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/ckeyformmappingcontext.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"
#include "ui/cinspectorpanel.h"
#include "ui/cmodeltreepanel.h"

#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTreeWidgetItemIterator>
#include <QtEndian>
#include <algorithm>
#include <limits>

using namespace live2dstruct;

namespace {
QString typeOf(const CModelDeformer& deformer)
{
    return deformer.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
}

QMap<QString, float> intermediateValues(const CModelDocument& document)
{
    QMap<QString, float> result;
    for (const auto& parameter : document.Parameters())
        result.insert(parameter.id, float(double(parameter.minimum) + (double(parameter.maximum) - parameter.minimum) * 0.37));
    return result;
}

QSet<QString> meshesBelow(const CModelDocument& document, int ancestor)
{
    QSet<QString> result;
    for (const auto& mesh : document.Drawables()) {
        QSet<int> visited;
        int index = mesh.parentDeformerIndex;
        while (index >= 0 && index < document.Deformers().size() && !visited.contains(index)) {
            if (index == ancestor) { result.insert(mesh.id); break; }
            visited.insert(index);
            index = document.DeformerParentIndex(document.Deformers()[index].id);
        }
    }
    return result;
}

int crossPartDeformer(const CModelDocument& document)
{
    for (int index = 0; index < document.Deformers().size(); ++index) {
        const auto& deformer = document.Deformers()[index];
        const int parent = deformer.parentDeformerIndex;
        if (deformer.parentPartIndex >= 0 && parent >= 0 && parent < document.Deformers().size() &&
            deformer.parentPartIndex != document.Deformers()[parent].parentPartIndex && !meshesBelow(document, index).isEmpty())
            return index;
    }
    return -1;
}

int animatedDeformer(const CModelDocument& document, const QString& type)
{
    for (int index = 0; index < document.Deformers().size(); ++index) {
        const auto& deformer = document.Deformers()[index];
        if (typeOf(deformer) != type) continue;
        const int count = deformer.type == CModelDeformer::Warp ? deformer.warpKeyforms.size() : deformer.rotationKeyforms.size();
        QMap<QString, float> pose;
        if (count >= 3 && document.KeyformParameterValues(deformer.id, type, 2, pose)) return index;
    }
    return -1;
}

CModelEdit changedForm(const CModelDocument& document, const QString& id, const QString& type, int keyform)
{
    if (type == QLatin1String("warp")) {
        const auto before = document.DeformerWarpKeyforms(id);
        auto after = before;
        after[keyform].positions[0] += 0.125f;
        return {id, type, CModelEdit::WarpKeyforms, QVariant::fromValue(before), QVariant::fromValue(after), keyform, false};
    }
    const auto before = document.DeformerRotationKeyforms(id);
    auto after = before;
    after[keyform].angle += 7.25f;
    return {id, type, CModelEdit::RotationKeyforms, QVariant::fromValue(before), QVariant::fromValue(after), keyform, false};
}

QVariant forms(const CModelDocument& document, const QString& id, const QString& type)
{
    return type == QLatin1String("warp") ? QVariant::fromValue(document.DeformerWarpKeyforms(id))
                                         : QVariant::fromValue(document.DeformerRotationKeyforms(id));
}

struct Editor
{
    CModelDocument document;
    CKeyformEditController controller;
    CInspectorPanel inspector;
    QMap<QString, float> current;
    bool accept = true;
    int attempts = 0, builds = 0, accepted = 0, activations = 0;
    QByteArray publishedBytes;
    QMap<QString, float> publishedPose;
    bool publishedEditing = false;
    QVector<CModelEdit> edits;

    bool load(const QString& model, QString* error)
    {
        if (!document.Load(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3", error)) return false;
        bind();
        return true;
    }

    void bind()
    {
        current = intermediateValues(document);
        controller.SetDocument(&document);
        controller.SetPublisher([this](const CModelDocument& candidate, const QMap<QString, float>& pose,
                                       bool editing, QString* error) {
            ++attempts;
            QByteArray bytes;
            if (!candidate.BuildPreviewMoc(bytes, error)) return false;
            auto reader = CReader::FromBytes(bytes);
            CSectionTable table(reader);
            if (!table.IsValid()) { if (error) *error = reader.Error(); return false; }
            ++builds;
            if (!accept) { if (error) *error = QStringLiteral("controlled deformer preview rejection"); return false; }
            ++accepted;
            publishedBytes = bytes;
            publishedPose = pose;
            publishedEditing = editing;
            return true;
        }, [](QString*) { return true; });
        inspector.SetDocument(&document);
        inspector.SetDeformerKeyformHandler([this](const QString& id, const QString& type, int keyform, QString* error) {
            ++activations;
            return controller.Activate(id, type, keyform, current, error);
        });
        inspector.SetEditHandler([this](const CModelEdit& edit, QString* error) {
            edits.append(edit);
            if (!CKeyformEditController::Handles(edit) || !controller.Submit(edit, current, edit.index, error)) return false;
            refresh();
            return true;
        });
        inspector.SetHistoryHandler([this](bool undo, CModelEdit& edit, QString* error) {
            if (!controller.History(undo, edit, error)) return false;
            refresh();
            return true;
        });
    }

    void refresh()
    {
        const QSignalBlocker blocker(&inspector);
        inspector.RefreshSelection();
        inspector.SyncHistoryState();
    }
};

QTreeWidgetItem* item(QTreeWidget* tree, const QString& id, const QString& type)
{
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toString() == id && (*it)->data(0, Qt::UserRole + 1).toString() == type) return *it;
    return nullptr;
}

QTreeWidgetItem* deformerAncestor(QTreeWidgetItem* value)
{
    for (auto* parent = value ? value->parent() : nullptr; parent; parent = parent->parent()) {
        const auto type = parent->data(0, Qt::UserRole + 1).toString();
        if (type == QLatin1String("warp") || type == QLatin1String("rotation")) return parent;
    }
    return nullptr;
}

QTreeWidget* partTree(CModelTreePanel& panel)
{
    for (auto* tree : panel.findChildren<QTreeWidget*>())
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole + 1).toString() == QLatin1String("artmesh")) return tree;
    return nullptr;
}

QStringList treeSnapshot(CModelTreePanel& panel)
{
    QStringList result;
    for (auto* tree : panel.findChildren<QTreeWidget*>()) {
        result.append(QStringLiteral("tree"));
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            QStringList path;
            for (auto* node = *it; node; node = node->parent())
                path.prepend(node->data(0, Qt::UserRole + 1).toString() + '|' + node->data(0, Qt::UserRole).toString());
            result.append(path.join('/') + QStringLiteral(":%1:%2:%3").arg((*it)->isSelected())
                          .arg((*it)->isExpanded()).arg(int((*it)->checkState(0))));
        }
    }
    return result;
}

void models()
{
    QTest::addColumn<QString>("model");
    for (const char* model : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(model) << QString::fromLatin1(model);
}
}

class DeformerEditorTests : public QObject
{
    Q_OBJECT

    static void verifyRejectedBindingBytes(const QByteArray& bytes)
    {
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error));
        const int index = animatedDeformer(document, QStringLiteral("warp"));
        QVERIFY(index >= 0);
        const auto target = document.Deformers()[index];
        CModelTreePanel panel;
        QVERIFY(panel.BuildTreeBytes(document.SourceBytes(), nullptr, &error));
        panel.HighlightObject(target.id, typeOf(target));
        panel.ApplyLockedState({{typeOf(target) + '|' + target.id, true}});
        panel.ApplyVisibilityState({{QStringLiteral("artmesh|") + document.Drawables().front().id, false}});
        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        auto* hierarchy = partTree(panel);
        QVERIFY(hierarchy);
        auto* deformers = trees[0] == hierarchy ? trees[1] : trees[0];
        auto* targetItem = item(deformers, target.id, typeOf(target));
        QVERIFY(targetItem);
        const auto snapshot = treeSnapshot(panel);
        const auto locks = panel.LockedState();
        const auto visibility = panel.VisibilityState();
        QSignalSpy meshes(&panel, &CModelTreePanel::ShowDeformerMeshes);
        QSignalSpy parameters(&panel, &CModelTreePanel::DeformerDragParamsChanged);
        QVERIFY(QMetaObject::invokeMethod(deformers, "itemClicked", Q_ARG(QTreeWidgetItem*, targetItem), Q_ARG(int, 0)));
        QCOMPARE(meshes.size(), 1);
        QCOMPARE(parameters.size(), 1);
        const auto meshMapping = meshes.takeFirst();
        const auto parameterMapping = parameters.takeFirst();
        auto reader = CReader::FromBytes(bytes);
        CSectionTable section(reader);
        QVERIFY2(section.IsValid(), qPrintable(reader.Error()));
        QSignalSpy selections(&panel, &CModelTreePanel::ObjectsSelected);
        QSignalSpy checked(&panel, &CModelTreePanel::VisibilityChanged);
        QSignalSpy locked(&panel, &CModelTreePanel::ObjectLockChanged);
        QVERIFY(!panel.BuildTreeBytes(bytes, nullptr, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(treeSnapshot(panel), snapshot);
        QCOMPARE(panel.LockedState(), locks);
        QCOMPARE(panel.VisibilityState(), visibility);
        QCOMPARE(item(deformers, target.id, typeOf(target)), targetItem);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("invalid-bindings.moc3"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        panel.BuildTree(path, nullptr);
        QCOMPARE(treeSnapshot(panel), snapshot);
        QCOMPARE(panel.LockedState(), locks);
        QCOMPARE(panel.VisibilityState(), visibility);
        QCOMPARE(item(deformers, target.id, typeOf(target)), targetItem);
        QCOMPARE(selections.size(), 0);
        QCOMPARE(checked.size(), 0);
        QCOMPARE(locked.size(), 0);
        QVERIFY(QMetaObject::invokeMethod(deformers, "itemClicked", Q_ARG(QTreeWidgetItem*, targetItem), Q_ARG(int, 0)));
        QCOMPARE(meshes.size(), 1);
        QCOMPARE(parameters.size(), 1);
        QCOMPARE(meshes.first(), meshMapping);
        QCOMPARE(parameters.first(), parameterMapping);
    }
private slots:
    void invalidBindingSlicesAreRejectedBeforeTreeReplacement_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("mutation");
        const QStringList kinds{QStringLiteral("parameter-owners"), QStringLiteral("binding-indices"), QStringLiteral("key-values")};
        const QStringList mutations{QStringLiteral("negative-begin"), QStringLiteral("negative-count"),
            QStringLiteral("int-max-begin"), QStringLiteral("max-plus-max"), QStringLiteral("int-max-count"),
            QStringLiteral("past-end"), QStringLiteral("overrun"), QStringLiteral("invalid-empty-begin"),
            QStringLiteral("int-max-empty-begin")};
        for (int kind = 0; kind < kinds.size(); ++kind)
            for (int mutation = 0; mutation < mutations.size(); ++mutation)
                QTest::newRow(qPrintable(kinds[kind] + '-' + mutations[mutation])) << kind << mutation;
    }

    void invalidBindingSlicesAreRejectedBeforeTreeReplacement()
    {
        QFETCH(int, kind);
        QFETCH(int, mutation);
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QByteArray bytes = document.SourceBytes();
        const int begins[]{0x120, 0x164, 0x16c};
        const int counts[]{0x124, 0x168, 0x170};
        const int pools[]{13, 11, 14};
        const int countOffset = qFromLittleEndian<qint32>(bytes.constData() + 0x40);
        const int poolSize = qFromLittleEndian<qint32>(bytes.constData() + countOffset + pools[kind] * 4);
        QVERIFY(poolSize > 0);
        int begin = 0, count = 1;
        switch (mutation) {
        case 0: begin = -1; break;
        case 1: count = -1; break;
        case 2: begin = std::numeric_limits<int>::max(); break;
        case 3: begin = count = std::numeric_limits<int>::max(); break;
        case 4: count = std::numeric_limits<int>::max(); break;
        case 5: begin = poolSize; break;
        case 6: begin = poolSize - 1; count = 2; break;
        case 7: begin = -2; count = 0; break;
        case 8: begin = std::numeric_limits<int>::max(); count = 0; break;
        }
        qToLittleEndian(qint32(begin), bytes.data() + qFromLittleEndian<qint32>(bytes.constData() + begins[kind]));
        qToLittleEndian(qint32(count), bytes.data() + qFromLittleEndian<qint32>(bytes.constData() + counts[kind]));
        verifyRejectedBindingBytes(bytes);
    }

    void invalidBindingReferencesPreserveTheTree_data()
    {
        QTest::addColumn<int>("slot");
        QTest::addColumn<int>("pool");
        QTest::addColumn<int>("value");
        for (const auto& entry : {qMakePair(0x70, 12), qMakePair(0xc8, 12), qMakePair(0x160, 13)}) {
            QTest::newRow(qPrintable(QStringLiteral("%1-negative").arg(entry.first))) << entry.first << entry.second << -2;
            QTest::newRow(qPrintable(QStringLiteral("%1-past-end").arg(entry.first))) << entry.first << entry.second << -99;
            QTest::newRow(qPrintable(QStringLiteral("%1-int-max").arg(entry.first)))
                << entry.first << entry.second << std::numeric_limits<int>::max();
        }
        QTest::newRow("binding-index-cannot-be-unbound") << 0x160 << 13 << -1;
    }

    void invalidBindingReferencesPreserveTheTree()
    {
        QFETCH(int, slot);
        QFETCH(int, pool);
        QFETCH(int, value);
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QByteArray bytes = document.SourceBytes();
        if (value == -99) {
            const int countOffset = qFromLittleEndian<qint32>(bytes.constData() + 0x40);
            value = qFromLittleEndian<qint32>(bytes.constData() + countOffset + pool * 4);
        }
        qToLittleEndian(qint32(value), bytes.data() + qFromLittleEndian<qint32>(bytes.constData() + slot));
        verifyRejectedBindingBytes(bytes);
    }

    void emptyBindingSlicesAndLastElementsRemainReadable_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("position");
        for (int kind = 0; kind < 3; ++kind)
            for (int position = 0; position < 4; ++position)
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(kind).arg(position))) << kind << position;
    }

    void emptyBindingSlicesAndLastElementsRemainReadable()
    {
        QFETCH(int, kind);
        QFETCH(int, position);
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QByteArray bytes = document.SourceBytes();
        const int begins[]{0x120, 0x164, 0x16c};
        const int counts[]{0x124, 0x168, 0x170};
        const int pools[]{13, 11, 14};
        const int countOffset = qFromLittleEndian<qint32>(bytes.constData() + 0x40);
        const int size = qFromLittleEndian<qint32>(bytes.constData() + countOffset + pools[kind] * 4);
        const int begin = position == 0 ? -1 : position == 1 ? 0 : position == 2 ? size : size - 1;
        const int count = position == 3 ? 1 : 0;
        qToLittleEndian(qint32(begin), bytes.data() + qFromLittleEndian<qint32>(bytes.constData() + begins[kind]));
        qToLittleEndian(qint32(count), bytes.data() + qFromLittleEndian<qint32>(bytes.constData() + counts[kind]));
        CModelTreePanel panel;
        QString error;
        QVERIFY2(panel.BuildTreeBytes(bytes, nullptr, &error), qPrintable(error));
        QVERIFY(partTree(panel));
    }

    void completelyEmptyBindingPoolsAllowUnboundObjects()
    {
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QByteArray bytes = document.SourceBytes();
        const int countOffset = qFromLittleEndian<qint32>(bytes.constData() + 0x40);
        const auto put = [&bytes](int slot, int row, int value) {
            const int offset = qFromLittleEndian<qint32>(bytes.constData() + slot);
            qToLittleEndian(qint32(value), bytes.data() + offset + row * 4);
        };
        for (int index : {11, 12, 13, 14}) qToLittleEndian(qint32(0), bytes.data() + countOffset + index * 4);
        for (int index = 0; index < document.Parameters().size(); ++index) {
            put(0x120, index, index % 2 ? -1 : 0);
            put(0x124, index, 0);
        }
        for (int index = 0; index < document.Deformers().size(); ++index) put(0x70, index, -1);
        for (int index = 0; index < document.Drawables().size(); ++index) put(0xc8, index, -1);
        CModelTreePanel panel;
        QString error;
        QVERIFY2(panel.BuildTreeBytes(bytes, nullptr, &error), qPrintable(error));
        QVERIFY(partTree(panel));
        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        auto* deformerTree = trees[0] == partTree(panel) ? trees[1] : trees[0];
        const int index = animatedDeformer(document, QStringLiteral("warp"));
        QVERIFY(index >= 0);
        const auto& target = document.Deformers()[index];
        auto* targetItem = item(deformerTree, target.id, typeOf(target));
        QVERIFY(targetItem);
        QSignalSpy parameters(&panel, &CModelTreePanel::DeformerDragParamsChanged);
        QVERIFY(QMetaObject::invokeMethod(deformerTree, "itemClicked", Q_ARG(QTreeWidgetItem*, targetItem), Q_ARG(int, 0)));
        QCOMPARE(parameters.size(), 1);
        QVERIFY(parameters.first()[0].toStringList().isEmpty());
    }

    void crossPartSourceParentsDetachAndRestoreThroughHistoryAndSession_data() { models(); }
    void crossPartSourceParentsDetachAndRestoreThroughHistoryAndSession()
    {
        QFETCH(QString, model);
        Editor editor;
        QString error;
        QVERIFY2(editor.load(model, &error), qPrintable(error));
        const CModelDocument baseline = editor.document;
        const int index = crossPartDeformer(editor.document);
        QVERIFY(index >= 0);
        const auto target = editor.document.Deformers()[index];
        const QString type = typeOf(target);
        const int sourceParent = target.parentDeformerIndex;
        editor.inspector.SetSelection(target.id, type);
        auto* parent = editor.inspector.findChild<QComboBox*>("deformerParentSelector");
        QVERIFY(parent && parent->findData(-1) >= 0 && parent->findData(sourceParent) >= 0);
        QCOMPARE(parent->currentData().toInt(), sourceParent);
        QCOMPARE(editor.attempts, 0);
        const quint64 revision = editor.document.Revision();
        QSignalSpy history(&editor.inspector, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&editor.inspector, &CInspectorPanel::EditRejected);
        editor.accept = false;
        parent->setCurrentIndex(parent->findData(-1));
        QCOMPARE(editor.attempts, 1);
        QCOMPARE(editor.builds, 1);
        QCOMPARE(editor.accepted, 0);
        QCOMPARE(editor.document.Revision(), revision);
        QCOMPARE(editor.document.DeformerParentIndex(target.id), sourceParent);
        QCOMPARE(parent->currentData().toInt(), sourceParent);
        QVERIFY(!editor.document.CanUndoEdit());
        QCOMPARE(history.count(), 0);
        QCOMPARE(rejected.count(), 1);
        editor.accept = true;
        parent->setCurrentIndex(parent->findData(-1));
        QCOMPARE(editor.document.DeformerParentIndex(target.id), -1);
        QCOMPARE(editor.document.Revision(), revision + 1);
        QVERIFY(!editor.controller.IsActive());
        QVERIFY(!editor.publishedEditing);
        QVERIFY(editor.publishedPose.isEmpty());
        QCOMPARE(editor.activations, 0);
        QCOMPARE(history.count(), 1);
        CModelSession detached;
        detached.sourceMoc = baseline.SourcePath();
        detached.sourceDigest = baseline.SourceDigest();
        detached.deformerParentIndices = editor.document.DeformerParentOverrides();
        editor.inspector.Undo();
        QCOMPARE(editor.document.DeformerParentIndex(target.id), sourceParent);
        QCOMPARE(parent->currentData().toInt(), sourceParent);
        QVERIFY(!editor.document.CanUndoEdit() && editor.document.CanRedoEdit());
        QCOMPARE(editor.publishedBytes, baseline.SourceBytes());
        CModelSession restoredSource = detached;
        restoredSource.deformerParentIndices = editor.document.DeformerParentOverrides();
        editor.inspector.Redo();
        QCOMPARE(editor.document.DeformerParentIndex(target.id), -1);
        QVERIFY(editor.document.CanUndoEdit() && !editor.document.CanRedoEdit());
        QCOMPARE(history.count(), 3);
        for (const CModelSession& sourceSession : {restoredSource, detached}) {
            CModelSession session;
            QVERIFY2(CModelSession::FromJson(sourceSession.ToJson(), &session, &error), qPrintable(error));
            CModelDocument candidate;
            QVERIFY2(session.RestoreDocument(baseline, candidate, &error), qPrintable(error));
            const int expected = sourceSession.deformerParentIndices.value(target.id);
            QVERIFY2(editor.controller.Restore(std::move(candidate), &error), qPrintable(error));
            editor.refresh();
            QCOMPARE(editor.document.DeformerParentIndex(target.id), expected);
            QCOMPARE(parent->currentData().toInt(), expected);
            QVERIFY(!editor.document.CanUndoEdit() && !editor.document.CanRedoEdit());
            QVERIFY(!editor.controller.IsActive());
        }
        QCOMPARE(editor.activations, 0);
        QCOMPARE(editor.document.SourceBytes(), baseline.SourceBytes());
    }

    void deformerSelectionAndTableEditingUseOneCandidate_data()
    {
        QTest::addColumn<QString>("type");
        QTest::newRow("warp") << QStringLiteral("warp");
        QTest::newRow("rotation") << QStringLiteral("rotation");
    }

    void deformerSelectionAndTableEditingUseOneCandidate()
    {
        QFETCH(QString, type);
        Editor editor;
        QString error;
        QVERIFY2(editor.load("Mark", &error), qPrintable(error));
        const int index = animatedDeformer(editor.document, type);
        QVERIFY(index >= 0);
        const auto target = editor.document.Deformers()[index];
        editor.inspector.SetSelection(target.id, type);
        auto* warp = editor.inspector.findChild<QComboBox*>("warpKeyformSelector");
        auto* table = editor.inspector.findChild<QTableWidget*>(type == QLatin1String("warp") ? "warpKeyformTable" : "rotationKeyformTable");
        QVERIFY(warp && table && table->rowCount());
        QCOMPARE(editor.attempts, 0);
        QCOMPARE(editor.activations, 0);
        const auto revision = editor.document.Revision();
        const auto original = forms(editor.document, target.id, type);
        const auto parameterValues = editor.document.ParameterValues();
        QSignalSpy rejected(&editor.inspector, &CInspectorPanel::EditRejected);
        QSignalSpy history(&editor.inspector, &CInspectorPanel::HistoryChanged);
        const auto selectSecond = [&] {
            if (type == QLatin1String("warp")) QTest::keyClick(warp, Qt::Key_Down);
            else table->setCurrentCell(1, 0);
        };
        const auto selected = [&] { return type == QLatin1String("warp") ? warp->currentIndex() : table->currentRow(); };
        QCOMPARE(selected(), 0);
        editor.accept = false;
        selectSecond();
        QCOMPARE(editor.attempts, 1);
        QCOMPARE(editor.activations, 1);
        QCOMPARE(selected(), 0);
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(history.count(), 0);
        QVERIFY(!editor.controller.IsActive());
        QCOMPARE(editor.document.Revision(), revision);
        editor.accept = true;
        selectSecond();
        QCOMPARE(editor.attempts, 2);
        QCOMPARE(editor.activations, 2);
        QCOMPARE(editor.controller.Type(), type);
        QCOMPARE(editor.controller.Target(), target.id);
        QCOMPARE(editor.controller.Keyform(), 1);
        QCOMPARE(selected(), 1);
        QMap<QString, float> corner;
        QVERIFY(editor.document.KeyformParameterValues(target.id, type, 1, corner));
        auto expectedPose = editor.current;
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) expectedPose.insert(it.key(), it.value());
        QCOMPARE(editor.controller.Pose(), expectedPose);
        QCOMPARE(editor.publishedPose, expectedPose);
        QCOMPARE(editor.document.ParameterValues(), parameterValues);
        QCOMPARE(editor.document.Revision(), revision);
        editor.inspector.RefreshSelection();
        QCOMPARE(editor.activations, 2);
        QCOMPARE(editor.attempts, 2);
        const int row = type == QLatin1String("warp") ? 0 : 1;
        const QString originalText = table->item(row, 1)->text();
        const QString editedText = QString::number(originalText.toDouble() + 0.125, 'g', 9);
        editor.accept = false;
        table->item(row, 1)->setText(editedText);
        QCOMPARE(editor.attempts, 3);
        QCOMPARE(editor.document.Revision(), revision);
        QCOMPARE(forms(editor.document, target.id, type), original);
        QCOMPARE(table->item(row, 1)->text(), originalText);
        QCOMPARE(selected(), 1);
        QCOMPARE(history.count(), 0);
        QCOMPARE(rejected.count(), 2);
        editor.accept = true;
        table->item(row, 1)->setText(editedText);
        const auto edited = forms(editor.document, target.id, type);
        QVERIFY(edited != original);
        QCOMPARE(editor.attempts, 4);
        QCOMPARE(editor.edits.last().index, 1);
        QCOMPARE(editor.document.Revision(), revision + 1);
        QCOMPARE(history.count(), 1);
        QCOMPARE(editor.activations, 2);
        editor.inspector.Undo();
        QCOMPARE(editor.attempts, 5);
        QCOMPARE(forms(editor.document, target.id, type), original);
        QVERIFY(!editor.document.CanUndoEdit() && editor.document.CanRedoEdit());
        editor.inspector.Redo();
        QCOMPARE(editor.attempts, 6);
        QCOMPARE(forms(editor.document, target.id, type), edited);
        QCOMPARE(editor.document.Revision(), revision + 3);
        QCOMPARE(history.count(), 3);
        QCOMPARE(editor.activations, 2);
        QCOMPARE(selected(), 1);
        QCOMPARE(editor.controller.Keyform(), 1);
        QCOMPARE(editor.publishedPose, expectedPose);
        editor.refresh();
        QCoreApplication::processEvents();
        QCOMPARE(editor.attempts, 6);
        QCOMPARE(editor.activations, 2);
    }

    void submittingAnotherFormUpdatesTheSameDeformerPose_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("type");
        for (const QString& model : {QStringLiteral("Mark"), QStringLiteral("Natori"), QStringLiteral("Rice"), QStringLiteral("Wanko")})
            for (const QString& type : {QStringLiteral("warp"), QStringLiteral("rotation")})
                QTest::newRow(qPrintable(model + '-' + type)) << model << type;
    }

    void submittingAnotherFormUpdatesTheSameDeformerPose()
    {
        QFETCH(QString, model);
        QFETCH(QString, type);
        Editor editor;
        QString error;
        QVERIFY(editor.load(model, &error));
        const int index = animatedDeformer(editor.document, type);
        QVERIFY(index >= 0);
        const QString id = editor.document.Deformers()[index].id;
        QVERIFY2(editor.controller.Activate(id, type, 0, editor.current, &error), qPrintable(error));
        const auto previousPose = editor.controller.Pose();
        const auto command = changedForm(editor.document, id, type, 2);
        QVERIFY2(editor.controller.Submit(command, editor.current, 2, &error), qPrintable(error));
        QCOMPARE(editor.attempts, 2);
        QCOMPARE(editor.controller.Target(), id);
        QCOMPARE(editor.controller.Type(), type);
        QCOMPARE(editor.controller.Keyform(), 2);
        QMap<QString, float> corner;
        QVERIFY(editor.document.KeyformParameterValues(id, type, 2, corner));
        auto expected = editor.current;
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) expected.insert(it.key(), it.value());
        QCOMPARE(editor.controller.Pose(), expected);
        QCOMPARE(editor.publishedPose, expected);
        QVERIFY(previousPose != expected);
        QVERIFY(!editor.controller.BeginDrag({0}, {0, 0}, &error));
        QVERIFY(!editor.controller.IsDragging());
        const auto pose = editor.controller.Pose();
        QVERIFY(!editor.controller.Activate(id, type == QLatin1String("warp") ? QStringLiteral("rotation") : QStringLiteral("warp"),
                                            0, editor.current, &error));
        QCOMPARE(editor.controller.Pose(), pose);
        QCOMPARE(editor.attempts, 2);
    }

    void actualBuilderRejectionDoesNotCommitTheDocumentOrPose()
    {
        CModelDocument original;
        const QString path = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        QVERIFY(original.Load(path));
        QByteArray bytes = original.SourceBytes();
        QVector<int> warps;
        for (int index = 0; index < original.Deformers().size(); ++index)
            if (original.Deformers()[index].type == CModelDeformer::Warp) warps.append(index);
        QVERIFY(warps.size() >= 2);
        const int specificColumn = qFromLittleEndian<qint32>(bytes.constData() + 0x88);
        const qint32 first = qFromLittleEndian<qint32>(bytes.constData() + specificColumn + warps[0] * 4);
        qToLittleEndian(first, bytes.data() + specificColumn + warps[1] * 4);
        Editor editor;
        QString error;
        QVERIFY2(editor.document.LoadBytes(bytes, path, &error), qPrintable(error));
        editor.bind();
        const QString id = editor.document.Deformers()[warps[0]].id;
        const auto before = editor.document.DeformerWarpKeyforms(id);
        const auto command = changedForm(editor.document, id, QStringLiteral("warp"), 0);
        const auto revision = editor.document.Revision();
        QVERIFY(!editor.controller.Submit(command, editor.current, 0, &error));
        QVERIFY2(error.contains(QStringLiteral("共享")), qPrintable(error));
        QCOMPARE(editor.attempts, 1);
        QCOMPARE(editor.builds, 0);
        QCOMPARE(editor.accepted, 0);
        QCOMPARE(editor.document.Revision(), revision);
        QCOMPARE(editor.document.DeformerWarpKeyforms(id), before);
        QVERIFY(!editor.document.CanUndoEdit());
        QVERIFY(!editor.controller.IsActive());
        QVERIFY(editor.publishedBytes.isEmpty());
    }

    void deformerActivationCannotReplaceAnActiveMeshDrag()
    {
        Editor editor;
        QString error;
        QVERIFY(editor.load("Mark", &error));
        QMap<QString, float> defaults;
        for (const auto& parameter : editor.document.Parameters()) defaults.insert(parameter.id, parameter.defaultValue);
        QString meshId;
        QVector<float> modelPositions;
        for (const auto& mesh : editor.document.Drawables()) {
            if (mesh.keyforms.isEmpty() || mesh.keyforms[0].positions.isEmpty()) continue;
            QMap<QString, float> corner;
            if (!editor.document.KeyformParameterValues(mesh.id, QStringLiteral("artmesh"), 0, corner)) continue;
            auto pose = defaults;
            for (auto it = corner.cbegin(); it != corner.cend(); ++it) pose.insert(it.key(), it.value());
            CKeyformMappingContext mapping;
            if (!mapping.Build(editor.document, mesh.id, 0, pose, &error) ||
                !mapping.ToModel(mesh.keyforms[0].positions, modelPositions, &error)) continue;
            meshId = mesh.id;
            break;
        }
        QVERIFY2(!meshId.isEmpty(), qPrintable(error));
        const int warpIndex = animatedDeformer(editor.document, QStringLiteral("warp"));
        QVERIFY(warpIndex >= 0);
        const QString warpId = editor.document.Deformers()[warpIndex].id;
        QVERIFY2(editor.controller.Activate(meshId, 0, defaults, &error), qPrintable(error));
        QVERIFY2(editor.controller.BeginDrag({0}, modelPositions, &error), qPrintable(error));
        const auto pose = editor.controller.Pose();
        QCOMPARE(editor.attempts, 1);
        QVERIFY(!editor.controller.Activate(warpId, QStringLiteral("warp"), 0, defaults, &error));
        QVERIFY(editor.controller.IsDragging());
        QCOMPARE(editor.controller.Type(), QStringLiteral("artmesh"));
        QCOMPARE(editor.controller.Target(), meshId);
        QCOMPARE(editor.controller.Pose(), pose);
        QCOMPARE(editor.attempts, 1);
        editor.controller.FinishDrag();
        QVERIFY2(editor.controller.Activate(warpId, QStringLiteral("warp"), 0, defaults, &error), qPrintable(error));
        QCOMPARE(editor.controller.Type(), QStringLiteral("warp"));
        QVERIFY(!editor.controller.BeginDrag({0}, modelPositions, &error));
        QCOMPARE(editor.attempts, 2);
        QVERIFY(!editor.document.CanUndoEdit());
    }

    void locksRejectTheTargetOldParentAndNewParentPart_data()
    {
        QTest::addColumn<int>("lockKind");
        QTest::newRow("target") << 0;
        QTest::newRow("old-parent") << 1;
        QTest::newRow("own-part") << 2;
        QTest::newRow("new-parent-part") << 3;
    }

    void locksRejectTheTargetOldParentAndNewParentPart()
    {
        QFETCH(int, lockKind);
        Editor editor;
        QString error;
        QVERIFY(editor.load("Mark", &error));
        int targetIndex = -1;
        for (int index = 0; index < editor.document.Deformers().size(); ++index) {
            const auto& target = editor.document.Deformers()[index];
            const int parent = target.parentDeformerIndex;
            if (parent < 0 || target.parentPartIndex < 0) continue;
            const int parentPart = editor.document.Deformers()[parent].parentPartIndex;
            if (parentPart < 0 || parentPart == target.parentPartIndex) continue;
            QSet<int> ancestors;
            for (int part = target.parentPartIndex; part >= 0 && !ancestors.contains(part); part = editor.document.Parts()[part].parentIndex)
                ancestors.insert(part);
            if (!ancestors.contains(parentPart)) { targetIndex = index; break; }
        }
        QVERIFY(targetIndex >= 0);
        const auto target = editor.document.Deformers()[targetIndex];
        const auto parent = editor.document.Deformers()[target.parentDeformerIndex];
        CModelEdit command;
        if (lockKind == 3) {
            QVERIFY(editor.document.SetDeformerParentIndex(target.id, -1));
            editor.document.SetLocked(QStringLiteral("part"), editor.document.Parts()[parent.parentPartIndex].id, true);
            command = {target.id, typeOf(target), CModelEdit::DeformerParent, -1, target.parentDeformerIndex};
        } else {
            if (lockKind == 0) editor.document.SetLocked(typeOf(target), target.id, true);
            if (lockKind == 1) editor.document.SetLocked(typeOf(parent), parent.id, true);
            if (lockKind == 2) editor.document.SetLocked(QStringLiteral("part"), editor.document.Parts()[target.parentPartIndex].id, true);
            command = changedForm(editor.document, target.id, typeOf(target), 0);
        }
        const auto revision = editor.document.Revision();
        const auto before = forms(editor.document, target.id, typeOf(target));
        const int previousParent = editor.document.DeformerParentIndex(target.id);
        QVERIFY(!editor.controller.Submit(command, editor.current, command.index, &error));
        QVERIFY2(error.contains(QStringLiteral("锁定")), qPrintable(error));
        QCOMPARE(editor.attempts, 0);
        QCOMPARE(editor.document.Revision(), revision);
        QCOMPARE(forms(editor.document, target.id, typeOf(target)), before);
        QCOMPARE(editor.document.DeformerParentIndex(target.id), previousParent);
        QVERIFY(!editor.document.CanUndoEdit());
        QVERIFY(!editor.controller.IsActive());
    }

    void treeRebuildMovesHierarchyAndAncestorMeshMembership_data() { models(); }
    void treeRebuildMovesHierarchyAndAncestorMeshMembership()
    {
        QFETCH(QString, model);
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3", &error));
        const int index = crossPartDeformer(document);
        QVERIFY(index >= 0);
        const auto target = document.Deformers()[index];
        const auto previousParent = document.Deformers()[target.parentDeformerIndex];
        const auto targetMeshes = meshesBelow(document, index);
        const auto before = meshesBelow(document, target.parentDeformerIndex);
        QVERIFY(!targetMeshes.isEmpty());
        CModelTreePanel panel;
        QVERIFY2(panel.BuildTreeBytes(document.SourceBytes(), nullptr, &error), qPrintable(error));
        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        auto* hierarchy = partTree(panel);
        QVERIFY(hierarchy);
        auto* deformers = trees[0] == hierarchy ? trees[1] : trees[0];
        auto* child = item(hierarchy, target.id, typeOf(target));
        QVERIFY(child && deformerAncestor(child));
        QCOMPARE(deformerAncestor(child)->data(0, Qt::UserRole).toString(), previousParent.id);
        QSignalSpy meshes(&panel, &CModelTreePanel::ShowDeformerMeshes);
        auto* parentItem = item(deformers, previousParent.id, typeOf(previousParent));
        QVERIFY(parentItem);
        QVERIFY(QMetaObject::invokeMethod(deformers, "itemClicked", Q_ARG(QTreeWidgetItem*, parentItem), Q_ARG(int, 0)));
        QCOMPARE(meshes.size(), 1);
        const auto oldList = meshes.takeFirst()[0].toStringList();
        QCOMPARE(QSet<QString>(oldList.cbegin(), oldList.cend()), before);
        QVERIFY(document.SetDeformerParentIndex(target.id, -1));
        QByteArray preview;
        QVERIFY2(document.BuildPreviewMoc(preview, &error), qPrintable(error));
        QSignalSpy selections(&panel, &CModelTreePanel::ObjectsSelected);
        QVERIFY2(panel.BuildTreeBytes(preview, nullptr, &error), qPrintable(error));
        QCOMPARE(selections.size(), 0);
        child = item(hierarchy, target.id, typeOf(target));
        QVERIFY(child);
        QVERIFY(!deformerAncestor(child));
        parentItem = item(deformers, previousParent.id, typeOf(previousParent));
        QVERIFY(parentItem);
        QVERIFY(QMetaObject::invokeMethod(deformers, "itemClicked", Q_ARG(QTreeWidgetItem*, parentItem), Q_ARG(int, 0)));
        QCOMPARE(meshes.size(), 1);
        const auto newList = meshes.takeFirst()[0].toStringList();
        QCOMPARE(QSet<QString>(newList.cbegin(), newList.cend()), meshesBelow(document, target.parentDeformerIndex));
        QCOMPARE(QSet<QString>(newList.cbegin(), newList.cend()), before - targetMeshes);
        panel.HighlightObject(target.id, typeOf(target));
        QCOMPARE(selections.size(), 1);
        QCOMPARE(selections.first(), (QList<QVariant>{QStringList{target.id}, QStringList{typeOf(target)}}));
        const auto snapshot = treeSnapshot(panel);
        QVERIFY(!panel.BuildTreeBytes(QByteArray("broken moc"), nullptr, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(treeSnapshot(panel), snapshot);
        QCOMPARE(selections.size(), 1);
    }

    void highlightObjectDistinguishesIdenticalIdsAcrossTypes()
    {
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const int index = animatedDeformer(document, QStringLiteral("warp"));
        QVERIFY(index >= 0);
        const QString shared = document.Deformers()[index].id;
        QByteArray bytes = document.SourceBytes();
        const int partIds = qFromLittleEndian<qint32>(bytes.constData() + 0x4c);
        bytes.replace(partIds, 64, shared.toUtf8().leftJustified(64, '\0'));
        CModelTreePanel panel;
        QString error;
        QVERIFY2(panel.BuildTreeBytes(bytes, nullptr, &error), qPrintable(error));
        QSignalSpy selections(&panel, &CModelTreePanel::ObjectsSelected);
        for (const QString& type : {QStringLiteral("part"), QStringLiteral("warp")}) {
            panel.HighlightObject(shared, type);
            QCOMPARE(selections.size(), 1);
            QCOMPARE(selections.takeFirst(), (QList<QVariant>{QStringList{shared}, QStringList{type}}));
            int selected = 0;
            for (auto* tree : panel.findChildren<QTreeWidget*>())
                for (auto* value : tree->selectedItems()) {
                    ++selected;
                    QCOMPARE(value->data(0, Qt::UserRole).toString(), shared);
                    QCOMPARE(value->data(0, Qt::UserRole + 1).toString(), type);
                }
            QVERIFY(selected > 0);
        }
        const auto snapshot = treeSnapshot(panel);
        panel.HighlightObject(shared, QStringLiteral("rotation"));
        QVERIFY(selections.isEmpty());
        QCOMPARE(treeSnapshot(panel), snapshot);
    }

    void invalidParentRelationsPreserveTheExistingTree_data()
    {
        QTest::addColumn<int>("mutation");
        const QStringList names{QStringLiteral("part-self"), QStringLiteral("part-cycle"),
            QStringLiteral("part-parent-range"), QStringLiteral("part-parent-negative"),
            QStringLiteral("deformer-self"), QStringLiteral("deformer-cycle"),
            QStringLiteral("deformer-parent-range"), QStringLiteral("deformer-parent-negative"),
            QStringLiteral("deformer-part-range"), QStringLiteral("deformer-part-negative"),
            QStringLiteral("artmesh-part-range"), QStringLiteral("artmesh-part-negative"),
            QStringLiteral("artmesh-parent-range"), QStringLiteral("artmesh-parent-negative")};
        for (int index = 0; index < names.size(); ++index) QTest::newRow(qPrintable(names[index])) << index;
    }

    void invalidParentRelationsPreserveTheExistingTree()
    {
        QFETCH(int, mutation);
        CModelDocument document;
        QString error;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error));
        QVERIFY(document.Parts().size() > 1 && document.Deformers().size() > 1 && !document.Drawables().isEmpty());
        const int index = animatedDeformer(document, QStringLiteral("warp"));
        QVERIFY(index >= 0);
        const auto target = document.Deformers()[index];
        CModelTreePanel panel;
        QVERIFY(panel.BuildTreeBytes(document.SourceBytes(), nullptr, &error));
        panel.HighlightObject(target.id, typeOf(target));
        panel.ApplyLockedState({{typeOf(target) + '|' + target.id, true}});
        panel.ApplyVisibilityState({{QStringLiteral("artmesh|") + document.Drawables().front().id, false}});
        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        auto* hierarchy = partTree(panel);
        QVERIFY(hierarchy);
        auto* deformers = trees[0] == hierarchy ? trees[1] : trees[0];
        auto* targetItem = item(deformers, target.id, typeOf(target));
        QVERIFY(targetItem);
        targetItem->setExpanded(false);
        const auto snapshot = treeSnapshot(panel);
        const auto locks = panel.LockedState();
        const auto visibility = panel.VisibilityState();
        QSignalSpy meshes(&panel, &CModelTreePanel::ShowDeformerMeshes);
        QSignalSpy parameters(&panel, &CModelTreePanel::DeformerDragParamsChanged);
        QVERIFY(QMetaObject::invokeMethod(deformers, "itemClicked", Q_ARG(QTreeWidgetItem*, targetItem), Q_ARG(int, 0)));
        QCOMPARE(meshes.size(), 1);
        QCOMPARE(parameters.size(), 1);
        const auto meshMapping = meshes.takeFirst();
        const auto parameterMapping = parameters.takeFirst();
        QByteArray bytes = document.SourceBytes();
        const auto put = [&bytes](int slot, int row, int value) {
            const int offset = qFromLittleEndian<qint32>(bytes.constData() + slot);
            qToLittleEndian(qint32(value), bytes.data() + offset + row * 4);
        };
        switch (mutation) {
        case 0: put(0x64, 0, 0); break;
        case 1: put(0x64, 0, 1); put(0x64, 1, 0); break;
        case 2: put(0x64, 0, document.Parts().size()); break;
        case 3: put(0x64, 0, -2); break;
        case 4: put(0x80, 0, 0); break;
        case 5: put(0x80, 0, 1); put(0x80, 1, 0); break;
        case 6: put(0x80, 0, document.Deformers().size()); break;
        case 7: put(0x80, 0, -2); break;
        case 8: put(0x7c, 0, document.Parts().size()); break;
        case 9: put(0x7c, 0, -2); break;
        case 10: put(0xdc, 0, document.Parts().size()); break;
        case 11: put(0xdc, 0, -2); break;
        case 12: put(0xe0, 0, document.Deformers().size()); break;
        case 13: put(0xe0, 0, -2); break;
        }
        auto reader = CReader::FromBytes(bytes);
        CSectionTable section(reader);
        QVERIFY2(section.IsValid(), qPrintable(reader.Error()));
        QSignalSpy selections(&panel, &CModelTreePanel::ObjectsSelected);
        QSignalSpy checked(&panel, &CModelTreePanel::VisibilityChanged);
        QSignalSpy locked(&panel, &CModelTreePanel::ObjectLockChanged);
        QVERIFY(!panel.BuildTreeBytes(bytes, nullptr, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(treeSnapshot(panel), snapshot);
        QCOMPARE(panel.LockedState(), locks);
        QCOMPARE(panel.VisibilityState(), visibility);
        QCOMPARE(item(deformers, target.id, typeOf(target)), targetItem);
        QCOMPARE(selections.size(), 0);
        QCOMPARE(checked.size(), 0);
        QCOMPARE(locked.size(), 0);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("invalid-relations.moc3"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        panel.BuildTree(path, nullptr);
        QCOMPARE(treeSnapshot(panel), snapshot);
        QCOMPARE(panel.LockedState(), locks);
        QCOMPARE(panel.VisibilityState(), visibility);
        QCOMPARE(item(deformers, target.id, typeOf(target)), targetItem);
        QCOMPARE(selections.size(), 0);
        QCOMPARE(checked.size(), 0);
        QCOMPARE(locked.size(), 0);
        QVERIFY(QMetaObject::invokeMethod(deformers, "itemClicked", Q_ARG(QTreeWidgetItem*, targetItem), Q_ARG(int, 0)));
        QCOMPARE(meshes.size(), 1);
        QCOMPARE(parameters.size(), 1);
        QCOMPARE(meshes.first(), meshMapping);
        QCOMPARE(parameters.first(), parameterMapping);
    }
};

QTEST_MAIN(DeformerEditorTests)
#include "deformer_editor_tests.moc"
