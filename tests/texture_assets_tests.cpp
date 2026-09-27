#include "live2dstruct/ctextureassets.h"
#include "live2dstruct/ctextureworkspace.h"
#include "live2dstruct/ctextureeditcontroller.h"
#include "live2dstruct/cmodelsession.h"
#include "std/cpngimage.h"
#include <QtTest>
#include <QBuffer>
#include <QImage>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>

using namespace live2dstruct;
using Assets = CTextureAssets;
namespace {
QByteArray png(QSize size = {3, 2}, QRgb color = qRgba(120, 30, 210, 93))
{
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(QColor::fromRgba(color));
    QBuffer buffer;
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    return buffer.data();
}
Assets::Catalog sources(const QString& path)
{
    Assets::Source image;
    if (!Assets::MakeSource(path, png(), image)) return {};
    return {{0, image}, {3, image}};
}
}
class TextureAssetsTests : public QObject
{
    Q_OBJECT
private slots:
    void generatedIdentityReplacementRequiresAnExplicitPolicy()
    {
        const auto catalog = sources("/tmp/workspace-source.png"); CTextureWorkspace::State before;
        QVERIFY(CTextureWorkspace::Initialize(catalog, {}, before));
        before.atlases.insert(4, {"atlas-A", "图集 A", -1, {5, 7}, png({5, 7})});
        auto after = before; after.atlases[4].id = "atlas-B"; after.atlases[4].name = "图集 B";
        after.atlases[4].png = png({5, 7}, qRgba(31, 47, 63, 0)); QString error;
        QVERIFY(!CTextureWorkspace::ValidateTransition(catalog, before, after, &error));
        QVERIFY(!CTextureWorkspace::ValidateTransition(catalog, before, after, CTextureWorkspace::Transition::PreserveIdentity, &error));
        QVERIFY2(CTextureWorkspace::ValidateTransition(catalog, before, after, CTextureWorkspace::Transition::ReplaceGenerated, &error), qPrintable(error));
        QVERIFY(CTextureWorkspace::ValidateTransition(catalog, after, before, CTextureWorkspace::Transition::ReplaceGenerated, &error));
        QCOMPARE(before.atlases[4].id, QString("atlas-A")); QCOMPARE(after.atlases[4].id, QString("atlas-B"));
    }
    void replacementPolicyStillProtectsSourcesAndDirectory_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 9; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void replacementPolicyStillProtectsSourcesAndDirectory()
    {
        QFETCH(int, fault); auto catalog = sources("/tmp/workspace-source.png"); CTextureWorkspace::State before;
        QVERIFY(CTextureWorkspace::Initialize(catalog, {}, before));
        before.atlases.insert(4, {"atlas-A", "图集 A", -1, {5, 7}, png({5, 7})});
        before.atlases.insert(5, {"atlas-C", "图集 C", -1, {5, 7}, png({5, 7})});
        auto after = before; after.atlases[4].id = "atlas-B";
        auto policy = CTextureWorkspace::Transition::ReplaceGenerated;
        if (fault == 0) after.atlases[0].id = "replacement-original";
        if (fault == 1) after.atlases[4].sourceSlot = 0;
        if (fault == 2) after.sourceFingerprint[0] ^= 1;
        if (fault == 3) after.atlases[4].id = after.atlases[5].id;
        if (fault == 4) after.atlases.remove(4);
        if (fault == 5) after.atlases.remove(3);
        if (fault == 6) catalog[0].digest[0] ^= 1;
        if (fault == 7) after.atlases[4].id = "source:99";
        if (fault == 8) policy = static_cast<CTextureWorkspace::Transition>(99);
        QString error; QVERIFY(!CTextureWorkspace::ValidateTransition(catalog, before, after, policy, &error)); QVERIFY(!error.isEmpty());
    }
    void workspaceDirectoryAndSourcesRemainIndependent()
    {
        const auto catalog = sources("/tmp/workspace-source.png"); const auto original = catalog;
        CTextureWorkspace::State state; QString error;
        QVERIFY2(CTextureWorkspace::Initialize(catalog, {}, state, &error), qPrintable(error));
        state.atlases[0].size = {6, 4}; state.atlases[0].png = png({6, 4});
        state.atlases.insert(4, {"generated-one", "新图集", -1, {5, 7}, png({5, 7}, qRgba(13, 40, 91, 0))});
        Assets::State effective; QVERIFY2(CTextureWorkspace::Effective(catalog, state, effective, &error), qPrintable(error));
        QCOMPARE(effective.keys(), (QList<int>{0, 3, 4})); QCOMPARE(effective[3], catalog[3].png);
        QCOMPARE(catalog, original); QCOMPARE(state.atlases[3].png, QByteArray());
        CTextureWorkspace::Draft draft, parsed; QVERIFY(CTextureWorkspace::CaptureDraft(catalog, state, draft, &error));
        QJsonObject json; QVERIFY(CTextureWorkspace::ToJson(draft, json, &error));
        QVERIFY2(CTextureWorkspace::FromJson(json, parsed, &error), qPrintable(error)); QCOMPARE(parsed, draft);
        CTextureWorkspace::State restored; QVERIFY(CTextureWorkspace::RestoreDraft(catalog, parsed, restored, &error)); QCOMPARE(restored, state);
        QByteArray first, second; QVERIFY(CTextureWorkspace::Fingerprint(catalog, state, first));
        restored.atlases[4].name = "名称改变"; QVERIFY(CTextureWorkspace::Fingerprint(catalog, restored, second)); QVERIFY(first != second);
    }
    void workspaceRejectsIdentityAndPixelMismatch_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 12; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void workspaceRejectsIdentityAndPixelMismatch()
    {
        QFETCH(int, fault); const auto catalog = sources("/tmp/workspace-source.png");
        CTextureWorkspace::State base; QVERIFY(CTextureWorkspace::Initialize(catalog, {}, base)); auto state = base;
        state.atlases.insert(4, {"generated-one", "图集", -1, {5, 7}, png({5, 7})});
        if (fault == 0) state.sourceFingerprint[0] ^= 1;
        if (fault == 1) state.atlases.remove(0);
        if (fault == 2) state.atlases[4].id = state.atlases[0].id;
        if (fault == 3) state.atlases[4].sourceSlot = 0;
        if (fault == 4) state.atlases[4].png.clear();
        if (fault == 5) state.atlases[4].size = {7, 5};
        if (fault == 6) state.atlases[4].png = state.atlases[4].png.left(40);
        if (fault == 7) state.atlases[0].png = catalog[0].png;
        if (fault == 8) state.atlases[0].size = {6, 4};
        if (fault == 9) { state.atlases[2] = state.atlases.take(4); }
        if (fault == 10) { state.atlases[6] = state.atlases.take(4); }
        if (fault == 11) state.atlases[0].id = "new-original";
        QString error; Assets::State out{{9, "unchanged"}}; const auto held = out;
        QVERIFY(!CTextureWorkspace::Effective(catalog, state, out, &error)); QCOMPARE(out, held); QVERIFY(!error.isEmpty());
    }
    void workspaceFreezesExternalStorageAndRejectsSourceReplacement()
    {
        const auto catalog = sources("/tmp/workspace-source.png"); CTextureWorkspace::State base;
        QVERIFY(CTextureWorkspace::Initialize(catalog, {}, base)); auto state = base;
        QByteArray bytes = png({5, 7}), identifier = "generated-one";
        state.atlases.insert(4, {QString::fromLatin1(identifier), "图集", -1, {5, 7}, QByteArray::fromRawData(bytes.constData(), bytes.size())});
        auto duplicated = state; duplicated.atlases[5] = duplicated.atlases[4]; duplicated.atlases[5].id = "generated-two";
        CTextureWorkspace::State shared; QVERIFY(CTextureWorkspace::Freeze(catalog, duplicated, shared));
        QVERIFY(shared.atlases[4].png.constData() == shared.atlases[5].png.constData());
        CTextureWorkspace::State frozen; QVERIFY(CTextureWorkspace::Freeze(catalog, state, frozen)); const auto held = frozen;
        QVERIFY(frozen.atlases[4].png.constData() != bytes.constData());
        const QByteArray original(bytes.constData(), bytes.size());
        bytes[bytes.size() / 2] ^= 1; QCOMPARE(frozen, held); QCOMPARE(frozen.atlases[4].png, original);
        QVERIFY(CTextureWorkspace::Validate(catalog, frozen));
        auto changed = frozen; changed.atlases[4].id = "replacement-id";
        QVERIFY(!CTextureWorkspace::ValidateTransition(catalog, frozen, changed));
        changed = frozen; changed.atlases.remove(4); QVERIFY(CTextureWorkspace::ValidateTransition(catalog, frozen, changed));
        CTextureWorkspace::Draft draft; QVERIFY(CTextureWorkspace::CaptureDraft(catalog, frozen, draft));
        auto other = catalog; Assets::Source replacement; QVERIFY(Assets::MakeSource("/tmp/workspace-source.png", png({3, 2}, qRgb(2, 4, 8)), replacement));
        other[0] = other[3] = replacement; CTextureWorkspace::State result = base;
        QVERIFY(!CTextureWorkspace::RestoreDraft(other, draft, result)); QCOMPARE(result, base);
    }
    void workspaceJsonFailuresDoNotReplaceResult_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 8; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void workspaceJsonFailuresDoNotReplaceResult()
    {
        QFETCH(int, fault); const auto catalog = sources("/tmp/workspace-source.png"); CTextureWorkspace::State state;
        QVERIFY(CTextureWorkspace::Initialize(catalog, {}, state));
        state.atlases.insert(4, {"generated-one", "图集", -1, {5, 7}, png({5, 7})});
        CTextureWorkspace::Draft draft; QVERIFY(CTextureWorkspace::CaptureDraft(catalog, state, draft));
        QJsonObject json; QVERIFY(CTextureWorkspace::ToJson(draft, json)); auto rows = json["atlases"].toArray(); auto row = rows.last().toObject();
        if (fault == 0) json["extra"] = true;
        if (fault == 1) json["sourceFingerprint"] = QString(64, 'a');
        if (fault == 2) row["png"] = row["png"].toString() + " ";
        if (fault == 3) row["slot"] = 4.5;
        if (fault == 4) row["sourceSlot"] = -2;
        if (fault == 5) row["width"] = 0;
        if (fault == 6) row["height"] = 500000000;
        if (fault == 7) rows.append(rows.first());
        if (fault != 7) rows[rows.size() - 1] = row;
        json["atlases"] = rows; auto result = draft;
        QVERIFY(!CTextureWorkspace::FromJson(json, result)); QCOMPARE(result, draft);
    }
    void straightAlphaAndImmutableSources()
    {
        QTemporaryDir directory; const auto path = directory.filePath("图集.png");
        const auto bytes = png(); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        const auto catalog = sources(path); QVERIFY(!catalog.isEmpty());
        QImage image; QString error; QVERIFY2(CPngImage::Decode(catalog[0].png, image, &error), qPrintable(error));
        QCOMPARE(image.pixel(1, 1), qRgba(120, 30, 210, 93)); QCOMPARE(image.format(), QImage::Format_RGBA8888);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(png({6, 4})); file.close(); QVERIFY(file.remove());
        QVERIFY(Assets::ValidateCatalog(catalog, &error)); QCOMPARE(Assets::Effective(catalog, {})[0], bytes);
    }
    void independentSlotsAndRestore()
    {
        const auto catalog = sources("/tmp/texture-source.png"); const auto replacement = png({3, 2}, qRgba(0, 230, 40, 7));
        Assets::State state; QString error; QVERIFY(Assets::Replace(catalog, {}, 3, replacement, state, &error));
        QCOMPARE(state.size(), 1); QCOMPARE(Assets::Effective(catalog, state)[0], catalog[0].png);
        QCOMPARE(Assets::Effective(catalog, state)[3], replacement);
        Assets::Draft draft; QVERIFY(Assets::CaptureDraft(catalog, state, draft, &error));
        Assets::State restored; QVERIFY(Assets::RestoreDraft(catalog, draft, restored, &error)); QCOMPARE(restored, state);
        QVERIFY(Assets::Replace(catalog, state, 3, catalog[3].png, restored, &error)); QVERIFY(restored.isEmpty());
    }
    void rejectedReplacementKeepsResult_data()
    {
        QTest::addColumn<int>("slot"); QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("unknown") << 2 << png();
        QTest::newRow("negative") << -1 << png();
        QTest::newRow("dimensions") << 0 << png({3, 3});
        QTest::newRow("empty") << 0 << QByteArray();
        QTest::newRow("garbage") << 0 << QByteArray("not png");
        QTest::newRow("truncated") << 0 << png().left(40);
    }
    void rejectedReplacementKeepsResult()
    {
        QFETCH(int, slot); QFETCH(QByteArray, bytes);
        const auto catalog = sources("/tmp/source.png"); Assets::State output{{7, "sentinel"}}; const auto before = output; QString error;
        QVERIFY(!Assets::Replace(catalog, {}, slot, bytes, output, &error)); QCOMPARE(output, before); QVERIFY(!error.isEmpty());
    }
    void staleSourcesAndCatalogValidation()
    {
        auto catalog = sources("/tmp/source.png"); const auto original = catalog;
        Assets::State state; QVERIFY(Assets::Replace(catalog, {}, 0, png({3, 2}, qRgb(1, 2, 3)), state));
        Assets::Draft draft; QVERIFY(Assets::CaptureDraft(catalog, state, draft));
        Assets::Source updated; QVERIFY(Assets::MakeSource("/tmp/source.png", png({3, 2}, qRgb(8, 9, 10)), updated));
        catalog[0] = catalog[3] = updated;
        Assets::State result{{6, "sentinel"}}; const auto held = result; QString error;
        QVERIFY(!Assets::RestoreDraft(catalog, draft, result, &error)); QCOMPARE(result, held);
        catalog = original; catalog[3].digest = QByteArray(32, 'x'); QVERIFY(!Assets::ValidateCatalog(catalog));
        catalog = original; catalog[3].size = QSize(10, 10); QVERIFY(!Assets::ValidateCatalog(catalog));
        catalog = original; catalog[3].path = "relative.png"; QVERIFY(!Assets::ValidateCatalog(catalog));
        catalog = original; catalog[3] = updated; QVERIFY(!Assets::ValidateCatalog(catalog));
    }
    void jsonRoundTripAndMalformed_data()
    {
        QTest::addColumn<int>("mutation");
        for (int mutation = 0; mutation < 12; ++mutation) QTest::newRow(qPrintable(QString::number(mutation))) << mutation;
    }
    void jsonRoundTripAndMalformed()
    {
        QFETCH(int, mutation); const auto catalog = sources("/tmp/来源.png"); Assets::State state;
        QVERIFY(Assets::Replace(catalog, {}, 3, png({3, 2}, qRgb(1, 255, 2)), state)); Assets::Draft draft;
        QVERIFY(Assets::CaptureDraft(catalog, state, draft)); QJsonArray array; QVERIFY(Assets::ToJson(draft, array));
        Assets::Draft restored; QVERIFY(Assets::FromJson(array, restored)); QCOMPARE(restored, draft);
        auto row = array[0].toObject();
        switch (mutation) {
        case 0: row["slot"] = 3.25; break;
        case 1: row["slot"] = -1; break;
        case 2: row["slot"] = Assets::MaximumSlots; break;
        case 3: row["png"] = "!!!!"; break;
        case 4: row["sourceDigest"] = "bad"; break;
        case 5: row["width"] = 5; break;
        case 6: row["height"] = 0; break;
        case 7: row["extra"] = true; break;
        case 8: row["sourcePath"] = "relative.png"; break;
        case 9: row.remove("sourceDigest"); break;
        case 10: row["png"] = row["png"].toString() + "\n"; break;
        case 11: array.append(array[0]); break;
        }
        array[0] = row; const auto held = restored; QString error;
        QVERIFY(!Assets::FromJson(array, restored, &error)); QCOMPARE(restored, held); QVERIFY(!error.isEmpty());
    }
    void controllerHistoryPublicationAndReentry()
    {
        CTextureEditController editor; const auto catalog = sources("/tmp/source.png"); QVERIFY(editor.Initialize(catalog));
        Assets::State runtime; int calls = 0; bool reject = false, throwing = false;
        editor.SetPublisher([&](const auto& state, QString* error) {
            ++calls; if (editor.Replace(0, png(), error) || editor.Initialize({}, {}, error)) throw std::runtime_error("reentry accepted");
            if (throwing) throw std::runtime_error("test rejection");
            if (reject) { *error = "controlled failure"; return false; }
            runtime = state; return true;
        });
        const auto replacement = png({3, 2}, qRgba(20, 50, 90, 4)); QString error;
        QVERIFY(editor.Replace(0, replacement, &error)); QVERIFY(editor.IsDirty()); QCOMPARE(editor.Current(), runtime); QCOMPARE(calls, 1);
        const auto revision = editor.Revision(); QVERIFY(editor.Replace(0, replacement, &error)); QCOMPARE(editor.Revision(), revision); QCOMPARE(calls, 1);
        editor.MarkSaved(); QVERIFY(!editor.IsDirty()); reject = true;
        QVERIFY(!editor.Undo(&error)); QCOMPARE(editor.Revision(), revision); QCOMPARE(editor.Current(), runtime); QVERIFY(editor.CanUndo());
        reject = false; throwing = true; QVERIFY(!editor.Undo(&error)); QCOMPARE(editor.Revision(), revision);
        throwing = false; QVERIFY(editor.Undo(&error)); QVERIFY(editor.Current().isEmpty()); QVERIFY(editor.IsDirty()); QVERIFY(editor.CanRedo());
        QVERIFY(editor.Redo(&error)); QCOMPARE(editor.Current(), runtime); QVERIFY(!editor.IsDirty());
        QVERIFY(editor.Undo(&error)); QVERIFY(editor.Replace(3, replacement, &error)); QVERIFY(!editor.CanRedo());
        QCOMPARE(editor.LastChangedSlot(), 3); QCOMPARE(editor.Current().size(), 1);
    }
    void preparedRestoreDoesNotPublish()
    {
        const auto catalog = sources("/tmp/source.png"); CTextureEditController editor; QVERIFY(editor.Initialize(catalog));
        int calls = 0; editor.SetPublisher([&](const auto&, QString*) { ++calls; return true; });
        QVERIFY(editor.Replace(0, png({3, 2}, qRgb(4, 5, 6)))); Assets::Draft draft;
        QVERIFY(Assets::CaptureDraft(editor.Sources(), editor.Current(), draft)); const auto revision = editor.Revision();
        CTextureEditController restored; QVERIFY(editor.PrepareRestore(draft, restored)); QCOMPARE(calls, 1); QCOMPARE(editor.Revision(), revision);
        QVERIFY(!restored.IsDirty()); QVERIFY(!restored.CanUndo()); QCOMPARE(restored.Current(), editor.Current());
        editor.Swap(restored); QVERIFY(!editor.CanUndo()); QCOMPARE(calls, 1);
        QVERIFY(editor.PrepareRestore({}, restored)); QVERIFY(restored.Current().isEmpty());
    }
    void sessionVersionAndAnchors()
    {
        const auto catalog = sources("/tmp/source.png"); Assets::State state; QVERIFY(Assets::Replace(catalog, {}, 0, png({3, 2}, qRgb(8, 7, 6)), state));
        CModelSession session; session.sourceMoc = "/tmp/model.moc3"; session.sourceDigest = QByteArray(32, 's');
        QVERIFY(Assets::CaptureDraft(catalog, state, session.textures)); const auto encoded = session.ToJson(); QVERIFY(!encoded.isEmpty());
        CModelSession loaded; QString error; QVERIFY2(CModelSession::FromJson(encoded, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.version, 7); QCOMPARE(loaded.textures, session.textures);
        auto json = QJsonDocument::fromJson(encoded).object(); json["version"] = 6;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &loaded));
        session.sourceDigest.clear(); QVERIFY(session.ToJson().isEmpty());
        session.textures.clear(); session.version = 6; QVERIFY(!session.ToJson().isEmpty());
    }
};
QTEST_GUILESS_MAIN(TextureAssetsTests)
#include "texture_assets_tests.moc"
