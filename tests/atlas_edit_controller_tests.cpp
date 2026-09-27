#include "geometry_object_pairs_document_fixture.h"
#include "live2dstruct/catlaseditcontroller.h"
#include <QtTest>
#include <QBuffer>
#include <QImage>
#include <QPointer>
#include <memory>
#include <stdexcept>

using namespace GeometryPairsData;
namespace {
QByteArray png(QColor color)
{
    QImage image(64, 32, QImage::Format_RGBA8888); image.fill(color);
    QBuffer buffer; buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return buffer.data();
}
bool initialize(CModelDocument& document)
{
    if (!load(document)) return false;
    CModelMaterialState materials; if (!document.CaptureDrawableMaterials(materials)) return false;
    CTextureAssets::Catalog sources;
    for (const auto& material : materials) if (material.textureIndex >= 0 && !sources.contains(material.textureIndex)) {
        CTextureAssets::Source source;
        if (!CTextureAssets::MakeSource(QString("/tmp/atlas-controller-source-%1.png").arg(material.textureIndex), png(QColor(12, 91, 73, 21)), source)) return false;
        sources.insert(material.textureIndex, source);
    }
    return document.InitializeTextureWorkspace(sources);
}
CModelAtlasState changed(CModelDocument& document)
{
    CModelAtlasState state; document.CaptureAtlasState(state);
    state.materials[0].uvs[0] += .0125f;
    state.workspace.atlases.first().png = png(QColor(71, 39, 142, 91)); return state;
}
struct Fixture {
    CModelDocument document;
    CAtlasEditController controller;
    CModelAtlasState runtime;
    QSet<QString> ids;
    int publications = 0;
    bool reject = false, throwing = false;
    bool Initialize()
    {
        if (!initialize(document) || !document.CaptureAtlasState(runtime)) return false;
        controller.SetDocument(&document);
        controller.SetPublisher([this](const auto& before, const auto& after, const auto& changedIds, QString* error) {
            ++publications;
            if (throwing) throw std::runtime_error("controlled preparation failure");
            if (reject) { if (error) *error = "GPU preparation rejected"; return false; }
            CModelAtlasState expected, result;
            if (!before.CaptureAtlasState(expected, error) || !after.CaptureAtlasState(result, error)) return false;
            if (expected != runtime) { if (error) *error = "runtime changed"; return false; }
            runtime = std::move(result); ids = changedIds; return true;
        });
        return true;
    }
    CModelAtlasState State() const { CModelAtlasState state; document.CaptureAtlasState(state); return state; }
};
}
class AtlasEditControllerTests : public QObject
{
    Q_OBJECT
private slots:
    void deletingAndRecreatingAnEmptyGeneratedSlotUsesOneHistory_data()
    {
        QTest::addColumn<bool>("cancel"); QTest::newRow("commit-and-history") << false; QTest::newRow("cancel-preview") << true;
    }
    void deletingAndRecreatingAnEmptyGeneratedSlotUsesOneHistory()
    {
        QFETCH(bool, cancel); Fixture f; QVERIFY(f.Initialize()); auto a = f.State();
        const int slot = a.workspace.atlases.isEmpty() ? 0 : a.workspace.atlases.lastKey() + 1;
        a.workspace.atlases.insert(slot, {"atlas-A", "图集 A", -1, {64, 32}, png(Qt::red)});
        QVERIFY(f.controller.Submit(a)); f.document.ClearEditHistory(); const auto revision = f.document.Revision();
        const auto sources = f.document.TextureSources(); QVERIFY(f.controller.Begin());
        auto removed = a; removed.workspace.atlases.remove(slot); QVERIFY(f.controller.Preview(removed)); QCOMPARE(f.runtime, removed);
        QCOMPARE(f.State(), a); QVERIFY(!f.document.CanUndoEdit());
        auto b = removed; b.workspace.atlases.insert(slot, {"atlas-B", "图集 B", -1, {64, 32}, png(Qt::blue)});
        QString error; QVERIFY2(f.controller.Preview(b, &error), qPrintable(error)); QCOMPARE(f.runtime, b);
        QCOMPARE(f.State(), a); QCOMPARE(f.document.Revision(), revision); QVERIFY(!f.document.CanUndoEdit());
        QVERIFY(f.ids.isEmpty());
        if (cancel) { QVERIFY(f.controller.Cancel()); QCOMPARE(f.runtime, a); QCOMPARE(f.State(), a); QVERIFY(!f.document.CanUndoEdit()); }
        else {
            QVERIFY2(f.controller.Commit(b, &error), qPrintable(error)); QCOMPARE(f.State(), b); QCOMPARE(f.runtime, b);
            CModelEdit edit; QVERIFY(f.controller.History(true, edit, &error)); QCOMPARE(f.State(), a); QCOMPARE(f.runtime, a);
            QVERIFY(!f.document.CanUndoEdit()); QVERIFY(f.document.CanRedoEdit());
            QVERIFY(f.controller.History(false, edit, &error)); QCOMPARE(f.State(), b); QCOMPARE(f.runtime, b); QVERIFY(!f.document.CanRedoEdit());
        }
        QCOMPARE(f.document.TextureSources(), sources);
    }
    void pixelsAndMaterialsCommitTogetherOnce()
    {
        Fixture f; QVERIFY(f.Initialize()); const auto original = f.State(); const auto revision = f.document.Revision();
        QVERIFY(f.controller.Begin()); auto next = changed(f.document);
        for (int step = 0; step < 4; ++step) {
            next.materials[0].uvs[0] += .01f; QVERIFY(f.controller.Preview(next)); QCOMPARE(f.runtime, next);
            QCOMPARE(f.State(), original); QCOMPARE(f.document.Revision(), revision); QVERIFY(!f.document.CanUndoEdit());
        }
        QCOMPARE(f.ids, QSet<QString>{next.materials[0].id}); QVERIFY(f.controller.Commit(next)); QCOMPARE(f.State(), next); QVERIFY(!f.controller.IsActive());
        CModelEdit edit; QVERIFY(f.controller.History(true, edit)); QCOMPARE(edit.field, CModelEdit::AtlasLayout); QCOMPARE(f.runtime, original); QCOMPARE(f.State(), original);
        QVERIFY(!f.document.CanUndoEdit()); QVERIFY(f.controller.History(false, edit)); QCOMPARE(f.runtime, next); QCOMPARE(f.State(), next);
    }
    void singleTextureSubmitUsesTheSameHistoryAndFailureLeavesNoDraft()
    {
        Fixture f; QVERIFY(f.Initialize()); const auto original = f.State(); auto next = original;
        next.workspace.atlases.first().png = png(Qt::red); f.reject = true; QVERIFY(!f.controller.Submit(next));
        QVERIFY(!f.controller.IsActive()); QCOMPARE(f.State(), original); QCOMPARE(f.runtime, original); QVERIFY(!f.document.CanUndoEdit());
        f.reject = false; QVERIFY(f.controller.Submit(next)); QVERIFY(f.ids.isEmpty()); auto third = next; third.materials[0].uvs[0] += .1f;
        QVERIFY(f.controller.Submit(third)); QCOMPARE(f.runtime, third);
        CModelEdit edit; QVERIFY(f.controller.History(true, edit)); QCOMPARE(f.runtime, next);
        QVERIFY(f.controller.History(true, edit)); QCOMPARE(f.runtime, original); QVERIFY(!f.document.CanUndoEdit());
        QVERIFY(f.controller.History(false, edit)); QCOMPARE(f.runtime, next);
    }
    void failuresPreserveDraftAndBothRuntimeParts_data()
    {
        QTest::addColumn<bool>("exception"); QTest::newRow("rejection") << false; QTest::newRow("exception") << true;
    }
    void failuresPreserveDraftAndBothRuntimeParts()
    {
        QFETCH(bool, exception); Fixture f; QVERIFY(f.Initialize()); const auto original = f.State();
        QVERIFY(f.controller.Begin()); const auto next = changed(f.document); QVERIFY(f.controller.Preview(next));
        auto third = next; third.workspace.atlases.first().png = png(Qt::blue); third.materials[1].uvs[1] += .01f;
        f.reject = !exception; f.throwing = exception; QString error;
        QVERIFY(!f.controller.Preview(third, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(f.runtime, next);
        QVERIFY(!f.controller.Commit(third)); QVERIFY(!f.controller.Cancel()); QVERIFY(f.controller.IsActive());
        QCOMPARE(f.runtime, next); QCOMPARE(f.State(), original); QVERIFY(!f.document.CanUndoEdit());
        f.reject = f.throwing = false; QVERIFY(f.controller.Cancel()); QCOMPARE(f.runtime, original); QVERIFY(!f.controller.IsActive());
    }
    void failedHistoryPreservesOutputAndCursor()
    {
        Fixture f; QVERIFY(f.Initialize()); const auto next = changed(f.document); QVERIFY(f.controller.Submit(next));
        const auto revision = f.document.Revision(); f.reject = true; CModelEdit edit; edit.name = "sentinel";
        QVERIFY(!f.controller.History(true, edit)); QCOMPARE(edit.name, QString("sentinel")); QCOMPARE(f.document.Revision(), revision);
        QCOMPARE(f.State(), next); QCOMPARE(f.runtime, next); QVERIFY(f.document.CanUndoEdit()); QVERIFY(!f.document.CanRedoEdit());
    }
    void returnToOriginIsGuardedAndDoesNotCreateHistory()
    {
        Fixture f; QVERIFY(f.Initialize()); const auto original = f.State(); const auto revision = f.document.Revision();
        QVERIFY(f.controller.Begin()); QVERIFY(f.controller.Preview(changed(f.document))); QVERIFY(f.controller.Preview(original));
        const int calls = f.publications; f.reject = true; QVERIFY(!f.controller.Commit(original)); QCOMPARE(f.publications, calls + 1); QVERIFY(f.controller.IsActive());
        f.reject = false; QVERIFY(f.controller.Commit(original)); QCOMPARE(f.document.Revision(), revision); QVERIFY(!f.document.CanUndoEdit());
    }
    void partialDuplicateAndStaleDraftsNeverPublish()
    {
        Fixture f; QVERIFY(f.Initialize()); QVERIFY(f.controller.Begin()); auto next = changed(f.document); const int calls = f.publications;
        auto invalid = next; invalid.materials.removeLast(); QVERIFY(!f.controller.Preview(invalid));
        invalid = next; invalid.materials[1] = invalid.materials[0]; QVERIFY(!f.controller.Commit(invalid)); QCOMPARE(f.publications, calls);
        f.document.SetLocked("artmesh", next.materials[0].id, true); QVERIFY(!f.controller.Preview(next)); QVERIFY(!f.controller.Cancel()); QCOMPARE(f.publications, calls);
    }
    void metadataAndPixelNoopKeepExactSourceIdentity()
    {
        Fixture f; QVERIFY(f.Initialize()); const auto original = f.State(); const auto sources = f.document.TextureSources();
        auto renamed = original; renamed.workspace.atlases.first().name = QStringLiteral("头部素材");
        QVERIFY(f.controller.Submit(renamed)); QVERIFY(f.ids.isEmpty()); QCOMPARE(f.document.TextureSources(), sources);
        CModelEdit edit; QVERIFY(f.controller.History(true, edit)); QCOMPARE(f.State(), original); QCOMPARE(f.document.TextureSources(), sources);
    }
    void publisherDestructionIsSafe_data()
    {
        QTest::addColumn<int>("phase"); QTest::addColumn<bool>("owningDocument");
        for (int phase = 0; phase < 6; ++phase) for (bool owner : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(phase).arg(owner))) << phase << owner;
    }
    void publisherDestructionIsSafe()
    {
        QFETCH(int, phase); QFETCH(bool, owningDocument);
        struct Owner : QObject { CModelDocument document; CAtlasEditController controller; };
        CModelDocument external; QVERIFY(initialize(external)); auto standalone = std::make_unique<CAtlasEditController>();
        QPointer<Owner> owner = owningDocument ? new Owner : nullptr; if (owner) QVERIFY(initialize(owner->document));
        auto* controller = owner ? &owner->controller : standalone.get(); auto* document = owner ? &owner->document : &external;
        const auto next = changed(*document); bool destroy = phase == 0 || phase == 5;
        controller->SetDocument(document); controller->SetPublisher([&](const auto&, const auto&, const auto&, QString*) {
            if (destroy) { if (owningDocument) delete owner.data(); else standalone.reset(); } return true;
        });
        QString error; CModelEdit edit;
        if (phase == 0) QVERIFY(!controller->Begin(&error));
        else if (phase == 5) QVERIFY(!controller->Submit(next, &error));
        else {
            QVERIFY(controller->Begin()); if (phase == 4) QVERIFY(controller->Commit(next)); else if (phase >= 2) QVERIFY(controller->Preview(next));
            destroy = true;
            if (phase == 1) QVERIFY(!controller->Preview(next, &error));
            if (phase == 2) QVERIFY(!controller->Commit(next, &error));
            if (phase == 3) QVERIFY(!controller->Cancel(&error));
            if (phase == 4) QVERIFY(!controller->History(true, edit, &error));
        }
        QVERIFY(!error.isEmpty()); if (owningDocument) QVERIFY(!owner); else QVERIFY(!standalone);
    }
    void reentryAndActiveRebindingAreRejected()
    {
        Fixture f; QVERIFY(f.Initialize()); CModelDocument other; QVERIFY(initialize(other)); bool nested = true;
        f.controller.SetPublisher([&](const auto&, const auto&, const auto&, QString*) {
            nested = f.controller.Submit(f.State()); f.controller.SetDocument(&other); f.controller.SetPublisher({}); return true;
        });
        QVERIFY(f.controller.Begin()); QVERIFY(!nested); f.controller.SetDocument(&other); f.controller.SetPublisher({});
        QVERIFY(f.controller.Cancel()); QVERIFY(!f.controller.IsActive()); QVERIFY(!nested);
    }
};
QTEST_GUILESS_MAIN(AtlasEditControllerTests)
#include "atlas_edit_controller_tests.moc"
