#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace live2dstruct;

namespace {
QString sample() { return QString(TEST_RESOURCES) + "/Mark/Mark.moc3"; }

QMap<QString, bool> selection(const CModelDocument& document)
{
    const auto all = document.VisibilityState();
    QMap<QString, bool> result;
    const auto add = [&](const QString& key) { result.insert(key, all.value(key)); };
    add("part|" + document.Parts().front().id);
    add("artmesh|" + document.Drawables().front().id);
    bool warp = false, rotation = false;
    for (const auto& deformer : document.Deformers()) {
        if (deformer.type == CModelDeformer::Warp && !warp) {
            add("warp|" + deformer.id);
            warp = true;
        } else if (deformer.type == CModelDeformer::Rotation && !rotation) {
            add("rotation|" + deformer.id);
            rotation = true;
        }
    }
    return result;
}

CModelEdit visibilityEdit(const QMap<QString, bool>& before, const QMap<QString, bool>& after)
{
    return {"visibility", "objects", CModelEdit::VisibilityValues,
            QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
}

QVector<bool> rawVisibility(const CModelDocument& document)
{
    QVector<bool> values;
    for (const auto& part : document.Parts()) values.append(part.visible);
    for (const auto& mesh : document.Drawables()) values.append(mesh.visible);
    for (const auto& deformer : document.Deformers()) values.append(deformer.visible);
    return values;
}
}

class VisibilityEditTests : public QObject
{
    Q_OBJECT
private slots:
    void lockedBatchesAreAtomicAndDoNotEnterHistory()
    {
        CModelDocument document;
        QVERIFY(document.Load(sample()));
        const auto before = selection(document);
        auto after = before;
        for (auto it = after.begin(); it != after.end(); ++it) it.value() = !it.value();
        QVERIFY(document.ExecuteEdit(visibilityEdit(before, after)));
        const auto visibility = document.VisibilityState();
        const auto source = document.SourceBytes();
        QMap<QString, bool> locks;
        for (auto it = before.cbegin(); it != before.cend(); ++it) locks.insert(it.key(), true);
        const auto revision = document.Revision();
        QVERIFY(document.SetLockedValues(locks));
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(document.LockedKeys(), QSet<QString>(locks.keyBegin(), locks.keyEnd()));
        QCOMPARE(document.VisibilityState(), visibility);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        CModelEdit pending;
        QVERIFY(document.PeekUndoEdit(&pending));
        QCOMPARE(pending.field, CModelEdit::VisibilityValues);
        QCOMPARE((pending.before.value<QMap<QString, bool>>()), before);
        QCOMPARE((pending.after.value<QMap<QString, bool>>()), after);
        const auto accepted = document.LockedKeys();
        const QMap<QString, bool> invalid{{locks.firstKey(), false}, {"wrong-type|missing", true}};
        QVERIFY(!document.SetLockedValues(invalid));
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(document.LockedKeys(), accepted);
        QCOMPARE(document.VisibilityState(), visibility);
        QVERIFY(document.UndoEdit());
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(document.LockedKeys(), accepted);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.VisibilityState(), visibility);
        auto unlock = locks;
        for (auto it = unlock.begin(); it != unlock.end(); ++it) it.value() = false;
        const auto unlockRevision = document.Revision();
        QVERIFY(document.SetLockedValues(unlock));
        QCOMPARE(document.Revision(), unlockRevision + 1);
        QVERIFY(document.LockedKeys().isEmpty());
        QCOMPARE(document.VisibilityState(), visibility);
        QCOMPARE(document.SourceBytes(), source);
        QVERIFY(document.PeekUndoEdit(&pending));
        QCOMPARE(pending.field, CModelEdit::VisibilityValues);
    }

