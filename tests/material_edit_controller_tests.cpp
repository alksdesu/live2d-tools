#include "geometry_object_pairs_document_fixture.h"
#include "live2dstruct/cmaterialeditcontroller.h"
#include "live2dstruct/cmodelsavefingerprint.h"
#include <QtTest>
#include <QPointer>
#include <memory>
#include <stdexcept>

using namespace GeometryPairsData;
namespace {
struct Fixture {
    CModelDocument document;
    CMaterialEditController editor;
    QByteArray runtime;
    bool reject = false, throwing = false;
    int publications = 0;
    QSet<QString> lastIds;
    bool Initialize()
    {
        if (!load(document) || !document.BuildRuntimeMoc(runtime)) return false;
        editor.SetDocument(&document);
        editor.SetPublisher([this](const auto& before, const auto& after, const auto& ids, QString* error) {
            ++publications;
            if (throwing) throw std::runtime_error("prepare failed");
            if (reject) { if (error) *error = QStringLiteral("renderer preparation failed"); return false; }
            QByteArray expected, candidate;
            if (!before.BuildRuntimeMoc(expected, error) || !after.BuildRuntimeMoc(candidate, error)) return false;
            if (expected != runtime) { if (error) *error = QStringLiteral("runtime changed"); return false; }
            runtime = std::move(candidate); lastIds = ids; return true;
        });
        return true;
    }
    CModelMaterialState State() const
    {
        CModelMaterialState result; document.CaptureDrawableMaterials(result); return result;
    }
};
}

