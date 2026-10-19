#include "geometry_batch_fixture.h"
#include <QtTest>
#include <limits>

using namespace GeometryBatchData;

namespace {
State fullFrames(const CModelDocument& document)
{
    auto frames = all(document);
    for (auto& frame : frames) {
        if (frame.type == Frame::Warp) frame.opacity = document.DeformerWarpKeyforms(frame.id)[frame.keyform].opacity;
        else if (frame.type == Frame::Rotation) frame.opacity = document.DeformerRotationKeyforms(frame.id)[frame.keyform].opacity;
    }
    return frames;
}

template <typename T>
QVector<T> deepCopy(const QVector<T>& source)
{
    QVector<T> result;
    result.reserve(source.size());
    for (const auto& value : source) result.append(value);
    return result;
}

QVector<CModelWarpKeyform> deepWarpCopy(const QVector<CModelWarpKeyform>& source)
{
    QVector<CModelWarpKeyform> result;
    result.reserve(source.size());
    for (const auto& form : source) {
        CModelWarpKeyform copy = form;
        copy.positions = deepCopy(form.positions);
        result.append(std::move(copy));
    }
    return result;
}

QVector<CModelRotationKeyform> deepRotationCopy(const QVector<CModelRotationKeyform>& source)
{
    QVector<CModelRotationKeyform> result;
    result.reserve(source.size());
    for (const auto& form : source) result.append(form);
    return result;
}

struct Observation {
    QVariantMap structure;
    quint64 revision = 0;
    QByteArray moc;
    QByteArray session;
};

bool observe(const CModelDocument& document, Observation& result, QString* error = nullptr)
{
    result.structure = document.AuthoringStructureState();
    result.revision = document.Revision();
    if (!document.BuildRuntimeMoc(result.moc, error)) return false;
    result.session = GeometryBatchData::session(document).ToJson();
    return true;
}

void compareObservation(const CModelDocument& document, const Observation& expected)
{
    QCOMPARE(document.AuthoringStructureState(), expected.structure);
    QCOMPARE(document.Revision(), expected.revision);
    QByteArray moc;
    QString error;
    QVERIFY2(document.BuildRuntimeMoc(moc, &error), qPrintable(error));
    QCOMPARE(moc, expected.moc);
    QCOMPARE(GeometryBatchData::session(document).ToJson(), expected.session);
}
}

class GeometryBatchDocumentTests : public QObject
{
    Q_OBJECT
private slots:
    void preparedCommandOwnsInputBuffers_data()
    {
        QTest::addColumn<int>("field");
        QTest::newRow("positions") << 0;
        QTest::newRow("frame-id") << 1;
        QTest::newRow("driver-id") << 2;
    }
    void preparedCommandOwnsInputBuffers()
    {
        QFETCH(int, field);
        CModelDocument document; QVERIFY(load(document));
        auto before = all(document), after = changed(before);
        auto& points = std::get<QVector<float>>(after[2].value);
        float* rawPoints = points.data();
        const float expectedPoint = points[0];
        QChar owner[]{'W', 'a', 'r', 'p'};
        QChar driverOwner[]{'M', 'e', 's', 'h'};
        after[2].id = QString::fromRawData(owner, 4);
        Frame driver = before[0]; driver.id = QString::fromRawData(driverOwner, 4);
        CModelEdit edit; QString error;
        QVERIFY2(document.MakeGeometryEdit(before, after, driver, edit, &error), qPrintable(error));
        if (field == 0) rawPoints[0] += 100;
        else if (field == 1) owner[0] = 'X';
        else driverOwner[0] = 'X';
        const auto owned = edit.after.value<State>();
        QCOMPARE(std::get<QVector<float>>(owned[2].value)[0], expectedPoint);
        QCOMPARE(owned[2].id, QString("Warp"));
        QCOMPARE(edit.name, QString("Mesh"));
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(document.DeformerWarpKeyforms("Warp")[0].positions[0], expectedPoint);
        QVERIFY(document.UndoEdit());
        QVERIFY(document.DeformerWarpKeyformOverrides().isEmpty());
    }