    void multiObjectChangeIsOneHistoryEntry()
    {
        CModelDocument document;
        QVERIFY(document.Load(sample()));
        const auto source = document.SourceBytes();
        const auto digest = document.SourceDigest();
        const auto beforeAll = document.VisibilityState();
        const auto before = selection(document);
        QCOMPARE(before.size(), 4);
        auto after = before;
        auto expected = beforeAll;
        for (auto it = after.begin(); it != after.end(); ++it) {
            it.value() = !it.value();
            expected[it.key()] = it.value();
        }
        const auto revision = document.Revision();
        QVERIFY(document.ExecuteEdit(visibilityEdit(before, after)));
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(document.VisibilityState(), expected);
        CModelEdit command;
        QVERIFY(document.PeekUndoEdit(&command));
        QCOMPARE(command.field, CModelEdit::VisibilityValues);
        QCOMPARE((command.before.value<QMap<QString, bool>>()), before);
        QCOMPARE((command.after.value<QMap<QString, bool>>()), after);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.VisibilityState(), beforeAll);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.VisibilityState(), expected);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(document.SourceBytes(), source);
        QCOMPARE(document.SourceDigest(), digest);
        QByteArray preview;
        QVERIFY(document.BuildPreviewMoc(preview));
        QCOMPARE(preview, source);
    }

    void invalidCommandsAreAtomic_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"wrong-before", "different-keys", "unknown", "wrong-type", "wrong-value", "empty"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }

    void invalidCommandsAreAtomic()
    {
        QFETCH(QString, kind);
        CModelDocument document;
        QVERIFY(document.Load(sample()));
        auto before = selection(document), after = before;
        for (auto it = after.begin(); it != after.end(); ++it) it.value() = !it.value();
        if (kind == "wrong-before") before.first() = !before.first();
        if (kind == "different-keys") {
            after.remove(after.firstKey());
            after.insert("part|missing", true);
        }
        if (kind == "unknown") {
            before.insert("part|missing", false);
            after.insert("part|missing", true);
        }
        if (kind == "empty") { before.clear(); after.clear(); }
        auto command = visibilityEdit(before, after);
        if (kind == "wrong-type") command.type = "part";
        if (kind == "wrong-value") command.after = QStringLiteral("invalid");
        const auto state = document.VisibilityState();
        const auto raw = rawVisibility(document);
        const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(command));
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.VisibilityState(), state);
        QCOMPARE(rawVisibility(document), raw);
        QVERIFY(!document.CanUndoEdit() && !document.CanRedoEdit());
        auto values = selection(document);
        values.insert("wrong-type|" + document.Drawables().front().id, false);
        QVERIFY(!document.SetVisibilityValues(values));
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(rawVisibility(document), raw);
    }

    void duplicateTypedObjectsRejectTheWholeBatch_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("part") << 0;
        QTest::newRow("mesh") << 1;
        QTest::newRow("deformer") << 2;
    }

    void duplicateTypedObjectsRejectTheWholeBatch()
    {
        QFETCH(int, kind);
        CModelDocument document;
        QVERIFY(document.Load(sample()));
        QString duplicate;
        if (kind == 0) {
            auto& parts = const_cast<QVector<CModelPart>&>(document.Parts());
            QVERIFY(parts.size() >= 2);
            parts[1].id = parts[0].id;
            duplicate = "part|" + parts[0].id;
        } else if (kind == 1) {
            auto& meshes = const_cast<QVector<CModelDrawable>&>(document.Drawables());
            QVERIFY(meshes.size() >= 2);
            meshes[1].id = meshes[0].id;
            duplicate = "artmesh|" + meshes[0].id;
        } else {
            auto& deformers = const_cast<QVector<CModelDeformer>&>(document.Deformers());
            QVERIFY(deformers.size() >= 2);
            deformers[1].id = deformers[0].id;
            deformers[1].type = deformers[0].type;
            duplicate = (deformers[0].type == CModelDeformer::Warp ? QStringLiteral("warp|") :
                         QStringLiteral("rotation|")) + deformers[0].id;
        }
        const auto before = rawVisibility(document);
        const auto revision = document.Revision();
        auto values = selection(document);
        for (auto it = values.begin(); it != values.end(); ++it) it.value() = !it.value();
        values.insert(duplicate, false);
        QVERIFY(!document.SetVisibilityValues(values));
        QCOMPARE(rawVisibility(document), before);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void typedNamesAndSessionRoundTripRemainIndependent()
    {
        CReader reader(sample());
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QByteArray bytes = reader.m_byte;
        const QByteArray id("SharedVisibility");
        for (int offset : {table.sectionOffsetTable->parts->ids, table.sectionOffsetTable->art_meshes->ids})
            bytes.replace(offset, 64, id.leftJustified(64, '\0'));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("visibility.moc3");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        const auto before = document.VisibilityState();
        QVERIFY(document.SetVisibilityValues({{"part|SharedVisibility", false}, {"artmesh|SharedVisibility", true}}));
        QCOMPARE(document.VisibilityState().value("part|SharedVisibility"), false);
        QCOMPARE(document.VisibilityState().value("artmesh|SharedVisibility"), true);
        for (auto it = before.cbegin(); it != before.cend(); ++it)
            if (it.key() != "part|SharedVisibility" && it.key() != "artmesh|SharedVisibility")
                QCOMPARE(document.VisibilityState().value(it.key()), it.value());
        CModelSession session;
        session.sourceMoc = path;
        session.sourceDigest = document.SourceDigest();
        session.visibility = document.VisibilityState();
        const auto sessionPath = directory.filePath("visibility.session.json");
        QVERIFY2(session.Save(sessionPath, &error), qPrintable(error));
        CModelSession loaded;
        QVERIFY2(CModelSession::Load(sessionPath, &loaded, &error), qPrintable(error));
        CModelDocument fresh, restored;
        QVERIFY2(fresh.Load(loaded.sourceMoc, &error), qPrintable(error));
        QVERIFY2(loaded.RestoreDocument(fresh, restored, &error), qPrintable(error));
        QCOMPARE(restored.VisibilityState(), document.VisibilityState());
        QCOMPARE(restored.SourceBytes(), bytes);
        QCOMPARE(restored.SourceDigest(), document.SourceDigest());
        QVERIFY(!restored.CanUndoEdit() && !restored.CanRedoEdit());
        QByteArray preview;
        QVERIFY(restored.BuildPreviewMoc(preview));
        QCOMPARE(preview, bytes);
    }
};

QTEST_APPLESS_MAIN(VisibilityEditTests)
#include "visibility_edit_tests.moc"
