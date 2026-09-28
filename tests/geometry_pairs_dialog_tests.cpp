#include "ui/cgeometrypairsdialog.h"
#include "ui/theme.h"
#include "ui/para/ccidjson.h"
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "deformer_replay_fixture.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLayout>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTableView>
#include <QTabWidget>
#include <stdexcept>

using namespace live2dstruct;
using Pairs = CGeometryObjectPairs;
namespace {
bool insert(CModelDocument& document, const QString& id, bool warp, QString* error)
{
    QByteArray preview; if (!document.BuildPreviewMoc(preview, error)) return false;
    auto request = DeformerReplayFixture::rootMesh(preview, warp); request.id = id; CModelEdit edit;
    if (!document.MakeDeformerStructureEdit({request, document.SourceDigest(), QCryptographicHash::hash(preview, QCryptographicHash::Sha256)}, edit, error)) return false;
    return document.ExecuteEdit(edit);
}
Pairs::State pairs(const CModelDocument& document, bool rotations = false)
{
    Pairs::Pair pair; pair.id = rotations ? "rotation" : "warp";
    const auto type = rotations ? Pairs::Type::Rotation : Pairs::Type::Warp;
    const auto prefix = rotations ? QString("DialogRotation") : QString("DialogWarp");
    if (!Pairs::DescribeEndpoint(document, {type, prefix + "A"}, pair.first) || !Pairs::DescribeEndpoint(document, {type, prefix + "B"}, pair.second)) return {};
    for (int vertex = 0; vertex < pair.first.vertexCount; ++vertex) pair.firstVertexToSecondVertex.append(vertex);
    for (const auto& axis : pair.first.axes) {
        QVector<int> keys; for (int key = 0; key < axis.keys.size(); ++key) keys.append(key);
        pair.axes.append({axis.index, axis.index, keys});
    }
    return {document.SourceDigest(), {pair}};
}
void click(CGeometryPairsDialog& dialog, const char* name)
{
    if (auto* button = dialog.findChild<QPushButton*>(QLatin1String(name))) button->click();
}
void select(QComboBox* combo, const QString& id)
{
    combo->setCurrentIndex(combo->findData(id));
}
void newWarp(CGeometryPairsDialog& dialog)
{
    select(dialog.findChild<QComboBox*>("geometryPairFirst"), "DialogWarpA");
    select(dialog.findChild<QComboBox*>("geometryPairSecond"), "DialogWarpB");
    click(dialog, "geometryPairReadObjects"); click(dialog, "geometryVerticesIdentity"); click(dialog, "geometryAxesIdentity");
}
CGeometryPairsDialog::Preview preview()
{
    CGeometryPairsDialog::Preview result{{{100, 100}, {180, 100}, {100, 180}, {180, 180}}, {{220, 100}, {300, 100}, {220, 180}, {300, 180}},
            {{225, 95}, {305, 100}, {220, 185}, {300, 180}}, {}, {},
            QStringLiteral("眨眼 = 0，左右 = -1"), QStringLiteral("眨眼 = 0，左右 = 1")};
    result.firstEdges = result.secondEdges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}}; return result;
}
double contrast(QColor first, QColor second)
{
    const auto luminance = [](QColor color) {
        const auto linear = [](double value) { return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4); };
        return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
    };
    const double firstLight = luminance(first), secondLight = luminance(second);
    return (qMax(firstLight, secondLight) + .05) / (qMin(firstLight, secondLight) + .05);
}
bool bindRotations(CModelDocument& document, QString* error)
{
    CMocParameterStructureBuilder::CreateRequest request; request.parameter.id = "PairDialogRotationAxis";
    request.bindings = {{{CMocParameterStructureBuilder::Owner::Rotation, "DialogRotationA"}, {-1, 0, 1}},
                        {{CMocParameterStructureBuilder::Owner::Rotation, "DialogRotationB"}, {-1, 0, 1}}};
    CModelEdit edit; return document.MakeCreateParameterEdit(request, {}, edit, error) && document.ExecuteEdit(edit);
}
}