    void executedRawCommandOwnsInputBuffers_data() { preparedCommandOwnsInputBuffers_data(); }
    void executedRawCommandOwnsInputBuffers()
    {
        QFETCH(int, field);
        CModelDocument document; QVERIFY(load(document));
        const auto original = document.AuthoringStructureState();
        auto before = all(document), after = changed(before);
        auto& points = std::get<QVector<float>>(after[2].value);
        float* rawPoints = points.data();
        const float expectedPoint = points[0];
        QChar owner[]{'W', 'a', 'r', 'p'};
        QChar driverOwner[]{'M', 'e', 's', 'h'};
        after[2].id = QString::fromRawData(owner, 4);
        CModelEdit edit{QString::fromRawData(driverOwner, 4), "artmesh", CModelEdit::GeometryKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(after), 0, false};
        QVERIFY(document.ExecuteEdit(edit));
        if (field == 0) rawPoints[0] += 100;
        else if (field == 1) owner[0] = 'X';
        else driverOwner[0] = 'X';
        QCOMPARE(document.DeformerWarpKeyforms("Warp")[0].positions[0], expectedPoint);
        CModelEdit archived; QVERIFY(document.PeekUndoEdit(&archived));
        const auto owned = archived.after.value<State>();
        QCOMPARE(std::get<QVector<float>>(owned[2].value)[0], expectedPoint);
        QCOMPARE(owned[2].id, QString("Warp"));
        QCOMPARE(archived.name, QString("Mesh"));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), original);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DeformerWarpKeyforms("Warp")[0].positions[0], expectedPoint);
    }
    void fullFramesCommitUndoRedoAndPersist()
    {
        CModelDocument document; QString error;
        QVERIFY2(load(document, &error), qPrintable(error));
        const auto source = document;
        const auto baseline = document.AuthoringStructureState();
        const auto before = fullFrames(document);
        auto after = changed(before);
        for (int index = 2; index < after.size(); ++index) after[index].opacity = 0.125f * (index - 2);
        CModelEdit edit;
        QVERIFY2(document.MakeGeometryEdit(before, after, before[0], edit, &error), qPrintable(error));
        QCOMPARE(document.AuthoringStructureState(), baseline);
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(fullFrames(document), after);
        QCOMPARE(historyCount(document), 1);
        for (const auto& frame : all(document)) QVERIFY(!frame.opacity.has_value());
        CModelEdit archived;
        QVERIFY(document.UndoEdit(&archived));
        QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(fullFrames(document), before);
        QVERIFY(document.DeformerWarpKeyformOverrides().isEmpty());
        QVERIFY(document.DeformerRotationKeyformOverrides().isEmpty());
        QVERIFY(document.DrawableKeyformPositionOverrides().isEmpty());
        QVERIFY(document.RedoEdit());
        QCOMPARE(fullFrames(document), after);
        QCOMPARE(historyCount(document), 1);
        QByteArray bytes;
        QVERIFY2(document.BuildRuntimeMoc(bytes, &error), qPrintable(error));
        CModelDocument compiled;
        QVERIFY2(compiled.LoadBytes(bytes, "full-frames", &error), qPrintable(error));
        QCOMPARE(fullFrames(compiled), after);
        CModelSession stored;
        QVERIFY2(CModelSession::FromJson(session(document).ToJson(), &stored, &error), qPrintable(error));
        CModelDocument restored;
        QVERIFY2(stored.RestoreDocument(source, restored, &error), qPrintable(error));
        QCOMPARE(fullFrames(restored), after);
        QVERIFY(!restored.CanUndoEdit());
        QVERIFY2(restored.BuildRuntimeMoc(bytes, &error), qPrintable(error));
        QCOMPARE(compiled.Drawables()[0].uvs, source.Drawables()[0].uvs);
        QCOMPARE(compiled.DrawableIndices("Mesh"), source.DrawableIndices("Mesh"));
    }

    void opacityOnlyBatchAndLegacyGeometryPreserveTheirScopes()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto original = fullFrames(document);
        auto after = original;
        for (int index = 2; index < after.size(); ++index) after[index].opacity = index % 2 ? 1.0f : 0.0f;
        CModelEdit edit;
        QVERIFY(document.MakeGeometryEdit(original, after, original[2], edit));
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(fullFrames(document), after);
        for (int index = 0; index < original.size(); ++index) QCOMPARE(all(document)[index].value, original[index].value);
        const auto geometryBefore = all(document), geometryAfter = changed(geometryBefore);
        QVERIFY(document.MakeGeometryEdit(geometryBefore, geometryAfter, geometryBefore[2], edit));
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(all(document), geometryAfter);
        for (int index = 2; index < after.size(); ++index)
            QCOMPARE(fullFrames(document)[index].opacity, after[index].opacity);
        QCOMPARE(historyCount(document), 2);
        QVERIFY(document.UndoEdit()); QCOMPARE(fullFrames(document), after);
        QVERIFY(document.UndoEdit()); QCOMPARE(fullFrames(document), original);
        QVERIFY(document.RedoEdit()); QVERIFY(document.RedoEdit());
        QCOMPARE(all(document), geometryAfter);
        for (int index = 2; index < after.size(); ++index)
            QCOMPARE(fullFrames(document)[index].opacity, after[index].opacity);
    }

    void invalidFullFrameOpacityIsAtomic_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<int>("failure");
        for (int type : {int(Frame::Warp), int(Frame::Rotation)})
            for (int failure = 0; failure < 11; ++failure)
                QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(failure))) << type << failure;
    }
    void invalidFullFrameOpacityIsAtomic()
    {
        QFETCH(int, type); QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document));
        QVERIFY(document.ExecuteEdit(command(document))); QVERIFY(document.UndoEdit());
        auto before = fullFrames(document), after = changed(before);
        const int target = type == Frame::Warp ? 2 : 4;
        const float invalid[] = {std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(), -0.125f, 1.125f};
        if (failure < 4) after[target].opacity = invalid[failure];
        if (failure >= 4 && failure < 8) before[target].opacity = invalid[failure - 4];
        if (failure == 8) before[target].opacity.reset();
        if (failure == 9) after[target].opacity.reset();
        if (failure == 10) before[target].opacity = *before[target].opacity == 0.5f ? 0.25f : 0.5f;
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision();
        CModelEdit edit{"sentinel", "type", CModelEdit::Opacity, 1.0f, 0.5f}; QString error;
        QVERIFY(!document.MakeGeometryEdit(before, after, before[0], edit, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(edit.name, QString("sentinel"));
        CModelEdit raw{before[0].id, before[0].TypeName(), CModelEdit::GeometryKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(after), before[0].keyform, false};
        QVERIFY(!document.ExecuteEdit(raw));
        QCOMPARE(document.AuthoringStructureState(), state); QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
    }

    void artMeshOptionalOpacityIsRejected_data()
    {
        QTest::addColumn<int>("side");
        for (int side = 0; side < 3; ++side) QTest::newRow(qPrintable(QString::number(side))) << side;
    }
    void artMeshOptionalOpacityIsRejected()
    {
        QFETCH(int, side);
        CModelDocument document; QVERIFY(load(document));
        auto before = all(document), after = changed(before);
        if (side != 1) before[0].opacity = 0.5f;
        if (side != 0) after[0].opacity = 0.75f;
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision();
        CModelEdit edit;
        QVERIFY(!document.MakeGeometryEdit(before, after, before[0], edit));
        CModelEdit raw{before[0].id, before[0].TypeName(), CModelEdit::GeometryKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(after), before[0].keyform, false};
        QVERIFY(!document.ExecuteEdit(raw));
        QCOMPARE(document.AuthoringStructureState(), state); QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void fullFrameRejectsOwnerOpacityChangedAfterPreparation_data()
    {
        QTest::addColumn<bool>("rotation");
        QTest::newRow("warp") << false; QTest::newRow("rotation") << true;
    }
    void fullFrameRejectsOwnerOpacityChangedAfterPreparation()
    {
        QFETCH(bool, rotation);
        CModelDocument document; QVERIFY(load(document));
        const auto before = fullFrames(document); auto after = changed(before);
        for (int index = 2; index < after.size(); ++index) after[index].opacity = 0.125f;
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, before[0], edit));
        if (rotation) {
            auto forms = document.DeformerRotationKeyforms("Root"); forms[1].opacity = 0.375f;
            QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        } else {
            auto forms = document.DeformerWarpKeyforms("Warp"); forms[1].opacity = 0.375f;
            QVERIFY(document.SetDeformerWarpKeyforms("Warp", forms));
        }
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(edit));
        QCOMPARE(document.AuthoringStructureState(), state); QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void fullFrameHistoryRejectsOpacityDrift_data()
    {
        QTest::addColumn<bool>("rotation"); QTest::addColumn<bool>("redo"); QTest::addColumn<int>("keyform");
        for (bool rotation : {false, true})
            for (bool redo : {false, true})
                for (int keyform = 0; keyform < 2; ++keyform)
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(rotation).arg(redo).arg(keyform))) << rotation << redo << keyform;
    }
    void fullFrameHistoryRejectsOpacityDrift()
    {
        QFETCH(bool, rotation); QFETCH(bool, redo); QFETCH(int, keyform);
        CModelDocument document; QVERIFY(load(document));
        const State before{fullFrames(document)[rotation ? 4 : 2]}; auto after = changed(before);
        after[0].opacity = 0.125f;
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, before[0], edit));
        QVERIFY(document.ExecuteEdit(edit)); if (redo) QVERIFY(document.UndoEdit());
        if (rotation) {
            auto forms = document.DeformerRotationKeyforms("Root"); forms[keyform].opacity = 0.375f;
            QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        } else {
            auto forms = document.DeformerWarpKeyforms("Warp"); forms[keyform].opacity = 0.375f;
            QVERIFY(document.SetDeformerWarpKeyforms("Warp", forms));
        }
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!(redo ? document.RedoEdit() : document.UndoEdit()));
        QCOMPARE(document.AuthoringStructureState(), state); QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.CanRedoEdit(), redo); QCOMPARE(document.CanUndoEdit(), !redo);
    }

    void fullFrameHistoryRejectsTamperedOpacityStorage_data()
    {
        QTest::addColumn<bool>("rotation"); QTest::addColumn<bool>("redo"); QTest::addColumn<int>("keyform");
        QTest::addColumn<bool>("invalid");
        for (bool rotation : {false, true})
            for (bool redo : {false, true})
                for (int keyform = 0; keyform < 2; ++keyform)
                    for (bool invalid : {false, true})
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4").arg(rotation).arg(redo).arg(keyform).arg(invalid)))
                            << rotation << redo << keyform << invalid;
    }
    void fullFrameHistoryRejectsTamperedOpacityStorage()
    {
        QFETCH(bool, rotation); QFETCH(bool, redo); QFETCH(int, keyform); QFETCH(bool, invalid);
        CModelDocument document; QVERIFY(load(document));
        if (rotation) {
            auto forms = document.DeformerRotationKeyforms("Root"); forms[0].opacity = 0.75f;
            QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        } else {
            auto forms = document.DeformerWarpKeyforms("Warp"); forms[0].opacity = 0.75f;
            QVERIFY(document.SetDeformerWarpKeyforms("Warp", forms));
        }
        const State before{fullFrames(document)[rotation ? 4 : 2]}; auto after = changed(before);
        after[0].opacity = 0.125f;
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, before[0], edit));
        QVERIFY(document.ExecuteEdit(edit));
        CModelEdit archived;
        if (redo) QVERIFY(document.UndoEdit(&archived)); else QVERIFY(document.PeekUndoEdit(&archived));
        QVERIFY(archived.storage);
        auto storage = std::const_pointer_cast<CModelEditStorageState>(archived.storage);
        auto& destination = redo ? storage->after : storage->before;
        const QString owner = rotation ? "Root" : "Warp", category = rotation ? "rotation" : "warp";
        auto entries = destination.value(category).toMap(); auto entry = entries.value(owner).toMap();
        const float opacity = invalid ? std::numeric_limits<float>::quiet_NaN() : 0.375f;
        if (rotation) {
            auto forms = entry.value("value").value<QVector<CModelRotationKeyform>>();
            QCOMPARE(forms.size(), 2); forms[keyform].opacity = opacity;
            entry["value"] = QVariant::fromValue(forms);
        } else {
            auto forms = entry.value("value").value<QVector<CModelWarpKeyform>>();
            QCOMPARE(forms.size(), 2); forms[keyform].opacity = opacity;
            entry["value"] = QVariant::fromValue(forms);
        }
        entries[owner] = entry; destination[category] = entries;
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!(redo ? document.RedoEdit() : document.UndoEdit()));
        QCOMPARE(document.AuthoringStructureState(), state); QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.CanRedoEdit(), redo); QCOMPARE(document.CanUndoEdit(), !redo);
    }

    void optionalOpacityRetainsBitExactZero_data()
    {
        QTest::addColumn<bool>("rotation");
        QTest::newRow("warp") << false; QTest::newRow("rotation") << true;
    }
    void optionalOpacityRetainsBitExactZero()
    {
        QFETCH(bool, rotation);
        CModelDocument document; QVERIFY(load(document));
        if (rotation) {
            auto forms = document.DeformerRotationKeyforms("Root"); forms[0].opacity = 0.0f;
            QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        } else {
            auto forms = document.DeformerWarpKeyforms("Warp"); forms[0].opacity = 0.0f;
            QVERIFY(document.SetDeformerWarpKeyforms("Warp", forms));
        }
        const State before{fullFrames(document)[rotation ? 4 : 2]}; auto after = before;
        after[0].opacity = -0.0f; QVERIFY(before != after);
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, before[0], edit));
        QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(std::signbit(*fullFrames(document)[rotation ? 4 : 2].opacity));
        QVERIFY(!document.ExecuteEdit(edit));
        QVERIFY(document.UndoEdit());
        QVERIFY(!std::signbit(*fullFrames(document)[rotation ? 4 : 2].opacity));
        QVERIFY(document.RedoEdit());
        QVERIFY(std::signbit(*fullFrames(document)[rotation ? 4 : 2].opacity));
    }

    void oneHistoryRestoresEveryOwnerAndSparseStorage()
    {
        CModelDocument document; QString error;
        QVERIFY2(load(document, &error), qPrintable(error));
        const auto before = all(document), after = changed(before);
        QCOMPARE(before.size(), 6);
        const auto baseline = document.AuthoringStructureState();
        const auto originalWarp = document.DeformerWarpKeyforms("Warp");
        const auto originalRotation = document.DeformerRotationKeyforms("Root");
        CModelEdit edit;
        QVERIFY2(document.MakeGeometryEdit(before, after, before[0], edit, &error), qPrintable(error));
        QCOMPARE(document.AuthoringStructureState(), baseline);
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(all(document), after);
        QCOMPARE(historyCount(document), 1);
        QVERIFY(!document.CanRedoEdit());
        for (int index = 0; index < 2; ++index) {
            QCOMPARE(document.DeformerWarpKeyforms("Warp")[index].opacity, originalWarp[index].opacity);
            QCOMPARE(document.DeformerRotationKeyforms("Root")[index].opacity, originalRotation[index].opacity);
        }
        CModelEdit archived;
        QVERIFY(document.UndoEdit(&archived));
        QCOMPARE(archived.field, CModelEdit::GeometryKeyforms);
        QCOMPARE(document.AuthoringStructureState(), baseline);
        QVERIFY(document.DrawableKeyformPositionOverrides().isEmpty());
        QVERIFY(document.DeformerWarpKeyformOverrides().isEmpty());
        QVERIFY(document.DeformerRotationKeyformOverrides().isEmpty());
        QVERIFY(document.RedoEdit());
        QCOMPARE(all(document), after);
        QCOMPARE(historyCount(document), 1);
    }

    void rejectsWholeBatch_data()
    {
        QTest::addColumn<int>("failure");
        for (int index = 0; index < 15; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void rejectsWholeBatch()
    {
        QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document));
        const auto initial = command(document); QVERIFY(document.ExecuteEdit(initial)); QVERIFY(document.UndoEdit());
        auto before = all(document), after = changed(before);
        auto driver = before[0];
        if (failure == 0) std::get<QVector<float>>(before[3].value)[0] += 1;
        if (failure == 1) after[3].id = "missing";
        if (failure == 2) { before.append(before[3]); after.append(after[3]); }
        if (failure == 3) after[3].value = CModelRotationTransform{};
        if (failure == 4) std::get<QVector<float>>(after[3].value)[0] = std::numeric_limits<float>::quiet_NaN();
        if (failure == 5) std::get<QVector<float>>(after[3].value).removeLast();
        if (failure == 6) { before[3].keyform = 20; after[3].keyform = 20; }
        if (failure == 7) { before[3].type = Frame::Type(99); after[3].type = Frame::Type(99); }
        if (failure == 8) driver.id = "other";
        if (failure == 9) after = before;
        if (failure == 10) after.removeLast();
        if (failure == 11) document.SetLocked("warp", "Warp", true);
        if (failure == 12) document.SetLocked("rotation", "Root", true);
        if (failure == 13) std::get<CModelRotationTransform>(after[4].value).scale = std::numeric_limits<float>::infinity();
        if (failure == 14) { before[3].id = "Root"; after[3].id = "Root"; }
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        CModelEdit output{"sentinel", "type", CModelEdit::Opacity, 1.0f, 0.5f};
        QString error;
        QVERIFY(!document.MakeGeometryEdit(before, after, driver, output, &error));
        QVERIFY(!error.isEmpty()); QCOMPARE(output.name, QString("sentinel"));
        CModelEdit raw{driver.id, driver.TypeName(), CModelEdit::GeometryKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(after), driver.keyform, false};
        QVERIFY(!document.ExecuteEdit(raw));
        QCOMPARE(document.AuthoringStructureState(), baseline); QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
    }

    void malformedCommandIsRejected_data()
    {
        QTest::addColumn<int>("failure");
        for (int index = 0; index < 7; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void malformedCommandIsRejected()
    {
        QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document)); auto edit = command(document);
        if (failure == 0) edit.coalesce = true;
        if (failure == 1) edit.before = QVariantList{};
        if (failure == 2) edit.after = QVariantMap{};
        if (failure == 3) edit.parameterChange = CModelEdit::ParameterChange::Create;
        if (failure == 4) edit.freezeValue = 0.5f;
        if (failure == 5) edit.type = "objects";
        if (failure == 6) edit.index = -1;
        const auto state = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit());
    }

    void unchangedLockedDriverCanBeRead()
    {
        CModelDocument document; QVERIFY(load(document));
        auto before = all(document); auto after = before;
        std::get<QVector<float>>(after[2].value)[0] += 0.1f;
        document.SetLocked("artmesh", "Mesh", true);
        CModelEdit edit; QString error;
        QVERIFY2(document.MakeGeometryEdit(before, after, before[0], edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(all(document), after);
        QVERIFY(!document.DrawableKeyformPositionOverrides().contains("Mesh"));
        QVERIFY(document.UndoEdit()); QCOMPARE(all(document), before);
        QVERIFY(document.IsLocked("artmesh", "Mesh"));
        QVERIFY(document.RedoEdit()); QCOMPARE(all(document), after);
    }

    void signedZeroIsAChangeAndStaleSignIsRejected()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerRotationKeyforms("Root"); forms[0].scale = 0.0f;
        QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        Frame frame; QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, frame));
        State before{frame}, after = before;
        std::get<CModelRotationTransform>(after[0].value).scale = -0.0f;
        QVERIFY(before != after);
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, frame, edit));
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(std::signbit(document.DeformerRotationKeyforms("Root")[0].scale));
        QVERIFY(!document.ExecuteEdit(edit));
        QVERIFY(document.UndoEdit()); QVERIFY(!std::signbit(document.DeformerRotationKeyforms("Root")[0].scale));
        QVERIFY(document.RedoEdit()); QVERIFY(std::signbit(document.DeformerRotationKeyforms("Root")[0].scale));
    }

    void locksAddedLaterDoNotTrapGeometryHistory()
    {
        CModelDocument document; QVERIFY(load(document)); const auto before = all(document);
        QVERIFY(document.ExecuteEdit(command(document)));
        document.SetLocked("rotation", "Root", true);
        QVERIFY(document.UndoEdit()); QCOMPARE(all(document), before);
        QVERIFY(document.IsLocked("rotation", "Root"));
        QVERIFY(document.RedoEdit()); QCOMPARE(all(document), changed(before));
    }

    void rawRotationReflectionRemainsAnAllowedStoredGeometry()
    {
        CModelDocument document; QVERIFY(load(document));
        Frame frame; QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, frame));
        const State before{frame}; State after = before;
        auto& transform = std::get<CModelRotationTransform>(after[0].value);
        transform.reflectX = !transform.reflectX; transform.reflectY = !transform.reflectY; transform.scale = -0.75f;
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, frame, edit));
        QVERIFY(document.ExecuteEdit(edit));
        Frame actual; QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, actual));
        QCOMPARE(actual, after[0]); QVERIFY(document.UndoEdit());
        QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, actual)); QCOMPARE(actual, before[0]);
    }

    void historyRejectsInterveningOwnerMutation()
    {
        CModelDocument document; QVERIFY(load(document)); auto edit = command(document);
        QVERIFY(document.ExecuteEdit(edit));
        auto positions = document.DrawableKeyformPositions("Mesh", 1); positions[0] += 1;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 1, positions));
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(document.Revision(), revision); QVERIFY(document.CanUndoEdit());
    }

    void savedSessionAndCompiledModelRetainBothKeyforms()
    {
        CModelDocument document; QString error; QVERIFY(load(document));
        const auto source = document; const auto before = all(document), expected = changed(before);
        QVERIFY(document.ExecuteEdit(command(document)));
        QByteArray bytes; QVERIFY2(document.BuildRuntimeMoc(bytes, &error), qPrintable(error));
        CModelDocument compiled; QVERIFY2(compiled.LoadBytes(bytes, "compiled", &error), qPrintable(error));
        QCOMPARE(all(compiled), expected);
        QCOMPARE(compiled.Drawables()[0].uvs, source.Drawables()[0].uvs);
        QCOMPARE(compiled.DrawableIndices("Mesh"), source.DrawableIndices("Mesh"));
        QCOMPARE(compiled.DeformerWarpKeyforms("Warp")[1].opacity, source.DeformerWarpKeyforms("Warp")[1].opacity);
        CModelSession stored; QVERIFY2(CModelSession::FromJson(session(document).ToJson(), &stored, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(stored.RestoreDocument(source, restored, &error), qPrintable(error));
        QCOMPARE(all(restored), expected); QVERIFY(!restored.CanUndoEdit());
        QVERIFY(restored.BuildRuntimeMoc(bytes, &error));
    }

    void exactStorageRejectsSignedZeroDrift()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerRotationKeyforms("Root"); forms[0].scale = 0.0f;
        QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        Frame driver; QVERIFY(document.CaptureGeometryKeyform(Frame::Rotation, "Root", 0, driver));
        State before{driver}, after = before;
        std::get<CModelRotationTransform>(after[0].value).scale = -0.0f;
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, driver, edit));
        QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(document.Revision(), revision); QVERIFY(document.CanUndoEdit());
    }

    void unrelatedParameterChangesDoNotInvalidateGeometryStorage()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto before = all(document); QVERIFY(document.ExecuteEdit(command(document)));
        QVERIFY(document.SetParameterValue("P", 0.4f)); QVERIFY(document.UndoEdit());
        QCOMPARE(all(document), before); QCOMPARE(document.ParameterValue("P"), 0.4f);
        QVERIFY(document.RedoEdit()); QCOMPARE(all(document), changed(before));
    }

    void partAncestorsAndEffectiveDeformerParentsAreRespected_data()
    {
        QTest::addColumn<bool>("deformerPart");
        QTest::newRow("mesh-part") << false; QTest::newRow("ancestor-deformer-part") << true;
    }
    void partAncestorsAndEffectiveDeformerParentsAreRespected()
    {
        QFETCH(bool, deformerPart);
        DeformerSnapshotData::Fixture source; const auto& o = *source.table.sectionOffsetTable;
        source.count(0, 2);
        for (int index = 0; index < 2; ++index) {
            source.id(o.parts->ids, index, index ? "PartChild" : "PartRoot");
            source.column(o.parts->keyform_binding_sources_indices, index, -1);
            source.column(o.parts->keyform_sources_begin_indices, index, 0);
            source.column(o.parts->keyform_sources_counts, index, 0);
            source.column(o.parts->is_visible, index, 1); source.column(o.parts->is_enabled, index, 1);
            source.column(o.parts->parent_part_indices, index, index - 1);
        }
        source.column(deformerPart ? o.deformers->parent_part_indices : o.art_meshes->parent_part_indices,
                      deformerPart ? 1 : 0, 1);
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(source.bytes, "parts", &error), qPrintable(error));
        Frame frame; QVERIFY(document.CaptureGeometryKeyform(Frame::ArtMesh, "Mesh", 0, frame));
        const State before{frame}, after = changed(before);
        document.SetLocked("part", "PartRoot", true);
        CModelEdit edit; QVERIFY(!document.MakeGeometryEdit(before, after, frame, edit, &error));
        QVERIFY(error.contains("PartRoot"));
        if (deformerPart) {
            QVERIFY(document.SetDeformerParentIndex("Child", -1));
            QVERIFY2(document.MakeGeometryEdit(before, after, frame, edit, &error), qPrintable(error));
            QVERIFY(document.SetDeformerParentIndex("Child", 1));
            QVERIFY(!document.ExecuteEdit(edit));
        }
    }

    void sharedNormalRowsDetachWithoutChangingTheOtherOwner()
    {
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(sharedFixture(false), "shared", &error), qPrintable(error));
        Frame source, target;
        QVERIFY(document.CaptureGeometryKeyform(Frame::ArtMesh, "Mesh", 0, source));
        QVERIFY(document.CaptureGeometryKeyform(Frame::ArtMesh, "Twin", 0, target));
        const State before{source, target}; auto after = changed(before);
        CModelEdit edit; QVERIFY(document.MakeGeometryEdit(before, after, source, edit, &error));
        QVERIFY(document.ExecuteEdit(edit)); QByteArray compiled;
        QVERIFY2(document.BuildRuntimeMoc(compiled, &error), qPrintable(error));
        CModelDocument rebuilt; QVERIFY(rebuilt.LoadBytes(compiled, "rebuilt", &error));
        Frame observed;
        QVERIFY(rebuilt.CaptureGeometryKeyform(Frame::ArtMesh, "Mesh", 0, observed)); QCOMPARE(observed, after[0]);
        QVERIFY(rebuilt.CaptureGeometryKeyform(Frame::ArtMesh, "Twin", 0, observed)); QCOMPARE(observed, after[1]);
        QCOMPARE(rebuilt.DrawableKeyformPositions("Mesh", 1), document.Drawables()[0].keyforms[1].positions);
        QCOMPARE(rebuilt.DrawableKeyformPositions("Twin", 1), document.Drawables()[1].keyforms[1].positions);
        QVERIFY(document.UndoEdit()); QVERIFY(document.DrawableKeyformPositionOverrides().isEmpty());
    }

    void captureRejectsWrongTypesWithoutTouchingOutput()
    {
        CModelDocument document; QVERIFY(load(document));
        Frame output{Frame::ArtMesh, "sentinel", 33, QVector<float>{7, 9}}; const auto before = output;
        QVERIFY(!document.CaptureGeometryKeyform(Frame::Rotation, "Warp", 0, output)); QCOMPARE(output, before);
        QVERIFY(!document.CaptureGeometryKeyform(Frame::ArtMesh, "Mesh", 4, output)); QCOMPARE(output, before);
        QVERIFY(!document.CaptureGeometryKeyform(Frame::Type(44), "Mesh", 0, output)); QCOMPARE(output, before);
    }
    void directSetterOwnsDrawableCallerBuffer()
    {
        CModelDocument document; QVERIFY(load(document));
        auto positions = document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0);
        QVERIFY(!positions.isEmpty()); positions[0] += 0.25f;
        float* caller = positions.data();
        const QVector<float> expected = deepCopy(positions);
        QString id = QStringLiteral("Mesh");
        QVERIFY(document.SetDrawableKeyformPositions(id, 0, positions));
        Observation committed; QVERIFY(observe(document, committed));
        caller[0] += 100.0f;
        QCOMPARE(document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0), expected);
        compareObservation(document, committed);
    }

    void directSetterOwnsWarpInnerCallerBuffer()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerWarpKeyforms(QStringLiteral("Warp"));
        QVERIFY(!forms.isEmpty() && !forms[0].positions.isEmpty()); forms[0].positions[0] += 0.25f;
        float* caller = forms[0].positions.data();
        const auto expected = deepWarpCopy(forms);
        QVERIFY(document.SetDeformerWarpKeyforms(QStringLiteral("Warp"), forms));
        Observation committed; QVERIFY(observe(document, committed));
        caller[0] += 100.0f;
        QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), expected);
        compareObservation(document, committed);
    }

    void directSetterOwnsWarpOuterCallerBuffer()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerWarpKeyforms(QStringLiteral("Warp"));
        QVERIFY(!forms.isEmpty()); forms[0].opacity = 0.625f;
        CModelWarpKeyform* caller = forms.data();
        const auto expected = deepWarpCopy(forms);
        QVERIFY(document.SetDeformerWarpKeyforms(QStringLiteral("Warp"), forms));
        Observation committed; QVERIFY(observe(document, committed));
        caller[0].opacity = 0.125f;
        QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), expected);
        compareObservation(document, committed);
    }

    void directSetterOwnsRotationOuterCallerBuffer()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerRotationKeyforms(QStringLiteral("Root"));
        QVERIFY(!forms.isEmpty()); forms[0].angle += 2.5f;
        CModelRotationKeyform* caller = forms.data();
        const auto expected = deepRotationCopy(forms);
        QVERIFY(document.SetDeformerRotationKeyforms(QStringLiteral("Root"), forms));
        Observation committed; QVERIFY(observe(document, committed));
        caller[0].angle += 100.0f;
        QCOMPARE(document.DeformerRotationKeyforms(QStringLiteral("Root")), expected);
        compareObservation(document, committed);
    }

    void directSetterCopiesRawIdBeforeMapInsertion()
    {
        CModelDocument document; QVERIFY(load(document));
        auto positions = document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0);
        QVERIFY(!positions.isEmpty()); positions[0] += 0.25f;
        const auto expected = deepCopy(positions);
        QChar chars[]{'M', 'e', 's', 'h'};
        const QString id = QString::fromRawData(chars, 4);
        QVERIFY(document.SetDrawableKeyformPositions(id, 0, positions));
        Observation committed; QVERIFY(observe(document, committed));
        chars[0] = 'X';
        QCOMPARE(document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0), expected);
        compareObservation(document, committed);
    }

    void directSetterCopiesRawWarpIdBeforeMapInsertion()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerWarpKeyforms(QStringLiteral("Warp"));
        QVERIFY(!forms.isEmpty()); forms[0].opacity = 0.625f;
        const auto expected = deepWarpCopy(forms);
        QChar chars[]{'W', 'a', 'r', 'p'};
        const QString id = QString::fromRawData(chars, 4);
        QVERIFY(document.SetDeformerWarpKeyforms(id, forms));
        Observation committed; QVERIFY(observe(document, committed));
        chars[0] = 'X';
        QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), expected);
        compareObservation(document, committed);
    }

    void directSetterCopiesRawRotationIdBeforeMapInsertion()
    {
        CModelDocument document; QVERIFY(load(document));
        auto forms = document.DeformerRotationKeyforms(QStringLiteral("Root"));
        QVERIFY(!forms.isEmpty()); forms[0].angle += 2.5f;
        const auto expected = deepRotationCopy(forms);
        QChar chars[]{'R', 'o', 'o', 't'};
        const QString id = QString::fromRawData(chars, 4);
        QVERIFY(document.SetDeformerRotationKeyforms(id, forms));
        Observation committed; QVERIFY(observe(document, committed));
        chars[0] = 'X';
        QCOMPARE(document.DeformerRotationKeyforms(QStringLiteral("Root")), expected);
        compareObservation(document, committed);
    }

    void oldKeyformPositionsCommandOwnsCallerBuffer()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto before = document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0);
        auto after = before; QVERIFY(!after.isEmpty()); after[0] += 0.25f;
        const auto expected = deepCopy(after);
        float* caller = after.data();
        CModelEdit edit{"Mesh", "artmesh", CModelEdit::KeyformPositions,
                        QVariant::fromValue(before), QVariant::fromValue(after), 0, false};
        QVERIFY(document.ExecuteEdit(edit));
        Observation committed; QVERIFY(observe(document, committed));
        caller[0] += 100.0f;
        QCOMPARE(document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0), expected);
        compareObservation(document, committed);
        CModelEdit archived; QVERIFY(document.PeekUndoEdit(&archived));
        QCOMPARE(archived.name, QStringLiteral("Mesh"));
        QCOMPARE(archived.type, QStringLiteral("artmesh"));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0), before);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0), expected);
    }

    void oldWarpKeyformsCommandOwnsInnerCallerBuffer()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto before = document.DeformerWarpKeyforms(QStringLiteral("Warp"));
        auto after = before; QVERIFY(!after.isEmpty() && !after[0].positions.isEmpty());
        after[0].positions[0] += 0.25f;
        const auto expected = deepWarpCopy(after);
        float* caller = after[0].positions.data();
        CModelEdit edit{"Warp", "warp", CModelEdit::WarpKeyforms,
                        QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
        QVERIFY(document.ExecuteEdit(edit));
        Observation committed; QVERIFY(observe(document, committed));
        caller[0] += 100.0f;
        QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), expected);
        compareObservation(document, committed);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), before);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), expected);
    }

    void oldWarpKeyformsCommandOwnsOuterCallerBuffer()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto before = document.DeformerWarpKeyforms(QStringLiteral("Warp"));
        auto after = before; QVERIFY(!after.isEmpty()); after[0].opacity = 0.625f;
        const auto expected = deepWarpCopy(after);
        CModelWarpKeyform* caller = after.data();
        CModelEdit edit{"Warp", "warp", CModelEdit::WarpKeyforms,
                        QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
        QVERIFY(document.ExecuteEdit(edit));
        Observation committed; QVERIFY(observe(document, committed));
        caller[0].opacity = 0.125f;
        QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), expected);
        compareObservation(document, committed);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), before);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DeformerWarpKeyforms(QStringLiteral("Warp")), expected);
    }

    void oldRotationKeyformsCommandOwnsOuterCallerBuffer()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto before = document.DeformerRotationKeyforms(QStringLiteral("Root"));
        auto after = before; QVERIFY(!after.isEmpty()); after[0].angle += 2.5f;
        const auto expected = deepRotationCopy(after);
        CModelRotationKeyform* caller = after.data();
        CModelEdit edit{"Root", "rotation", CModelEdit::RotationKeyforms,
                        QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
        QVERIFY(document.ExecuteEdit(edit));
        Observation committed; QVERIFY(observe(document, committed));
        caller[0].angle += 100.0f;
        QCOMPARE(document.DeformerRotationKeyforms(QStringLiteral("Root")), expected);
        compareObservation(document, committed);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.DeformerRotationKeyforms(QStringLiteral("Root")), before);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.DeformerRotationKeyforms(QStringLiteral("Root")), expected);
    }

    void oldCommandCopiesRawNameAndTypeBeforeHistory()
    {
        CModelDocument document; QVERIFY(load(document));
        const auto before = document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0);
        auto after = before; QVERIFY(!after.isEmpty()); after[0] += 0.25f;
        const auto expected = deepCopy(after);
        QChar nameChars[]{'M', 'e', 's', 'h'};
        QChar typeChars[]{'a', 'r', 't', 'm', 'e', 's', 'h'};
        CModelEdit edit{QString::fromRawData(nameChars, 4), QString::fromRawData(typeChars, 7),
                        CModelEdit::KeyformPositions,
                        QVariant::fromValue(before), QVariant::fromValue(after), 0, false};
        QVERIFY(document.ExecuteEdit(edit));
        nameChars[0] = 'X'; typeChars[0] = 'X';
        CModelEdit archived; QVERIFY(document.PeekUndoEdit(&archived));
        QCOMPARE(archived.name, QStringLiteral("Mesh"));
        QCOMPARE(archived.type, QStringLiteral("artmesh"));
        QVERIFY(document.UndoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DrawableKeyformPositions(QStringLiteral("Mesh"), 0), expected);
    }

    void legacyCoalescedCommandsOwnCallerBuffers_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("buffer");
        const QStringList kinds{"positions", "warp", "rotation"};
        const QStringList buffers{"before", "after", "name", "type"};
        for (int kind = 0; kind < kinds.size(); ++kind)
            for (int buffer = 0; buffer < buffers.size(); ++buffer)
                QTest::newRow(qPrintable(kinds[kind] + '-' + buffers[buffer])) << kind << buffer;
    }

    void legacyCoalescedCommandsOwnCallerBuffers()
    {
        QFETCH(int, kind); QFETCH(int, buffer);
        CModelDocument document; QVERIFY(load(document));
        const auto initialState = document.AuthoringStructureState();
        const QString name = kind == 0 ? "Mesh" : kind == 1 ? "Warp" : "Root";
        const QString type = kind == 0 ? "artmesh" : kind == 1 ? "warp" : "rotation";
        const CModelEdit::Field fields[]{CModelEdit::KeyformPositions, CModelEdit::WarpKeyforms,
                                       CModelEdit::RotationKeyforms};
        auto run = [&](auto read, auto copy, auto raw) {
            using Forms = decltype(read());
            auto before = copy(read());
            const auto expectedInitial = copy(before);
            auto first = copy(before); *raw(first) += 0.125f;
            const auto expectedFirst = copy(first);
            auto secondBefore = copy(first);
            auto final = copy(first); *raw(final) += 0.25f;
            const auto expectedFinal = copy(final);
            float* beforeBuffer = raw(before);
            float* firstBuffer = raw(first);
            float* secondBeforeBuffer = raw(secondBefore);
            float* finalBuffer = raw(final);
            QString firstName = name, firstType = type, secondName = name, secondType = type;
            QChar* firstNameBuffer = firstName.data(); QChar* firstTypeBuffer = firstType.data();
            QChar* secondNameBuffer = secondName.data(); QChar* secondTypeBuffer = secondType.data();
            CModelEdit firstEdit{QString::fromRawData(firstNameBuffer, firstName.size()),
                QString::fromRawData(firstTypeBuffer, firstType.size()), fields[kind],
                QVariant::fromValue(before), QVariant::fromValue(first), kind == 0 ? 0 : -1, true};
            QVERIFY(document.ExecuteEdit(firstEdit));
            Observation firstCommitted; QVERIFY(observe(document, firstCommitted));
            if (buffer == 0) *beforeBuffer += 100.0f;
            if (buffer == 1) *firstBuffer += 100.0f;
            if (buffer == 2) firstNameBuffer[0] = 'X';
            if (buffer == 3) firstTypeBuffer[0] = 'X';
            CModelEdit archived; QVERIFY(document.PeekUndoEdit(&archived));
            QCOMPARE(archived.before.value<Forms>(), expectedInitial);
            QCOMPARE(archived.after.value<Forms>(), expectedFirst);
            QCOMPARE(archived.name, name); QCOMPARE(archived.type, type);
            QCOMPARE(read(), expectedFirst);
            compareObservation(document, firstCommitted);
            CModelEdit secondEdit{QString::fromRawData(secondNameBuffer, secondName.size()),
                QString::fromRawData(secondTypeBuffer, secondType.size()), fields[kind],
                QVariant::fromValue(secondBefore), QVariant::fromValue(final), kind == 0 ? 0 : -1, true};
            QVERIFY(document.ExecuteEdit(secondEdit));
            Observation finalCommitted; QVERIFY(observe(document, finalCommitted));
            if (buffer == 0) *secondBeforeBuffer += 100.0f;
            if (buffer == 1) *finalBuffer += 100.0f;
            if (buffer == 2) secondNameBuffer[0] = 'X';
            if (buffer == 3) secondTypeBuffer[0] = 'X';
            QVERIFY(document.PeekUndoEdit(&archived));
            QCOMPARE(archived.before.value<Forms>(), expectedInitial);
            QCOMPARE(archived.after.value<Forms>(), expectedFinal);
            QCOMPARE(archived.name, name); QCOMPARE(archived.type, type);
            QCOMPARE(read(), expectedFinal);
            compareObservation(document, finalCommitted);
            QCOMPARE(historyCount(document), 1);
            QVERIFY(document.UndoEdit());
            QCOMPARE(document.AuthoringStructureState(), initialState);
            QCOMPARE(read(), expectedInitial);
            QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
            QVERIFY(document.RedoEdit());
            QCOMPARE(read(), expectedFinal);
            finalCommitted.revision = document.Revision();
            compareObservation(document, finalCommitted);
            QCOMPARE(historyCount(document), 1);
            QVERIFY(!document.CanRedoEdit());
        };
        if (kind == 0) run([&] { return document.DrawableKeyformPositions(name, 0); },
            [](const QVector<float>& values) { return deepCopy(values); },
            [](QVector<float>& values) { return values.data(); });
        else if (kind == 1) run([&] { return document.DeformerWarpKeyforms(name); }, deepWarpCopy,
            [](QVector<CModelWarpKeyform>& values) { return values[0].positions.data(); });
        else run([&] { return document.DeformerRotationKeyforms(name); }, deepRotationCopy,
            [](QVector<CModelRotationKeyform>& values) { return &values.data()[0].angle; });
    }

    void invalidLegacyKeyformCommandIsAtomic_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("failure");
        const QStringList kinds{"positions", "warp", "rotation"};
        const QStringList failures{"before-type", "after-type", "nonfinite", "shape", "missing-id"};
        for (int kind = 0; kind < kinds.size(); ++kind)
            for (int failure = 0; failure < failures.size(); ++failure)
                QTest::newRow(qPrintable(kinds[kind] + '-' + failures[failure])) << kind << failure;
        QTest::newRow("positions-negative-index") << 0 << 5;
        QTest::newRow("positions-outside-index") << 0 << 6;
    }

    void invalidLegacyKeyformCommandIsAtomic()
    {
        QFETCH(int, kind); QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document));
        const auto initial = command(document);
        QVERIFY(document.ExecuteEdit(initial)); QVERIFY(document.UndoEdit());
        Observation baseline; QVERIFY(observe(document, baseline));
        CModelEdit pending; QVERIFY(document.PeekRedoEdit(&pending));
        CModelEdit edit;
        if (kind == 0) {
            const auto before = document.DrawableKeyformPositions("Mesh", 0);
            auto after = deepCopy(before); after[0] += 0.125f;
            if (failure == 2) after[0] = std::numeric_limits<float>::quiet_NaN();
            if (failure == 3) after.removeLast();
            edit = {"Mesh", "artmesh", CModelEdit::KeyformPositions,
                QVariant::fromValue(before), QVariant::fromValue(after), 0, false};
        } else if (kind == 1) {
            const auto before = document.DeformerWarpKeyforms("Warp");
            auto after = deepWarpCopy(before); after[0].positions[0] += 0.125f;
            if (failure == 2) after[0].positions[0] = std::numeric_limits<float>::quiet_NaN();
            if (failure == 3) after.removeLast();
            edit = {"Warp", "warp", CModelEdit::WarpKeyforms,
                QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
        } else {
            const auto before = document.DeformerRotationKeyforms("Root");
            auto after = deepRotationCopy(before); after[0].angle += 0.125f;
            if (failure == 2) after[0].angle = std::numeric_limits<float>::quiet_NaN();
            if (failure == 3) after.removeLast();
            edit = {"Root", "rotation", CModelEdit::RotationKeyforms,
                QVariant::fromValue(before), QVariant::fromValue(after), -1, false};
        }
        if (failure == 0) edit.before = QVariantMap{};
        if (failure == 1) edit.after = QVariantMap{};
        if (failure == 4) edit.name = "missing";
        if (failure == 5) edit.index = -1;
        if (failure == 6) edit.index = 1000;
        QVERIFY(!document.ExecuteEdit(edit));
        compareObservation(document, baseline);
        QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
        CModelEdit stillPending; QVERIFY(document.PeekRedoEdit(&stillPending));
        QCOMPARE(stillPending.name, pending.name); QCOMPARE(stillPending.type, pending.type);
        QCOMPARE(stillPending.field, pending.field); QCOMPARE(stillPending.index, pending.index);
        QCOMPARE(stillPending.before, pending.before); QCOMPARE(stillPending.after, pending.after);
        QVERIFY(document.RedoEdit());
        QCOMPARE(all(document), initial.after.value<State>());
    }
};

QTEST_APPLESS_MAIN(GeometryBatchDocumentTests)
#include "geometry_batch_document_tests.moc"
