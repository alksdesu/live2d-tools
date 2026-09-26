#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmodelsession.h"
#include "deformer_snapshot_fixture.h"

#include <QtTest>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace live2dstruct;

namespace {
QString sample(const QString& name)
{
    return QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
}

quint32 parameterWord(const QByteArray& bytes, int slot, int row)
{
    const int offset = qFromLittleEndian<qint32>(bytes.constData() + 0x40 + slot * 4);
    return offset ? qFromLittleEndian<quint32>(bytes.constData() + offset + row * 4) : 0;
}

float parameterFloat(const QByteArray& bytes, int slot, int row)
{
    const quint32 word = parameterWord(bytes, slot, row);
    float result;
    std::memcpy(&result, &word, sizeof(result));
    return result;
}

QByteArray parameterId(const QByteArray& bytes, int row)
{
    const int offset = qFromLittleEndian<qint32>(bytes.constData() + 0x40 + 50 * 4);
    return bytes.mid(offset + row * 64, 64);
}

CModelSession sessionFor(const CModelDocument& document)
{
    CModelSession session;
    session.sourceMoc = document.SourcePath();
    session.sourceDigest = document.SourceDigest();
    for (auto value = document.ParameterValues().cbegin(); value != document.ParameterValues().cend(); ++value)
        session.parameters.insert(value.key(), value.value());
    session.parameterMetadata = document.ParameterMetadataOverrides();
    session.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    return session;
}

bool sameEdit(const CModelEdit& left, const CModelEdit& right)
{
    return left.name == right.name && left.type == right.type && left.field == right.field &&
        left.before == right.before && left.after == right.after && left.index == right.index && left.coalesce == right.coalesce;
}

struct Snapshot {
    QByteArray data, source, digest;
    quint64 revision;
    bool undo, redo;
    CModelEdit nextUndo, nextRedo;
};

Snapshot snapshot(const CModelDocument& document)
{
    Snapshot result{sessionFor(document).ToJson(), document.SourceBytes(), document.SourceDigest(),
                    document.Revision(), false, false, {}, {}};
    result.undo = document.PeekUndoEdit(&result.nextUndo);
    result.redo = document.PeekRedoEdit(&result.nextRedo);
    return result;
}

bool matches(const CModelDocument& document, const Snapshot& before)
{
    const auto after = snapshot(document);
    return before.data == after.data && before.source == after.source && before.digest == after.digest &&
        before.revision == after.revision && before.undo == after.undo && before.redo == after.redo &&
        (!before.undo || sameEdit(before.nextUndo, after.nextUndo)) && (!before.redo || sameEdit(before.nextRedo, after.nextRedo));
}

struct Publication {
    int attempts = 0, accepted = 0, ordinary = 0, ended = 0;
    int fault = 0;
    bool keep = false;
    QByteArray bytes;
    QMap<QString, float> values;
    std::function<void()> during;

    void connect(CKeyformEditController& controller)
    {
        controller.SetPublisher([this](const CModelDocument& candidate, const auto&, bool, QString* error) {
            ++ordinary;
            QByteArray preview;
            return candidate.BuildPreviewMoc(preview, error);
        }, [this](QString*) { ++ended; return true; });
        controller.SetDefinitionPublisher([this](const CModelDocument& candidate, const auto& base, bool preserve, QString* error) {
            ++attempts;
            QByteArray preview;
            if (!candidate.BuildPreviewMoc(preview, error)) return false;
            if (during) during();
            if (fault == 1) { if (error) *error = QStringLiteral("controlled publication rejection"); return false; }
            if (fault == 2) throw std::runtime_error("controlled publication exception");
            bytes = std::move(preview);
            values = base;
            keep = preserve;
            ++accepted;
            return true;
        });
    }
};
}