class GeometryPairsDialogTests : public QObject
{
    Q_OBJECT
    CModelDocument m_document;
private slots:
    void initTestCase()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat); QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QString error; QVERIFY(m_document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error));
        for (const auto& id : {QString("DialogWarpA"), QString("DialogWarpB"), QString("DialogRotationA"), QString("DialogRotationB")})
            QVERIFY2(insert(m_document, id, id.contains("Warp"), &error), qPrintable(error));
        CMocParameterStructureBuilder::CreateRequest request; request.parameter.id = "PairDialogAxis";
        request.bindings = {{{CMocParameterStructureBuilder::Owner::Warp, "DialogWarpA"}, {-1, 0, 1}},
                            {{CMocParameterStructureBuilder::Owner::Warp, "DialogWarpB"}, {-1, 0, 1}}};
        CModelEdit edit; QVERIFY2(m_document.MakeCreateParameterEdit(request, {}, edit, &error), qPrintable(error)); QVERIFY(m_document.ExecuteEdit(edit));
        QVERIFY(Pairs::Validate(pairs(m_document), m_document)); QVERIFY(Pairs::Validate(pairs(m_document, true), m_document));
    }
    void labelsUseDocumentNamesAndKeepTypedIdentifiers()
    {
        auto document = m_document; QString error;
        QTemporaryDir directory; const auto path = directory.filePath("source.cdi3.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonObject source{{"Deformers", QJsonArray{
            QJsonObject{{"Id", "DialogWarpA"}, {"Name", QStringLiteral("<b>眉毛</b>")}},
            QJsonObject{{"Id", "DialogRotationA"}, {"Name", QStringLiteral("<b>眉毛</b>")}}}}};
        const auto bytes = QJsonDocument(source).toJson(); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        CCidJson names; QVERIFY(names.Parse(path, &error));
        QVERIFY(document.SetDeformerDisplayNames(names.DeformerDisplayNames(), &error));
        const auto mesh = document.Drawables().first().id;
        auto displayNames = document.DisplayNames(); displayNames.artMeshes.insert(mesh, QStringLiteral("<b>眉毛</b>"));
        QVERIFY(document.SetDisplayNames(displayNames, &error));
        CGeometryPairsDialog dialog(document, &names, {});
        for (const auto* name : {"geometryPairFirst", "geometryPairSecond"}) {
            auto* combo = dialog.findChild<QComboBox*>(name); QVERIFY(combo);
            for (const auto& id : {QString("DialogWarpA"), QString("DialogRotationA"), mesh}) {
                const int row = combo->findData(id); QVERIFY(row >= 0);
                QVERIFY(combo->itemText(row).startsWith(QStringLiteral("<b>眉毛</b> · ") + id));
                QCOMPARE(combo->itemData(row).toString(), id);
                combo->setCurrentIndex(row); QVERIFY(combo->toolTip().contains("&lt;b&gt;"));
            }
        }
    }
    void createNeedsExplicitMappingsAndAppliesWholeDraft()
    {
        CGeometryPairsDialog dialog(m_document, nullptr, {{Pairs::Type::Warp, "DialogWarpA"}, {Pairs::Type::Warp, "DialogWarpB"}}); dialog.show();
        QCOMPARE(dialog.findChild<QComboBox*>("geometryPairFirst")->currentData().toString(), QString("DialogWarpA"));
        click(dialog, "geometryPairReadObjects"); dialog.accept(); QVERIFY(dialog.isVisible()); QVERIFY(dialog.DraftResult().pairs.isEmpty());
        QVERIFY(!dialog.findChild<QLabel*>("geometryPairError")->text().isEmpty());
        auto* vertex = dialog.findChild<QTableView*>("geometryVertexMap"); QCOMPARE(vertex->model()->rowCount(), 4);
        QVERIFY(vertex->model()->data(vertex->model()->index(0, 1), Qt::EditRole).toString().isEmpty());
        click(dialog, "geometryVerticesColumns"); click(dialog, "geometryAxesIdentity"); click(dialog, "geometryKeysReverse");
        int calls = 0; dialog.SetSubmissionHandler([&](const auto& state, QString*) { ++calls; return Pairs::Validate(state, m_document); });
        dialog.accept(); QCOMPARE(calls, 1); QCOMPARE(dialog.result(), int(QDialog::Accepted));
        const auto& pair = dialog.DraftResult().pairs[0]; QVERIFY(pair.reversedWinding); QCOMPARE(pair.firstVertexToSecondVertex, QVector<int>({1, 0, 3, 2}));
        QCOMPARE(pair.axes[0].firstKeyToSecondKey, QVector<int>({2, 1, 0})); QVERIFY(m_document.GeometryObjectPairs().pairs.isEmpty());
    }
    void invalidTextAndScopeStayVisibleUntilDiscarded()
    {
        auto document = m_document; QVERIFY(document.SetGeometryObjectPairs(pairs(document)));
        CGeometryPairsDialog dialog(document, nullptr); dialog.show();
        auto* vertex = dialog.findChild<QTableView*>("geometryVertexMap"); const auto cell = vertex->model()->index(1, 1);
        QVERIFY(vertex->model()->setData(cell, "wrong", Qt::EditRole)); click(dialog, "geometryPairNew");
        QCOMPARE(vertex->model()->data(cell, Qt::EditRole).toString(), QString("wrong")); QCOMPARE(dialog.DraftResult().pairs.size(), 1);
        QVERIFY(!dialog.findChild<QLabel*>("geometryPairError")->text().isEmpty());
        dialog.accept(); QVERIFY(dialog.isVisible()); QCOMPARE(vertex->model()->data(cell, Qt::EditRole).toString(), QString("wrong"));
        click(dialog, "geometryPairDiscard"); QCOMPARE(vertex->model()->data(cell, Qt::EditRole).toString(), QString("2"));
        select(dialog.findChild<QComboBox*>("geometryPairSecond"), "DialogRotationA"); dialog.accept();
        QVERIFY(dialog.isVisible()); QVERIFY(dialog.findChild<QLabel*>("geometryPairError")->text().contains(QStringLiteral("读取")));
        click(dialog, "geometryPairDiscard"); QCOMPARE(dialog.findChild<QComboBox*>("geometryPairSecond")->currentData().toString(), QString("DialogWarpB"));
    }
    void selectionPrefillsExistingOrNewPairWithoutGuessing()
    {
        auto document = m_document; auto state = pairs(document); state.pairs.append(pairs(document, true).pairs[0]);
        QVERIFY(document.SetGeometryObjectPairs(state));
        CGeometryPairsDialog existing(document, nullptr, {{Pairs::Type::Rotation, "DialogRotationB"}});
        QCOMPARE(existing.findChild<QComboBox*>("geometryPairList")->currentIndex(), 1);
        QCOMPARE(existing.findChild<QComboBox*>("geometryPairFirst")->currentData().toString(), QString("DialogRotationA"));
        const auto mesh = document.Drawables()[0].id;
        CGeometryPairsDialog fresh(document, nullptr, {{Pairs::Type::ArtMesh, mesh}});
        QCOMPARE(fresh.findChild<QComboBox*>("geometryPairList")->currentIndex(), -1);
        QCOMPARE(fresh.findChild<QComboBox*>("geometryPairFirst")->currentData().toString(), mesh);
        QCOMPARE(fresh.findChild<QComboBox*>("geometryPairSecond")->currentIndex(), -1);
        fresh.accept(); QCOMPARE(fresh.result(), int(QDialog::Accepted)); QCOMPARE(fresh.DraftResult(), state);
    }
    void validDraftIsCommittedBeforeSwitchAndRemoveIsExplicit()
    {
        auto document = m_document; auto state = pairs(document); state.pairs.append(pairs(document, true).pairs[0]);
        QVERIFY(document.SetGeometryObjectPairs(state)); CGeometryPairsDialog dialog(document, nullptr); dialog.show();
        dialog.findChild<QCheckBox*>("geometryPairEnabled")->setChecked(false);
        dialog.findChild<QComboBox*>("geometryPairList")->setCurrentIndex(1);
        QVERIFY(!dialog.DraftResult().pairs[0].enabled); QCOMPARE(dialog.findChild<QComboBox*>("geometryPairFirst")->currentData().toString(), QString("DialogRotationA"));
        click(dialog, "geometryPairRemove"); QCOMPARE(dialog.DraftResult().pairs.size(), 1);
        dialog.accept(); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QVERIFY(!dialog.DraftResult().pairs[0].enabled);
    }
    void rejectedThrowingAndStaleSubmissionsKeepDraft()
    {
        auto document = m_document; QVERIFY(document.SetGeometryObjectPairs(pairs(document)));
        CGeometryPairsDialog dialog(document, nullptr); dialog.show();
        auto* position = dialog.findChild<QDoubleSpinBox*>("geometryPairPosition"); position->setValue(137.25);
        int mode = 0, calls = 0; const auto revision = document.Revision();
        dialog.SetSubmissionHandler([&](const auto&, QString* error) {
            ++calls; if (mode == 0) { *error = "controlled rejection"; return false; }
            if (mode == 1) throw std::runtime_error("injected");
            if (mode == 2 && revision != document.Revision()) { *error = "stale document"; return false; }
            return true;
        });
        dialog.accept(); QCOMPARE(calls, 1); QVERIFY(dialog.isVisible()); QCOMPARE(position->value(), 137.25);
        QCOMPARE(dialog.findChild<QLabel*>("geometryPairError")->text(), QString("controlled rejection"));
        mode = 1; dialog.accept(); QVERIFY(dialog.isVisible()); QVERIFY(!dialog.findChild<QLabel*>("geometryPairError")->text().isEmpty());
        mode = 2; document.SetLocked("warp", "DialogWarpA", true); dialog.accept(); QVERIFY(dialog.isVisible());
        QCOMPARE(dialog.findChild<QLabel*>("geometryPairError")->text(), QString("stale document"));
        mode = 3; dialog.accept(); QCOMPARE(calls, 4); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void reviewRequiresExplicitConfirmation()
    {
        auto document = m_document; auto state = pairs(document); state.pairs[0].requiresReview = true; QVERIFY(document.SetGeometryObjectPairs(state));
        CGeometryPairsDialog dialog(document, nullptr); dialog.show();
        click(dialog, "geometryAxesIdentity"); dialog.accept(); QVERIFY(dialog.DraftResult().pairs[0].requiresReview);
        CGeometryPairsDialog repaired(document, nullptr); repaired.show(); newWarp(repaired); click(repaired, "geometryPairConfirm");
        QVERIFY(!repaired.DraftResult().pairs[0].requiresReview); repaired.accept(); QCOMPARE(repaired.result(), int(QDialog::Accepted));
    }
    void pixelAxisRoundTripsThroughCanvasGeometry()
    {
        CGeometryPairsDialog dialog(m_document, nullptr); newWarp(dialog);
        dialog.findChild<QComboBox*>("geometryPairLine")->setCurrentIndex(1);
        dialog.findChild<QDoubleSpinBox*>("geometryPairPosition")->setValue(231.5); dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted)); const auto& pair = dialog.DraftResult().pairs[0];
        auto reader = CReader::FromBytes(m_document.WorkingBaseBytes()); CSectionTable table(reader); QPointF point;
        QVERIFY(CCanvasGeometry::ModelToPixels(table.canvas_info, pair.linePoint, point)); QVERIFY(std::abs(point.y() - 231.5) < 1e-9);
        QVERIFY(pair.lineDirection.x() != 0); QCOMPARE(pair.lineDirection.y(), 0.0);
    }
    void unrelatedEditsPreserveExactAxisAndRotationMatrix()
    {
        auto document = m_document; auto state = pairs(document, true);
        state.pairs[0].linePoint = QPointF(.1234567890123, -.444444444444);
        state.pairs[0].lineDirection = QPointF(0, -37.25);
        state.pairs[0].rotationLocalX = QPointF(-1, 0); QVERIFY(document.SetGeometryObjectPairs(state));
        CGeometryPairsDialog dialog(document, nullptr); dialog.findChild<QCheckBox*>("geometryPairEnabled")->setChecked(false); dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted)); const auto& pair = dialog.DraftResult().pairs[0];
        QCOMPARE(pair.linePoint.x(), state.pairs[0].linePoint.x()); QCOMPARE(pair.linePoint.y(), state.pairs[0].linePoint.y());
        QCOMPARE(pair.lineDirection.y(), state.pairs[0].lineDirection.y()); QCOMPARE(pair.rotationLocalX, state.pairs[0].rotationLocalX);
    }
    void rotationCorrespondenceOnlyChangesOnExplicitMatch()
    {
        auto document = m_document; QVERIFY(document.SetGeometryObjectPairs(pairs(document, true)));
        CGeometryPairsDialog dialog(document, nullptr); dialog.show(); int initialized = 0, previews = 0, reference = -1;
        dialog.SetPairInitializer([&](auto& pair, int keyform, QString*) { ++initialized; reference = keyform; pair.rotationLocalX = QPointF(-1, 0); return true; });
        dialog.SetPreviewProvider([&](const auto&, int, auto& result, QString*) { ++previews; result = preview(); return true; });
        dialog.findChild<QDoubleSpinBox*>("geometryPairPosition")->setValue(150);
        click(dialog, "geometryPairRefreshPreview"); click(dialog, "geometryPairConfirm"); QCOMPARE(initialized, 0); QVERIFY(previews >= 1);
        click(dialog, "geometryPairMatchRotation"); QCOMPARE(initialized, 1); QCOMPARE(reference, 0);
        dialog.findChild<QDoubleSpinBox*>("geometryPairPosition")->setValue(190); click(dialog, "geometryPairRefreshPreview"); dialog.accept();
        QCOMPARE(initialized, 1); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(dialog.DraftResult().pairs[0].rotationLocalX, QPointF(-1, 0));
    }
    void newRotationMustMatchAndBadInitializerKeepsDraft()
    {
        CGeometryPairsDialog dialog(m_document, nullptr, {{Pairs::Type::Rotation, "DialogRotationA"}, {Pairs::Type::Rotation, "DialogRotationB"}}); dialog.show();
        click(dialog, "geometryPairReadObjects"); dialog.accept(); QVERIFY(dialog.isVisible());
        QVERIFY(dialog.findChild<QLabel*>("geometryPairError")->text().contains(QStringLiteral("匹配")));
        dialog.SetPairInitializer([](auto& pair, int, QString*) { pair.first.object.id = "altered"; return true; });
        click(dialog, "geometryPairMatchRotation"); QVERIFY(dialog.DraftResult().pairs.isEmpty());
        dialog.SetPairInitializer([](auto& pair, int, QString*) { pair.rotationLocalX = QPointF(-1, 0); return true; });
        click(dialog, "geometryPairMatchRotation"); dialog.accept(); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void callbacksMayCloseOwner_data()
    {
        QTest::addColumn<int>("kind"); for (int kind = 0; kind < 3; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void callbacksMayCloseOwner()
    {
        QFETCH(int, kind); auto document = m_document; QVERIFY(document.SetGeometryObjectPairs(pairs(document, kind == 1)));
        auto* owner = new QWidget; auto* dialog = new CGeometryPairsDialog(document, nullptr, {}, owner); QPointer<CGeometryPairsDialog> guard(dialog);
        if (kind == 0) { dialog->SetSubmissionHandler([&](const auto&, QString*) { delete owner; return true; }); dialog->accept(); }
        if (kind == 1) { dialog->SetPairInitializer([&](auto&, int, QString*) { delete owner; return true; }); click(*dialog, "geometryPairMatchRotation"); }
        if (kind == 2) { dialog->SetPreviewProvider([&](const auto&, int, auto&, QString*) { delete owner; return false; }); click(*dialog, "geometryPairRefreshPreview"); }
        QVERIFY(guard.isNull());
    }
    void previewValidationAndReentryKeepControlsUsable()
    {
        auto document = m_document; QVERIFY(document.SetGeometryObjectPairs(pairs(document)));
        CGeometryPairsDialog dialog(document, nullptr); dialog.show(); int calls = 0;
        dialog.SetPreviewProvider([&](const auto&, int, auto& result, QString*) {
            ++calls; dialog.accept(); dialog.reject(); result = preview(); result.firstEdges[0].first = 100; return true;
        });
        click(dialog, "geometryPairRefreshPreview"); QCOMPARE(calls, 1); QVERIFY(dialog.isVisible());
        QVERIFY(!dialog.findChild<QLabel*>("geometryPairError")->text().isEmpty()); QVERIFY(dialog.findChild<QPushButton*>("geometryPairRefreshPreview")->isEnabled());
        dialog.SetPreviewProvider([](const auto&, int, auto& result, QString*) { result = preview(); result.secondEdges[0].second = -1; return true; });
        click(dialog, "geometryPairRefreshPreview"); QVERIFY(!dialog.findChild<QLabel*>("geometryPairError")->text().isEmpty());
        dialog.SetPreviewProvider([](const auto&, int, auto& result, QString*) { result = preview(); return true; });
        click(dialog, "geometryPairRefreshPreview"); QVERIFY(dialog.findChild<QLabel*>("geometryPairError")->text().isEmpty());
        QVERIFY(dialog.findChild<QLabel*>("geometryPairCorners")->text().contains(QStringLiteral("眨眼")));
    }
    void mappingPagesFollowTypeAndKeepUncommittedCells()
    {
        CGeometryPairsDialog dialog(m_document, nullptr); dialog.show(); newWarp(dialog);
        auto* tabs = dialog.findChild<QTabWidget*>("geometryPairTabs"); auto* vertices = dialog.findChild<QTableView*>("geometryVertexMap");
        auto* first = dialog.findChild<QComboBox*>("geometryPairFirst"); auto* second = dialog.findChild<QComboBox*>("geometryPairSecond");
        QVERIFY(tabs->isVisible()); QVERIFY(tabs->isTabVisible(0)); QVERIFY(tabs->isTabVisible(1));
        const auto cell = vertices->model()->index(0, 1); QVERIFY(vertices->model()->setData(cell, "draft text", Qt::EditRole));
        select(first, "DialogRotationA"); QVERIFY(tabs->isHidden());
        select(first, "DialogWarpA"); QVERIFY(tabs->isVisible()); QVERIFY(tabs->isTabVisible(0));
        QCOMPARE(vertices->model()->data(cell, Qt::EditRole).toString(), QString("draft text"));
        select(first, m_document.Drawables()[0].id); select(second, m_document.Drawables()[1].id); click(dialog, "geometryPairReadObjects");
        QVERIFY(tabs->isVisible()); QVERIFY(tabs->isTabVisible(0)); QVERIFY(vertices->model()->rowCount() > 0);
        QVERIFY(dialog.findChild<QPushButton*>("geometryVerticesColumns")->isHidden());
        select(first, "DialogRotationA"); select(second, "DialogRotationB"); click(dialog, "geometryPairReadObjects");
        QVERIFY(tabs->isHidden()); QVERIFY(!tabs->isTabVisible(0)); QVERIFY(!tabs->isTabVisible(1));
        QVERIFY(vertices->isHidden() || !vertices->isVisibleTo(&dialog));
        QVERIFY(!dialog.findChild<QTableView*>("geometryKeyMap")->isVisibleTo(&dialog));
        QVERIFY(dialog.findChild<QLabel*>("geometryPairNoAxes")->isVisible());
        auto* layout = tabs->parentWidget()->layout(); bool absentFromLayout = false;
        for (int index = 0; index < layout->count(); ++index) if (layout->itemAt(index)->widget() == tabs) absentFromLayout = layout->itemAt(index)->isEmpty();
        QVERIFY(absentFromLayout);
        select(first, "DialogWarpA"); select(second, "DialogWarpB"); click(dialog, "geometryPairReadObjects");
        QVERIFY(tabs->isVisible()); QVERIFY(tabs->isTabVisible(0)); QVERIFY(tabs->isTabVisible(1));
    }
    void saveIsPrimaryDefaultAcceptAction()
    {
        CGeometryPairsDialog dialog(m_document, nullptr); dialog.show(); auto* buttons = dialog.findChild<QDialogButtonBox*>();
        auto* save = buttons->button(QDialogButtonBox::Save); auto* cancel = buttons->button(QDialogButtonBox::Cancel);
        QVERIFY(save); QVERIFY(cancel); QVERIFY(!buttons->button(QDialogButtonBox::Apply));
        QCOMPARE(buttons->buttonRole(save), QDialogButtonBox::AcceptRole); QCOMPARE(save->objectName(), QString("primaryBtn"));
        QVERIFY(save->isDefault()); QCOMPARE(save->text(), QStringLiteral("保存配对"));
        if (QGuiApplication::platformName() == QLatin1String("cocoa")) QTRY_VERIFY(save->x() > cancel->x());
        int calls = 0; dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; });
        QTest::keyClick(&dialog, Qt::Key_Return); QCOMPARE(calls, 1); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void rotationLayoutShowsOnlyRelevantControls_data()
    {
        QTest::addColumn<bool>("axes"); QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool axes : {false, true}) for (bool dark : {false, true}) for (int width : {380, 1000})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(axes).arg(dark).arg(width))) << axes << dark << width;
    }
    void rotationLayoutShowsOnlyRelevantControls()
    {
        QFETCH(bool, axes); QFETCH(bool, dark); QFETCH(int, width); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        auto document = m_document; QString error; if (axes) QVERIFY2(bindRotations(document, &error), qPrintable(error));
        QVERIFY(document.SetGeometryObjectPairs(pairs(document, true))); CGeometryPairsDialog dialog(document, nullptr);
        dialog.SetPreviewProvider([](const auto&, int, auto& result, QString*) {
            result.firstPoints = {{100, 100}, {150, 100}, {100, 100}, {100, 150}};
            result.secondPoints = {{300, 100}, {250, 100}, {300, 100}, {300, 150}};
            result.proposedSecondPoints = {{305, 105}, {255, 100}, {305, 105}, {300, 155}};
            result.firstCorner = QStringLiteral("源旋转对象 · 关键形 1"); result.secondCorner = QStringLiteral("目标旋转对象 · 关键形 1"); return true;
        });
        dialog.resize(width, width == 1000 ? 845 : 720); dialog.show(); QTest::qWait(30); click(dialog, "geometryPairRefreshPreview");
        auto* tabs = dialog.findChild<QTabWidget*>("geometryPairTabs"); auto* scroll = dialog.findChild<QScrollArea*>("geometryPairsScroll");
        auto* save = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
        QCOMPARE(save->palette().color(QPalette::Button), QColor(Theme::tokens(dark).value("@accentFill@")));
        QCOMPARE(save->palette().color(QPalette::ButtonText), QColor(Theme::tokens(dark).value("@accentText@")));
        QVERIFY(!tabs->isTabVisible(0)); QVERIFY(!dialog.findChild<QTableView*>("geometryVertexMap")->isVisibleTo(&dialog));
        QCOMPARE(tabs->isTabVisible(1), axes); QCOMPARE(tabs->isVisible(), axes);
        if (axes) { QCOMPARE(tabs->currentIndex(), 1); QCOMPARE(dialog.findChild<QTableView*>("geometryKeyMap")->model()->rowCount(), 3); }
        else { QVERIFY(dialog.findChild<QLabel*>("geometryPairNoAxes")->isVisible()); QVERIFY(!dialog.findChild<QTableView*>("geometryKeyMap")->isVisibleTo(&dialog)); }
        QVERIFY(dialog.findChild<QPushButton*>("geometryPairMatchRotation")->isVisible());
        QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        auto* canvas = dialog.findChild<QWidget*>("geometryPairPreview");
        if (!axes && width == 1000) QTRY_VERIFY(scroll->viewport()->rect().contains(canvas->mapTo(scroll->viewport(), canvas->rect().center())));
        const auto destination = qEnvironmentVariable("GEOMETRY_PAIRS_SCREENSHOT_DIR");
        if (!destination.isEmpty()) {
            QDir().mkpath(destination); QTest::qWait(20);
            QVERIFY(dialog.grab().save(destination + QString("/rotation-%1-%2-%3.png").arg(axes ? "axes" : "zero").arg(dark ? "dark" : "light").arg(width)));
        }
        auto* refresh = dialog.findChild<QPushButton*>("geometryPairRefreshPreview"); scroll->ensureWidgetVisible(refresh);
        QTRY_VERIFY(scroll->viewport()->rect().contains(refresh->mapTo(scroll->viewport(), refresh->rect().center())));
        qApp->setStyleSheet({});
    }
    void themesAndSmallWindows_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {380, 1000}) QTest::newRow(qPrintable(QString("%1-%2").arg(dark).arg(width))) << dark << width;
    }
    void themesAndSmallWindows()
    {
        QFETCH(bool, dark); QFETCH(int, width); qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        auto document = m_document; QVERIFY(document.SetGeometryObjectPairs(pairs(document)));
        CGeometryPairsDialog dialog(document, nullptr); dialog.SetPreviewProvider([](const auto&, int, auto& result, QString*) { result = preview(); return true; });
        dialog.resize(width, 720); dialog.show(); QTest::qWait(30); click(dialog, "geometryPairRefreshPreview");
        auto* scroll = dialog.findChild<QScrollArea*>("geometryPairsScroll"); QVERIFY(scroll); QVERIFY(scroll->widgetResizable());
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        auto* table = dialog.findChild<QTableView*>("geometryVertexMap");
        QVERIFY(contrast(table->palette().color(QPalette::Text), table->palette().color(QPalette::Base)) >= 4.5);
        QVERIFY(contrast(table->palette().color(QPalette::Text), table->palette().color(QPalette::AlternateBase)) >= 4.5);
        QVERIFY(dialog.findChild<QDialogButtonBox*>()->isVisible()); QVERIFY(dialog.findChild<QPushButton*>("geometryPairConfirm")->isEnabled());
        auto* corners = dialog.findChild<QLabel*>("geometryPairCorners"); QTRY_VERIFY(corners->height() >= corners->heightForWidth(corners->width()));
        const auto destination = qEnvironmentVariable("GEOMETRY_PAIRS_SCREENSHOT_DIR");
        if (!destination.isEmpty()) {
            QDir().mkpath(destination); QVERIFY(dialog.grab().save(destination + QString("/top-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); QTest::qWait(20);
            auto* refresh = dialog.findChild<QPushButton*>("geometryPairRefreshPreview"); scroll->ensureWidgetVisible(refresh);
            QTRY_VERIFY(scroll->viewport()->rect().contains(refresh->mapTo(scroll->viewport(), refresh->rect().center())));
            QVERIFY(dialog.grab().save(destination + QString("/preview-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
            dialog.findChild<QTabWidget*>("geometryPairTabs")->setCurrentIndex(1);
            scroll->ensureWidgetVisible(dialog.findChild<QTableView*>("geometryKeyMap")); QTest::qWait(20);
            QVERIFY(dialog.grab().save(destination + QString("/keys-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
        }
        qApp->setStyleSheet({});
    }
};

QTEST_MAIN(GeometryPairsDialogTests)
#include "geometry_pairs_dialog_tests.moc"