class MaterialEditControllerTests : public QObject
{
    Q_OBJECT
private slots:
    void previewsCommitAsOneHistoryEntry()
    {
        Fixture f; QVERIFY(f.Initialize()); const auto source = f.runtime; const auto original = f.State();
        const auto revision = f.document.Revision(); QString error;
        QVERIFY(f.editor.Begin(&error)); auto state = original;
        for (int step = 1; step <= 5; ++step) {
            state[0].uvs[0] = original[0].uvs[0] + step * .01f;
            QVERIFY2(f.editor.Preview(state, &error), qPrintable(error));
            QCOMPARE(f.document.Revision(), revision); QCOMPARE(f.State(), original); QVERIFY(!f.document.CanUndoEdit());
            QCOMPARE(f.lastIds, QSet<QString>{state[0].id});
        }
        QVERIFY(f.runtime != source); const auto edited = f.runtime;
        QVERIFY2(f.editor.Commit(state, &error), qPrintable(error)); QVERIFY(!f.editor.IsActive());
        QCOMPARE(f.State(), state); QCOMPARE(f.runtime, edited); QVERIFY(f.document.CanUndoEdit());
        CModelEdit edit; QVERIFY(f.editor.History(true, edit, &error)); QCOMPARE(f.runtime, source);
        QCOMPARE(f.State(), original); QVERIFY(!f.document.CanUndoEdit()); QVERIFY(f.document.CanRedoEdit());
        QVERIFY(f.editor.History(false, edit, &error)); QCOMPARE(f.runtime, edited); QCOMPARE(f.State(), state);
    }
    void cancelRestoresDocumentAndRuntime()
    {
        Fixture f; QVERIFY(f.Initialize()); const auto baseline = f.document.AuthoringStructureState(); const auto source = f.runtime;
        QVERIFY(f.editor.Begin()); auto state = f.State(); state[0].textureIndex = 1; state[0].uvs[2] += .25f;
        QVERIFY(f.editor.Preview(state)); QVERIFY(f.runtime != source); QVERIFY(f.editor.Cancel());
        QCOMPARE(f.runtime, source); QCOMPARE(f.document.AuthoringStructureState(), baseline); QVERIFY(!f.document.CanUndoEdit());
    }
    void returnToOriginDoesNotCreateHistory()
    {
        Fixture f; QVERIFY(f.Initialize()); const auto original = f.State(); const auto revision = f.document.Revision();
        const auto source = f.runtime; QVERIFY(f.editor.Begin()); auto changed = original; changed[0].uvs[0] += .25f;
        QVERIFY(f.editor.Preview(changed)); QVERIFY(f.editor.Preview(original)); QVERIFY(f.editor.Commit(original));
        QCOMPARE(f.runtime, source); QCOMPARE(f.document.Revision(), revision); QVERIFY(!f.document.CanUndoEdit());
    }
    void failedPublicationsKeepDraftAndHistory_data()
    {
        QTest::addColumn<bool>("throwing"); QTest::newRow("false") << false; QTest::newRow("exception") << true;
    }
    void failedPublicationsKeepDraftAndHistory()
    {
        QFETCH(bool, throwing); Fixture f; QVERIFY(f.Initialize()); const auto original = f.State(); const auto source = f.runtime;
        QVERIFY(f.editor.Begin()); auto changed = original; changed[0].uvs[0] += .25f; QVERIFY(f.editor.Preview(changed));
        const auto preview = f.runtime; f.reject = !throwing; f.throwing = throwing; QString error;
        auto second = changed; second[0].uvs[1] += .2f;
        QVERIFY(!f.editor.Preview(second, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(f.runtime, preview);
        QVERIFY(!f.editor.Commit(second, &error)); QVERIFY(f.editor.IsActive()); QCOMPARE(f.State(), original);
        QVERIFY(!f.editor.Cancel(&error)); QVERIFY(f.editor.IsActive()); QCOMPARE(f.runtime, preview);
        f.reject = f.throwing = false; QVERIFY(f.editor.Cancel(&error)); QCOMPARE(f.runtime, source);
        QVERIFY(!f.document.CanUndoEdit());
    }
    void historyFailurePreservesUndoCursor()
    {
        Fixture f; QVERIFY(f.Initialize()); QVERIFY(f.editor.Begin()); auto changed = f.State(); changed[0].uvs[0] += .3f;
        QVERIFY(f.editor.Commit(changed)); const auto accepted = f.runtime; const auto revision = f.document.Revision();
        f.reject = true; CModelEdit edit; edit.name = "sentinel"; QVERIFY(!f.editor.History(true, edit));
        QCOMPARE(edit.name, QString("sentinel")); QCOMPARE(f.document.Revision(), revision); QCOMPARE(f.runtime, accepted);
        QVERIFY(f.document.CanUndoEdit()); QVERIFY(!f.document.CanRedoEdit()); f.reject = false; QVERIFY(f.editor.History(true, edit));
    }
    void lockedUnchangedObjectsRemainInSnapshot()
    {
        Fixture f; QVERIFY(f.Initialize()); auto state = f.State(); QVERIFY(state.size() >= 2);
        f.document.SetLocked("artmesh", state[1].id, true); QVERIFY(f.editor.Begin()); state[0].uvs[0] += .2f;
        QVERIFY(f.editor.Commit(state)); QCOMPARE(f.lastIds, QSet<QString>{state[0].id});
    }
    void staleDocumentAndInvalidStateNeverPublish()
    {
        Fixture f; QVERIFY(f.Initialize()); QVERIFY(f.editor.Begin()); auto state = f.State(); const auto runtime = f.runtime;
        const int calls = f.publications; state.append(state[0]); QVERIFY(!f.editor.Preview(state));
        state.removeLast(); state[0].id = "missing"; QVERIFY(!f.editor.Commit(state)); QCOMPARE(f.publications, calls);
        state = f.State(); f.document.SetLocked("artmesh", state[0].id, true);
        QVERIFY(!f.editor.Preview(state)); QVERIFY(!f.editor.Cancel()); QCOMPARE(f.publications, calls); QCOMPARE(f.runtime, runtime);
    }
    void reentryIsRejectedDuringPublication()
    {
        Fixture f; QVERIFY(f.Initialize()); bool nestedAccepted = true;
        f.editor.SetPublisher([&](const auto&, const auto&, const auto&, QString*) {
            nestedAccepted = f.editor.Begin(); f.editor.SetDocument(nullptr); return true;
        });
        QVERIFY(f.editor.Begin()); QVERIFY(!nestedAccepted); QVERIFY(f.editor.Cancel()); QVERIFY(!f.editor.IsActive());
    }
    void signedZeroIsARealEdit()
    {
        Fixture f; QVERIFY(f.Initialize()); auto state = f.State(); state[0].uvs[0] = 0.0f;
        QVERIFY(f.document.SetDrawableMaterials({state[0]})); QVERIFY(f.document.BuildRuntimeMoc(f.runtime));
        QVERIFY(f.editor.Begin()); state[0].uvs[0] = -0.0f; QVERIFY(f.editor.Commit(state));
        QCOMPARE(f.State(), state); QVERIFY(f.document.CanUndoEdit()); CModelEdit edit; QVERIFY(f.editor.History(true, edit));
        state[0].uvs[0] = 0.0f; QCOMPARE(f.State(), state);
    }
    void publisherCanDestroyController_data()
    {
        QTest::addColumn<int>("phase"); QTest::addColumn<bool>("owningObject");
        for (int phase = 0; phase < 5; ++phase)
            for (bool owningObject : {false, true}) QTest::newRow(qPrintable(QString("%1-%2").arg(phase).arg(owningObject))) << phase << owningObject;
    }
    void publisherCanDestroyController()
    {
        QFETCH(int, phase); QFETCH(bool, owningObject);
        struct Owner : QObject { CModelDocument document; CMaterialEditController editor; };
        CModelDocument standaloneDocument; QVERIFY(load(standaloneDocument));
        auto standalone = std::make_unique<CMaterialEditController>();
        QPointer<Owner> owner = owningObject ? new Owner : nullptr;
        if (owner) QVERIFY(load(owner->document));
        auto* editor = owner ? &owner->editor : standalone.get(); auto* document = owner ? &owner->document : &standaloneDocument;
        CModelMaterialState values; QVERIFY(document->CaptureDrawableMaterials(values)); values[0].uvs[0] += .1f;
        editor->SetDocument(document);
        bool destroy = phase == 0;
        editor->SetPublisher([&](const auto&, const auto&, const auto&, QString*) {
            if (destroy) { if (owningObject) delete owner.data(); else standalone.reset(); }
            return true;
        });
        QString error; CModelEdit edit;
        if (phase == 0) QVERIFY(!editor->Begin(&error));
        else {
            QVERIFY(editor->Begin(&error));
            if (phase == 4) QVERIFY(editor->Commit(values, &error));
            else if (phase == 2 || phase == 3) QVERIFY(editor->Preview(values, &error));
            destroy = true;
            if (phase == 1) QVERIFY(!editor->Preview(values, &error));
            if (phase == 2) QVERIFY(!editor->Commit(values, &error));
            if (phase == 3) QVERIFY(!editor->Cancel(&error));
            if (phase == 4) QVERIFY(!editor->History(true, edit, &error));
        }
        QVERIFY(!error.isEmpty());
        if (owningObject) QVERIFY(!owner); else QVERIFY(!standalone);
    }
    void activeDraftCannotBeSilentlyReboundOrRepublishedElsewhere()
    {
        Fixture f; QVERIFY(f.Initialize()); CModelDocument other; QVERIFY(load(other)); QVERIFY(f.editor.Begin());
        auto changed=f.State(); changed[0].uvs[0]+=.1f; QVERIFY(f.editor.Preview(changed));
        const auto preview=f.runtime; bool unexpected=false;
        f.editor.SetDocument(nullptr); f.editor.SetDocument(&other);
        f.editor.SetPublisher([&](const auto&,const auto&,const auto&,QString*) { unexpected=true; return true; });
        QVERIFY(f.editor.IsActive()); QVERIFY(f.editor.Cancel()); QVERIFY(!unexpected); QVERIFY(f.runtime!=preview); QVERIFY(!other.CanUndoEdit());
    }
};
QTEST_GUILESS_MAIN(MaterialEditControllerTests)
#include "material_edit_controller_tests.moc"