class ParameterDefinitionEditTests : public QObject
{
    Q_OBJECT
private slots:
    void compoundCommandRestoresImplicitAndExplicitOverrides_data()
    {
        QTest::addColumn<bool>("explicitValue");
        QTest::addColumn<bool>("metadataOverride");
        for (bool value : {false, true}) for (bool metadata : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("value-%1-metadata-%2").arg(value).arg(metadata))) << value << metadata;
    }

    void compoundCommandRestoresImplicitAndExplicitOverrides()
    {
        QFETCH(bool, explicitValue);
        QFETCH(bool, metadataOverride);
        DeformerSnapshotData::Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "compound.moc3"));
        if (metadataOverride) {
            auto metadata = document.ParameterMetadata("P");
            metadata.displayName = QStringLiteral("previous metadata");
            QVERIFY(document.SetParameterMetadata("P", metadata));
        }
        if (explicitValue) QVERIFY(document.SetParameterValue("P", .75f));
        const auto before = document.ParameterMetadataState("P");
        QCOMPARE(before.value("hasValueOverride").toBool(), explicitValue);
        QCOMPARE(before.value("hasMetadataOverride").toBool(), metadataOverride);
        const auto revision = document.Revision();
        auto metadata = document.ParameterMetadata("P");
        metadata.maximum = .5f;
        metadata.defaultValue = .25f;
        metadata.decimalPlaces = 7;
        metadata.repeat = true;
        CModelEdit command;
        QString error;
        QVERIFY2(document.MakeParameterMetadataEdit("P", metadata, command, &error), qPrintable(error));
        QCOMPARE(document.ParameterMetadataState("P"), before);
        QCOMPARE(command.before.toMap(), before);
        QCOMPARE(command.after.toMap().value("value").toFloat(), explicitValue ? .5f : 0.0f);
        QVERIFY(command.after.toMap().value("hasValueOverride").toBool());
        QVERIFY(document.ExecuteEdit(command));
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(document.ParameterValue("P"), explicitValue ? .5f : 0.0f);
        QVERIFY(document.ParameterMetadata("P") == metadata);
        const auto after = document.ParameterMetadataState("P");
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterMetadataState("P"), before);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.CanRedoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.ParameterMetadataState("P"), after);
        QCOMPARE(document.SourceBytes(), fixture.bytes);
    }

    void defaultOnlyChangePreservesBaselineAndNoOpDoesNotTruncateRedo()
    {
        DeformerSnapshotData::Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "default.moc3"));
        const auto initial = document.ParameterMetadataState("P");
        auto metadata = document.ParameterMetadata("P");
        metadata.defaultValue = .5f;
        CModelEdit edit;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, edit));
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(document.ParameterValue("P"), 0.0f);
        QVERIFY(document.ParameterValues().contains("P"));
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterMetadataState("P"), initial);
        QVERIFY(!document.ParameterValues().contains("P"));
        const auto before = snapshot(document);
        QVERIFY(document.MakeParameterMetadataEdit("P", document.ParameterMetadata("P"), edit));
        QCOMPARE(edit.before, edit.after);
        QVERIFY(!document.ExecuteEdit(edit));
        QVERIFY(matches(document, before));
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.ParameterValue("P"), 0.0f);
        QCOMPARE(document.ParameterMetadata("P").defaultValue, .5f);
    }

    void nameOnlyEditAndHistoryPreserveNegativeZeroBaselineAndPreview()
    {
        DeformerSnapshotData::Fixture fixture;
        fixture.BindRoot(false);
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "negative-zero.moc3"));
        QCOMPARE(document.ParameterMetadata("P").minimum, 0.0f);
        QVERIFY(!std::signbit(document.ParameterMetadata("P").minimum));
        QVERIFY(document.SetParameterValue("P", -0.0f));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Publication publication;
        publication.connect(controller);
        QVERIFY(controller.Activate("Mesh", 0, {{"P", -0.0f}}));
        QVERIFY(std::signbit(controller.Pose().value("P")));
        auto metadata = document.ParameterMetadata("P");
        metadata.displayName = QStringLiteral("negative zero");
        CModelEdit edit;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, edit));
        QVERIFY(std::signbit(edit.before.toMap().value("value").toFloat()));
        QVERIFY(std::signbit(edit.after.toMap().value("value").toFloat()));
        QVERIFY(controller.Submit(edit));
        QVERIFY(std::signbit(document.ParameterValue("P")));
        QVERIFY(std::signbit(controller.Pose().value("P")));
        CModelEdit history;
        QVERIFY(controller.History(true, history));
        QVERIFY(std::signbit(document.ParameterValue("P")));
        QVERIFY(std::signbit(controller.Pose().value("P")));
        QVERIFY(controller.History(false, history));
        QVERIFY(std::signbit(document.ParameterValue("P")));
        QVERIFY(std::signbit(controller.Pose().value("P")));
        QCOMPARE(publication.bytes, fixture.bytes);
        QCOMPARE(document.SourceBytes(), fixture.bytes);
    }

    void staleCommandsAndInvalidPayloadsAreAtomic_data()
    {
        QTest::addColumn<QString>("fault");
        for (const QString& name : {QStringLiteral("stale"), QStringLiteral("missing-value"), QStringLiteral("missing-flag"),
                QStringLiteral("non-bool-flag"), QStringLiteral("nonfinite"), QStringLiteral("underflow"),
                QStringLiteral("outside-range"), QStringLiteral("bad-range"), QStringLiteral("bad-default"),
                QStringLiteral("bad-decimals"), QStringLiteral("metadata-flag"), QStringLiteral("value-flag"),
                QStringLiteral("wrong-type"), QStringLiteral("unknown-id")})
            QTest::newRow(qPrintable(name)) << name;
    }

    void staleCommandsAndInvalidPayloadsAreAtomic()
    {
        QFETCH(QString, fault);
        DeformerSnapshotData::Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "invalid.moc3"));
        auto metadata = document.ParameterMetadata("P");
        metadata.displayName = "first";
        CModelEdit first;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, first));
        QVERIFY(document.ExecuteEdit(first));
        metadata.displayName = "second";
        CModelEdit second;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, second));
        QVERIFY(document.ExecuteEdit(second));
        QVERIFY(document.UndoEdit());
        metadata.maximum = 2;
        metadata.defaultValue = .25f;
        CModelEdit command;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, command));
        auto after = command.after.toMap();
        if (fault == "stale") QVERIFY(document.SetParameterValue("P", .125f));
        if (fault == "missing-value") after.remove("value");
        if (fault == "missing-flag") after.remove("hasMetadataOverride");
        if (fault == "non-bool-flag") after["hasValueOverride"] = "true";
        if (fault == "nonfinite") after["value"] = std::numeric_limits<float>::quiet_NaN();
        if (fault == "underflow") after["value"] = 1e-100;
        if (fault == "outside-range") after["value"] = 3.0f;
        if (fault == "bad-range") after["minimum"] = 3.0f;
        if (fault == "bad-default") after["defaultValue"] = 3.0f;
        if (fault == "bad-decimals") after["decimalPlaces"] = 10;
        if (fault == "metadata-flag") after["hasMetadataOverride"] = false;
        if (fault == "value-flag") after["hasValueOverride"] = false;
        if (fault == "wrong-type") command.type = "artmesh";
        if (fault == "unknown-id") command.name = "missing";
        command.after = after;
        const auto before = snapshot(document);
        QVERIFY(!document.ExecuteEdit(command));
        QVERIFY(matches(document, before));
    }

    void invalidMakePreservesItsOutputAndTheDocument()
    {
        DeformerSnapshotData::Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "make.moc3"));
        const auto before = snapshot(document);
        CModelEdit output{"sentinel", "sentinel", CModelEdit::Opacity, 1.0f, .5f, 9, true};
        const auto sentinel = output;
        auto metadata = document.ParameterMetadata("P");
        metadata.maximum = std::numeric_limits<float>::infinity();
        QString error;
        QVERIFY(!document.MakeParameterMetadataEdit("P", metadata, output, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(sameEdit(output, sentinel));
        QVERIFY(!document.MakeParameterMetadataEdit("missing", document.ParameterMetadata("P"), output, &error));
        QVERIFY(sameEdit(output, sentinel));
        QVERIFY(matches(document, before));
    }

    void originalModelsPublishIndependentParameterByteColumns_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }

    void originalModelsPublishIndependentParameterByteColumns()
    {
        QFETCH(QString, model);
        CModelDocument document;
        QVERIFY(document.Load(sample(model)));
        const auto original = document.SourceBytes();
        const auto digest = document.SourceDigest();
        const QString id = document.Parameters().front().id;
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Publication publication;
        publication.connect(controller);
        auto metadata = document.ParameterMetadata(id);
        metadata.displayName = QStringLiteral("编辑参数");
        metadata.minimum = parameterFloat(original, 52, 0) - 3;
        metadata.maximum = parameterFloat(original, 51, 0) + 7;
        metadata.defaultValue = .25f;
        metadata.repeat = !bool(parameterWord(original, 54, 0));
        metadata.decimalPlaces = 6;
        CModelEdit edit;
        QVERIFY(document.MakeParameterMetadataEdit(id, metadata, edit));
        const auto before = snapshot(document);
        bool unchangedDuringPublication = false;
        publication.during = [&] { unchangedDuringPublication = matches(document, before); };
        QString error;
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QVERIFY(unchangedDuringPublication);
        QCOMPARE(publication.attempts, 1);
        QCOMPARE(publication.accepted, 1);
        QCOMPARE(publication.ordinary, 0);
        QCOMPARE(publication.ended, 0);
        QVERIFY(!publication.keep);
        QVERIFY(publication.values.isEmpty());
        QCOMPARE(parameterFloat(publication.bytes, 51, 0), metadata.maximum);
        QCOMPARE(parameterFloat(publication.bytes, 52, 0), metadata.minimum);
        QCOMPARE(parameterFloat(publication.bytes, 53, 0), .25f);
        QCOMPARE(parameterWord(publication.bytes, 54, 0), metadata.repeat ? quint32(1) : quint32(0));
        QCOMPARE(parameterWord(publication.bytes, 55, 0), quint32(6));
        for (int row = 0; row < document.Parameters().size(); ++row) {
            QCOMPARE(parameterId(publication.bytes, row), parameterId(original, row));
            if (row) for (int slot : {51, 52, 53, 54, 55})
                QCOMPARE(parameterWord(publication.bytes, slot, row), parameterWord(original, slot, row));
        }
        QCOMPARE(document.SourceBytes(), original);
        QCOMPARE(document.SourceDigest(), digest);
    }

    void activeCornerPublicationPreservesOrExitsOnlyOnSuccess_data()
    {
        QTest::addColumn<bool>("reachable");
        QTest::addColumn<int>("fault");
        for (bool reachable : {false, true}) for (int fault : {0, 1, 2})
            QTest::newRow(qPrintable(QStringLiteral("reachable-%1-fault-%2").arg(reachable).arg(fault))) << reachable << fault;
    }

    void activeCornerPublicationPreservesOrExitsOnlyOnSuccess()
    {
        QFETCH(bool, reachable);
        QFETCH(int, fault);
        DeformerSnapshotData::Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "corner.moc3"));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Publication publication;
        publication.connect(controller);
        QString error;
        QVERIFY2(controller.Activate("Mesh", 1, {{"P", .3f}}, &error), qPrintable(error));
        QCOMPARE(controller.Pose().value("P"), 1.0f);
        const auto originalPose = controller.Pose();
        const auto before = snapshot(document);
        auto metadata = document.ParameterMetadata("P");
        metadata.maximum = reachable ? 2 : .5f;
        CModelEdit edit;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, edit));
        publication.fault = fault;
        const bool accepted = controller.Submit(edit, &error);
        QCOMPARE(accepted, fault == 0);
        QCOMPARE(publication.attempts, 1);
        QCOMPARE(publication.ordinary, 1);
        QCOMPARE(publication.ended, 0);
        if (fault) {
            QVERIFY(matches(document, before));
            QVERIFY(controller.IsActive());
            QCOMPARE(controller.Pose(), originalPose);
            QVERIFY(!error.isEmpty());
            publication.fault = 0;
            QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        }
        QCOMPARE(controller.IsActive(), reachable);
        QCOMPARE(publication.keep, reachable);
        if (reachable) {
            QCOMPARE(controller.Target(), QStringLiteral("Mesh"));
            QCOMPARE(controller.Keyform(), 1);
            QCOMPARE(controller.Pose(), originalPose);
        } else {
            QVERIFY(controller.Target().isEmpty());
            QVERIFY(controller.Pose().isEmpty());
        }
        QCOMPARE(document.SourceBytes(), fixture.bytes);
    }

    void publicationCannotReenterOrReplaceItsControllerBindings()
    {
        DeformerSnapshotData::Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "reentry.moc3"));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Publication publication;
        publication.connect(controller);
        QVERIFY(controller.Activate("Mesh", 1, {{"P", 0}}));
        auto metadata = document.ParameterMetadata("P");
        metadata.maximum = 2;
        CModelEdit edit;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, edit));
        bool submit = true, history = true, end = true, restore = true;
        const auto before = snapshot(document);
        publication.during = [&] {
            CModelEdit ignored;
            submit = controller.Submit(edit);
            history = controller.History(true, ignored);
            end = controller.End();
            restore = controller.Restore(document);
            controller.SetDocument(nullptr);
            controller.SetDefinitionPublisher({});
        };
        QVERIFY(controller.Submit(edit));
        QVERIFY(!submit && !history && !end && !restore);
        QCOMPARE(publication.attempts, 1);
        QCOMPARE(document.Revision(), before.revision + 1);
        QVERIFY(controller.IsActive());
        publication.during = {};
        CModelEdit undone;
        QVERIFY(controller.History(true, undone));
        QCOMPARE(publication.attempts, 2);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(controller.IsActive());
    }

    void metadataHistoryFailurePreservesCursorValuesAndPose_data()
    {
        QTest::addColumn<bool>("undo");
        QTest::addColumn<int>("fault");
        for (bool undo : {false, true}) for (int fault : {1, 2})
            QTest::newRow(qPrintable(QStringLiteral("undo-%1-fault-%2").arg(undo).arg(fault))) << undo << fault;
    }

    void metadataHistoryFailurePreservesCursorValuesAndPose()
    {
        QFETCH(bool, undo);
        QFETCH(int, fault);
        DeformerSnapshotData::Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "history.moc3"));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Publication publication;
        publication.connect(controller);
        QVERIFY(controller.Activate("Mesh", 0, {{"P", 0}}));
        auto metadata = document.ParameterMetadata("P");
        metadata.maximum = 2;
        CModelEdit edit;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, edit));
        QVERIFY(controller.Submit(edit));
        CModelEdit ignored;
        if (!undo) QVERIFY(controller.History(true, ignored));
        const auto before = snapshot(document);
        const auto pose = controller.Pose();
        publication.fault = fault;
        CModelEdit output{"sentinel", "part", CModelEdit::Opacity, 1, 0};
        const auto sentinel = output;
        QString error;
        QVERIFY(!controller.History(undo, output, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(sameEdit(output, sentinel));
        QVERIFY(matches(document, before));
        QCOMPARE(controller.Pose(), pose);
        QVERIFY(controller.IsActive());
        publication.fault = 0;
        QVERIFY(controller.History(undo, output, &error));
        QCOMPARE(document.ParameterMetadata("P").maximum, undo ? 1.0f : 2.0f);
    }

    void sessionFileRestorePublishesDefinitionsAtomically_data()
    {
        QTest::addColumn<int>("fault");
        for (int fault : {0, 1, 2}) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }

    void sessionFileRestorePublishesDefinitionsAtomically()
    {
        QFETCH(int, fault);
        DeformerSnapshotData::Fixture fixture;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString modelPath = directory.filePath("session.moc3");
        QVERIFY(DeformerSnapshotData::write(modelPath, fixture.bytes));
        CModelDocument edited;
        QVERIFY(edited.Load(modelPath));
        auto metadata = edited.ParameterMetadata("P");
        metadata.maximum = 2;
        metadata.defaultValue = .5f;
        metadata.displayName = QStringLiteral("session parameter");
        CModelEdit command;
        QVERIFY(edited.MakeParameterMetadataEdit("P", metadata, command));
        QVERIFY(edited.ExecuteEdit(command));
        QVERIFY(edited.SetParameterValue("P", 1.5f));
        const QString sessionPath = directory.filePath("state.json");
        QVERIFY(sessionFor(edited).Save(sessionPath));
        CModelSession loaded;
        QString error;
        QVERIFY2(CModelSession::Load(sessionPath, &loaded, &error), qPrintable(error));
        CModelDocument document, candidate;
        QVERIFY(document.Load(modelPath));
        QVERIFY2(loaded.RestoreDocument(document, candidate, &error), qPrintable(error));
        QCOMPARE(candidate.ParameterMetadataState("P"), edited.ParameterMetadataState("P"));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        Publication publication;
        publication.connect(controller);
        QVERIFY(controller.Activate("Mesh", 1, {{"P", 0}}));
        const auto before = snapshot(document);
        const auto pose = controller.Pose();
        publication.fault = fault;
        const bool accepted = controller.Restore(candidate, &error);
        QCOMPARE(accepted, fault == 0);
        if (fault) {
            QVERIFY(matches(document, before));
            QVERIFY(controller.IsActive());
            QCOMPARE(controller.Pose(), pose);
            publication.fault = 0;
            QVERIFY2(controller.Restore(candidate, &error), qPrintable(error));
        }
        QVERIFY(!controller.IsActive());
        QVERIFY(!publication.keep);
        QCOMPARE(publication.values, (QMap<QString, float>{{"P", 1.5f}}));
        QCOMPARE(parameterFloat(publication.bytes, 51, 0), 2.0f);
        QCOMPARE(parameterFloat(publication.bytes, 53, 0), .5f);
        QCOMPARE(document.ParameterMetadataState("P"), edited.ParameterMetadataState("P"));
        QCOMPARE(document.SourceBytes(), fixture.bytes);
        QCOMPARE(document.SourceDigest(), candidate.SourceDigest());
    }

    void expandedRangeMappingUsesThePatchedDefinition()
    {
        DeformerSnapshotData::Fixture fixture;
        fixture.BindRoot(false);
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "mapping.moc3"));
        CKeyformMappingContext before;
        QString error;
        QVERIFY(!before.Build(document, "Mesh", 0, {{"P", 2}}, &error));
        auto metadata = document.ParameterMetadata("P");
        metadata.maximum = 2;
        CModelEdit command;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, command));
        QVERIFY(document.ExecuteEdit(command));
        CKeyformMappingContext mapping;
        QVERIFY2(mapping.Build(document, "Mesh", 0, {{"P", 2}}, &error), qPrintable(error));
        QVector<float> world, restored;
        const auto local = document.DrawableKeyformPositions("Mesh", 0);
        QVERIFY(mapping.ToModel(local, world));
        QVERIFY(DeformerSnapshotData::near(world, DeformerSnapshotData::coreCoordinates(DeformerSnapshotData::rigidMeshCanvasPositions())));
        QVERIFY(mapping.FromModel(world, restored));
        QVERIFY(DeformerSnapshotData::near(restored, local));
        QByteArray bytes;
        QVERIFY(document.BuildPreviewMoc(bytes));
        QCOMPARE(parameterFloat(bytes, 51, 0), 2.0f);
        QVERIFY(document.UndoEdit());
        CKeyformMappingContext undone;
        QVERIFY(!undone.Build(document, "Mesh", 0, {{"P", 2}}, &error));
        QVERIFY(mapping.MatchesRuntime(world));
    }

    void diagnosticsUseEffectiveRangeAndKeepUnreachableSourceKeys()
    {
        DeformerSnapshotData::Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "diagnostics.moc3"));
        QCOMPARE(document.Parameters().front().keyValues, (QVector<float>{0, 1}));
        const auto has = [&document](const QString& code, CModelDiagnostic::Severity severity) {
            const auto diagnostics = document.Validate();
            return std::any_of(diagnostics.cbegin(), diagnostics.cend(), [&](const auto& item) {
                return item.code == code && item.severity == severity;
            });
        };
        auto metadata = document.ParameterMetadata("P");
        metadata.maximum = 2;
        CModelEdit edit;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, edit));
        QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.SetParameterValue("P", 1.5f));
        QVERIFY(!has("parameter_override_range", CModelDiagnostic::Error));
        QVERIFY(!has("parameter_keys", CModelDiagnostic::Warning));
        metadata.maximum = .5f;
        QVERIFY(document.MakeParameterMetadataEdit("P", metadata, edit));
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(document.ParameterValue("P"), .5f);
        QCOMPARE(document.Parameters().front().keyValues, (QVector<float>{0, 1}));
        QVERIFY(has("parameter_keys", CModelDiagnostic::Warning));
        QVERIFY(!has("parameter_override_range", CModelDiagnostic::Error));
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterValue("P"), 1.5f);
        QVERIFY(!has("parameter_keys", CModelDiagnostic::Warning));
        QVERIFY(!has("parameter_override_range", CModelDiagnostic::Error));
        QCOMPARE(document.SourceBytes(), fixture.bytes);
    }
};

QTEST_GUILESS_MAIN(ParameterDefinitionEditTests)
#include "parameter_definition_edit_tests.moc"
