#include "ui/cpsdimportdialog.h"
#include "cmo_artwork_source_fixture.h"
#include "cmo3/ccmo3project.h"
#include "ui/cartworksourceview.h"
#include "ui/theme.h"
#include "std/cpngimage.h"
#include <QtTest>
#include <QtEndian>
#include <QBuffer>
#include <QCheckBox>
#include <QColorSpace>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPointer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTreeView>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextLayout>
#include <stdexcept>
using namespace live2dstruct;
using Dialog = CPsdImportDialog;
using Planner = CArtworkUpdatePlanner;
using Artwork = CModelArtwork;
namespace {
QByteArray hash(const QByteArray& value) { return QCryptographicHash::hash(value, QCryptographicHash::Sha256); }
QString derivedIdentity(const CArtworkSourceImport::SourceCandidate& source, Artwork::ColorPolicy policy, const QTransform& mapping, bool versioned)
{
    QByteArray bytes; QDataStream stream(&bytes, QIODevice::WriteOnly); stream.setVersion(QDataStream::Qt_6_0);
    stream << source.revision.id << source.revision.familyId << source.revision.sourceDigest << qint32(policy)
        << mapping.m11() << mapping.m12() << mapping.m21() << mapping.m22() << mapping.dx() << mapping.dy();
    if (versioned && source.kind == Artwork::SourceKind::Cmo3) stream << source.revision.sourceDocumentId << CArtworkSourceImport::CmoInterpretationVersion;
    else if (versioned) stream << CArtworkSourceImport::PsdInterpretationVersion;
    return QStringLiteral("revision:") + QString::fromLatin1(hash(bytes).toHex());
}
QByteArray u16(quint16 value) { char data[2]; qToBigEndian(value, data); return QByteArray(data, 2); }
QByteArray u32(quint32 value) { char data[4]; qToBigEndian(value, data); return QByteArray(data, 4); }
QByteArray block(const QByteArray& value) { return u32(value.size()) + value; }
QByteArray tag(const QByteArray& key, const QByteArray& value) { return "8BIM" + key + block(value); }
struct Layer { QString name = "A"; int id = 17; bool pixels = true; QRect bounds{-3, -5, 3, 2}; quint8 opacity = 128; };
QByteArray psd(const QVector<Layer>& layers = {Layer()}, QSize canvas = {8, 6}, const QByteArray& icc = {})
{
    QByteArray records, data;
    for (const auto& layer : layers) {
        records += u32(layer.bounds.y()) + u32(layer.bounds.x()) + u32(layer.bounds.y() + layer.bounds.height()) + u32(layer.bounds.x() + layer.bounds.width());
        const int count = layer.pixels ? 4 : 0, area = layer.bounds.width() * layer.bounds.height(); records += u16(count);
        for (int c = 0; c < count; ++c) {
            records += u16(c == 3 ? -1 : c) + u32(area + 2); data += u16(0);
            for (int pixel = 0; pixel < area; ++pixel) data += char(c == 3 ? (pixel * 51) % 256 : (64 + c * 35 + pixel * 19) % 256);
        }
        records += "8BIMnorm"; records += char(layer.opacity); records += QByteArray(3, 0);
        QByteArray name = u32(layer.name.size()); for (QChar c : layer.name) name += u16(c.unicode());
        QByteArray extra = u32(0) + u32(0) + QByteArray(4, 0) + tag("luni", name);
        if (layer.id >= 0) extra += tag("lyid", u32(layer.id));
        records += block(extra);
    }
    QByteArray info = u16(layers.size()) + records + data; if (info.size() % 2) info += char(0);
    QByteArray resources;
    if (!icc.isEmpty()) resources = QByteArray("8BIM") + u16(1039) + QByteArray(2, 0) + block(icc) + QByteArray(icc.size() % 2, 0);
    return QByteArray("8BPS") + u16(1) + QByteArray(6, 0) + u16(3) + u32(canvas.height()) + u32(canvas.width()) + u16(8) + u16(3) +
        u32(0) + block(resources) + block(block(info) + u32(0)) + u16(0) + QByteArray(canvas.width() * canvas.height() * 3, 0);
}
CArtworkSourceImport::SourceCandidate incoming(QVector<Layer> layers = {Layer()}, QString id = "revision", QSize canvas = {8, 6}, QByteArray icc = {})
{
    CPsdSnapshot snapshot; snapshot.bytes = psd(layers, canvas, icc); snapshot.canonicalPath = "/not-on-disk/source.psd";
    CArtworkSourceImport::SourceCandidate result; QString error;
    if (!CArtworkSourceImport::Prepare(snapshot, "family", id, Artwork::ColorPolicy::PreserveValues, {}, result, &error)) qFatal("%s", qPrintable(error));
    return result;
}
CArtworkSourceImport::SourceCandidate cmoIncoming(const QString& documentId = "d0-guid")
{
    cmo3::CCmo3Project project; QString error;
    if (!cmo3::CCmo3Project::OpenBytes(cmo_artwork_source_fixture::archive(), "/not-on-disk/source.cmo3", project, &error)) qFatal("%s", qPrintable(error));
    CArtworkSourceImport::SourceCandidate result;
    if (!CArtworkSourceImport::Prepare(project, documentId, "family", "cmo-revision", Artwork::ColorPolicy::PreserveValues, {}, result, &error)) qFatal("%s", qPrintable(error));
    return result;
}
QByteArray png(QImage image) { QBuffer output; output.open(QIODevice::WriteOnly); image.save(&output, "PNG"); return output.data(); }
Dialog::Context context(CArtworkSourceImport::SourceCandidate source = incoming())
{
    Dialog::Context result; result.familyName = "原稿"; result.incoming = source; result.selectedMeshIds = {"A"};
    auto& s = result.snapshot; s.sourceDigest = hash("model source"); s.documentRevision = 7;
    QImage image(16, 12, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) image.setPixelColor(x, y, QColor(50 + x * 8, 70 + y * 10, 180, (x + y) * 9 % 256));
    CTextureAssets::Source texture; CTextureAssets::MakeSource("/not-on-disk/atlas.png", png(image), texture); s.sources.insert(0, texture);
    CTextureWorkspace::Initialize(s.sources, {}, s.baseline.workspace); result.atlasImages.insert(0, image);
    for (const auto& id : {QString("A"), QString("B")}) {
        CAtlasRepackPlanner::Mesh mesh; mesh.material = {id, 0, {.125f, .25f, .25f, .25f, .25f, .5f, .125f, .5f}}; mesh.triangles = {0, 1, 2, 0, 2, 3};
        s.meshes.append(mesh); s.baseline.materials.append(mesh.material);
    }
    return result;
}
Dialog::Context boundContext()
{
    auto c = context(); Planner::Request request; request.incoming = c.incoming; request.familyName = c.familyName;
    request.decisions = {{"A", Planner::Action::Associate, c.incoming.revision.layers.first().id, QTransform::fromTranslate(-5, -8), true, true}};
    Planner::Plan plan; QString error; if (!Planner::Build(c.snapshot, request, plan, &error)) qFatal("%s", qPrintable(error));
    c.snapshot.baseline = plan.state; ++c.snapshot.documentRevision; c.atlasImages = plan.images;
    for (auto& mesh : c.snapshot.meshes) for (const auto& material : plan.state.materials) if (mesh.material.id == material.id) mesh.material = material;
    return c;
}
void connectHandlers(Dialog& d, int* previews = nullptr, int* saves = nullptr, int* cancels = nullptr)
{
    d.SetPreviewHandler([previews](const auto&, QString*) { if (previews) ++*previews; return true; });
    d.SetSubmissionHandler([saves](const auto&, QString*) { if (saves) ++*saves; return true; });
    d.SetCancellationHandler([cancels](QString*) { if (cancels) ++*cancels; return true; });
}
bool settled(Dialog& d)
{
    QElapsedTimer timer; timer.start(); while (d.IsBusy() && timer.elapsed() < 15000) { QCoreApplication::processEvents(); QTest::qWait(5); }
    QCoreApplication::processEvents(); return d.IsReady() && !d.IsBusy();
}
template<class T> T* control(Dialog& d, const char* name) { auto* result = d.findChild<T*>(QString::fromLatin1(name)); if (!result) qFatal("Missing control: %s", name); return result; }
void click(Dialog& d, const char* name) { control<QPushButton>(d, name)->click(); }
void action(Dialog& d, Planner::Action value) { auto* box = control<QComboBox>(d, "psdAction"); box->setCurrentIndex(box->findData(int(value))); }
void confirm(Dialog& d, bool value = true) { control<QCheckBox>(d, "psdConfirmAssociation")->setChecked(value); }
bool associate(Dialog& d, bool confirmed = true)
{
    action(d, Planner::Action::Associate); confirm(d, confirmed); click(d, "psdApplySelection"); return settled(d);
}
void selectMesh(Dialog& d, const QStringList& ids)
{
    auto* view = control<QListView>(d, "psdMeshes"); QItemSelection selected;
    for (int row = 0; row < view->model()->rowCount(); ++row) if (ids.contains(view->model()->index(row, 0).data(Qt::UserRole).toString())) selected.select(view->model()->index(row, 0), view->model()->index(row, 0));
    view->selectionModel()->select(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}
Planner::Decision decision(Dialog& d, const QString& id)
{ for (const auto& value : d.DraftRequest().decisions) if (value.meshId == id) return value; return {}; }
}
class PsdImportDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void meshDisplayNamesPreserveIdentityAndDoNotConfirmSourceNames()
    {
        auto source = context(); source.meshDisplayNames = {{"A", QStringLiteral("<b>眉毛</b>")}, {"B", QStringLiteral("眼睛")}};
        Dialog dialog(source); QVERIFY(settled(dialog));
        auto* view = control<QListView>(dialog, "psdMeshes");
        QVERIFY(view); auto* model = view->model(); QCOMPARE(model->index(0, 0).data(Qt::UserRole).toString(), QString("A"));
        QVERIFY(model->index(0, 0).data().toString().contains(QStringLiteral("<b>眉毛</b> · A")));
        QVERIFY(model->index(0, 0).data(Qt::ToolTipRole).toString().contains("&lt;b&gt;"));
        auto* search = control<QLineEdit>(dialog, "psdMeshSearch"); search->setText(QStringLiteral("眉毛"));
        QCOMPARE(model->rowCount(), 1); QCOMPARE(model->index(0, 0).data(Qt::UserRole).toString(), QString("A"));
        search->setText("A"); QCOMPARE(model->rowCount(), 1); QCOMPARE(model->index(0, 0).data(Qt::UserRole).toString(), QString("A"));
        search->setText(QStringLiteral("眼睛")); QCOMPARE(model->rowCount(), 1); QCOMPARE(model->index(0, 0).data(Qt::UserRole).toString(), QString("B"));
        for (const auto& decision : dialog.DraftRequest().decisions) QVERIFY(!decision.confirmed);
        QCOMPARE(dialog.DraftRequest().incoming.revision.layers.first().name, source.incoming.revision.layers.first().name);
    }
    void initialReadOnlyNameCandidateNeedsConfirmation()
    {
        Dialog d(context()); int previews = 0; connectHandlers(d, &previews); QVERIFY(settled(d)); QVERIFY2(d.IsValid(), qPrintable(d.Error()));
        QCOMPARE(previews, 0); QVERIFY(d.Result().state.artwork.bindings.isEmpty()); QCOMPARE(decision(d, "A").action, Planner::Action::Keep);
        QVERIFY(control<QLabel>(d, "psdSelectionInfo")->text().contains("名称相同"));
        QVERIFY(associate(d, false)); QCOMPARE(previews, 1); QCOMPARE(d.Result().unconfirmed, QSet<QString>{"A"});
        d.accept(); QVERIFY(d.Error().contains("确认")); confirm(d); click(d, "psdApplySelection"); QVERIFY(settled(d)); QVERIFY(d.Result().unconfirmed.isEmpty());
    }
    void sourceOnlyDoesNotCreateMeshOrAlterMaterial()
    {
        auto c = context(); Dialog d(c); int saves = 0; connectHandlers(d, nullptr, &saves); QVERIFY(settled(d)); click(d, "psdSaveSourceOnly"); QVERIFY(settled(d));
        QCOMPARE(saves, 1); QVERIFY(d.Result().state.artwork.bindings.isEmpty()); QCOMPARE(d.Result().state.materials, c.snapshot.baseline.materials);
        QCOMPARE(d.Result().state.workspace, c.snapshot.baseline.workspace); QCOMPARE(d.Result().state.artwork.revisions.size(), 1); QCOMPARE(d.result(), int(QDialog::Accepted));
    }
    void sourceOnlyAfterPreviewRestoresModelPlanInOneSubmission()
    {
        auto c = context(); Dialog d(c); int saves = 0; connectHandlers(d, nullptr, &saves); QVERIFY(settled(d)); QVERIFY(associate(d)); QVERIFY(!d.Result().state.artwork.bindings.isEmpty());
        click(d, "psdSaveSourceOnly"); QVERIFY(settled(d)); QCOMPARE(saves, 1); QCOMPARE(d.Result().state.materials, c.snapshot.baseline.materials); QVERIFY(d.Result().state.artwork.bindings.isEmpty());
    }
    void stableIdUpdateAndMissingLayerKeepAreExplicit()
    {
        auto c = boundContext(); c.incoming = incoming({Layer{"renamed", 17}}, "new"); Dialog d(c); int previews = 0; connectHandlers(d, &previews); QVERIFY(settled(d));
        QCOMPARE(decision(d, "A").action, Planner::Action::Keep); QCOMPARE(previews, 0); QVERIFY(control<QPushButton>(d, "psdConfirmKnown")->isEnabled());
        click(d, "psdConfirmKnown"); QVERIFY(settled(d)); QCOMPARE(decision(d, "A").action, Planner::Action::Update); QVERIFY(d.Result().unconfirmed.isEmpty());
        QCOMPARE(d.Result().state.artwork.bindings["A"].familyCoordinates, c.snapshot.baseline.artwork.bindings["A"].familyCoordinates);
        c.incoming = incoming({}, "missing"); Dialog missing(c); connectHandlers(missing); QVERIFY(settled(missing));
        QVERIFY(!control<QPushButton>(missing, "psdConfirmKnown")->isEnabled()); QCOMPARE(missing.Result().state.materials, c.snapshot.baseline.materials);
        QVERIFY(control<QLabel>(missing, "psdSelectionInfo")->text().contains("保留旧素材"));
    }
    void duplicateIdsNeverAutoConfirm()
    {
        auto c = boundContext(); c.incoming = incoming({Layer{"A", 17}, Layer{"A", 17}}, "duplicates"); Dialog d(c); connectHandlers(d); QVERIFY(settled(d));
        QVERIFY(!control<QPushButton>(d, "psdConfirmKnown")->isEnabled()); QVERIFY(d.Result().unconfirmed.isEmpty()); QCOMPARE(decision(d, "A").action, Planner::Action::Keep);
        action(d, Planner::Action::Update); click(d, "psdApplySelection"); QVERIFY(settled(d)); QCOMPARE(d.Result().unconfirmed, QSet<QString>{"A"});
    }
    void unsupportedLayerCanBeSavedAsSourceOnly()
    {
        auto c = context(incoming({Layer{"unsupported", 17, false}})); Dialog d(c); int saved = 0; connectHandlers(d, nullptr, &saved); QVERIFY(settled(d));
        action(d, Planner::Action::Associate); click(d, "psdApplySelection"); QVERIFY(!d.IsBusy()); QVERIFY(d.Error().contains("栅格图层"));
        click(d, "psdDiscardInput"); QVERIFY(settled(d)); click(d, "psdSaveSourceOnly"); QVERIFY(settled(d)); QCOMPARE(saved, 1);
    }
    void proposalUsesAtlasPixelsAndNegativeLayerBounds()
    {
        Dialog d(context()); connectHandlers(d); QVERIFY(settled(d)); click(d, "psdProposeAlignment"); auto* view = d.findChild<CArtworkSourceView*>();
        QCOMPARE(view->Mapping().map(QPointF(1, 2)), QPointF(-3, -5)); QCOMPARE(view->Mapping().map(QPointF(5, 7)), QPointF(0, -3));
        QVERIFY(!control<QCheckBox>(d, "psdConfirmAssociation")->isChecked()); QVERIFY(associate(d));
        QCOMPARE(d.Result().state.artwork.bindings["A"].familyCoordinates.first(), QPointF(-2.25, -4.6));
    }
    void reflectedMappingAndIndependentMultipleMeshes()
    {
        Dialog d(context()); connectHandlers(d); QVERIFY(settled(d)); control<QDoubleSpinBox>(d, "psdScaleX")->setValue(-100); click(d, "psdApplyTransform"); QVERIFY(associate(d));
        const auto first = d.Result().state.artwork.bindings["A"].familyCoordinates;
        selectMesh(d, {"B"}); QVERIFY(settled(d)); QVERIFY(associate(d)); QCOMPARE(d.Result().state.artwork.bindings["A"].familyCoordinates, first);
        QVERIFY(d.Result().state.artwork.bindings["B"].familyCoordinates != first);
        selectMesh(d, {"A", "B"}); QVERIFY(settled(d)); QVERIFY(control<QLabel>(d, "psdAlignmentInfo")->text().contains("不同的映射"));
        action(d, Planner::Action::Associate); click(d, "psdApplySelection"); QVERIFY(!d.IsBusy()); QVERIFY(d.Error().contains("逐对象"));
    }
    void pendingInputsCannotSaveOrSwitchObjects()
    {
        Dialog d(context()); int saves = 0; connectHandlers(d, nullptr, &saves); QVERIFY(settled(d)); control<QDoubleSpinBox>(d, "psdTranslateX")->setValue(6);
        selectMesh(d, {"B"}); QVERIFY(d.Error().contains("放弃")); auto* selected = control<QListView>(d, "psdMeshes"); QCOMPARE(selected->selectionModel()->selectedRows().first().data(Qt::UserRole).toString(), QString("A"));
        d.accept(); QCOMPARE(saves, 0); click(d, "psdDiscardInput"); QVERIFY(settled(d)); QCOMPARE(control<QDoubleSpinBox>(d, "psdTranslateX")->value(), 0.);
        control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(2); d.accept(); QCOMPARE(saves, 0); QVERIFY(!control<QPushButton>(d, "psdSaveSourceOnly")->isEnabled());
    }
    void existingRevisionMappingCreatesImmutableRevisionAndCanRevertRecipe()
    {
        auto c = boundContext(); Dialog d(c); connectHandlers(d); QVERIFY(settled(d)); control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(2); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        const auto changed = d.DraftRequest().incoming.revision.id; QVERIFY(changed != c.incoming.revision.id); QCOMPARE(d.Result().state.artwork.revisions[c.incoming.revision.id], c.incoming.revision);
        QCOMPARE(d.Result().state.artwork.revisions[changed].documentToFamily.dx(), 2.); QCOMPARE(d.Result().state.materials, c.snapshot.baseline.materials);
        control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(-2); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision.id, c.incoming.revision.id);
        QCOMPARE(d.Result().state.artwork.revisions.size(), 1);
    }
    void colorReprocessesFrozenBytesAndKeepsAlpha()
    {
        const auto source = incoming({Layer()}, "revision", {8, 6}, QColorSpace(QColorSpace::SRgbLinear).iccProfile()); Dialog d(context(source)); connectHandlers(d); QVERIFY(settled(d));
        auto* color = control<QComboBox>(d, "psdColorPolicy"); color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb))); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error())); QCOMPARE(d.DraftRequest().incoming.revision.sourceBytes, source.revision.sourceBytes);
        QImage before, after; QVERIFY(CPngImage::Decode(source.images.first().png, before)); QVERIFY(CPngImage::Decode(d.DraftRequest().incoming.images.first().png, after));
        QVERIFY(after.pixelColor(0, 0).red() > before.pixelColor(0, 0).red()); QCOMPARE(after.pixelColor(0, 0).alpha(), before.pixelColor(0, 0).alpha());
        color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::PreserveValues))); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision.id, source.revision.id);
    }
    void cmoColorRecipeRetainsNativeDocumentAndFrozenSource()
    {
        const auto source = cmoIncoming("d1-guid"); const auto c = context(source); Dialog d(c); connectHandlers(d); QVERIFY(settled(d));
        auto* color = control<QComboBox>(d, "psdColorPolicy"); QVERIFY(color->isEnabled());
        color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb))); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error())); const auto accepted = d.DraftRequest().incoming;
        QCOMPARE(accepted.kind, Artwork::SourceKind::Cmo3); QCOMPARE(accepted.revision.sourceDocumentId, QString("d1-guid"));
        QCOMPARE(accepted.revision.sourceBytes, source.revision.sourceBytes); QCOMPARE(accepted.revision.sourceDigest, source.revision.sourceDigest);
        QCOMPARE(accepted.revision.layers.size(), source.revision.layers.size());
        QCOMPARE(accepted.revision.id, derivedIdentity(source, Artwork::ColorPolicy::ConvertToSRgb, {}, true));
        for (int i = 0; i < source.revision.layers.size(); ++i) {
            QCOMPARE(accepted.revision.layers[i].id, source.revision.layers[i].id);
            QCOMPARE(accepted.revision.layers[i].parentIndex, source.revision.layers[i].parentIndex);
        }
        QImage before, after; QVERIFY(CPngImage::Decode(source.images.first().png, before)); QVERIFY(CPngImage::Decode(accepted.images.first().png, after));
        QCOMPARE(after.size(), before.size()); for (int x = 0; x < before.width(); ++x) QCOMPARE(after.pixelColor(x, 0).alpha(), before.pixelColor(x, 0).alpha());
        control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(3); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QCOMPARE(d.DraftRequest().incoming.images, accepted.images);
        QCOMPARE(d.DraftRequest().incoming.revision.id, derivedIdentity(source, Artwork::ColorPolicy::ConvertToSRgb, QTransform::fromTranslate(3, 0), true));
        click(d, "psdUndo"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision, accepted.revision);
        click(d, "psdRedo"); QVERIFY(settled(d)); click(d, "psdSaveSourceOnly"); QVERIFY(settled(d));
        QCOMPARE(d.result(), int(QDialog::Accepted)); QCOMPARE(d.Result().state.materials, c.snapshot.baseline.materials);
        QCOMPARE(d.Result().state.artwork.bindings, c.snapshot.baseline.artwork.bindings);
    }
    void cmoMissingDocumentRecipeDoesNotFallBackToAnotherDocument()
    {
        auto source = cmoIncoming(); source.revision.sourceDocumentId = "missing-doc"; Dialog d(context(source)); connectHandlers(d); QVERIFY(settled(d));
        const auto before = d.Result(); auto* color = control<QComboBox>(d, "psdColorPolicy");
        color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb))); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QVERIFY(!d.Error().isEmpty()); QCOMPARE(d.Result().token, before.token); QCOMPARE(d.Result().state.artwork.revisions, before.state.artwork.revisions);
        QCOMPARE(d.DraftRequest().incoming.revision.sourceDocumentId, source.revision.sourceDocumentId);
        click(d, "psdDiscardInput"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision, source.revision);
    }
    void cmoMappingOnlyUsesPreparedPixelsAndVersionedDocumentIdentity()
    {
        auto source = cmoIncoming(); source.revision.sourceBytes = "saved CMO source with prepared pixels"; source.revision.sourceDigest = hash(source.revision.sourceBytes);
        Dialog d(context(source)); connectHandlers(d); QVERIFY(settled(d));
        control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(2); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error()));
        const auto identity = derivedIdentity(source, source.revision.colorPolicy, QTransform::fromTranslate(2, 0), true);
        QCOMPARE(d.DraftRequest().incoming.revision.id, identity); QCOMPARE(d.DraftRequest().incoming.images, source.images);
        auto other = source; other.revision.sourceDocumentId = "d1-guid";
        QVERIFY(identity != derivedIdentity(other, other.revision.colorPolicy, QTransform::fromTranslate(2, 0), true));
    }
    void mappingOnlyUsesAcceptedPixelsWithoutReadingPsdAgain()
    {
        auto c = context(); c.incoming.revision.sourceBytes = "saved source with prepared pixels"; c.incoming.revision.sourceDigest = hash(c.incoming.revision.sourceBytes);
        Dialog d(c); connectHandlers(d); QVERIFY(settled(d)); const auto images = d.DraftRequest().incoming.images;
        control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(2); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error()));
        const auto expected = derivedIdentity(c.incoming, c.incoming.revision.colorPolicy, QTransform::fromTranslate(2, 0), true);
        QCOMPARE(d.DraftRequest().incoming.revision.id, expected); QCOMPARE(d.DraftRequest().incoming.images, images);
        QVERIFY(expected != derivedIdentity(c.incoming, c.incoming.revision.colorPolicy, QTransform::fromTranslate(2, 0), false));
        QVERIFY(associate(d)); click(d, "psdPreview"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error()));
        QCOMPARE(d.DraftRequest().incoming.images, images); QCOMPARE(d.DraftRequest().incoming.revision.id, expected);
        click(d, "psdSaveSourceOnly"); QVERIFY(settled(d)); QCOMPARE(d.result(), int(QDialog::Accepted));
        QVERIFY(d.Result().state.artwork.bindings.isEmpty()); QCOMPARE(d.Result().state.artwork.revisions[expected].sourceBytes, c.incoming.revision.sourceBytes);
    }
    void acceptedColorRecipeSurvivesAllBuildConsumers()
    {
        auto c = boundContext(); c.incoming = incoming({Layer()}, "next", {8, 6}, QColorSpace(QColorSpace::SRgbLinear).iccProfile());
        Dialog d(c); connectHandlers(d); QVERIFY(settled(d));
        control<QComboBox>(d, "psdColorPolicy")->setCurrentIndex(control<QComboBox>(d, "psdColorPolicy")->findData(int(Artwork::ColorPolicy::ConvertToSRgb)));
        click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error()));
        const auto accepted = d.DraftRequest().incoming;
        QCOMPARE(accepted.revision.id, derivedIdentity(c.incoming, Artwork::ColorPolicy::ConvertToSRgb, {}, true));
        click(d, "psdConfirmKnown"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error()));
        QCOMPARE(d.DraftRequest().incoming.revision, accepted.revision); QCOMPARE(d.DraftRequest().incoming.images, accepted.images);
        action(d, Planner::Action::Keep); click(d, "psdApplySelection"); QVERIFY(settled(d));
        control<QLineEdit>(d, "psdFamilyName")->setText("accepted recipe"); click(d, "psdPreview"); QVERIFY(settled(d));
        QCOMPARE(d.DraftRequest().incoming.revision, accepted.revision); QCOMPARE(d.DraftRequest().incoming.images, accepted.images); QCOMPARE(d.DraftRequest().incoming.diagnostics, accepted.diagnostics);
        control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(3); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QCOMPARE(d.DraftRequest().incoming.images, accepted.images); QCOMPARE(d.DraftRequest().incoming.revision.id, derivedIdentity(c.incoming, Artwork::ColorPolicy::ConvertToSRgb, QTransform::fromTranslate(3, 0), true));
        click(d, "psdUndo"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision, accepted.revision);
        click(d, "psdRedo"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.images, accepted.images);
        click(d, "psdSaveSourceOnly"); QVERIFY(settled(d)); QCOMPARE(d.result(), int(QDialog::Accepted));
        QCOMPARE(d.Result().state.materials, c.snapshot.baseline.materials); QCOMPARE(d.Result().state.artwork.bindings, c.snapshot.baseline.artwork.bindings);
    }
    void frozenRecipeIgnoresBorrowedInputChanges()
    {
        auto c = context(incoming({Layer()}, "revision", {8, 6}, QColorSpace(QColorSpace::SRgbLinear).iccProfile()));
        QByteArray bytes = c.incoming.revision.sourceBytes, png = c.incoming.images.first().png; QString name = c.incoming.revision.layers[0].name;
        auto* sourceMemory = bytes.data(); auto* pixelMemory = png.data(); auto* nameMemory = name.data();
        c.incoming.revision.sourceBytes = QByteArray::fromRawData(sourceMemory, bytes.size()); c.incoming.images.first().png = QByteArray::fromRawData(pixelMemory, png.size());
        c.incoming.revision.layers[0].name = QString::fromRawData(nameMemory, name.size());
        Dialog d(c); connectHandlers(d); QVERIFY(settled(d)); const auto original = d.DraftRequest().incoming;
        sourceMemory[0] ^= 1; pixelMemory[0] ^= 1; nameMemory[0] = 'X';
        auto* color = control<QComboBox>(d, "psdColorPolicy"); color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb)));
        click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error())); const auto converted = d.DraftRequest().incoming;
        QCOMPARE(converted.revision.sourceBytes, original.revision.sourceBytes); QCOMPARE(converted.revision.layers[0].name, original.revision.layers[0].name);
        click(d, "psdPreview"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision, converted.revision); QCOMPARE(d.DraftRequest().incoming.images, converted.images);
        color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::PreserveValues))); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QCOMPARE(d.DraftRequest().incoming.revision, original.revision); QCOMPARE(d.DraftRequest().incoming.images, original.images);
    }
    void recipeFailuresAndHistoryBranchesRemainTransactional_data()
    {
        QTest::addColumn<int>("failure"); QTest::newRow("preview-rejected") << 0; QTest::newRow("preview-exception") << 1; QTest::newRow("canceled") << 2;
    }
    void recipeFailuresAndHistoryBranchesRemainTransactional()
    {
        QFETCH(int, failure); const auto source = incoming({Layer()}, "revision", {8, 6}, QColorSpace(QColorSpace::SRgbLinear).iccProfile());
        Dialog d(context(source)); connectHandlers(d); QVERIFY(settled(d)); const auto original = d.Result().token;
        if (failure == 0) d.SetPreviewHandler([](const auto&, QString* error) { *error = "reject recipe"; return false; });
        if (failure == 1) d.SetPreviewHandler([](const auto&, QString*) -> bool { throw std::runtime_error("recipe"); });
        auto* color = control<QComboBox>(d, "psdColorPolicy"); color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb)));
        click(d, "psdApplyDocumentAlignment"); if (failure == 2) click(d, "psdStop"); QVERIFY(settled(d));
        QCOMPARE(d.Result().token, original); QVERIFY(!d.Error().isEmpty());
        click(d, "psdDiscardInput"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision, source.revision);
        connectHandlers(d); color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb))); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error())); const auto converted = d.DraftRequest().incoming;
        click(d, "psdUndo"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision, source.revision);
        control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(2); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QVERIFY(!control<QPushButton>(d, "psdRedo")->isEnabled());
        color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb))); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QCOMPARE(d.DraftRequest().incoming.images, converted.images); QCOMPARE(d.DraftRequest().incoming.revision.id, derivedIdentity(source, Artwork::ColorPolicy::ConvertToSRgb, QTransform::fromTranslate(2, 0), true));
        const auto accepted = d.Result().token; d.SetSubmissionHandler([](const auto&, QString* error) { *error = "retry save"; return false; });
        click(d, "psdSaveSourceOnly"); QVERIFY(settled(d)); QCOMPARE(d.Result().token, accepted); QVERIFY(d.Error().contains("retry save"));
        int submitted = 0; connectHandlers(d, nullptr, &submitted); click(d, "psdSaveSourceOnly"); QVERIFY(settled(d)); QCOMPARE(submitted, 1);
    }
    void preparedRecipesRespectTheExistingHistoryLimit()
    {
        Dialog d(context(incoming({Layer()}, "revision", {8, 6}, QColorSpace(QColorSpace::SRgbLinear).iccProfile()))); connectHandlers(d); QVERIFY(settled(d));
        auto* color = control<QComboBox>(d, "psdColorPolicy"); color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb)));
        click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); const auto pixels = d.DraftRequest().incoming.images;
        for (int index = 0; index < 126; ++index) {
            control<QLineEdit>(d, "psdFamilyName")->setText(QString::number(index)); click(d, "psdPreview"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error()));
        }
        const auto before = d.Result().token; control<QLineEdit>(d, "psdFamilyName")->setText("overflow"); click(d, "psdPreview"); QVERIFY(settled(d));
        QVERIFY(d.Error().contains("上限")); QCOMPARE(d.Result().token, before);
        QVERIFY(control<QPushButton>(d, "psdDiscardInput")->isEnabled()); QVERIFY(control<QPushButton>(d, "psdPreview")->isEnabled());
        click(d, "psdDiscardInput"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.images, pixels);
        QCOMPARE(control<QLineEdit>(d, "psdFamilyName")->text(), QString("125")); QVERIFY(control<QPushButton>(d, "psdUndo")->isEnabled());
        click(d, "psdUndo"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error())); QCOMPARE(d.DraftRequest().incoming.images, pixels);
        QVERIFY(d.Result().token != before); QCOMPARE(control<QLineEdit>(d, "psdFamilyName")->text(), QString("124"));
    }
    void canvasChangeNeedsDocumentAlignmentEvenForSourceOnly()
    {
        auto c = boundContext(); c.incoming = incoming({Layer()}, "resized", {20, 10}); Dialog d(c); int saves = 0; connectHandlers(d, nullptr, &saves); QVERIFY(settled(d));
        click(d, "psdSaveSourceOnly"); QCOMPARE(saves, 0); QVERIFY(d.Error().contains("对齐")); control<QComboBox>(d, "psdDocumentAlignment")->setCurrentIndex(1);
        click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision.documentToFamily.map(QPointF(10, 5)), QPointF(4, 3));
        click(d, "psdSaveSourceOnly"); QVERIFY(settled(d)); QCOMPARE(saves, 1);
    }
    void undoRedoRestoresPlanNamesAndRecipe()
    {
        Dialog d(context()); int previews = 0; connectHandlers(d, &previews); QVERIFY(settled(d)); const auto initial = d.Result().token;
        control<QLineEdit>(d, "psdFamilyName")->setText("新来源名称"); click(d, "psdPreview"); QVERIFY(settled(d)); const auto named = d.Result().token; QVERIFY(initial != named);
        click(d, "psdUndo"); QVERIFY(settled(d)); QCOMPARE(d.Result().token, initial); QCOMPARE(control<QLineEdit>(d, "psdFamilyName")->text(), QString("原稿"));
        click(d, "psdRedo"); QVERIFY(settled(d)); QCOMPARE(d.Result().token, named); QCOMPARE(control<QLineEdit>(d, "psdFamilyName")->text(), QString("新来源名称")); QCOMPARE(previews, 3);
    }
    void previewFailureRetainsAcceptedPlanAndRetry()
    {
        Dialog d(context()); connectHandlers(d); QVERIFY(settled(d)); const auto initial = d.Result().token;
        d.SetPreviewHandler([](const auto&, QString* error) { *error = "rejected preview"; return false; }); QVERIFY(associate(d)); QCOMPARE(d.Result().token, initial); QVERIFY(d.Error().contains("rejected"));
        d.SetPreviewHandler([](const auto&, QString*) { return true; }); click(d, "psdApplySelection"); QVERIFY(settled(d)); QVERIFY(d.Result().token != initial);
    }
    void submissionCancelFailureAndExceptionsRemainRetryable()
    {
        Dialog d(context()); connectHandlers(d); QVERIFY(settled(d)); QVERIFY(associate(d));
        d.SetSubmissionHandler([](const auto&, QString*) -> bool { throw std::runtime_error("save"); }); d.accept(); QVERIFY(d.Error().contains("保存失败")); QVERIFY(d.IsValid());
        d.SetCancellationHandler([](QString* error) { *error = "cannot restore"; return false; }); d.reject(); QVERIFY(d.Error().contains("cannot restore")); QVERIFY(!d.IsBusy());
        d.SetCancellationHandler([](QString*) { return true; }); d.reject(); QCOMPARE(d.result(), int(QDialog::Rejected));
    }
    void callbackDestructionAndReentryAreSafe_data()
    { QTest::addColumn<int>("mode"); for (int mode : {0, 1, 2, 3}) QTest::newRow(qPrintable(QString::number(mode))) << mode; }
    void callbackDestructionAndReentryAreSafe()
    {
        QFETCH(int, mode); auto* owner = new QWidget; QPointer<Dialog> d(new Dialog(context(), owner)); connectHandlers(*d); QVERIFY(settled(*d));
        if (mode == 0) { d->SetPreviewHandler([owner](const auto&, QString*) { delete owner; return true; }); action(*d, Planner::Action::Associate); confirm(*d); click(*d, "psdApplySelection"); }
        if (mode == 1) { d->SetSubmissionHandler([owner](const auto&, QString*) { delete owner; return true; }); d->accept(); }
        if (mode == 2) { d->SetCancellationHandler([owner](QString*) { delete owner; return true; }); d->reject(); }
        if (mode == 3) {
            int calls = 0; d->SetPreviewHandler([&](const auto&, QString*) { ++calls; d->reject(); d->accept(); click(*d, "psdUndo"); return true; });
            QVERIFY(associate(*d)); QCOMPARE(calls, 1); QVERIFY(d); delete owner;
        }
        QTRY_VERIFY_WITH_TIMEOUT(d.isNull(), 10000);
    }
    void stopCloseAndOwnerDestroyedDuringWorker_data()
    { QTest::addColumn<int>("mode"); QTest::newRow("stop") << 0; QTest::newRow("reject") << 1; QTest::newRow("owner") << 2; }
    void stopCloseAndOwnerDestroyedDuringWorker()
    {
        QFETCH(int, mode); auto* owner = new QWidget; QPointer<Dialog> d(new Dialog(context(), owner)); connectHandlers(*d);
        if (mode == 0) { click(*d, "psdStop"); QTRY_VERIFY(!d->IsBusy()); QVERIFY(!d->IsValid()); delete owner; }
        if (mode == 1) { d->reject(); QTRY_VERIFY(!d->IsBusy()); QCOMPARE(d->result(), int(QDialog::Rejected)); delete owner; }
        if (mode == 2) delete owner;
        QTest::qWait(50); QVERIFY(d.isNull());
    }
    void contextWithoutRedundantImagesUsesFrozenBaselineReferences()
    {
        auto c = context(); c.atlasImages.clear(); Dialog d(c); connectHandlers(d); QVERIFY(settled(d)); QVERIFY2(d.IsValid(), qPrintable(d.Error()));
        auto* view = d.findChild<CArtworkSourceView*>(); QVERIFY(view->CoordinatesAt(view->DocumentToViewport({2, 3})).hasAtlas);
        const auto before = view->Mapping(); QVERIFY(associate(d)); selectMesh(d, {"B"}); QVERIFY(settled(d));
        QVERIFY(view->CoordinatesAt(view->DocumentToViewport({2, 3})).hasAtlas); QCOMPARE(view->Mapping(), before);
        QCOMPARE(d.Result().state.materials[1], c.snapshot.baseline.materials[1]);
    }
    void frozenAtlasMismatchRejectsBeforePreview()
    {
        auto c = context(); c.atlasImages[0].setPixelColor(0, 0, Qt::green); Dialog d(c); int previews = 0; connectHandlers(d, &previews); QTRY_VERIFY(!d.IsBusy());
        QVERIFY(!d.IsValid()); QVERIFY(d.Error().contains("图集像素")); QCOMPARE(previews, 0);
    }
    void usedFilterAndActualIdentitySurviveSortedLayerRows()
    {
        auto c = context(incoming({Layer{"A", 17}, Layer{"second", 27}})); Dialog d(c); connectHandlers(d); QVERIFY(settled(d));
        auto* tree = control<QTreeView>(d, "psdLayers"); QCOMPARE(tree->currentIndex().data(Qt::UserRole).toString(), QString("record:0")); QVERIFY(associate(d)); QCOMPARE(decision(d, "A").layerId, QString("record:0"));
        control<QComboBox>(d, "psdLayerFilter")->setCurrentIndex(1); QCOMPARE(tree->model()->rowCount(), 1); QCOMPARE(tree->model()->index(0, 0).data(Qt::UserRole).toString(), QString("record:1"));
    }
    void buildStopAndDestructionNeverPublishLateResult()
    {
        Dialog d(context()); int previews = 0; connectHandlers(d, &previews); QVERIFY(settled(d)); const auto before = d.Result().token;
        action(d, Planner::Action::Associate); confirm(d); click(d, "psdApplySelection"); QVERIFY(d.IsBusy()); click(d, "psdStop"); QVERIFY(settled(d));
        QCOMPARE(previews, 0); QCOMPARE(d.Result().token, before); QVERIFY(d.Error().contains("停止"));
        click(d, "psdApplySelection"); QVERIFY(settled(d)); QCOMPARE(previews, 1);
        auto* owner = new QWidget; auto* doomed = new Dialog(context(), owner); connectHandlers(*doomed, &previews); QVERIFY(settled(*doomed));
        action(*doomed, Planner::Action::Associate); confirm(*doomed); click(*doomed, "psdApplySelection"); delete owner; QTest::qWait(50); QCOMPARE(previews, 1);
    }
    void failedRecipeAndPlannerKeepAcceptedPreview()
    {
        auto source = incoming({Layer()}, "revision", {8, 6}, "invalid-profile"); Dialog d(context(source)); connectHandlers(d); QVERIFY(settled(d)); const auto before = d.Result().token;
        auto* color = control<QComboBox>(d, "psdColorPolicy"); color->setCurrentIndex(color->findData(int(Artwork::ColorPolicy::ConvertToSRgb))); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d));
        QVERIFY(d.Error().contains("ICC")); QCOMPARE(d.Result().token, before); click(d, "psdDiscardInput"); QVERIFY(settled(d)); QCOMPARE(d.DraftRequest().incoming.revision, source.revision);
        action(d, Planner::Action::Update); confirm(d); click(d, "psdApplySelection"); QVERIFY(settled(d)); QVERIFY(!d.Error().isEmpty()); QCOMPARE(d.Result().token, before);
    }
    void multiMeshAssociationKeepsIdentityAndGestureSelectsAssociation()
    {
        Dialog d(context()); connectHandlers(d); QVERIFY(settled(d)); selectMesh(d, {"A", "B"}); QVERIFY(settled(d)); QVERIFY(associate(d));
        QCOMPARE(d.Result().state.artwork.bindings.size(), 2); QCOMPARE(d.Result().state.artwork.bindings["A"].familyCoordinates, d.Result().state.artwork.bindings["B"].familyCoordinates);
        selectMesh(d, {"A"}); QVERIFY(settled(d)); action(d, Planner::Action::Keep); click(d, "psdApplySelection"); QVERIFY(settled(d));
        control<QDoubleSpinBox>(d, "psdTranslateX")->setValue(2); click(d, "psdApplyTransform"); QCOMPARE(control<QComboBox>(d, "psdAction")->currentData().toInt(), int(Planner::Action::Associate));
        QVERIFY(!control<QCheckBox>(d, "psdConfirmAssociation")->isChecked());
    }
    void nonPsdSourceMappingDoesNotInvokePsdReader()
    {
        auto c = context(); c.incoming.kind = Artwork::SourceKind::Png; c.incoming.revision.sourceBytes = "frozen png source"; c.incoming.revision.sourceDigest = hash(c.incoming.revision.sourceBytes);
        Dialog d(c); connectHandlers(d); QVERIFY(settled(d)); QVERIFY2(d.IsValid(), qPrintable(d.Error())); QVERIFY(!control<QComboBox>(d, "psdColorPolicy")->isEnabled());
        control<QDoubleSpinBox>(d, "psdDocumentX")->setValue(2); click(d, "psdApplyDocumentAlignment"); QVERIFY(settled(d)); QVERIFY2(d.Error().isEmpty(), qPrintable(d.Error()));
        QCOMPARE(d.DraftRequest().incoming.kind, Artwork::SourceKind::Png); QCOMPARE(d.DraftRequest().incoming.revision.sourceBytes, c.incoming.revision.sourceBytes);
        QCOMPARE(d.DraftRequest().incoming.revision.id, derivedIdentity(c.incoming, c.incoming.revision.colorPolicy, QTransform::fromTranslate(2, 0), false));
    }
    void baseDoneCannotBypassFailedCancellation()
    {
        Dialog d(context()); connectHandlers(d); QVERIFY(settled(d)); QVERIFY(associate(d)); d.show();
        d.SetCancellationHandler([](QString* error) { *error = "retain draft"; return false; }); d.done(QDialog::Rejected); QVERIFY(d.isVisible()); QVERIFY(d.Error().contains("retain draft"));
        d.SetCancellationHandler([](QString*) { return true; }); d.done(QDialog::Rejected); QVERIFY(!d.isVisible());
    }
    void differentLayerAndMixedSelectionRequireFreshConfirmation()
    {
        Dialog d(context(incoming({Layer{"A", 17}, Layer{"Other", 19}}))); connectHandlers(d); QVERIFY(settled(d)); QVERIFY(associate(d));
        auto* tree = control<QTreeView>(d, "psdLayers"); const auto current = tree->currentIndex().data(Qt::UserRole).toString();
        for (int row = 0; row < tree->model()->rowCount(); ++row) { const auto index = tree->model()->index(row, 0); if (index.data(Qt::UserRole).toString() != current) { tree->setCurrentIndex(index); break; } }
        QVERIFY(settled(d)); QVERIFY(!control<QCheckBox>(d, "psdConfirmAssociation")->isChecked());
        click(d, "psdApplySelection"); QVERIFY(settled(d)); QCOMPARE(d.Result().unconfirmed, QSet<QString>{"A"});
        confirm(d); click(d, "psdApplySelection"); QVERIFY(settled(d)); selectMesh(d, {"A", "B"}); QVERIFY(settled(d)); QVERIFY(!control<QCheckBox>(d, "psdConfirmAssociation")->isChecked());
    }
    void failedReferenceCannotAssociateUsingPreviouslyVisibleMesh()
    {
        auto c = context(); c.selectedMeshIds = {"B"}; c.snapshot.limits.maximumOperations = 10000;
        c.snapshot.meshes[0].material.uvs = {0, 0, 100, 0, 100, 100, 0, 100}; c.snapshot.baseline.materials[0] = c.snapshot.meshes[0].material;
        Dialog d(c); connectHandlers(d); QVERIFY(settled(d)); QVERIFY2(d.IsValid(), qPrintable(d.Error()));
        selectMesh(d, {"A"}); QVERIFY(settled(d)); QVERIFY(d.Error().contains("预算")); const auto before = d.Result().token;
        action(d, Planner::Action::Associate); confirm(d); click(d, "psdApplySelection"); QVERIFY(!d.IsBusy()); QVERIFY(d.Error().contains("画布读取失败")); QCOMPARE(d.Result().token, before);
    }
    void finalColorPolicyHasOneUserExplanationWithoutChangingDiagnostics_data()
    {
        QTest::addColumn<int>("policy"); QTest::addColumn<int>("profile");
        QTest::newRow("preserve-untagged") << int(Artwork::ColorPolicy::PreserveValues) << 0;
        QTest::newRow("srgb-untagged") << int(Artwork::ColorPolicy::ConvertToSRgb) << 0;
        QTest::newRow("preserve-profile") << int(Artwork::ColorPolicy::PreserveValues) << 1;
        QTest::newRow("srgb-profile") << int(Artwork::ColorPolicy::ConvertToSRgb) << 1;
        QTest::newRow("preserve-invalid") << int(Artwork::ColorPolicy::PreserveValues) << 2;
    }
    void finalColorPolicyHasOneUserExplanationWithoutChangingDiagnostics()
    {
        QFETCH(int, policy); QFETCH(int, profile);
        const QByteArray icc = profile == 1 ? QColorSpace(QColorSpace::SRgbLinear).iccProfile() : profile == 2 ? QByteArray("invalid-profile") : QByteArray();
        auto source = incoming({Layer()}, "revision", {8, 6}, icc);
        if (policy == int(Artwork::ColorPolicy::ConvertToSRgb)) {
            CPsdSnapshot snapshot; snapshot.bytes = source.revision.sourceBytes; snapshot.canonicalPath = source.revision.path;
            QVERIFY(CArtworkSourceImport::Prepare(snapshot, "family", "revision", Artwork::ColorPolicy(policy), {}, source));
        }
        const auto diagnostics = source.diagnostics; QVERIFY(diagnostics.size() > 1);
        Dialog d(context(source)); connectHandlers(d); QVERIFY(settled(d)); const auto rows = control<QPlainTextEdit>(d, "psdImpacts")->toPlainText().split(QLatin1Char('\n'));
        int colorRows = 0; QString notice;
        for (const auto& text : rows) {
            QVERIFY(!text.contains("color-policy:") && !text.contains("missing-icc:") && !text.contains("invalid-icc:"));
            if (text.startsWith(QStringLiteral("颜色："))) { ++colorRows; notice = text; }
        }
        QCOMPARE(colorRows, 1); QCOMPARE(d.DraftRequest().incoming.diagnostics, diagnostics);
        if (policy == int(Artwork::ColorPolicy::PreserveValues)) QVERIFY(notice.contains("不进行色彩转换"));
        else if (profile == 0) QVERIFY(notice.contains("RGB 数值保持不变"));
        else QVERIFY(notice.contains("转换到 sRGB"));
        if (profile == 0) QVERIFY(notice.contains("未带 ICC"));
        if (profile == 2) QVERIFY(notice.contains("无法用于转换"));
    }
    void diagnosticsStayCompleteAndSelectableWhileWrapping_data()
    { QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true; }
    void diagnosticsStayCompleteAndSelectableWhileWrapping()
    {
        QFETCH(bool, dark); const auto previous = qApp->styleSheet(); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        Dialog d(context()); connectHandlers(d); d.resize(1380, 620); d.show(); QVERIFY(settled(d)); QTest::qWait(100);
        auto* text = control<QPlainTextEdit>(d, "psdImpacts"); const auto complete = text->toPlainText(); QVERIFY(text->isReadOnly()); QVERIFY(!complete.isEmpty());
        auto* scroll = control<QScrollArea>(d, "psdControlsScroll"); scroll->ensureWidgetVisible(text); QTest::qWait(100);
        QVERIFY(text->document()->firstBlock().layout()->lineCount() >= 2); QCOMPARE(text->horizontalScrollBar()->maximum(), 0);
        text->selectAll(); QCOMPARE(text->textCursor().selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n')), complete);
        QTest::keyClicks(text, "replacement"); QCOMPARE(text->toPlainText(), complete);
        d.resize(740, 620); auto* tabs = control<QTabWidget>(d, "psdCompactWorkspace"); tabs->setCurrentIndex(2); QTest::qWait(100);
        QCOMPARE(text->toPlainText(), complete); QCOMPARE(text->horizontalScrollBar()->maximum(), 0); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        auto cursor = text->textCursor(); cursor.movePosition(QTextCursor::End); text->setTextCursor(cursor); text->ensureCursorVisible(); QTest::qWait(20);
        QVERIFY(text->viewport()->rect().intersects(text->cursorRect())); d.reject(); qApp->setStyleSheet(previous);
    }
    void visibleLayoutAndFirstFit_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width"); QTest::addColumn<int>("height");
        for (bool dark : {false, true}) for (int width : {740, 1380}) for (int height : {620, 960}) QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark).arg(width).arg(height))) << dark << width << height;
    }
    void visibleLayoutAndFirstFit()
    {
        QFETCH(bool, dark); QFETCH(int, width); QFETCH(int, height); const auto before = qApp->styleSheet(); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        Dialog d(context()); connectHandlers(d); d.resize(width, height); d.show(); QVERIFY(settled(d)); QTest::qWait(100);
        auto* view = d.findChild<CArtworkSourceView*>(); QVERIFY(view->isVisible()); QVERIFY(view->height() >= view->minimumHeight()); QVERIFY(d.height() <= height);
        for (auto point : {QPointF(-3, -5), QPointF(8, 6)}) { const auto p = view->DocumentToViewport(point); QVERIFY2(view->rect().contains(p.toPoint()), qPrintable(QString("%1,%2 outside %3x%4").arg(p.x()).arg(p.y()).arg(view->width()).arg(view->height()))); }
        const auto directory = qEnvironmentVariable("PSD_DIALOG_IMAGES"); if (!directory.isEmpty()) { QDir().mkpath(directory); QVERIFY(d.grab().save(directory + QString("/psd-%1-%2-%3.png").arg(dark ? "dark" : "light").arg(width).arg(height))); }
        auto* tabs = control<QTabWidget>(d, "psdCompactWorkspace"); if (tabs->isVisible()) tabs->setCurrentIndex(2);
        auto* scroll = control<QScrollArea>(d, "psdControlsScroll"); scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QTest::qWait(100);
        QCOMPARE(scroll->widget()->width(), scroll->viewport()->width());
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0); QVERIFY(!scroll->horizontalScrollBar()->isVisible());
        for (auto* area : {static_cast<QAbstractScrollArea*>(control<QListView>(d, "psdMeshes")), static_cast<QAbstractScrollArea*>(control<QPlainTextEdit>(d, "psdImpacts"))}) {
            QCOMPARE(area->horizontalScrollBar()->maximum(), 0); QVERIFY(!area->horizontalScrollBar()->isVisible());
        }
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QVERIFY(control<QPushButton>(d, "psdApplyDocumentAlignment")->isVisibleTo(scroll));
        QVERIFY(control<QPushButton>(d, "psdSaveSourceOnly")->geometry().bottom() <= d.height());
        if (!directory.isEmpty()) QVERIFY(d.grab().save(directory + QString("/psd-controls-%1-%2-%3.png").arg(dark ? "dark" : "light").arg(width).arg(height)));
        d.reject(); qApp->setStyleSheet(before);
    }
};
QTEST_MAIN(PsdImportDialogTests)
#include "psd_import_dialog_tests.moc"
