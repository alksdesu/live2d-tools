#include "ui/cinspectorpanel.h"
#include "ui/ckeyformmatrix.h"
#include "ui/cbindingkeysdialog.h"
#include "ui/theme.h"
#include "wide_mesh_fixture.h"
#include <QScopeGuard>
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/cmesheditplanner.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QFormLayout>
#include <QLabel>
#include <QHeaderView>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QDir>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableView>
#include <QPushButton>
#include <QSignalSpy>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QLineEdit>
#include <QTimer>
#include <QApplication>
#include <QMainWindow>
#include <QPointer>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMessageBox>
#include <QtEndian>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <memory>
#include <cstring>

namespace {
class IndexTextProbe : public QStyledItemDelegate
{
public:
    static bool Fits(QTableWidget* table, int row)
    {
        IndexTextProbe probe; QStyleOptionViewItem option; option.initFrom(table); option.widget = table;
        const auto index = table->model()->index(row,0); probe.initStyleOption(&option,index); option.rect = table->visualItemRect(table->item(row,0));
        const auto area = table->style()->subElementRect(QStyle::SE_ItemViewItemText,&option,table);
        const int required = probe.QStyledItemDelegate::sizeHint(option,index).width();
        return option.rect.width() >= required && option.fontMetrics.elidedText(option.text,Qt::ElideRight,area.width()) == option.text;
    }
};
bool topologyDocument(const QVector<float>& positions, const QVector<quint16>& indices,
                      live2dstruct::CModelDocument& document, QString& id, QString* error, bool multipleKeyforms = false)
{
    using namespace live2dstruct;
    if (!document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", error)) return false;
    const auto found = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(), [&](const auto& value) {
        return value.vertexCount >= positions.size() / 2 && (!multipleKeyforms || value.keyforms.size() > 1);
    });
    if (found == document.Drawables().cend()) return false;
    const auto& source = *found;
    id = source.id;
    if (source.vertexCount < positions.size() / 2) return false;
    CMocMeshStructureBuilder::MeshEdit mesh;
    mesh.artMeshId = id;
    mesh.oldToNew = QVector<int>(source.vertexCount, -1);
    for (int vertex = 0; vertex < positions.size() / 2; ++vertex) {
        mesh.vertices.append({{vertex}, {1.0}});
        mesh.oldToNew[vertex] = vertex;
    }
    mesh.indices = indices;
    CMocMeshStructureBuilder::Request request;
    request.meshes.append(mesh);
    CModelEdit edit;
    if (!document.MakeMeshStructureEdit(request, edit, error) || !document.ExecuteEdit(edit) ||
        !document.SetDrawableKeyformPositions(id, 0, positions)) return false;
    document.ClearEditHistory();
    return true;
}
QSet<quint32> topologyBoundary(const QVector<quint16>& indices)
{
    QMap<quint32, int> edges;
    for (int face = 0; face < indices.size(); face += 3) for (int side = 0; side < 3; ++side) {
        const auto a = indices[face + side], b = indices[face + (side + 1) % 3];
        ++edges[(quint32(qMin(a, b)) << 16) | qMax(a, b)];
    }
    QSet<quint32> boundary;
    for (auto edge = edges.cbegin(); edge != edges.cend(); ++edge) if (edge.value() == 1) boundary.insert(edge.key());
    return boundary;
}
double topologyArea(const QVector<float>& positions, const QVector<quint16>& indices)
{
    double area = 0;
    for (int face = 0; face < indices.size(); face += 3) {
        const int a = indices[face] * 2, b = indices[face + 1] * 2, c = indices[face + 2] * 2;
        area += std::abs((double(positions[b]) - positions[a]) * (double(positions[c + 1]) - positions[a + 1]) -
                         (double(positions[b + 1]) - positions[a + 1]) * (double(positions[c]) - positions[a])) / 2;
    }
    return area;
}

bool withBindingDialog(CInspectorPanel* panel, const std::function<void(QPointer<CBindingKeysDialog>)>& action)
{
    const QPointer<CInspectorPanel> owner(panel);
    const QPointer<QPushButton> button(panel->findChild<QPushButton*>("editBindingKeys"));
    if (!button || !button->isEnabled()) return false;
    bool invoked = false;
    QTimer::singleShot(0, qApp, [&, owner] {
        if (!owner) return;
        const QPointer<CBindingKeysDialog> dialog(owner->findChild<CBindingKeysDialog*>("bindingKeysDialog"));
        if (!dialog) {
            for (auto* pending : owner->findChildren<QDialog*>()) pending->reject();
            return;
        }
        const auto close = qScopeGuard([dialog] { if (dialog && dialog->isVisible()) dialog->reject(); });
        invoked = true;
        action(dialog);
    });
    button->clicked(false);
    return invoked;
}
}

class InspectorHistoryTests : public QObject
{
    Q_OBJECT
private slots:
    void bindingKeyDialogCommitsOneHistoryEntry_data()
    {
        QTest::addColumn<QString>("model"); QTest::addColumn<QString>("id"); QTest::addColumn<QString>("type");
        QTest::addColumn<float>("original"); QTest::addColumn<float>("moved");
        QTest::newRow("artmesh") << QString("Mark") << QString("EyeL4") << QString("artmesh") << -1.0f << -.5f;
        QTest::newRow("warp") << QString("Mark") << QString("Warp6") << QString("warp") << -30.0f << -15.0f;
        QTest::newRow("rotation") << QString("Natori") << QString("Rotation2") << QString("rotation") << -10.0f << -5.0f;
    }

    void bindingKeyDialogCommitsOneHistoryEntry()
    {
        QFETCH(QString, model); QFETCH(QString, id); QFETCH(QString, type); QFETCH(float, original); QFETCH(float, moved);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3"));
        const auto source = document.SourceBytes(), digest = document.SourceDigest();
        const auto before = document.ParameterStructureState();
        CInspectorPanel panel; panel.SetDocument(&document); panel.SetSelection(id, type);
        auto* matrix = panel.findChild<CKeyformMatrix*>("keyformMatrix");
        QVERIFY(matrix);
        auto* view = matrix->findChild<QTableView*>("keyformMatrixTable");
        QVERIFY(view);
        QCOMPARE(view->model()->headerData(0, Qt::Horizontal).toFloat(), original);
        QSignalSpy structure(&panel, &CInspectorPanel::StructureChanged), history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        int calls = 0;
        panel.SetEditHandler([&](const CModelEdit& edit, QString*) {
            ++calls;
            return edit.parameterChange == CModelEdit::ParameterChange::BindingKeys && document.ExecuteEdit(edit);
        });
        QPointer<CBindingKeysDialog> pending;
        QVERIFY(withBindingDialog(&panel, [&](auto dialog) {
            pending = dialog;
            auto* table = dialog->template findChild<QTableView*>("bindingKeysTable");
            auto* apply = dialog->template findChild<QPushButton*>("bindingKeysApply");
            QVERIFY(table && apply); QCOMPARE(dialog->SelectedAxis(), 0);
            QVERIFY(table->model()->setData(table->model()->index(0, 0), QString::number(moved, 'g', 9), Qt::EditRole));
            CMocParameterStructureBuilder::EditBindingKeysRequest request;
            QString error;
            QVERIFY2(dialog->BuildRequest(request, &error), qPrintable(error));
            QCOMPARE(request.expectedKeys.first(), original); QCOMPARE(request.keys.first().value, moved);
            QVERIFY(apply->isEnabled()); apply->click();
            QVERIFY(dialog && !dialog->isVisible());
        }));
        QVERIFY(!pending); QCOMPARE(calls, 1); QCOMPARE(structure.count(), 1); QCOMPARE(history.count(), 1); QCOMPARE(rejected.count(), 0);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(view->model()->headerData(0, Qt::Horizontal).toFloat(), moved);
        const auto after = document.ParameterStructureState(); QVERIFY(after != before);
        panel.Undo();
        QCOMPARE(document.ParameterStructureState(), before); QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(view->model()->headerData(0, Qt::Horizontal).toFloat(), original);
        panel.Redo();
        QCOMPARE(document.ParameterStructureState(), after); QCOMPARE(history.count(), 3);
        QCOMPARE(view->model()->headerData(0, Qt::Horizontal).toFloat(), moved);
        QCOMPARE(document.SourceBytes(), source); QCOMPARE(document.SourceDigest(), digest);
        QFile sourceFile(document.SourcePath()); QVERIFY(sourceFile.open(QIODevice::ReadOnly)); QCOMPARE(sourceFile.readAll(), source);
    }

    void bindingKeyDialogRetainsInputAfterRejectedPublication_data()
    {
        QTest::addColumn<bool>("throws"); QTest::newRow("reject") << false; QTest::newRow("exception") << true;
    }

    void bindingKeyDialogRetainsInputAfterRejectedPublication()
    {
        QFETCH(bool, throws);
        using namespace live2dstruct;
        CModelDocument document; QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto before = document.ParameterStructureState(); const auto revision = document.Revision();
        CInspectorPanel panel; panel.SetDocument(&document); panel.SetSelection("EyeL4", "artmesh");
        QSignalSpy structure(&panel, &CInspectorPanel::StructureChanged), history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        int calls = 0; bool accept = false;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++calls;
            if (!accept) {
                if (throws) throw std::runtime_error("controlled binding rejection");
                *error = QStringLiteral("controlled binding rejection"); return false;
            }
            return document.ExecuteEdit(edit);
        });
        QVERIFY(withBindingDialog(&panel, [&](auto dialog) {
            auto* table = dialog->template findChild<QTableView*>("bindingKeysTable");
            auto* apply = dialog->template findChild<QPushButton*>("bindingKeysApply");
            auto* error = dialog->template findChild<QLabel*>("bindingKeysError");
            QVERIFY(table && apply && error);
            QVERIFY(table->model()->setData(table->model()->index(0, 0), QString("-0.5"), Qt::EditRole));
            const auto input = table->model()->index(0, 0).data(Qt::EditRole);
            apply->click();
            QVERIFY(dialog && dialog->isVisible()); QCOMPARE(calls, 1); QCOMPARE(rejected.count(), 1);
            QCOMPARE(table->model()->index(0, 0).data(Qt::EditRole), input);
            QVERIFY(error->isVisible()); QVERIFY(error->text().contains("controlled binding rejection"));
            QCOMPARE(document.ParameterStructureState(), before); QCOMPARE(document.Revision(), revision);
            QCOMPARE(structure.count(), 0); QCOMPARE(history.count(), 0); QVERIFY(!document.CanUndoEdit());
            accept = true; QVERIFY(apply->isEnabled()); apply->click();
            QVERIFY(dialog && !dialog->isVisible());
        }));
        QCOMPARE(calls, 2); QCOMPARE(structure.count(), 1); QCOMPARE(history.count(), 1); QCOMPARE(rejected.count(), 1);
        QVERIFY(document.CanUndoEdit());
    }

    void bindingKeyDialogCancellationAndStaleStateDoNotSubmit_data()
    {
        QTest::addColumn<QString>("action");
        for (const auto* action : {"cancel", "no-op", "document", "null-document", "source", "revision", "selection", "active", "lock", "reentry"})
            QTest::newRow(action) << QString::fromLatin1(action);
    }

    void bindingKeyDialogCancellationAndStaleStateDoNotSubmit()
    {
        QFETCH(QString, action);
        using namespace live2dstruct;
        CModelDocument document, other;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3")); QVERIFY(other.Load(document.SourcePath()));
        const auto source = document.SourceBytes();
        CInspectorPanel panel; panel.SetDocument(&document); panel.SetSelection("EyeL4", "artmesh");
        QSignalSpy structure(&panel, &CInspectorPanel::StructureChanged), history(&panel, &CInspectorPanel::HistoryChanged);
        int calls = 0;
        panel.SetEditHandler([&](const CModelEdit&, QString*) { ++calls; return true; });
        QPointer<CBindingKeysDialog> pending;
        QVERIFY(withBindingDialog(&panel, [&](auto dialog) {
            pending = dialog;
            auto* table = dialog->template findChild<QTableView*>("bindingKeysTable");
            auto* apply = dialog->template findChild<QPushButton*>("bindingKeysApply");
            auto* error = dialog->template findChild<QLabel*>("bindingKeysError");
            QVERIFY(table && apply && error);
            if (action == "no-op") {
                QVERIFY(!apply->isEnabled()); apply->click(); QCOMPARE(calls, 0);
                dialog->ApplyRequested();
                QVERIFY(dialog->isVisible()); QVERIFY(!error->text().isEmpty()); return;
            }
            QVERIFY(table->model()->setData(table->model()->index(0, 0), QString("-0.5"), Qt::EditRole));
            if (action == "cancel") { dialog->reject(); return; }
            if (action == "reentry") {
                auto* button = panel.findChild<QPushButton*>("editBindingKeys"); QVERIFY(button);
                button->clicked(false);
                QCOMPARE(panel.findChildren<CBindingKeysDialog*>().size(), 1);
                QCOMPARE(panel.findChild<CBindingKeysDialog*>(), dialog.data()); return;
            }
            if (action == "document") panel.SetDocument(&other);
            else if (action == "null-document") panel.SetDocument(nullptr);
            else if (action == "source") QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Natori/Natori.moc3"));
            else if (action == "revision") QVERIFY(document.SetParameterValue("ParamEyeBallX", .25f));
            else if (action == "selection") panel.SetSelection("Hair2", "artmesh");
            else if (action == "active") QVERIFY(panel.SyncActiveKeyform("EyeL4", "artmesh", 1));
            else if (action == "lock") panel.SetLockedState({"artmesh|EyeL4"});
            const auto state = document.ParameterStructureState(); const auto revision = document.Revision();
            const auto input = table->model()->index(0, 0).data(Qt::EditRole);
            apply->click();
            QVERIFY(dialog && dialog->isVisible()); QVERIFY(!error->text().isEmpty());
            QCOMPARE(table->model()->index(0, 0).data(Qt::EditRole), input);
            QCOMPARE(document.ParameterStructureState(), state); QCOMPARE(document.Revision(), revision);
        }));
        QVERIFY(!pending); QCOMPARE(calls, 0); QCOMPARE(structure.count(), 0); QCOMPARE(history.count(), 0);
        QVERIFY(!document.CanUndoEdit());
        if (action != "source") QCOMPARE(document.SourceBytes(), source);
    }

    void bindingKeyDialogDoesNotOutliveItsPanel_data()
    {
        QTest::addColumn<int>("destruction");
        QTest::newRow("panel") << 0; QTest::newRow("window") << 1;
        QTest::newRow("submit-callback") << 2; QTest::newRow("throwing-submit-callback") << 3;
    }

    void bindingKeyDialogDoesNotOutliveItsPanel()
    {
        QFETCH(int, destruction);
        using namespace live2dstruct;
        CModelDocument document; QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto state = document.ParameterStructureState(); const auto revision = document.Revision();
        QPointer<QMainWindow> window = new QMainWindow;
        const auto cleanup = qScopeGuard([&] { if (window) delete window.data(); });
        QPointer<CInspectorPanel> panel = new CInspectorPanel(window);
        window->addDockWidget(Qt::RightDockWidgetArea, panel);
        panel->SetDocument(&document); panel->SetSelection("EyeL4", "artmesh");
        int calls = 0;
        panel->SetEditHandler([&](const CModelEdit&, QString*) {
            ++calls; delete panel.data();
            if (destruction == 3) throw std::runtime_error("panel destroyed");
            return true;
        });
        QPointer<CBindingKeysDialog> pending;
        QVERIFY(withBindingDialog(panel, [&](auto dialog) {
            pending = dialog;
            if (destruction == 0) delete panel.data();
            else if (destruction == 1) delete window.data();
            else {
                auto* table = dialog->template findChild<QTableView*>("bindingKeysTable"); QVERIFY(table);
                QVERIFY(table->model()->setData(table->model()->index(0, 0), QString("-0.5"), Qt::EditRole));
                dialog->ApplyRequested();
            }
            QVERIFY(!panel); QVERIFY(!pending);
        }));
        QVERIFY(!panel); QVERIFY(!pending); QCOMPARE(calls, destruction >= 2 ? 1 : 0);
        QCOMPARE(document.ParameterStructureState(), state); QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit());
    }

    void callbackPanelDestructionStopsEverySubmissionPath_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<int>("outcome");
        for (const auto* path : {"edit", "history", "artmesh", "warp", "rotation"}) for (int outcome = 0; outcome < 3; ++outcome)
            QTest::newRow(qPrintable(QString("%1-%2").arg(path).arg(outcome))) << QString::fromLatin1(path) << outcome;
    }

    void callbackPanelDestructionStopsEverySubmissionPath()
    {
        QFETCH(QString, path); QFETCH(int, outcome);
        using namespace live2dstruct;
        CModelDocument document;
        const QString model = path == "rotation" ? "Natori" : "Mark";
        QVERIFY(document.Load(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3"));
        const QString type = path == "warp" || path == "rotation" ? path : QString("artmesh");
        const QString id = type == "warp" ? "Warp6" : type == "rotation" ? "Rotation2" : "EyeL4";
        if (path == "history") {
            const auto target = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(), [&](const auto& mesh) { return mesh.id == id; });
            QVERIFY(target != document.Drawables().cend());
            QVERIFY(document.ExecuteEdit({id, type, CModelEdit::Opacity, document.DrawableOpacity(*target), .25f}));
        }
        const auto state = document.ParameterStructureState(); const auto revision = document.Revision();
        QPointer<CInspectorPanel> panel = new CInspectorPanel;
        const auto cleanup = qScopeGuard([&] { if (panel) delete panel.data(); });
        panel->SetDocument(&document); panel->SetSelection(id, type);
        QSignalSpy history(panel, &CInspectorPanel::HistoryChanged), rejected(panel, &CInspectorPanel::EditRejected);
        int calls = 0;
        const auto destroy = [&]() -> bool {
            ++calls; delete panel.data();
            if (outcome == 2) throw std::runtime_error("destroyed in callback");
            return outcome == 1;
        };
        bool stableArguments = true;
        const auto destroyWithTarget = [&](const QString& name, const QString& requestedType) {
            const auto check = qScopeGuard([&] { stableArguments = name == id && requestedType == type; });
            return destroy();
        };
        if (path == "edit") {
            panel->SetEditHandler([&](const CModelEdit&, QString*) { return destroy(); });
            auto* table = panel->findChild<QTableWidget*>("keyformVertexTable"); QVERIFY(table && table->rowCount());
            {
                const QSignalBlocker blocked(table);
                table->item(0, 1)->setText(QString::number(table->item(0, 1)->text().toFloat() + .125f, 'g', 9));
            }
            table->cellChanged(0, 1);
        } else if (path == "history") {
            panel->SetHistoryHandler([&](bool, CModelEdit&, QString*) { return destroy(); });
            panel->Undo();
        } else if (path == "artmesh") {
            panel->SetKeyformHandler([&](const QString& name, int, QString*) { return destroyWithTarget(name, type); });
            auto* combo = panel->findChild<QComboBox*>("keyformSelector"); QVERIFY(combo && combo->count() > 1);
            combo->activated(1);
        } else {
            panel->SetDeformerKeyformHandler([&](const QString& name, const QString& requestedType, int, QString*) {
                return destroyWithTarget(name, requestedType);
            });
            if (path == "warp") {
                auto* combo = panel->findChild<QComboBox*>("warpKeyformSelector"); QVERIFY(combo && combo->count() > 1);
                combo->activated(1);
            } else {
                auto* table = panel->findChild<QTableWidget*>("rotationKeyformTable"); QVERIFY(table && table->rowCount() > 1);
                table->currentCellChanged(1, 0, 0, 0);
            }
        }
        QCOMPARE(calls, 1); QVERIFY(!panel); QVERIFY(stableArguments); QCOMPARE(history.count(), 0); QCOMPARE(rejected.count(), 0);
        QCOMPARE(document.ParameterStructureState(), state); QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.CanUndoEdit(), path == "history");
        QCoreApplication::processEvents();
    }

    void matrixNavigationUsesTheExistingPreviewHandler_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("type");
        QTest::addColumn<bool>("accepted");
        for (bool accepted : {false, true}) {
            const auto suffix = accepted ? "accept" : "reject";
            QTest::newRow(qPrintable(QString("mesh-") + suffix)) << QString("Mark") << QString("EyeL4") << QString("artmesh") << accepted;
            QTest::newRow(qPrintable(QString("warp-") + suffix)) << QString("Mark") << QString("Warp6") << QString("warp") << accepted;
            QTest::newRow(qPrintable(QString("rotation-") + suffix)) << QString("Natori") << QString("Rotation2") << QString("rotation") << accepted;
        }
    }

    void matrixNavigationUsesTheExistingPreviewHandler()
    {
        QFETCH(QString, model);
        QFETCH(QString, id);
        QFETCH(QString, type);
        QFETCH(bool, accepted);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3"));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, type);
        auto* matrix = panel.findChild<CKeyformMatrix*>("keyformMatrix");
        QVERIFY(matrix);
        QCOMPARE(matrix->ActiveKeyform(), 0);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        int requests = 0;
        const auto handler = [&](const QString& name, const QString& requestedType, int keyform, QString* error) {
            ++requests;
            if (name != id || requestedType != type || keyform != 5) return false;
            if (!accepted) { *error = QStringLiteral("preview rejected"); return false; }
            QMap<QString, float> pose;
            return document.KeyformParameterValues(name, requestedType, keyform, pose, error);
        };
        panel.SetKeyformHandler([&](const QString& name, int keyform, QString* error) { return handler(name, "artmesh", keyform, error); });
        panel.SetDeformerKeyformHandler(handler);
        const auto revision = document.Revision();
        matrix->KeyformRequested(5);
        QCOMPARE(requests, 1);
        QCOMPARE(rejected.count(), accepted ? 0 : 1);
        QCOMPARE(matrix->ActiveKeyform(), accepted ? 5 : 0);
        if (type == "artmesh") QCOMPARE(panel.GetActiveKeyformIndex(), accepted ? 5 : 0);
        else if (type == "warp") QCOMPARE(panel.findChild<QComboBox*>("warpKeyformSelector")->currentIndex(), accepted ? 5 : 0);
        else QCOMPARE(panel.findChild<QTableWidget*>("rotationKeyformTable")->currentRow(), accepted ? 5 : 0);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
        panel.SetLockedState({type + '|' + id});
        matrix->KeyformRequested(4);
        QCOMPARE(requests, 1);
        panel.ClearSelection();
        QCOMPARE(matrix->ActiveKeyform(), -1);
        QVERIFY(matrix->isHidden());
    }

    void matrixRechecksChangedRangesWithoutDiscardingParameterNavigation()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection("EyeL4", "artmesh");
        auto* matrix = panel.findChild<CKeyformMatrix*>("keyformMatrix");
        QVERIFY(matrix);
        int requests = 0;
        panel.SetKeyformHandler([&](const QString&, int, QString*) { ++requests; return true; });
        QVERIFY(document.SetParameterValue("ParamEyeBallX", .5f));
        matrix->KeyformRequested(2);
        QCOMPARE(requests, 1);
        QCOMPARE(matrix->ActiveKeyform(), 2);
        auto metadata = document.ParameterMetadata("ParamEyeBallX");
        metadata.minimum = 0;
        QVERIFY(document.SetParameterMetadata("ParamEyeBallX", metadata));
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        matrix->KeyformRequested(0);
        QCOMPARE(requests, 1);
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(matrix->ActiveKeyform(), 2);
        QVERIFY(!document.CanUndoEdit());
    }

    void topologyActionsExplainAndPreserveTheirDomain_data()
    {
        QTest::addColumn<QString>("shape");
        QTest::addColumn<QVector<float>>("positions");
        QTest::addColumn<QVector<quint16>>("indices");
        QTest::addColumn<QString>("action");
        const QVector<QVector<float>> points{{0,0,4,0,4,1,1,1,1,4,0,4},
            {-2,-2,2,-2,2,2,-2,2,-1,-1,1,-1,1,1,-1,1}, {0,0,1,0,0,1,3,0,4,0,4,1}, {0,0,4,0,3,1,0,3}};
        const QVector<QVector<quint16>> faces{{0,1,3,1,2,3,0,3,5,3,4,5},
            {0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7}, {0,1,2,3,4,5}, {0,1,3,1,2,3}};
        const QStringList shapes{"concave", "ring", "disconnected", "quad"};
        for (int index = 0; index < shapes.size(); ++index)
            for (const QString& action : {QString("optimize"), QString("rebuild"), QString("cancel"), QString("reject")})
                QTest::newRow(qPrintable(shapes[index] + '-' + action)) << shapes[index] << points[index] << faces[index] << action;
    }

    void topologyActionsExplainAndPreserveTheirDomain()
    {
        QFETCH(QString, shape);
        QFETCH(QVector<float>, positions);
        QFETCH(QVector<quint16>, indices);
        QFETCH(QString, action);
        using namespace live2dstruct;
        CModelDocument document;
        QString id, error;
        QVERIFY2(topologyDocument(positions, indices, document, id, &error), qPrintable(error));
        const CModelDocument original = document;
        const auto uvs = document.Drawables().first().uvs;
        const auto revision = document.Revision();
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, "artmesh");
        QSignalSpy changes(&panel, &CInspectorPanel::DrawableIndicesChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        int submissions = 0, dialogs = 0;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* message) {
            ++submissions;
            if (action == "reject") { *message = "candidate rejected"; return false; }
            return document.ExecuteEdit(edit);
        });
        auto* button = panel.findChild<QPushButton*>(action == "optimize" ? "optimizeTopology" : "autoTriangulate");
        QVERIFY(button && button->isEnabled());
        QVERIFY(button->toolTip().contains(QStringLiteral("孔洞")));
        QTimer responder;
        connect(&responder, &QTimer::timeout, &panel, [&] {
            if (auto* dialog = panel.findChild<QMessageBox*>("rebuildTopologyConfirmation")) {
                const auto close = qScopeGuard([&] { if (dialog->isVisible()) dialog->reject(); });
                ++dialogs;
                QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
                QVERIFY(dialog->text().contains(QString::number(positions.size() / 2)));
                QVERIFY(dialog->informativeText().contains(QStringLiteral("所有关键形")));
                dialog->done(action == "cancel" ? QMessageBox::Cancel : QMessageBox::Yes);
            }
        });
        responder.start(0);
        button->click();
        responder.stop();
        const auto after = document.DrawableIndices(id);
        const bool changed = action == "optimize" ? after != indices : action == "rebuild";
        QCOMPARE(dialogs, action == "optimize" ? 0 : 1);
        QCOMPARE(submissions, action == "optimize" ? int(changed) : action == "cancel" ? 0 : 1);
        QCOMPARE(changes.count(), int(changed));
        QCOMPARE(rejected.count(), action == "reject" ? 1 : 0);
        if (action == "optimize") {
            QCOMPARE(topologyBoundary(after), topologyBoundary(indices));
            QCOMPARE(topologyArea(positions, after), topologyArea(positions, indices));
            if (shape == "quad") QVERIFY(changed);
        } else if (action == "rebuild") {
            QVERIFY(after != indices);
            if (shape != "quad") QVERIFY(topologyArea(positions, after) > topologyArea(positions, indices));
        } else {
            QCOMPARE(after, indices);
            QCOMPARE(document.Revision(), revision);
        }
        QCOMPARE(document.Drawables().first().uvs, uvs);
        for (int form = 0; form < document.Drawables().first().keyforms.size(); ++form)
            QCOMPARE(document.DrawableKeyformPositions(id, form), original.DrawableKeyformPositions(id, form));
        QCOMPARE(document.SourceBytes(), original.SourceBytes());
        QCOMPARE(document.CanUndoEdit(), changed);
        if (changed) {
            QByteArray compiled;
            QVERIFY2(document.BuildRuntimeMoc(compiled, &error), qPrintable(error));
            CModelDocument decoded;
            QVERIFY2(decoded.LoadBytes(compiled, "rebuilt.moc3", &error), qPrintable(error));
            QCOMPARE(decoded.DrawableIndices(id), after);
            panel.Undo();
            QCOMPARE(document.DrawableIndices(id), indices);
            QVERIFY(!document.CanUndoEdit());
            panel.Redo();
            QCOMPARE(document.DrawableIndices(id), after);
            QVERIFY(!document.CanRedoEdit());
            const auto acceptedRevision = document.Revision();
            const int acceptedSubmissions = submissions;
            responder.start(0);
            button->click();
            responder.stop();
            QCOMPARE(document.Revision(), acceptedRevision);
            QCOMPARE(submissions, acceptedSubmissions);
            QCOMPARE(dialogs, action == "optimize" ? 0 : 1);
        }
    }

    void rebuildingTopologyRejectsStaleConfirmation_data()
    {
        QTest::addColumn<QString>("mutation");
        for (const QString& mutation : {QString("document"), QString("keyform"), QString("selection"),
                                       QString("locked"), QString("cleared"), QString("nested")})
            QTest::newRow(qPrintable(mutation)) << mutation;
    }

    void rebuildingTopologyRejectsStaleConfirmation()
    {
        QFETCH(QString, mutation);
        using namespace live2dstruct;
        CModelDocument document;
        QString id, error;
        const QVector<float> positions{0,0,4,0,4,1,1,1,1,4,0,4};
        const QVector<quint16> indices{0,1,3,1,2,3,0,3,5,3,4,5};
        QVERIFY2(topologyDocument(positions, indices, document, id, &error, mutation == "keyform"), qPrintable(error));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, "artmesh");
        auto* button = panel.findChild<QPushButton*>("autoTriangulate");
        QVERIFY(button);
        int submissions = 0, dialogs = 0;
        panel.SetEditHandler([&](const CModelEdit& edit, QString*) { ++submissions; return document.ExecuteEdit(edit); });
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        QTimer::singleShot(0, &panel, [&] {
            auto* dialog = panel.findChild<QMessageBox*>("rebuildTopologyConfirmation");
            QVERIFY(dialog);
            const auto close = qScopeGuard([&] { if (dialog->isVisible()) dialog->reject(); });
            ++dialogs;
            if (mutation == "document") {
                auto changed = positions; changed[0] += .125f;
                QVERIFY(document.SetDrawableKeyformPositions(id, 0, changed));
            } else if (mutation == "keyform") {
                auto* combo = panel.findChild<QComboBox*>("keyformSelector");
                QVERIFY(combo && combo->count() > 1);
                combo->setCurrentIndex(1);
            } else if (mutation == "selection") panel.SetSelection(document.Drawables().at(1).id, "artmesh");
            else if (mutation == "locked") panel.SetLockedState({"artmesh|" + id});
            else if (mutation == "cleared") panel.SetDocument(nullptr);
            else { button->click(); dialog->done(QMessageBox::Cancel); return; }
            dialog->done(QMessageBox::Yes);
        });
        button->click();
        QCOMPARE(dialogs, 1);
        QCOMPARE(submissions, 0);
        QCOMPARE(rejected.count(), mutation == "nested" ? 0 : 1);
        QCOMPARE(document.DrawableIndices(id), indices);
        QVERIFY(!document.CanUndoEdit());
    }

    void signedRotationEditsUseHistoryAndCommitter_data()
    {
        QTest::addColumn<float>("sourceScale");
        QTest::addColumn<int>("column");
        QTest::addColumn<bool>("accepted");
        for (float scale : {-2.0f, 0.0f, 2.0f})
            for (int column = 1; column <= 6; ++column)
                for (bool accepted : {false, true})
                    QTest::newRow(qPrintable(QStringLiteral("%1-column%2-%3").arg(scale).arg(column)
                                            .arg(accepted ? "accept" : "reject"))) << scale << column << accepted;
    }

    void signedRotationEditsUseHistoryAndCommitter()
    {
        QFETCH(float, sourceScale);
        QFETCH(int, column);
        QFETCH(bool, accepted);
        using namespace live2dstruct;
        QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto bytes = file.readAll();
        auto reader = CReader::FromBytes(bytes);
        CSectionTable sections(reader);
        QVERIFY(sections.IsValid());
        const auto deformer = std::find_if(sections.deformers.cbegin(), sections.deformers.cend(),
                                         [](const auto* value) { return value->types == 1; });
        QVERIFY(deformer != sections.deformers.cend());
        const QString id = (*deformer)->ids;
        const auto* rotation = sections.rotation_deformers[(*deformer)->specific_sources_indices];
        quint32 scaleBits;
        std::memcpy(&scaleBits, &sourceScale, sizeof(scaleBits));
        for (int row = 0; row < rotation->keyform_sources_counts; ++row)
            qToLittleEndian(scaleBits, bytes.data() + sections.sectionOffsetTable->rotation_deformer_keyforms->scales +
                            (rotation->keyform_sources_begin_indices + row) * 4);
        CModelDocument document;
        QVERIFY(document.LoadBytes(bytes, file.fileName()));
        const auto before = document.DeformerRotationKeyforms(id);
        QVERIFY(!before.isEmpty());
        auto after = before;
        QString text;
        if (column == 1) { after[0].angle += 7.5f; text = QString::number(after[0].angle, 'g', 9); }
        else if (column == 2) { after[0].originX += 0.25f; text = QString::number(after[0].originX, 'g', 9); }
        else if (column == 3) { after[0].originY -= 0.25f; text = QString::number(after[0].originY, 'g', 9); }
        else if (column == 4) { after[0].scale = -3.0f; text = QStringLiteral("-3"); }
        else if (column == 5) { after[0].reflectX = !after[0].reflectX; text = after[0].reflectX ? "true" : "false"; }
        else { after[0].reflectY = !after[0].reflectY; text = after[0].reflectY ? "true" : "false"; }
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("rotation"));
        auto* table = panel.findChild<QTableWidget*>("rotationKeyformTable");
        QVERIFY(table && table->rowCount() == before.size() && table->isEnabled());
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        int calls = 0;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++calls;
            if (!accepted) { *error = QStringLiteral("candidate rejected"); return false; }
            return document.ExecuteEdit(edit);
        });
        const auto revision = document.Revision();
        table->item(0, column)->setText(text);
        QCOMPARE(calls, 1);
        QCOMPARE(history.count(), accepted ? 1 : 0);
        QCOMPARE(rejected.count(), accepted ? 0 : 1);
        QCOMPARE(document.DeformerRotationKeyforms(id), accepted ? after : before);
        QCOMPARE(table->item(0, 4)->text().toFloat(), accepted ? after[0].scale : before[0].scale);
        QCOMPARE(document.CanUndoEdit(), accepted);
        if (accepted) {
            panel.Undo();
            QCOMPARE(document.DeformerRotationKeyforms(id), before);
            QVERIFY(!document.CanUndoEdit());
            panel.Redo();
            QCOMPARE(document.DeformerRotationKeyforms(id), after);
            QVERIFY(!document.CanRedoEdit());
        } else {
            QCOMPARE(document.Revision(), revision);
            QVERIFY(!document.CanRedoEdit());
        }
        QCOMPARE(document.SourceBytes(), bytes);
    }

    void invalidRotationNumbersLeaveTableAndHistoryUnchanged_data()
    {
        QTest::addColumn<int>("column");
        QTest::addColumn<QString>("text");
        for (int column = 1; column <= 4; ++column)
            for (const QString& text : {QStringLiteral("nan"), QStringLiteral("inf"), QStringLiteral("-inf"),
                                        QStringLiteral("1e100"), QStringLiteral("-1e100")})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(column).arg(text))) << column << text;
    }

    void invalidRotationNumbersLeaveTableAndHistoryUnchanged()
    {
        QFETCH(int, column);
        QFETCH(QString, text);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto deformer = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(),
                                         [](const auto& value) { return value.type == CModelDeformer::Rotation; });
        QVERIFY(deformer != document.Deformers().cend());
        const QString id = deformer->id;
        auto forms = document.DeformerRotationKeyforms(id);
        forms[0].scale = -2.0f;
        QVERIFY(document.SetDeformerRotationKeyforms(id, forms));
        const auto revision = document.Revision();
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("rotation"));
        int calls = 0;
        panel.SetEditHandler([&](const CModelEdit&, QString*) { ++calls; return true; });
        auto* table = panel.findChild<QTableWidget*>("rotationKeyformTable");
        QVERIFY(table && table->rowCount());
        const QString before = table->item(0, column)->text();
        table->item(0, column)->setText(text);
        QCOMPARE(table->item(0, column)->text(), before);
        QCOMPARE(calls, 0);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.DeformerRotationKeyforms(id), forms);
        QVERIFY(!document.CanUndoEdit());
    }

    void narrowInspectorKeepsLongChoicesScrollableAndRowsSynchronized_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }

    void narrowInspectorKeepsLongChoicesScrollableAndRowsSynchronized()
    {
        QFETCH(bool, dark);
        const auto previousStyle = qApp->styleSheet();
        const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(previousStyle); });
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString mesh = document.Drawables().first().id;
        QString warp, rotation;
        for (const auto& deformer : document.Deformers()) {
            if (warp.isEmpty() && deformer.type == CModelDeformer::Warp) warp = deformer.id;
            if (rotation.isEmpty() && deformer.type == CModelDeformer::Rotation) rotation = deformer.id;
        }
        QVERIFY(!warp.isEmpty() && !rotation.isEmpty());
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(mesh, "artmesh");
        panel.resize(300, 420);
        panel.show();
        auto* scroll = panel.findChild<QScrollArea*>("inspectorScrollArea");
        auto* form = panel.findChild<QFormLayout*>("inspectorForm");
        QVERIFY(scroll && form);
        QVERIFY(scroll->widgetResizable());
        QCOMPARE(scroll->frameShape(), QFrame::NoFrame);
        QTRY_COMPARE(panel.size(), QSize(300, 420));
        QVERIFY(panel.minimumSizeHint().width() <= 300);
        QVERIFY(panel.minimumSizeHint().height() <= 420);
        auto* art = panel.findChild<QComboBox*>("keyformSelector");
        auto* parent = panel.findChild<QComboBox*>("deformerParentSelector");
        auto* warpCombo = panel.findChild<QComboBox*>("warpKeyformSelector");
        auto* artTable = panel.findChild<QTableWidget*>("keyformVertexTable");
        auto* warpTable = panel.findChild<QTableWidget*>("warpKeyformTable");
        auto* rotationTable = panel.findChild<QTableWidget*>("rotationKeyformTable");
        const QString longName = QStringLiteral("长关键形和父变形器名称_").repeated(40);
        const auto revision = document.Revision();
        for (const auto& selection : QVector<QPair<QString, QString>>{{mesh, "artmesh"}, {warp, "warp"}, {rotation, "rotation"}, {mesh, "artmesh"}}) {
            panel.SetSelection(selection.first, selection.second);
            for (QComboBox* combo : {art, parent, warpCombo}) if (combo->count()) {
                const int current = combo->currentIndex();
                combo->setItemText(current, longName);
                QCOMPARE(combo->currentText(), longName);
                QVERIFY(combo->toolTip().contains(longName));
                QCOMPARE(combo->view()->textElideMode(), Qt::ElideNone);
                QCOMPARE(combo->view()->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
            }
            QCoreApplication::processEvents();
            QCOMPARE(panel.size(), QSize(300, 420));
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            for (QTableWidget* table : {artTable, warpTable, rotationTable}) if (!table->isHidden() && table->rowCount()) {
                for (int column : {1, 2}) {
                    QVERIFY(table->item(0, column));
                    QVERIFY(table->columnWidth(column) >= table->fontMetrics().horizontalAdvance(table->item(0, column)->text()));
                }
            }
            for (const auto& pair : QVector<QPair<QWidget*, bool>>{{artTable, selection.second == "artmesh"},
                     {warpTable, selection.second == "warp"}, {rotationTable, selection.second == "rotation"},
                     {warpCombo, selection.second == "warp"}, {parent, selection.second != "artmesh"}}) {
                QCOMPARE(form->isRowVisible(pair.first), pair.second);
                auto* label = form->labelForField(pair.first);
                QVERIFY(label);
                QCOMPARE(label->isHidden(), !pair.second);
                QCOMPARE(pair.first->isHidden(), !pair.second);
            }
        }
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        qInfo() << "narrow inspector" << panel.size() << "minimum" << panel.minimumSizeHint()
                << "content minimum" << scroll->widget()->minimumSizeHint();
        const QString pictures = qEnvironmentVariable("LIVE2D_INSPECTOR_SCREENSHOTS");
        const QString appearance = dark ? QStringLiteral("dark") : QStringLiteral("light");
        if (!pictures.isEmpty()) {
            QVERIFY(QDir().mkpath(pictures));
            scroll->verticalScrollBar()->setValue(0);
            QCoreApplication::processEvents();
            QVERIFY(panel.grab().save(pictures + '/' + appearance + "-top.png"));
            scroll->ensureWidgetVisible(artTable);
            QCoreApplication::processEvents();
            QVERIFY(panel.grab().save(pictures + '/' + appearance + "-table.png"));
        }
        scroll->ensureWidgetVisible(art);
        art->showPopup();
        QTRY_VERIFY(art->view()->isVisible());
        QCOMPARE(art->view()->model()->index(art->currentIndex(), 0).data().toString(), longName);
        const int completeWidth = art->view()->fontMetrics().horizontalAdvance(longName);
        qInfo() << "popup text width" << completeWidth << "viewport" << art->view()->viewport()->width()
                << "horizontal range" << art->view()->horizontalScrollBar()->maximum();
        QVERIFY(completeWidth <= art->view()->viewport()->width() || art->view()->horizontalScrollBar()->maximum() > 0);
        if (!pictures.isEmpty()) QVERIFY(art->view()->window()->grab().save(pictures + '/' + appearance + "-choices.png"));
        art->hidePopup();
        QCOMPARE(document.Revision(), revision);
        panel.ClearSelection();
        for (int row = 0; row < form->rowCount(); ++row) QVERIFY(!form->isRowVisible(row));
    }

    void inspectorKeyboardScrollAndPendingCellSurviveResize()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Drawables().first().id;
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, "artmesh");
        panel.resize(300, 420);
        panel.show();
        panel.activateWindow();
        auto* scroll = panel.findChild<QScrollArea*>("inspectorScrollArea");
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(scroll && combo && table && table->rowCount() > 0);
        combo->setFocus();
        QTRY_VERIFY(combo->hasFocus());
        bool reached = false;
        for (int step = 0; step < 50; ++step) {
            auto* focused = QApplication::focusWidget();
            if (focused == table || (focused && table->isAncestorOf(focused))) { reached = true; break; }
            QVERIFY(focused);
            QTest::keyClick(focused, Qt::Key_Tab);
        }
        QVERIFY(reached);
        QVERIFY(scroll->verticalScrollBar()->value() > 0);
        table->setCurrentCell(0, 1);
        table->editItem(table->item(0, 1));
        auto* editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());
        const QString original = table->item(0, 1)->text();
        editor->selectAll();
        QTest::keyClicks(editor, "0.123456");
        const auto revision = document.Revision();
        QSignalSpy changed(&panel, &CInspectorPanel::HistoryChanged);
        const QPointer<QLineEdit> savedEditor(editor);
        panel.resize(360, 460);
        scroll->verticalScrollBar()->setValue(0);
        QCoreApplication::processEvents();
        QVERIFY(savedEditor);
        QCOMPARE(savedEditor->text(), QString("0.123456"));
        QCOMPARE(table->item(0, 1)->text(), original);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(changed.size(), 0);
        panel.resize(300, 420);
        scroll->ensureWidgetVisible(savedEditor);
        QCoreApplication::processEvents();
        QVERIFY(savedEditor && savedEditor->hasFocus());
        QCOMPARE(savedEditor->text(), QString("0.123456"));
        QTest::keyClick(savedEditor, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(table->item(0, 1)->text(), original);
    }

    void destroyingFocusedFloatingKeyformEditorDoesNotSubmit_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<int>("destruction");
        for (const auto* type : {"artmesh", "warp", "rotation"})
            for (int destruction = 0; destruction < 3; ++destruction)
                QTest::newRow(qPrintable(QString("%1-%2").arg(type).arg(destruction))) << QString(type) << destruction;
    }

    void destroyingFocusedFloatingKeyformEditorDoesNotSubmit()
    {
        using namespace live2dstruct;
        QFETCH(QString, type);
        QFETCH(int, destruction);
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QString id;
        if (type == "artmesh") {
            for (const auto& drawable : document.Drawables())
                if (!drawable.keyforms.isEmpty()) { id = drawable.id; break; }
        } else {
            for (const auto& deformer : document.Deformers()) {
                if (type == "warp" && deformer.type == CModelDeformer::Warp && !deformer.warpKeyforms.isEmpty()) {
                    id = deformer.id; break;
                }
                if (type == "rotation" && deformer.type == CModelDeformer::Rotation && !deformer.rotationKeyforms.isEmpty()) {
                    id = deformer.id; break;
                }
            }
        }
        QVERIFY(!id.isEmpty());
        int calls = 0;
        auto window = std::make_unique<QMainWindow>();
        QPointer<CInspectorPanel> panel = new CInspectorPanel(window.get());
        panel->SetDocument(&document);
        panel->SetSelection(id, QString::fromUtf8(type.toUtf8()));
        panel->SetEditHandler([&](const CModelEdit& edit, QString*) {
            ++calls;
            return document.ExecuteEdit(edit);
        });
        window->addDockWidget(Qt::RightDockWidgetArea, panel);
        window->show();
        panel->setFloating(true);
        panel->resize(700, 1100);
        panel->show();
        panel->activateWindow();
        const auto tableName = type == "artmesh" ? "keyformVertexTable" : type == "warp" ? "warpKeyformTable" : "rotationKeyformTable";
        auto* table = panel->findChild<QTableWidget*>(tableName);
        QVERIFY(table && table->isEnabled() && table->rowCount() > 0);
        table->setCurrentCell(0, 1);
        const auto previous = table->item(0, 1)->text();
        table->editItem(table->item(0, 1));
        auto* editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());
        editor->selectAll();
        QTest::keyClicks(editor, qPrintable(QString::number(previous.toDouble() + 0.25, 'g', 9)));
        QCOMPARE(table->item(0, 1)->text(), previous);
        const auto revision = document.Revision();
        QCOMPARE(calls, 0);
        if (destruction == 2) {
            window->removeDockWidget(panel);
            QCOMPARE(calls, 1);
            QCOMPARE(document.Revision(), revision + 1);
        }
        const auto beforeDestruction = document.Revision();
        const int submitted = calls;
        if (destruction == 1) window.reset();
        else delete panel.data();
        QCoreApplication::processEvents();
        QVERIFY(!panel);
        QCOMPARE(calls, submitted);
        QCOMPARE(document.Revision(), beforeDestruction);
    }

    void synchronizedKeyformTracksTheDocumentWithoutActivation_data()
    {
        QTest::addColumn<QString>("type");
        for (const char* type : {"artmesh", "warp", "rotation"}) QTest::newRow(type) << QString::fromLatin1(type);
    }

    void synchronizedKeyformTracksTheDocumentWithoutActivation()
    {
        using namespace live2dstruct;
        QFETCH(QString, type);
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Natori/Natori.moc3"));
        QString id;
        int count = 0;
        if (type == "artmesh") {
            for (const auto& mesh : document.Drawables()) if (mesh.keyforms.size() > 1) { id = mesh.id; count = mesh.keyforms.size(); break; }
        } else {
            for (const auto& deformer : document.Deformers()) {
                if ((type == "warp") != (deformer.type == CModelDeformer::Warp)) continue;
                const int frames = type == "warp" ? deformer.warpKeyforms.size() : deformer.rotationKeyforms.size();
                if (frames > 1) { id = deformer.id; count = frames; break; }
            }
        }
        QVERIFY(!id.isEmpty());
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, type);
        int requests = 0;
        panel.SetKeyformHandler([&](const QString&, int, QString*) { ++requests; return false; });
        panel.SetDeformerKeyformHandler([&](const QString&, const QString&, int, QString*) { ++requests; return false; });
        QSignalSpy activated(&panel, &CInspectorPanel::DrawableKeyformActivated);
        QSignalSpy edited(&panel, &CInspectorPanel::HistoryChanged);
        const auto revision = document.Revision();
        QVERIFY(panel.SyncActiveKeyform(id, type, count - 1));
        QCOMPARE(requests, 0);
        QCOMPARE(activated.size(), 0);
        QCOMPARE(edited.size(), 0);
        QCOMPARE(document.Revision(), revision);
        if (type == "artmesh") {
            QCOMPARE(panel.GetActiveKeyformIndex(), count - 1);
            QCOMPARE(panel.findChild<QComboBox*>("keyformSelector")->currentIndex(), count - 1);
            const auto values = document.DrawableKeyformPositions(id, count - 1);
            QCOMPARE(panel.findChild<QTableWidget*>("keyformVertexTable")->item(0, 1)->text(), QString::number(values[0], 'g', 9));
        } else if (type == "warp") {
            QCOMPARE(panel.findChild<QComboBox*>("warpKeyformSelector")->currentIndex(), count - 1);
            QCOMPARE(panel.findChild<QTableWidget*>("warpKeyformTable")->item(0, 1)->text(),
                     QString::number(document.DeformerWarpKeyforms(id)[count - 1].positions[0], 'g', 9));
        } else QCOMPARE(panel.findChild<QTableWidget*>("rotationKeyformTable")->currentRow(), count - 1);
        QVERIFY(!panel.SyncActiveKeyform(id, type, count));
        QVERIFY(!panel.SyncActiveKeyform("missing", type, 0));
        QCOMPARE(panel.GetSelectionId(), id);
        QCOMPARE(panel.GetSelectionType(), type);
        QCOMPARE(requests, 0);
        QCOMPARE(document.Revision(), revision);
    }

    void hiddenSelectionBecomesVisibleWithOneClick()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString first = document.Drawables()[0].id, second = document.Drawables()[1].id;
        const QMap<QString, bool> hidden{{"artmesh|" + first, false}, {"artmesh|" + second, false}};
        QVERIFY(document.SetVisibilityValues(hidden));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelections({first, second}, {"artmesh", "artmesh"});
        auto* check = panel.findChild<QCheckBox*>();
        QVERIFY(check && check->isEnabled());
        QCOMPARE(check->checkState(), Qt::Unchecked);
        QSignalSpy batch(&panel, &CInspectorPanel::VisibilityBatchChanged);
        const auto revision = document.Revision();
        check->click();
        QCOMPARE(check->checkState(), Qt::Checked);
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(batch.size(), 1);
        const QMap<QString, bool> visible{{"artmesh|" + first, true}, {"artmesh|" + second, true}};
        QCOMPARE((batch[0][0].value<QMap<QString, bool>>()), visible);
        panel.Undo();
        QCOMPARE(check->checkState(), Qt::Unchecked);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(check->checkState(), Qt::Checked);
        check->click();
        QCOMPARE(check->checkState(), Qt::Unchecked);
        QCOMPARE((batch.last()[0].value<QMap<QString, bool>>()), hidden);
    }

    void batchVisibilitySkipsLockedItemsAndKeepsMixedState()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QVERIFY(document.Drawables().size() >= 3);
        const QString locked = document.Drawables()[0].id;
        const QString editable = document.Drawables()[1].id;
        const QString visible = document.Drawables()[2].id;
        QVERIFY(document.SetVisibilityValues({{"artmesh|" + locked, false}, {"artmesh|" + editable, false},
                                              {"artmesh|" + visible, true}}));
        document.SetLocked("artmesh", locked, true);
        const auto original = document.VisibilityState();
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelections({locked, editable, visible}, {"artmesh", "artmesh", "artmesh"});
        panel.SetLockedState({"artmesh|" + locked});
        auto* check = panel.findChild<QCheckBox*>();
        QVERIFY(check && check->isEnabled());
        QCOMPARE(check->checkState(), Qt::PartiallyChecked);
        auto* opacity = panel.findChild<QDoubleSpinBox*>();
        auto* order = panel.findChild<QSpinBox*>();
        QVERIFY(opacity && order);
        QVERIFY(!opacity->isEnabled());
        QVERIFY(!order->isEnabled());
        QSignalSpy batch(&panel, &CInspectorPanel::VisibilityBatchChanged);
        QSignalSpy single(&panel, &CInspectorPanel::VisibilityChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        const auto revision = document.Revision();
        check->click();
        const QMap<QString, bool> changed{{"artmesh|" + editable, true}};
        auto expected = original;
        expected["artmesh|" + editable] = true;
        QCOMPARE(document.VisibilityState(), expected);
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(check->checkState(), Qt::PartiallyChecked);
        QCOMPARE(batch.size(), 1);
        QCOMPARE((batch[0][0].value<QMap<QString, bool>>()), changed);
        QCOMPARE(single.size(), 0);
        QCOMPARE(history.size(), 1);
        check->click();
        QCOMPARE(document.VisibilityState(), expected);
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(check->checkState(), Qt::PartiallyChecked);
        QCOMPARE(batch.size(), 1);
        QCOMPARE(history.size(), 1);
        panel.Undo();
        QCOMPARE(document.VisibilityState(), original);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(check->checkState(), Qt::PartiallyChecked);
        QCOMPARE(batch.size(), 2);
        const QMap<QString, bool> restored{{"artmesh|" + editable, false}};
        QCOMPARE((batch[1][0].value<QMap<QString, bool>>()), restored);
        panel.Redo();
        QCOMPARE(document.VisibilityState(), expected);
        QCOMPARE(check->checkState(), Qt::PartiallyChecked);
        QCOMPARE(batch.size(), 3);
        QCOMPARE((batch[2][0].value<QMap<QString, bool>>()), changed);
        QCOMPARE(history.size(), 3);
        QVERIFY(document.IsLocked("artmesh", locked));
        panel.SetLockedState({"artmesh|" + locked, "artmesh|" + editable, "artmesh|" + visible});
        QVERIFY(!check->isEnabled());
        const auto acceptedRevision = document.Revision();
        check->click();
        QCOMPARE(document.Revision(), acceptedRevision);
        QCOMPARE(document.VisibilityState(), expected);
        QCOMPARE(batch.size(), 3);
    }

    void mixedTypedVisibilityPublishesOneAtomicBatch()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        auto bytes = document.SourceBytes();
        const QByteArray shared("VisibilityShared");
        for (int slot : {0x4c, 0xc4}) {
            const int offset = qFromLittleEndian<qint32>(bytes.constData() + slot);
            bytes.replace(offset, 64, shared.leftJustified(64, '\0'));
        }
        QVERIFY(document.LoadBytes(bytes, "typed-visibility"));
        QString warp, rotation;
        for (const auto& deformer : document.Deformers()) {
            if (deformer.type == CModelDeformer::Warp && warp.isEmpty()) warp = deformer.id;
            if (deformer.type == CModelDeformer::Rotation && rotation.isEmpty()) rotation = deformer.id;
        }
        QVERIFY(!warp.isEmpty() && !rotation.isEmpty());
        const QMap<QString, bool> initial{{"part|VisibilityShared", false}, {"artmesh|VisibilityShared", false},
            {"warp|" + warp, true}, {"rotation|" + rotation, true}};
        QVERIFY(document.SetVisibilityValues(initial));
        const auto allBefore = document.VisibilityState();
        const QMap<QString, bool> before{{"part|VisibilityShared", false}, {"artmesh|VisibilityShared", false}};
        const QMap<QString, bool> after{{"part|VisibilityShared", true}, {"artmesh|VisibilityShared", true}};
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelections({"VisibilityShared", "VisibilityShared", warp, rotation}, {"part", "artmesh", "warp", "rotation"});
        auto* check = panel.findChild<QCheckBox*>();
        QVERIFY(check && check->isEnabled());
        QCOMPARE(check->checkState(), Qt::PartiallyChecked);
        bool accept = false;
        int editCalls = 0, historyCalls = 0;
        CModelEdit submitted;
        panel.SetEditHandler([&](const CModelEdit& command, QString* error) {
            ++editCalls;
            submitted = command;
            CModelDocument candidate = document;
            if (!candidate.ExecuteEdit(command)) return false;
            if (!accept) { *error = "controlled visibility rejection"; return false; }
            document.CommitCandidate(std::move(candidate));
            return true;
        });
        panel.SetHistoryHandler([&](bool undo, CModelEdit& command, QString* error) {
            ++historyCalls;
            CModelDocument candidate = document;
            if (!(undo ? candidate.UndoEdit(&command) : candidate.RedoEdit(&command))) return false;
            if (!accept) { *error = "controlled visibility history rejection"; return false; }
            document.CommitCandidate(std::move(candidate));
            return true;
        });
        QSignalSpy batch(&panel, &CInspectorPanel::VisibilityBatchChanged);
        QSignalSpy single(&panel, &CInspectorPanel::VisibilityChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        const auto revision = document.Revision();
        check->click();
        QCOMPARE(editCalls, 1);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.VisibilityState(), allBefore);
        QCOMPARE(check->checkState(), Qt::PartiallyChecked);
        QVERIFY(!document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(batch.size(), 0);
        QCOMPARE(single.size(), 0);
        QCOMPARE(history.size(), 0);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(submitted.field, CModelEdit::VisibilityValues);
        QCOMPARE(submitted.type, QStringLiteral("objects"));
        QCOMPARE((submitted.before.value<QMap<QString, bool>>()), before);
        QCOMPARE((submitted.after.value<QMap<QString, bool>>()), after);

        accept = true;
        check->click();
        QCOMPARE(editCalls, 2);
        QCOMPARE(document.Revision(), revision + 1);
        auto allAfter = allBefore;
        for (auto it = after.cbegin(); it != after.cend(); ++it) allAfter[it.key()] = it.value();
        QCOMPARE(document.VisibilityState(), allAfter);
        QCOMPARE(check->checkState(), Qt::Checked);
        QCOMPARE(batch.size(), 1);
        QCOMPARE((batch[0][0].value<QMap<QString, bool>>()), after);
        QCOMPARE(single.size(), 0);
        QCOMPARE(history.size(), 1);
        accept = false;
        panel.Undo();
        QCOMPARE(historyCalls, 1);
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(document.VisibilityState(), allAfter);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(check->checkState(), Qt::Checked);
        QCOMPARE(batch.size(), 1);
        QCOMPARE(history.size(), 1);
        QCOMPARE(rejected.size(), 2);
        accept = true;
        panel.Undo();
        QCOMPARE(document.VisibilityState(), allBefore);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(check->checkState(), Qt::PartiallyChecked);
        QCOMPARE(batch.size(), 2);
        QCOMPARE((batch[1][0].value<QMap<QString, bool>>()), before);
        panel.Redo();
        QCOMPARE(document.VisibilityState(), allAfter);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(check->checkState(), Qt::Checked);
        QCOMPARE(batch.size(), 3);
        QCOMPARE((batch[2][0].value<QMap<QString, bool>>()), after);
        QCOMPARE(history.size(), 3);
        QCOMPARE(single.size(), 0);

        panel.SetSelection("VisibilityShared", "artmesh");
        check->click();
        QCOMPARE(single.size(), 1);
        QCOMPARE(single[0], (QList<QVariant>{QStringLiteral("VisibilityShared"), QStringLiteral("artmesh"), false}));
        QCOMPARE(batch.size(), 3);
        QCOMPARE(document.VisibilityState().value("part|VisibilityShared"), true);
        QCOMPARE(document.VisibilityState().value("artmesh|VisibilityShared"), false);
    }

    void largeFiniteKeyformToolsKeepTheirHistoryAtomic_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<float>("sign");
        for (const QString& operation : {QStringLiteral("mirrorHorizontal"), QStringLiteral("mirrorVertical"),
             QStringLiteral("interpolatePreviousKeyform"), QStringLiteral("scale"), QStringLiteral("rotate")})
            for (float sign : {-1.0f, 1.0f})
                QTest::newRow(qPrintable(operation + QString::number(sign))) << operation << sign;
    }

    void largeFiniteKeyformToolsKeepTheirHistoryAtomic()
    {
        QFETCH(QString, operation);
        QFETCH(float, sign);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto found = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& mesh) { return mesh.vertexCount >= 2 && mesh.keyforms.size() >= 2; });
        QVERIFY(found != document.Drawables().cend());
        const QString id = found->id;
        const float h = std::ldexp(1.0f, 127) * sign;
        const bool average = operation == QLatin1String("interpolatePreviousKeyform");
        const int keyform = average ? 1 : 0;
        QVector<float> before(found->vertexCount * 2);
        for (int row = 0; row < found->vertexCount; ++row) {
            before[row * 2] = row % 2 ? 1.5f * h : h;
            before[row * 2 + 1] = row % 2 ? h : 1.5f * h;
        }
        if (average) {
            before.fill(1.5f * h);
            QVERIFY(document.SetDrawableKeyformPositions(id, 0, QVector<float>(before.size(), h)));
        }
        QVERIFY(document.SetDrawableKeyformPositions(id, keyform, before));
        QVector<float> expected = before;
        if (average) expected.fill(1.25f * h);
        else if (operation.startsWith("mirror")) {
            const int axis = operation == QLatin1String("mirrorHorizontal") ? 0 : 1;
            for (int row = 0; row < found->vertexCount; ++row)
                expected[row * 2 + axis] = before[row * 2 + axis] == h ? 1.5f * h : h;
        } else if (operation == QLatin1String("scale")) {
            expected[0] = 0.75f * h;
            expected[1] = 1.75f * h;
            expected[2] = 1.75f * h;
            expected[3] = 0.75f * h;
        } else {
            expected[0] = h;
            expected[1] = h;
            expected[2] = 1.5f * h;
            expected[3] = 1.5f * h;
        }
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, "artmesh");
        auto* selector = panel.findChild<QComboBox*>("keyformSelector");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(selector && table);
        selector->setCurrentIndex(keyform);
        const auto revision = document.Revision();
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        const bool transform = operation == QLatin1String("scale") || operation == QLatin1String("rotate");
        if (transform) {
            table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
            auto* scale = panel.findChild<QDoubleSpinBox*>("vertexScale");
            auto* rotation = panel.findChild<QDoubleSpinBox*>("vertexRotation");
            QVERIFY(scale && rotation);
            scale->setValue(operation == QLatin1String("scale") ? 2.0 : 1.0);
            rotation->setValue(operation == QLatin1String("rotate") ? 90.0 : 0.0);
        }
        auto* button = panel.findChild<QPushButton*>(transform ? "transformSelectedVertices" : operation);
        QVERIFY(button && button->isEnabled());
        button->click();
        QCOMPARE(document.DrawableKeyformPositions(id, keyform), expected);
        for (float value : document.DrawableKeyformPositions(id, keyform)) QVERIFY(std::isfinite(value));
        QCOMPARE(document.Revision(), revision + 1);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(history.size(), 1);
        QCOMPARE(rejected.size(), 0);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, keyform), before);
        QVERIFY(!document.CanUndoEdit());
        QCOMPARE(selector->currentIndex(), keyform);
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, keyform), expected);
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(selector->currentIndex(), keyform);
        QCOMPARE(changed.size(), 3);
        QCOMPARE(history.size(), 3);
        QCOMPARE(rejected.size(), 0);
    }

    void unrepresentableTransformRejectsWithoutPartialChanges_data()
    {
        QTest::addColumn<bool>("rotation");
        QTest::newRow("scale-overflow") << false;
        QTest::newRow("rotation-overflow") << true;
    }

    void unrepresentableTransformRejectsWithoutPartialChanges()
    {
        QFETCH(bool, rotation);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto found = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& mesh) { return mesh.vertexCount >= 2 && !mesh.keyforms.isEmpty(); });
        QVERIFY(found != document.Drawables().cend());
        const QString id = found->id;
        const float h = std::ldexp(1.0f, 127) * (rotation ? 1.5f : 1.0f);
        QVector<float> before(found->vertexCount * 2, 0.0f);
        before[0] = before[1] = -h;
        before[2] = before[3] = h;
        QVERIFY(document.SetDrawableKeyformPositions(id, 0, before));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, "artmesh");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        auto* scale = panel.findChild<QDoubleSpinBox*>("vertexScale");
        auto* angle = panel.findChild<QDoubleSpinBox*>("vertexRotation");
        auto* button = panel.findChild<QPushButton*>("transformSelectedVertices");
        QVERIFY(table && scale && angle && button);
        table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        QVERIFY(button->isEnabled());
        scale->setValue(rotation ? 1.0 : 2.0);
        angle->setValue(rotation ? 45.0 : 0.0);
        const auto revision = document.Revision();
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        button->click();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), before);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(history.size(), 0);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(rejected.size(), 1);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(table->item(0, 1)->text(), QString::number(before[0], 'g', 9));
        QCOMPARE(table->item(1, 2)->text(), QString::number(before[3], 'g', 9));
    }

    void tinyCoordinateEditsAndUnmodifiedConfirmationPreservePrecision()
    {
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto found = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& mesh) { return mesh.vertexCount > 0 && !mesh.keyforms.isEmpty(); });
        QVERIFY(found != document.Drawables().cend());
        const QString id = found->id;
        auto before = document.DrawableKeyformPositions(id, 0);
        before[0] = 1e-9f;
        QVERIFY(document.SetDrawableKeyformPositions(id, 0, before));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, "artmesh");
        panel.resize(700, 900);
        panel.show();
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(table);
        QCOMPARE(table->item(0, 1)->text(), QString::number(before[0], 'g', 9));
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        const auto revision = document.Revision();
        table->setCurrentCell(0, 1);
        table->editItem(table->item(0, 1));
        QApplication::processEvents();
        auto* editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        QTest::keyClick(editor, Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), before);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(history.size(), 0);
        QCOMPARE(changed.size(), 0);
        table->item(0, 1)->setText(QString::number(2e-9f, 'g', 9));
        auto expected = before;
        expected[0] = 2e-9f;
        QCOMPARE(document.DrawableKeyformPositions(id, 0), expected);
        QCOMPARE(history.size(), 1);
        QCOMPARE(changed.size(), 1);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), before);
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), expected);
        const auto acceptedRevision = document.Revision();
        for (const QString& invalid : {QStringLiteral("1e100"), QStringLiteral("-1e100"),
                                      QStringLiteral("nan"), QStringLiteral("inf")}) {
            table->item(0, 1)->setText(invalid);
            QCOMPARE(document.DrawableKeyformPositions(id, 0), expected);
            QCOMPARE(document.Revision(), acceptedRevision);
            QCOMPARE(table->item(0, 1)->text(), QString::number(expected[0], 'g', 9));
        }
        QCOMPARE(history.size(), 3);
        QCOMPARE(changed.size(), 3);
    }

    void controllerBackedEditsAndHistoryPublishExactlyOnce_data()
    {
        QTest::addColumn<bool>("copyToNext");
        QTest::newRow("vertex-table") << false;
        QTest::newRow("copy-next") << true;
    }

    void controllerBackedEditsAndHistoryPublishExactlyOnce()
    {
        QFETCH(bool, copyToNext);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable){ return drawable.vertexCount && drawable.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const auto previewPositions = document.DrawableKeyformPositions(id, 1);
        auto nextPositions = previewPositions;
        nextPositions[0] += 0.5f;
        QVERIFY(document.SetDrawableKeyformPositions(id, 2, nextPositions));
        const int editedKeyform = copyToNext ? 2 : 1;
        const auto before = document.DrawableKeyformPositions(id, editedKeyform);
        QMap<QString, float> currentValues;
        for (const auto& parameter : document.Parameters())
            currentValues.insert(parameter.id, float(double(parameter.minimum) +
                (double(parameter.maximum) - parameter.minimum) * 0.37));
        QMap<QString, float> corner;
        QVERIFY(document.KeyformParameterValues(id, QStringLiteral("artmesh"), 1, corner));
        auto expectedPose = currentValues;
        for (auto it = corner.cbegin(); it != corner.cend(); ++it) expectedPose.insert(it.key(), it.value());
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        auto* copy = panel.findChild<QPushButton*>("copyKeyformToNext");
        QVERIFY(combo && table && copy);
        combo->setCurrentIndex(1);
        CKeyformEditController controller;
        controller.SetDocument(&document);
        bool acceptPublication = false;
        int publications = 0, builtCandidates = 0, editCalls = 0, historyCalls = 0, activationCalls = 0;
        QMap<QString, float> publishedPose;
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy activated(&panel, &CInspectorPanel::DrawableKeyformActivated);
        QSignalSpy selected(&panel, &CInspectorPanel::DrawableKeyformSelected);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        controller.SetPublisher([&](const CModelDocument& candidate, const QMap<QString, float>& pose,
                                    bool editing, QString* error) {
            ++publications;
            QByteArray bytes;
            if (!editing || !candidate.BuildPreviewMoc(bytes, error) || bytes.isEmpty()) return false;
            ++builtCandidates;
            publishedPose = pose;
            if (!acceptPublication) { *error = QStringLiteral("controlled preview rejection"); return false; }
            return true;
        }, [](QString*) { return true; });
        const auto refreshAcceptedSelection = [&] {
            const QSignalBlocker blocker(&panel);
            panel.RefreshSelection();
            panel.SyncHistoryState();
        };
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++editCalls;
            if (!controller.Submit(edit, currentValues, panel.GetActiveKeyformIndex(), error)) return false;
            refreshAcceptedSelection();
            return true;
        });
        panel.SetHistoryHandler([&](bool undo, CModelEdit& edit, QString* error) {
            ++historyCalls;
            if (!controller.History(undo, edit, error)) return false;
            refreshAcceptedSelection();
            return true;
        });
        panel.SetKeyformHandler([&](const QString& target, int keyform, QString* error) {
            ++activationCalls;
            return controller.Activate(target, keyform, currentValues, error);
        });
        const auto editFromUi = [&] {
            if (copyToNext) copy->click();
            else table->item(0, 1)->setText(QString::number(previewPositions[0] + 0.25f, 'g', 9));
        };
        const quint64 startRevision = document.Revision();
        editFromUi();
        QCOMPARE(publications, 1);
        QCOMPARE(builtCandidates, 1);
        QCOMPARE(editCalls, 1);
        QCOMPARE(document.Revision(), startRevision);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), before);
        QVERIFY(!document.CanUndoEdit() && !document.CanRedoEdit());
        QVERIFY(!controller.IsActive());
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(table->item(0, 1)->text(), QString::number(previewPositions[0], 'g', 9));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        QCOMPARE(activated.count(), 0);
        QCOMPARE(rejected.count(), 1);
        acceptPublication = true;
        editFromUi();
        const auto after = document.DrawableKeyformPositions(id, editedKeyform);
        QVERIFY(after != before);
        if (copyToNext) QCOMPARE(after, previewPositions);
        QCOMPARE(publications, 2);
        QCOMPARE(editCalls, 2);
        QCOMPARE(document.Revision(), startRevision + 1);
        QCOMPARE(publishedPose, expectedPose);
        QCOMPARE(controller.Pose(), expectedPose);
        QCOMPARE(controller.Target(), id);
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        CModelEdit pending;
        QVERIFY(document.PeekUndoEdit(&pending));
        QCOMPARE(pending.index, editedKeyform);
        QCOMPARE(pending.before.value<QVector<float>>(), before);
        QCOMPARE(pending.after.value<QVector<float>>(), after);
        acceptPublication = false;
        const int successActivations = activated.count();
        panel.Undo();
        QCOMPARE(publications, 3);
        QCOMPARE(document.Revision(), startRevision + 1);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), after);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QCOMPARE(activated.count(), successActivations);
        QCOMPARE(rejected.count(), 2);
        acceptPublication = true;
        panel.Undo();
        QCOMPARE(publications, 4);
        QCOMPARE(document.Revision(), startRevision + 2);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), before);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(changed.count(), 2);
        QCOMPARE(history.count(), 2);
        panel.Undo();
        QCOMPARE(publications, 4);
        QCOMPARE(document.Revision(), startRevision + 2);
        acceptPublication = false;
        const int undoActivations = activated.count();
        panel.Redo();
        QCOMPARE(publications, 5);
        QCOMPARE(document.Revision(), startRevision + 2);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), before);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(changed.count(), 2);
        QCOMPARE(history.count(), 2);
        QCOMPARE(activated.count(), undoActivations);
        QCOMPARE(rejected.count(), 3);
        acceptPublication = true;
        panel.Redo();
        QCOMPARE(publications, 6);
        QCOMPARE(builtCandidates, 6);
        QCOMPARE(historyCalls, 4);
        QCOMPARE(document.Revision(), startRevision + 3);
        QCOMPARE(document.DrawableKeyformPositions(id, editedKeyform), after);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(changed.count(), 3);
        QCOMPARE(history.count(), 3);
        QCOMPARE(publishedPose, expectedPose);
        refreshAcceptedSelection();
        QCoreApplication::processEvents();
        QCOMPARE(publications, 6);
        QCOMPARE(document.Revision(), startRevision + 3);
        QCOMPARE(activationCalls, 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(panel.GetSelectionId(), id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(history.count(), 3);
        for (const auto& notification : activated) {
            QCOMPARE(notification[0].toString(), id);
            QCOMPARE(notification[1].toInt(), 1);
        }
    }

    void keyformToolsUseInjectedCommitter_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("accepted");
        const QStringList operations{QStringLiteral("cell"), QStringLiteral("mirrorHorizontal"),
            QStringLiteral("mirrorVertical"), QStringLiteral("interpolatePreviousKeyform"),
            QStringLiteral("copyKeyformToNext"), QStringLiteral("resetKeyform"),
            QStringLiteral("pasteKeyform"), QStringLiteral("moveSelectedVertices"),
            QStringLiteral("transformSelectedVertices")};
        for (const auto& operation : operations)
            for (bool accepted : {false, true})
                QTest::newRow(qPrintable(operation + (accepted ? QStringLiteral("-accept") : QStringLiteral("-reject"))))
                    << operation << accepted;
    }

    void keyformToolsUseInjectedCommitter()
    {
        QFETCH(QString, operation);
        QFETCH(bool, accepted);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& value){ return value.vertexCount >= 5 && value.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        auto current = document.DrawableKeyformPositions(id, 1);
        current[0] += 0.3f;
        current[1] += 0.15f;
        auto previous = current;
        previous[0] += 0.5f;
        auto next = current;
        next[0] -= 0.5f;
        QVERIFY(document.SetDrawableKeyformPositions(id, 0, previous));
        QVERIFY(document.SetDrawableKeyformPositions(id, 1, current));
        QVERIFY(document.SetDrawableKeyformPositions(id, 2, next));
        const auto originalOverrides = document.DrawableKeyformPositionOverrides();
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(combo && table);
        combo->setCurrentIndex(1);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        auto paste = current;
        paste[0] += 0.75f;
        QJsonArray pastedValues;
        for (float value : paste) pastedValues.append(double(value));
        const QJsonObject clipboard{{QStringLiteral("format"), QStringLiteral("live2d2psd-keyform-v1")},
                                    {QStringLiteral("vertexCount"), current.size() / 2},
                                    {QStringLiteral("positions"), pastedValues}};
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        QSignalSpy selected(&panel, &CInspectorPanel::DrawableKeyformSelected);
        int calls = 0;
        bool calledBeforePublishing = false;
        CModelEdit submitted;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++calls;
            submitted = edit;
            calledBeforePublishing = changed.isEmpty() && history.isEmpty() &&
                                     document.DrawableKeyformPositionOverrides() == originalOverrides;
            if (!accepted) { *error = QStringLiteral("candidate rejected"); return false; }
            return document.ExecuteEdit(edit);
        });
        if (operation == QLatin1String("cell")) {
            table->item(0, 1)->setText(QString::number(current[0] + 0.25f, 'g', 9));
        } else {
            if (operation == QLatin1String("pasteKeyform")) {
                auto* copy = panel.findChild<QPushButton*>("copyKeyform");
                QVERIFY(copy && copy->isEnabled());
                copy->click();
                QCOMPARE(calls, 0);
                QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(clipboard).toJson()));
            }
            if (operation == QLatin1String("moveSelectedVertices") || operation == QLatin1String("transformSelectedVertices")) {
                table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
                panel.findChild<QDoubleSpinBox*>("vertexDeltaX")->setValue(0.125);
                panel.findChild<QDoubleSpinBox*>("vertexDeltaY")->setValue(-0.25);
                panel.findChild<QDoubleSpinBox*>("vertexScale")->setValue(1.5);
                panel.findChild<QDoubleSpinBox*>("vertexRotation")->setValue(30);
            }
            auto* button = panel.findChild<QPushButton*>(operation);
            QVERIFY(button && button->isEnabled());
            button->click();
        }
        QCOMPARE(calls, 1);
        QVERIFY(calledBeforePublishing);
        QCOMPARE(submitted.field, CModelEdit::KeyformPositions);
        QCOMPARE(submitted.name, id);
        QCOMPARE(submitted.index, operation == QLatin1String("copyKeyformToNext") ? 2 : 1);
        QCOMPARE(panel.GetSelectionId(), id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(selected.count(), 0);
        if (accepted) {
            QCOMPARE(document.DrawableKeyformPositions(id, submitted.index), submitted.after.value<QVector<float>>());
            QVERIFY(document.CanUndoEdit());
            QCOMPARE(changed.count(), 1);
            QCOMPARE(history.count(), 1);
            QCOMPARE(rejected.count(), 0);
        } else {
            QCOMPARE(document.DrawableKeyformPositionOverrides(), originalOverrides);
            QVERIFY(!document.CanUndoEdit());
            QCOMPARE(changed.count(), 0);
            QCOMPARE(history.count(), 0);
            QCOMPARE(rejected.count(), 1);
            QCOMPARE(rejected.first().first().toString(), QStringLiteral("candidate rejected"));
            QCOMPARE(table->item(0, 1)->text(), QString::number(current[0], 'g', 9));
        }
    }

    void basicPropertiesUseInjectedCommitter_data()
    {
        QTest::addColumn<int>("field");
        QTest::addColumn<bool>("accepted");
        for (int field = 0; field < 3; ++field)
            for (bool accepted : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(field).arg(accepted))) << field << accepted;
    }

    void basicPropertiesUseInjectedCommitter()
    {
        QFETCH(int, field);
        QFETCH(bool, accepted);
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Drawables().front().id;
        const bool beforeVisibility = document.Drawables().front().visible;
        const float beforeOpacity = document.DrawableOpacity(document.Drawables().front());
        const int beforeOrder = qRound(document.DrawableDrawOrder(document.Drawables().front()));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* visibility = panel.findChild<QCheckBox*>();
        auto* opacity = panel.findChild<QDoubleSpinBox*>();
        auto* order = panel.findChild<QSpinBox*>();
        QVERIFY(visibility && opacity && order);
        QSignalSpy visibleChanged(&panel, &CInspectorPanel::VisibilityChanged);
        QSignalSpy opacityChanged(&panel, &CInspectorPanel::DrawableOpacityChanged);
        QSignalSpy orderChanged(&panel, &CInspectorPanel::DrawableDrawOrderChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        int calls = 0;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++calls;
            if (!accepted) { *error = QStringLiteral("rejected property"); return false; }
            return document.ExecuteEdit(edit);
        });
        if (field == 0) visibility->click();
        if (field == 1) opacity->setValue(beforeOpacity == 0.25f ? 0.75 : 0.25);
        if (field == 2) order->setValue(beforeOrder == 321 ? 322 : 321);
        QCOMPARE(calls, 1);
        QCOMPARE(history.count(), accepted ? 1 : 0);
        QCOMPARE(rejected.count(), accepted ? 0 : 1);
        QCOMPARE(visibleChanged.count() + opacityChanged.count() + orderChanged.count(), accepted ? 1 : 0);
        QCOMPARE(document.CanUndoEdit(), accepted);
        if (!accepted) {
            QCOMPARE(document.Drawables().front().visible, beforeVisibility);
            QCOMPARE(document.DrawableOpacity(document.Drawables().front()), beforeOpacity);
            QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().front())), beforeOrder);
            QCOMPARE(visibility->isChecked(), beforeVisibility);
            QCOMPARE(opacity->value(), double(beforeOpacity));
            QCOMPARE(order->value(), beforeOrder);
        }
    }

    void historyCommitterRejectsThenCommitsUndoAndRedo()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Drawables().front().id;
        const auto before = document.DrawableKeyformPositions(id, 0);
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(table && table->rowCount());
        table->item(0, 1)->setText(QString::number(before[0] + 0.25f, 'g', 9));
        const auto after = document.DrawableKeyformPositions(id, 0);
        QVERIFY(after != before);
        bool accepted = false;
        QList<bool> directions;
        panel.SetHistoryHandler([&](bool undo, CModelEdit& edit, QString* error) {
            directions.append(undo);
            if (!accepted) { *error = QStringLiteral("history candidate rejected"); return false; }
            return undo ? document.UndoEdit(&edit) : document.RedoEdit(&edit);
        });
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), after);
        QVERIFY(document.CanUndoEdit() && !document.CanRedoEdit());
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        QCOMPARE(rejected.count(), 1);
        accepted = true;
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), before);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        accepted = false;
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), before);
        QVERIFY(!document.CanUndoEdit() && document.CanRedoEdit());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QCOMPARE(rejected.count(), 2);
        accepted = true;
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), after);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(history.count(), 2);
        QCOMPARE(directions, QList<bool>({true, true, false, false}));
    }

    void keyformCommitterRunsOnlyForUserChoicesWhileBrowsingRemainsAvailable()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& value){ return value.vertexCount && value.keyforms.size() >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const auto other = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [&mesh](const auto& value){ return value.id != mesh->id && value.vertexCount && !value.keyforms.isEmpty(); });
        QVERIFY(other != document.Drawables().cend());
        for (const auto& parameter : document.Parameters()) {
            const auto metadata = document.ParameterMetadata(parameter.id);
            const float value = float(double(metadata.minimum) + (double(metadata.maximum) - metadata.minimum) * 0.37);
            QVERIFY(document.SetParameterValue(parameter.id, value));
        }
        const auto parameterValues = document.ParameterValues();
        CInspectorPanel panel;
        panel.SetDocument(&document);
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(combo && table);
        bool accepted = false;
        QVector<QPair<QString, int>> requests;
        QMap<QString, float> requestedPose;
        panel.SetKeyformHandler([&](const QString& id, int index, QString* error) {
            requests.append({id, index});
            if (!accepted) { *error = QStringLiteral("pose candidate rejected"); return false; }
            return document.KeyformParameterValues(id, QStringLiteral("artmesh"), index, requestedPose, error);
        });
        QSignalSpy selected(&panel, &CInspectorPanel::DrawableKeyformSelected);
        QSignalSpy activated(&panel, &CInspectorPanel::DrawableKeyformActivated);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(requests.size(), 0);
        QCOMPARE(panel.GetSelectionId(), mesh->id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(rejected.count(), 0);
        panel.RefreshSelection();
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        panel.SetDocument(&document);
        panel.SetLockedState({});
        QCOMPARE(requests.size(), 0);
        combo->setCurrentIndex(2);
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 0);
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        activated.clear();
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.last(), qMakePair(mesh->id, 1));
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(activated.count(), 0);
        accepted = true;
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(requests.size(), 2);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(panel.GetActiveKeyformIndex(), 1);
        QCOMPARE(selected.count(), 1);
        QCOMPARE(activated.count(), 1);
        QMap<QString, float> expectedPose;
        QVERIFY(document.KeyformParameterValues(mesh->id, QStringLiteral("artmesh"), 1, expectedPose));
        QCOMPARE(requestedPose, expectedPose);
        const auto selectedPositions = document.DrawableKeyformPositions(mesh->id, 1);
        QCOMPARE(table->item(0, 1)->text(), QString::number(selectedPositions[0], 'g', 9));
        panel.RefreshSelection();
        QVERIFY(QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, 1)));
        QCOMPARE(requests.size(), 2);
        accepted = false;
        const int acceptedSignals = activated.count();
        panel.SetLockedState({QStringLiteral("artmesh|") + other->id});
        const int refreshedSignals = activated.count();
        QVERIFY(refreshedSignals >= acceptedSignals);
        panel.SetSelection(other->id, QStringLiteral("artmesh"));
        QCOMPARE(requests.size(), 2);
        QCOMPARE(panel.GetSelectionId(), other->id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        QCOMPARE(combo->currentIndex(), 0);
        QVERIFY(!combo->isEnabled());
        QCOMPARE(activated.count(), refreshedSignals + 1);
        QCOMPARE(selected.count(), 1);
        QCOMPARE(history.count(), 0);
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!document.CanUndoEdit());
        QCOMPARE(document.ParameterValues(), parameterValues);
    }

    void unsupportedBlendShapeCanBeInspectedWithoutActivatingIt()
    {
        using namespace live2dstruct;
        CModelDocument original;
        const QString path = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        QVERIFY(original.Load(path));
        QByteArray bytes = original.SourceBytes();
        const int typeOffset = bytes.size();
        for (int index = 0; index < original.Parameters().size(); ++index) {
            char value[4];
            qToLittleEndian<quint32>(1, reinterpret_cast<uchar*>(value));
            bytes.append(value, 4);
        }
        qToLittleEndian<quint32>(typeOffset, reinterpret_cast<uchar*>(bytes.data() + 0x208));
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(bytes, path, &error), qPrintable(error));
        QVERIFY(std::all_of(document.Parameters().cbegin(), document.Parameters().cend(),
                            [](const auto& parameter){ return parameter.type == 1; }));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable){ return drawable.vertexCount && drawable.keyforms.size() > 1; });
        QVERIFY(mesh != document.Drawables().cend());
        QMap<QString, float> pose;
        QVERIFY(!document.KeyformParameterValues(mesh->id, QStringLiteral("artmesh"), 0, pose, &error));
        QVERIFY2(error.contains(QStringLiteral("类型 1")), qPrintable(error));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        int requests = 0;
        panel.SetKeyformHandler([&](const QString& id, int keyform, QString* error) {
            ++requests;
            return document.KeyformParameterValues(id, QStringLiteral("artmesh"), keyform, pose, error);
        });
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(panel.GetSelectionId(), mesh->id);
        QCOMPARE(panel.GetActiveKeyformIndex(), 0);
        QCOMPARE(requests, 0);
        QCOMPARE(rejected.count(), 0);
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        auto* combo = panel.findChild<QComboBox*>("keyformSelector");
        QVERIFY(table && combo);
        QCOMPARE(table->rowCount(), mesh->vertexCount);
        QTest::keyClick(combo, Qt::Key_Down);
        QCOMPARE(requests, 1);
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(panel.GetSelectionId(), mesh->id);
        QVERIFY(!document.CanUndoEdit());
    }

    void otherEditPathsUseInjectedCommitter_data()
    {
        QTest::addColumn<QString>("operation");
        QTest::addColumn<bool>("accepted");
        const QStringList operations{QStringLiteral("rotation"), QStringLiteral("warp"), QStringLiteral("parent"),
            QStringLiteral("inversion"), QStringLiteral("masks"), QStringLiteral("pasteTopology"),
            QStringLiteral("mergeSelectedVertices"), QStringLiteral("autoTriangulate")};
        for (const auto& operation : operations)
            for (bool accepted : {false, true})
                QTest::newRow(qPrintable(operation + (accepted ? QStringLiteral("-accept") : QStringLiteral("-reject"))))
                    << operation << accepted;
    }

    void otherEditPathsUseInjectedCommitter()
    {
        QFETCH(QString, operation);
        QFETCH(bool, accepted);
        using namespace live2dstruct;
        CModelDocument document;
        const bool masks = operation == QLatin1String("masks") || operation == QLatin1String("inversion");
        QVERIFY(document.Load(QString(TEST_RESOURCES) + (masks ? "/Rice/Rice.moc3" : "/Mark/Mark.moc3")));
        QString id, type = QStringLiteral("artmesh");
        if (operation == QLatin1String("rotation") || operation == QLatin1String("warp") || operation == QLatin1String("parent")) {
            const auto deformer = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(), [&](const auto& value) {
                if (operation == QLatin1String("rotation")) return !value.rotationKeyforms.isEmpty();
                if (operation == QLatin1String("warp")) return !value.warpKeyforms.isEmpty();
                return value.parentDeformerIndex >= 0 && !value.warpKeyforms.isEmpty();
            });
            QVERIFY(deformer != document.Deformers().cend());
            id = deformer->id;
            type = operation == QLatin1String("rotation") ? QStringLiteral("rotation") : QStringLiteral("warp");
        } else {
            const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(), [&](const auto& value) {
                return value.vertexCount >= 5 && !value.keyforms.isEmpty() && (!masks || !value.maskIndices.isEmpty());
            });
            QVERIFY(mesh != document.Drawables().cend());
            id = mesh->id;
            if (!masks) QVERIFY(document.SetDrawableIndices(id, {0, 2, 3, 0, 3, 4, 1, 2, 3}));
            if (operation == QLatin1String("autoTriangulate")) {
                auto positions = document.DrawableKeyformPositions(id, 0);
                for (int i = 0; i < positions.size() / 2; ++i) {
                    positions[i * 2] = float(i) * 0.01f;
                    positions[i * 2 + 1] = float(i) * float(i) * 0.001f;
                }
                QVERIFY(document.SetDrawableKeyformPositions(id, 0, positions));
                QVERIFY(document.SetDrawableIndices(id, {0, 1, 2}));
            }
        }
        const auto rotationBefore = document.DeformerRotationKeyforms(id);
        const auto warpBefore = document.DeformerWarpKeyforms(id);
        const auto parentBefore = document.DeformerParentIndex(id);
        const auto topologyBefore = document.DrawableIndices(id);
        const auto masksBefore = document.DrawableMaskIndices(id);
        const auto inversionBefore = document.DrawableMaskInverted(id);
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, type);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        QSignalSpy topology(&panel, &CInspectorPanel::DrawableIndicesChanged);
        QSignalSpy maskChanges(&panel, &CInspectorPanel::DrawableMaskIndicesChanged);
        int calls = 0;
        CModelEdit submitted;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            ++calls;
            submitted = edit;
            if (!accepted) { *error = QStringLiteral("rejected candidate"); return false; }
            return document.ExecuteEdit(edit);
        });
        if (operation == QLatin1String("rotation")) {
            auto* table = panel.findChild<QTableWidget*>("rotationKeyformTable");
            QVERIFY(table && table->rowCount());
            table->item(0, 1)->setText(QString::number(rotationBefore[0].angle + 1.0f, 'g', 9));
        } else if (operation == QLatin1String("warp")) {
            auto* table = panel.findChild<QTableWidget*>("warpKeyformTable");
            QVERIFY(table && table->rowCount());
            table->item(0, 1)->setText(QString::number(warpBefore[0].positions[0] + 0.25f, 'g', 9));
        } else if (operation == QLatin1String("parent")) {
            auto* selector = panel.findChild<QComboBox*>("deformerParentSelector");
            QVERIFY(selector && selector->findData(-1) >= 0);
            selector->setCurrentIndex(selector->findData(-1));
        } else if (operation == QLatin1String("inversion")) {
            auto* checkbox = panel.findChild<QCheckBox*>("maskInverted");
            QVERIFY(checkbox && checkbox->isEnabled());
            checkbox->click();
        } else if (operation == QLatin1String("masks")) {
            bool answered = false;
            QTimer::singleShot(0, &panel, [&] {
                auto* dialog = panel.findChild<QDialog*>();
                if (!dialog) return;
                auto* list = dialog->findChild<QListWidget*>();
                if (!list) { dialog->reject(); return; }
                for (int row = 0; row < list->count(); ++row) list->item(row)->setCheckState(Qt::Unchecked);
                answered = true;
                dialog->accept();
            });
            panel.findChild<QPushButton*>("editMasks")->click();
            QVERIFY(answered);
        } else if (operation == QLatin1String("pasteTopology")) {
            panel.findChild<QPushButton*>("copyTopology")->click();
            QCOMPARE(calls, 0);
            QJsonObject object = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
            object.insert(QStringLiteral("indices"), QJsonArray{0, 1, 2});
            QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(object).toJson()));
            panel.findChild<QPushButton*>("pasteTopology")->click();
        } else if (operation == QLatin1String("autoTriangulate")) {
            QTimer dialogGuard;
            dialogGuard.setSingleShot(true);
            connect(&dialogGuard, &QTimer::timeout, &panel, [&] {
                if (auto* dialog = panel.findChild<QMessageBox*>("rebuildTopologyConfirmation"))
                    dialog->done(QMessageBox::Yes);
            });
            dialogGuard.start(0);
            panel.findChild<QPushButton*>("autoTriangulate")->click();
        } else {
            auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
            table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
            panel.findChild<QPushButton*>("mergeSelectedVertices")->click();
        }
        QCOMPARE(calls, 1);
        QCOMPARE(history.count(), accepted ? 1 : 0);
        QCOMPARE(rejected.count(), accepted ? 0 : 1);
        QCOMPARE(document.CanUndoEdit(), accepted);
        if (accepted) {
            CModelEdit committed;
            QVERIFY(document.PeekUndoEdit(&committed));
            QCOMPARE(committed.name, submitted.name);
            QCOMPARE(committed.field, submitted.field);
        } else {
            QCOMPARE(document.DeformerRotationKeyforms(id), rotationBefore);
            QCOMPARE(document.DeformerWarpKeyforms(id), warpBefore);
            QCOMPARE(document.DeformerParentIndex(id), parentBefore);
            QCOMPARE(document.DrawableIndices(id), topologyBefore);
            QCOMPARE(document.DrawableMaskIndices(id), masksBefore);
            QCOMPARE(document.DrawableMaskInverted(id), inversionBefore);
            QCOMPARE(topology.count(), 0);
            QCOMPARE(maskChanges.count(), 0);
        }
    }

    void multiSelectionStopsAtFirstRejectedCommit()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QVERIFY(document.Drawables().size() >= 3);
        QStringList ids;
        QVector<float> before;
        for (int i = 0; i < 3; ++i) {
            ids.append(document.Drawables()[i].id);
            before.append(document.DrawableOpacity(document.Drawables()[i]));
        }
        const float after = std::all_of(before.cbegin(), before.cend(), [](float value){ return value != 0.25f; }) ? 0.25f : 0.75f;
        QVERIFY(std::all_of(before.cbegin(), before.cend(), [after](float value){ return value != after; }));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelections(ids, {QStringLiteral("artmesh"), QStringLiteral("artmesh"), QStringLiteral("artmesh")});
        QStringList submitted;
        panel.SetEditHandler([&](const CModelEdit& edit, QString* error) {
            submitted.append(edit.name);
            if (submitted.size() == 2) { *error = QStringLiteral("second candidate failed"); return false; }
            return document.ExecuteEdit(edit);
        });
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableOpacityChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        panel.findChild<QDoubleSpinBox*>()->setValue(after);
        QCOMPARE(submitted, ids.mid(0, 2));
        QCOMPARE(document.DrawableOpacity(document.Drawables()[0]), after);
        QCOMPARE(document.DrawableOpacity(document.Drawables()[1]), before[1]);
        QCOMPARE(document.DrawableOpacity(document.Drawables()[2]), before[2]);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QCOMPARE(rejected.count(), 1);
        panel.Undo();
        QCOMPARE(document.DrawableOpacity(document.Drawables()[0]), before[0]);
        QVERIFY(!document.CanUndoEdit());
    }

    void rejectedEditExceptionRestoresTableWithoutPublishing()
    {
        using namespace live2dstruct;
        CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const QString id = document.Drawables().front().id;
        const auto positions = document.DrawableKeyformPositions(id, 0);
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        panel.SetEditHandler([](const CModelEdit&, QString*) -> bool { throw std::runtime_error("upload failed"); });
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        table->item(0, 1)->setText(QString::number(positions[0] + 1.0f));
        QCOMPARE(rejected.count(), 1);
        QVERIFY(rejected.first().first().toString().contains(QStringLiteral("upload failed")));
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
        QCOMPARE(document.DrawableKeyformPositions(id, 0), positions);
        QCOMPARE(table->item(0, 1)->text(), QString::number(positions[0], 'g', 9));
        QVERIFY(!document.CanUndoEdit());
        panel.SetEditHandler({});
        table->item(0, 1)->setText(QString::number(positions[0] + 1.0f));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QVERIFY(document.CanUndoEdit());
    }

    void clearingMasksPreservesInversionInOneHistoryEntry_data()
    {
        QTest::addColumn<bool>("sourceInverted");
        QTest::newRow("source-inverted") << true;
        QTest::newRow("user-inverted") << false;
    }

    void clearingMasksPreservesInversionInOneHistoryEntry()
    {
        QFETCH(bool, sourceInverted);
        using namespace live2dstruct;
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Rice/Rice.moc3", &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [sourceInverted](const CModelDrawable& drawable) {
                return !drawable.maskIndices.isEmpty() && ((drawable.flags & 0x08) != 0) == sourceInverted;
            });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const QVector<int> originalMasks = document.DrawableMaskIndices(id);
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* inverted = panel.findChild<QCheckBox*>("maskInverted");
        auto* editMasks = panel.findChild<QPushButton*>("editMasks");
        QVERIFY(inverted && editMasks && inverted->isEnabled() && editMasks->isEnabled());
        QCOMPARE(inverted->isChecked(), sourceInverted);
        inverted->click();
        if (sourceInverted) panel.Undo();
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(inverted->isChecked());
        panel.ClearHistory();
        QVERIFY(!document.CanUndoEdit());
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableMaskIndicesChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        bool clearedInDialog = false;
        QTimer responder;
        responder.setSingleShot(true);
        connect(&responder, &QTimer::timeout, &panel, [&] {
            auto* dialog = panel.findChild<QDialog*>();
            if (!dialog) return;
            auto* list = dialog->findChild<QListWidget*>();
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            if (!list || !buttons || !buttons->button(QDialogButtonBox::Ok)) {
                dialog->reject();
                return;
            }
            for (int row = 0; row < list->count(); ++row) list->item(row)->setCheckState(Qt::Unchecked);
            clearedInDialog = true;
            buttons->button(QDialogButtonBox::Ok)->click();
        });
        responder.start(0);
        editMasks->click();
        QVERIFY(clearedInDialog);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(history.size(), 1);
        QVERIFY(document.DrawableMaskIndices(id).isEmpty());
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(inverted->isChecked());
        QVERIFY(!inverted->isEnabled());
        QVERIFY(document.Validate().isEmpty());
        panel.Undo();
        QCOMPARE(document.DrawableMaskIndices(id), originalMasks);
        QVERIFY(inverted->isChecked() && inverted->isEnabled());
        QVERIFY(!document.CanUndoEdit());
        QCOMPARE(changed.size(), 2);
        panel.Redo();
        QVERIFY(document.DrawableMaskIndices(id).isEmpty());
        QVERIFY(inverted->isChecked() && !inverted->isEnabled());
        QVERIFY(document.DrawableMaskInverted(id));
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(changed.size(), 3);
        QVERIFY(document.Validate().isEmpty());
    }

    void inspectorPreservesNativeInvertedMasksThroughHistory()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Rice/Rice.moc3", &error), qPrintable(error));
        const QString maskedId = QStringLiteral("ArtMesh50");
        QVERIFY(!document.DrawableMaskIndices(maskedId).isEmpty());
        QVERIFY(document.DrawableMaskInverted(maskedId));
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(maskedId, QStringLiteral("artmesh"));
        auto* inverted = panel.findChild<QCheckBox*>("maskInverted");
        QVERIFY(inverted && inverted->isEnabled() && inverted->isChecked());
        inverted->click();
        QVERIFY(!document.DrawableMaskInverted(maskedId));
        QVERIFY(!inverted->isChecked());
        panel.Undo();
        QVERIFY(document.DrawableMaskInverted(maskedId));
        QVERIFY(inverted->isChecked());
        panel.Redo();
        QVERIFY(!document.DrawableMaskInverted(maskedId));
        QVERIFY(!inverted->isChecked());
        const QString unmaskedId = QStringLiteral("ArtMesh168");
        QVERIFY(document.DrawableMaskIndices(unmaskedId).isEmpty());
        panel.SetSelection(unmaskedId, QStringLiteral("artmesh"));
        QVERIFY(inverted->isChecked());
        QVERIFY(!inverted->isEnabled());
    }

    void mergeTopologyDeduplicatesAndKeepsHistoryAtomic()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable) { return drawable.vertexCount >= 5 && !drawable.keyforms.isEmpty(); });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const QVector<quint16> before{0, 2, 3, 0, 3, 4, 1, 2, 3};
        const QVector<quint16> after{0, 2, 3, 0, 3, 4};
        QVERIFY(document.SetDrawableIndices(id, before));
        const auto positions = document.DrawableKeyformPositions(id, 0);
        const auto uvs = mesh->uvs;
        const int vertexCount = mesh->vertexCount;

        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* table = panel.findChild<QTableWidget*>("keyformVertexTable");
        auto* merge = panel.findChild<QPushButton*>("mergeSelectedVertices");
        QVERIFY(table && merge);
        QVERIFY(!merge->isEnabled());
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableIndicesChanged);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        QVERIFY(merge->isEnabled());
        merge->click();
        QCOMPARE(document.DrawableIndices(id), after);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        QVERIFY(document.CanUndoEdit());
        merge->click();
        QCOMPARE(document.DrawableIndices(id), after);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(history.count(), 1);
        panel.Undo();
        QCOMPARE(document.DrawableIndices(id), before);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(document.DrawableIndices(id), after);
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(document.DrawableKeyformPositions(id, 0), positions);
        const auto current = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [&](const auto& drawable) { return drawable.id == id; });
        QVERIFY(current != document.Drawables().cend());
        QCOMPARE(current->uvs, uvs);
        QCOMPARE(current->vertexCount, vertexCount);

        panel.ClearHistory();
        const QVector<quint16> collapsed{0, 1, 2};
        QVERIFY(document.SetDrawableIndices(id, collapsed));
        panel.RefreshSelection();
        table->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        changed.clear();
        history.clear();
        merge->click();
        QCOMPARE(document.DrawableIndices(id), collapsed);
        QVERIFY(!document.CanUndoEdit());
        QCOMPARE(changed.count(), 0);
        QCOMPARE(history.count(), 0);
    }

    void wideIndexColumnRemainsReadableAcrossNativeThemes()
    {
        WideMeshData::Fixture wide; QString error; QVERIFY2(WideMeshData::make(65536,WideMeshData::Layout::Sparse,wide,&error),qPrintable(error));
        live2dstruct::CModelDocument document; QVERIFY2(document.LoadBytes(wide.bytes,"wide-display.moc3",&error),qPrintable(error));
        const auto originalStyle = qApp->styleSheet(); const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(originalStyle); });
        CInspectorPanel panel; panel.SetDocument(&document); panel.SetSelection(wide.ids.first(),"artmesh"); panel.resize(520,640); panel.show();
        auto* vertices = panel.findChild<QTableWidget*>("keyformVertexTable"); QVERIFY(vertices); QCOMPARE(vertices->rowCount(),65536);
        for (bool dark : {false,true}) {
            qApp->setStyleSheet(Theme::globalStyleSheet(dark)); vertices->scrollToItem(vertices->item(65535,0)); QTest::qWait(30);
            const int needed = vertices->fontMetrics().horizontalAdvance("65535") + 2 * (vertices->style()->pixelMetric(QStyle::PM_FocusFrameHMargin)+1);
            QVERIFY2(vertices->columnWidth(0) >= needed,qPrintable(QString("index column %1, required %2").arg(vertices->columnWidth(0)).arg(needed)));
            QCOMPARE(vertices->item(65535,0)->text(),QString("65535")); QCOMPARE(vertices->horizontalHeader()->resizeContentsPrecision(),1000);
            for (int row = 65530; row <= 65535; ++row) QVERIFY2(IndexTextProbe::Fits(vertices,row),qPrintable(QString("Painted index %1 is elided").arg(row)));
            const QString directory = qEnvironmentVariable("WIDE_UI_IMAGES"); if (!directory.isEmpty()) { QDir().mkpath(directory); QVERIFY(vertices->grab().save(directory + QString("/inspector-%1.png").arg(dark ? "dark" : "light"))); }
        }
    }
    void wideIndicesRemainVisibleAndEditableThroughClipboardAndHistory()
    {
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) QSKIP("Uses the offscreen platform clipboard; never access the user's system clipboard");
        WideMeshData::Fixture wide; QString error; QVERIFY2(WideMeshData::make(65536,WideMeshData::Layout::Sparse,wide,&error),qPrintable(error));
        live2dstruct::CModelDocument document; QVERIFY2(document.LoadBytes(wide.bytes,"wide.moc3",&error),qPrintable(error)); const QString id = wide.ids.first();
        CInspectorPanel panel; panel.SetDocument(&document); panel.SetSelection(id,"artmesh"); panel.resize(500,700); panel.show(); QApplication::processEvents();
        auto* vertices = panel.findChild<QTableWidget*>("keyformVertexTable"); QVERIFY(vertices); QCOMPARE(vertices->rowCount(),65536);
        const auto before = document.DrawableKeyformPositions(id,0); QCOMPARE(before.size(),131072);
        for (int row : {32768,65535}) { QCOMPARE(vertices->item(row,0)->text(),QString::number(row)); QCOMPARE(vertices->item(row,1)->text().toFloat(),before[row*2]); QCOMPARE(vertices->item(row,2)->text().toFloat(),before[row*2+1]); }
        vertices->scrollToItem(vertices->item(65535,1)); QApplication::processEvents(); QVERIFY(vertices->verticalScrollBar()->value() > 0);
        const QString screenshot = qEnvironmentVariable("WIDE_INSPECTOR_SCREENSHOT"); if (!screenshot.isEmpty()) QVERIFY(vertices->grab().save(screenshot));
        const int needed = vertices->fontMetrics().horizontalAdvance("65535") + 2 * (vertices->style()->pixelMetric(QStyle::PM_FocusFrameHMargin) + 1);
        QVERIFY2(vertices->columnWidth(0) >= needed, qPrintable(QString("Index column %1 px, full text requires %2 px; header resize precision %3").arg(vertices->columnWidth(0)).arg(needed).arg(vertices->horizontalHeader()->resizeContentsPrecision())));
        QCOMPARE(vertices->horizontalHeader()->sectionResizeMode(0),QHeaderView::ResizeToContents); QCOMPARE(vertices->horizontalHeader()->resizeContentsPrecision(),1000);
        const auto originalFont = vertices->font(); auto larger = originalFont; larger.setPixelSize(vertices->fontMetrics().height()*2); vertices->setFont(larger); QTest::qWait(20);
        QVERIFY(vertices->columnWidth(0) >= vertices->fontMetrics().horizontalAdvance("65535") + 2 * (vertices->style()->pixelMetric(QStyle::PM_FocusFrameHMargin) + 1)); vertices->setFont(originalFont);
        const auto originalStyle = qApp->styleSheet(); const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(originalStyle); });
        for (bool dark : {true,false}) { qApp->setStyleSheet(Theme::globalStyleSheet(dark)); QTest::qWait(20); QVERIFY(vertices->columnWidth(0) >= vertices->fontMetrics().horizontalAdvance("65535") + 2 * (vertices->style()->pixelMetric(QStyle::PM_FocusFrameHMargin) + 1)); }
        QSignalSpy positions(&panel,&CInspectorPanel::DrawableKeyformPositionsChanged); QSignalSpy history(&panel,&CInspectorPanel::HistoryChanged);
        vertices->item(65535,1)->setText("1.25"); QCOMPARE(document.DrawableKeyformPositions(id,0)[131070],1.25f); QCOMPARE(document.DrawableKeyformPositions(id,0)[0],before[0]); QCOMPARE(positions.size(),1); QCOMPARE(history.size(),1);
        panel.Undo(); QCOMPARE(document.DrawableKeyformPositions(id,0),before); panel.Redo(); QCOMPARE(document.DrawableKeyformPositions(id,0)[131070],1.25f); panel.Undo(); document.ClearEditHistory(); positions.clear(); history.clear();
        auto* copy = panel.findChild<QPushButton*>("copyTopology"); auto* paste = panel.findChild<QPushButton*>("pasteTopology"); QVERIFY(copy && paste); QVERIFY(copy->isEnabled() && paste->isEnabled());
        copy->click(); QJsonObject clipboard = QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object(); QCOMPARE(clipboard["vertexCount"].toInt(),65536); QCOMPARE(clipboard["indices"].toArray().last().toInt(),65535);
        const auto original = document.DrawableIndices(id); const QVector<quint16> changed{65535,65534,32768}; QVERIFY(original != changed);
        clipboard["indices"] = QJsonArray{65535,65534,32768}; QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(clipboard).toJson()));
        QSignalSpy indices(&panel,&CInspectorPanel::DrawableIndicesChanged); paste->click(); QCOMPARE(document.DrawableIndices(id),changed); QCOMPARE(indices.size(),1); QCOMPARE(history.size(),1);
        panel.Undo(); QCOMPARE(document.DrawableIndices(id),original); panel.Redo(); QCOMPARE(document.DrawableIndices(id),changed);
        document.ClearEditHistory(); indices.clear(); history.clear();
        for (double invalid : {65536.0,-1.0,65535.5}) {
            clipboard["indices"] = QJsonArray{0,1,invalid}; QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(clipboard).toJson())); bool rejected = false;
            QTimer::singleShot(0,&panel,[&] { for (auto* message : panel.findChildren<QMessageBox*>()) { rejected = true; message->accept(); } }); paste->click();
            QVERIFY(rejected); QCOMPARE(document.DrawableIndices(id),changed); QVERIFY(!document.CanUndoEdit()); QCOMPARE(indices.size(),0); QCOMPARE(history.size(),0); QCOMPARE(vertices->item(65535,0)->text(),QString("65535"));
        }
        WideMeshData::Fixture smaller; QVERIFY2(WideMeshData::make(65535,WideMeshData::Layout::Sparse,smaller,&error),qPrintable(error));
        panel.SetDocument(nullptr); QVERIFY2(document.LoadBytes(smaller.bytes,"smaller-wide.moc3",&error),qPrintable(error)); panel.SetDocument(&document); panel.SetSelection(smaller.ids.first(),"artmesh");
        const auto smallerBefore = document.DrawableIndices(smaller.ids.first()); clipboard["vertexCount"] = 65535; clipboard["indices"] = QJsonArray{0,1,65535}; QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(clipboard).toJson()));
        bool rejected = false; QTimer::singleShot(0,&panel,[&] { for (auto* message : panel.findChildren<QMessageBox*>()) { rejected = true; message->accept(); } }); paste->click();
        QVERIFY(rejected); QCOMPARE(document.DrawableIndices(smaller.ids.first()),smallerBefore); QVERIFY(!document.CanUndoEdit());
    }
    void invalidTopologyLeavesDocumentUnchanged()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto mesh = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [](const auto& drawable) { return drawable.vertexCount >= 3; });
        QVERIFY(mesh != document.Drawables().cend());
        const QString id = mesh->id;
        const QVector<quint16> before{0, 1, 2};
        QVERIFY(document.SetDrawableIndices(id, before));
        for (const auto& invalid : {QVector<quint16>{}, QVector<quint16>{0, 1}, QVector<quint16>{0, 1, 32768}}) {
            QVERIFY(!document.SetDrawableIndices(id, invalid));
            QCOMPARE(document.DrawableIndices(id), before);
        }
        QVERIFY(!document.SetDrawableIndices(QStringLiteral("missing-artmesh"), before));
        QCOMPARE(document.DrawableIndices(id), before);
        const QVector<quint16> degenerate{0, 0, 1, 0, 0, 1};
        QVERIFY(document.SetDrawableIndices(id, degenerate));
        QCOMPARE(document.DrawableIndices(id), degenerate);
        QVERIFY(!document.CanUndoEdit());
    }

    void keyformSelectionTracksDisplayedMesh()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto sourceMeshes = document.Drawables();
        const auto mesh = std::find_if(sourceMeshes.cbegin(), sourceMeshes.cend(),
            [](const auto& drawable) { return drawable.vertexCount > 0 && drawable.keyforms.size() >= 3; });
        QVERIFY(mesh != sourceMeshes.cend());
        const auto other = std::find_if(sourceMeshes.cbegin(), sourceMeshes.cend(),
            [&mesh](const auto& drawable) {
                return drawable.vertexCount > 0 && !drawable.keyforms.isEmpty() &&
                       drawable.keyforms.size() < mesh->keyforms.size();
            });
        QVERIFY(other != sourceMeshes.cend());
        CInspectorPanel panel;
        panel.SetDocument(&document);
        QSignalSpy activated(&panel, &CInspectorPanel::DrawableKeyformActivated);
        QSignalSpy selected(&panel, &CInspectorPanel::DrawableKeyformSelected);
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy changed(&panel, &CInspectorPanel::DrawableKeyformPositionsChanged);
        auto* selector = panel.findChild<QComboBox*>("keyformSelector");
        auto* vertices = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(selector && vertices);
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.last().at(0).toString(), mesh->id);
        QCOMPARE(activated.last().at(1).toInt(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changed.count(), 0);
        const int index = mesh->keyforms.size() / 2;
        selector->setCurrentIndex(index);
        QCOMPARE(selected.count(), 1);
        selected.clear();
        QCOMPARE(activated.last().at(1).toInt(), index);
        panel.RefreshSelection();
        QCOMPARE(selector->currentIndex(), index);
        QCOMPARE(activated.last().at(1).toInt(), index);
        QCOMPARE(history.count(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changed.count(), 0);
        QVERIFY(!document.CanUndoEdit());

        const QVector<float> before = document.DrawableKeyformPositions(mesh->id, index);
        QVERIFY(vertices->rowCount() > 0);
        vertices->item(0, 1)->setText(QString::number(before.first() + 0.25f, 'f', 6));
        const auto edited = document.DrawableKeyformPositions(mesh->id, index);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.last().at(1).toInt(), index);
        QVERIFY(edited != before);
        const int historyCount = history.count();
        panel.RefreshSelection();
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(selector->currentIndex(), index);
        QCOMPARE(history.count(), historyCount);
        panel.Undo();
        QCOMPARE(selector->currentIndex(), index);
        QCOMPARE(activated.last().at(1).toInt(), index);
        QCOMPARE(document.DrawableKeyformPositions(mesh->id, index), before);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(selector->currentIndex(), index);
        QCOMPARE(activated.last().at(1).toInt(), index);
        QCOMPARE(document.DrawableKeyformPositions(mesh->id, index), edited);
        QVERIFY(!document.CanRedoEdit());
        QCOMPARE(selected.count(), 0);
        selector->setCurrentIndex(index - 1);
        QCOMPARE(vertices->rowCount(), mesh->vertexCount);
        QVERIFY(vertices->isEnabled());
        auto* copyNext = panel.findChild<QPushButton*>("copyKeyformToNext");
        QVERIFY(copyNext && copyNext->isEnabled());
        selected.clear();
        copyNext->click();
        QCOMPARE(changed.last().at(1).toInt(), index);
        QCOMPARE(selector->currentIndex(), index - 1);
        QCOMPARE(activated.last().at(1).toInt(), index - 1);
        QCOMPARE(selected.count(), 0);

        selector->setCurrentIndex(mesh->keyforms.size() - 1);
        activated.clear();
        selected.clear();
        changed.clear();
        panel.SetSelection(other->id, QStringLiteral("artmesh"));
        QCOMPARE(selector->currentIndex(), 0);
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.last().at(0).toString(), other->id);
        QCOMPARE(activated.last().at(1).toInt(), 0);
        QCOMPARE(vertices->rowCount(), other->vertexCount);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changed.count(), 0);
        panel.ClearSelection();
        activated.clear();
        panel.SetSelection(mesh->id, QStringLiteral("artmesh"));
        QCOMPARE(activated.count(), 1);
        QCOMPARE(selector->currentIndex(), 0);
        QCOMPARE(activated.last().at(1).toInt(), 0);
        QCOMPARE(selected.count(), 0);
        QCOMPARE(changed.count(), 0);
    }

    void warpKeyformSelectionSurvivesRefreshAndHistory()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const auto sourceDeformers = document.Deformers();
        const auto warp = std::find_if(sourceDeformers.cbegin(), sourceDeformers.cend(),
            [](const auto& deformer) {
                return deformer.warpKeyforms.size() > 1 && !deformer.warpKeyforms[1].positions.isEmpty();
            });
        QVERIFY(warp != sourceDeformers.cend());
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(warp->id, QStringLiteral("warp"));
        auto* selector = panel.findChild<QComboBox*>("warpKeyformSelector");
        auto* vertices = panel.findChild<QTableWidget*>("warpKeyformTable");
        QVERIFY(selector && vertices);
        selector->setCurrentIndex(1);
        panel.RefreshSelection();
        QCOMPARE(selector->currentIndex(), 1);
        QVERIFY(!document.CanUndoEdit());
        const auto before = document.DeformerWarpKeyforms(warp->id);
        QVERIFY(vertices->rowCount() > 0);
        vertices->item(0, 1)->setText(QString::number(before[1].positions.first() + 0.25f, 'g', 9));
        const auto edited = document.DeformerWarpKeyforms(warp->id);
        QVERIFY(edited != before);
        panel.Undo();
        QCOMPARE(selector->currentIndex(), 1);
        QCOMPARE(document.DeformerWarpKeyforms(warp->id), before);
        QVERIFY(!document.CanUndoEdit());
        panel.Redo();
        QCOMPARE(selector->currentIndex(), 1);
        QCOMPARE(document.DeformerWarpKeyforms(warp->id), edited);
        QVERIFY(!document.CanRedoEdit());
    }

    void editsUndoAndRedo()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const QString id = document.Drawables().first().id;
        const auto& drawable = document.Drawables().first();
        const bool initialVisibility = drawable.visible;
        const float initialOpacity = document.DrawableOpacity(drawable);
        const int initialDrawOrder = qRound(document.DrawableDrawOrder(drawable));

        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* visibility = panel.findChild<QCheckBox*>();
        auto* opacity = panel.findChild<QDoubleSpinBox*>();
        auto* drawOrder = panel.findChild<QSpinBox*>();
        QVERIFY(visibility && opacity && drawOrder);

        visibility->setChecked(!initialVisibility);
        QCOMPARE(document.Drawables().first().visible, !initialVisibility);
        panel.Undo();
        QCOMPARE(document.Drawables().first().visible, initialVisibility);
        panel.Redo();
        QCOMPARE(document.Drawables().first().visible, !initialVisibility);

        opacity->setValue(0.25);
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), 0.25f);
        panel.Undo();
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), initialOpacity);
        panel.Redo();
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), 0.25f);

        drawOrder->setValue(initialDrawOrder == 1000 ? 999 : initialDrawOrder + 1);
        const int changedDrawOrder = drawOrder->value();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), changedDrawOrder);
        panel.Undo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), initialDrawOrder);
        panel.Redo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), changedDrawOrder);

        auto* keyformSelector = panel.findChild<QComboBox*>("keyformSelector");
        auto* vertexTable = panel.findChild<QTableWidget*>("keyformVertexTable");
        QVERIFY(keyformSelector && vertexTable);
        QVERIFY(keyformSelector->count() > 0);
        QVERIFY(vertexTable->rowCount() > 0);
        QSignalSpy keyformSelections(&panel, &CInspectorPanel::DrawableKeyformSelected);
        if (keyformSelector->count() > 1) {
            keyformSelector->setCurrentIndex(1);
            QCOMPARE(keyformSelections.count(), 1);
            QCOMPARE(keyformSelections.last().at(0).toString(), id);
            QCOMPARE(keyformSelections.last().at(1).toInt(), 1);
            keyformSelector->setCurrentIndex(0);
        }
        const QVector<float> keyformBefore = document.DrawableKeyformPositions(id, 0);
        QVERIFY(!keyformBefore.isEmpty());
        const float editedX = keyformBefore.first() + 0.5f;
        vertexTable->item(0, 1)->setText(QString::number(editedX, 'f', 6));
        QVERIFY(qAbs(document.DrawableKeyformPositions(id, 0).first() - editedX) < 0.00001f);
        panel.Undo();
        QVERIFY(qAbs(document.DrawableKeyformPositions(id, 0).first() - keyformBefore.first()) < 0.00001f);
        panel.Redo();
        QVERIFY(qAbs(document.DrawableKeyformPositions(id, 0).first() - editedX) < 0.00001f);
        auto* moveVertices = panel.findChild<QPushButton*>("moveSelectedVertices");
        auto* deltaX = panel.findChild<QDoubleSpinBox*>("vertexDeltaX");
        auto* deltaY = panel.findChild<QDoubleSpinBox*>("vertexDeltaY");
        QVERIFY(moveVertices && deltaX && deltaY);
        const QVector<float> translateBefore = document.DrawableKeyformPositions(id, 0);
        QVERIFY(translateBefore.size() >= 4);
        vertexTable->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        deltaX->setValue(0.125);
        deltaY->setValue(-0.25);
        QVERIFY(moveVertices->isEnabled());
        moveVertices->click();
        const QVector<float> translateAfter = document.DrawableKeyformPositions(id, 0);
        QCOMPARE(translateAfter[0], translateBefore[0] + 0.125f);
        QCOMPARE(translateAfter[1], translateBefore[1] - 0.25f);
        QCOMPARE(translateAfter[2], translateBefore[2] + 0.125f);
        QCOMPARE(translateAfter[3], translateBefore[3] - 0.25f);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), translateBefore);
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), translateAfter);
        auto* mergeVertices = panel.findChild<QPushButton*>("mergeSelectedVertices");
        QVERIFY(mergeVertices);
        vertexTable->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        const QVector<quint16> mergeBefore = document.DrawableIndices(id);
        mergeVertices->click();
        const QVector<quint16> mergeAfter = document.DrawableIndices(id);
        QVERIFY(!mergeAfter.isEmpty() && mergeAfter.size() % 3 == 0);
        QVERIFY(mergeAfter != mergeBefore);
        panel.Undo();
        QCOMPARE(document.DrawableIndices(id), mergeBefore);
        panel.Redo();
        QCOMPARE(document.DrawableIndices(id), mergeAfter);
        auto* transformVertices = panel.findChild<QPushButton*>("transformSelectedVertices");
        auto* scaleVertices = panel.findChild<QDoubleSpinBox*>("vertexScale");
        auto* rotationVertices = panel.findChild<QDoubleSpinBox*>("vertexRotation");
        QVERIFY(transformVertices && scaleVertices && rotationVertices);
        vertexTable->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        const QVector<float> transformBefore = document.DrawableKeyformPositions(id, 0);
        const float centerX = (transformBefore[0] + transformBefore[2]) * 0.5f;
        const float centerY = (transformBefore[1] + transformBefore[3]) * 0.5f;
        scaleVertices->setValue(2.0);
        rotationVertices->setValue(0.0);
        transformVertices->click();
        const QVector<float> transformAfter = document.DrawableKeyformPositions(id, 0);
        QCOMPARE(transformAfter[0], centerX + (transformBefore[0] - centerX) * 2.0f);
        QCOMPARE(transformAfter[1], centerY + (transformBefore[1] - centerY) * 2.0f);
        QCOMPARE(transformAfter[2], centerX + (transformBefore[2] - centerX) * 2.0f);
        QCOMPARE(transformAfter[3], centerY + (transformBefore[3] - centerY) * 2.0f);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), transformBefore);
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), transformAfter);
        const QVector<float> rotateBefore = transformAfter;
        scaleVertices->setValue(1.0);
        rotationVertices->setValue(90.0);
        vertexTable->setRangeSelected(QTableWidgetSelectionRange(0, 0, 1, 2), true);
        transformVertices->click();
        const QVector<float> rotateAfter = document.DrawableKeyformPositions(id, 0);
        const float rotateCenterX = (rotateBefore[0] + rotateBefore[2]) * 0.5f;
        const float rotateCenterY = (rotateBefore[1] + rotateBefore[3]) * 0.5f;
        QCOMPARE(rotateAfter[0], rotateCenterX - (rotateBefore[1] - rotateCenterY));
        QCOMPARE(rotateAfter[1], rotateCenterY + (rotateBefore[0] - rotateCenterX));
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), rotateBefore);
        panel.ClearHistory();
        QString deformerId;
        int deformerParentChoice = -1;
        for (int i = 0; i < document.Deformers().size() && deformerId.isEmpty(); ++i) {
            for (int j = 0; j < document.Deformers().size(); ++j) {
                if (i == j || document.Deformers()[i].parentPartIndex != document.Deformers()[j].parentPartIndex ||
                    document.DeformerParentIndex(document.Deformers()[i].id) == j) continue;
                deformerId = document.Deformers()[i].id;
                deformerParentChoice = j;
                break;
            }
        }
        QVERIFY(!deformerId.isEmpty() && deformerParentChoice >= 0);
        QString deformerType = QStringLiteral("warp");
        for (const auto& deformer : document.Deformers())
            if (deformer.id == deformerId && deformer.type == live2dstruct::CModelDeformer::Rotation)
                deformerType = QStringLiteral("rotation");
        panel.SetSelection(deformerId, deformerType);
        auto* deformerParent = panel.findChild<QComboBox*>("deformerParentSelector");
        QVERIFY(deformerParent && deformerParent->count() > 1);
        const int parentRow = deformerParent->findData(deformerParentChoice);
        QVERIFY(parentRow >= 0);
        const int oldDeformerParent = document.DeformerParentIndex(deformerId);
        deformerParent->setCurrentIndex(parentRow);
        QCOMPARE(document.DeformerParentIndex(deformerId), deformerParentChoice);
        panel.Undo();
        QCOMPARE(document.DeformerParentIndex(deformerId), oldDeformerParent);
        panel.Redo();
        QCOMPARE(document.DeformerParentIndex(deformerId), deformerParentChoice);
        if (deformerType == QLatin1String("rotation")) {
            auto* rotationTable = panel.findChild<QTableWidget*>("rotationKeyformTable");
            QVERIFY(rotationTable && rotationTable->rowCount() > 0 && rotationTable->isEnabled());
            const auto rotationBefore = document.DeformerRotationKeyforms(deformerId);
            const float angleBefore = rotationBefore.first().angle;
            rotationTable->item(0, 1)->setText(QString::number(angleBefore + 6.0f));
            const auto rotationAfter = document.DeformerRotationKeyforms(deformerId);
            QCOMPARE(rotationAfter.first().angle, angleBefore + 6.0f);
            panel.Undo();
            QCOMPARE(document.DeformerRotationKeyforms(deformerId).first().angle, angleBefore);
            panel.Redo();
            QCOMPARE(document.DeformerRotationKeyforms(deformerId).first().angle, angleBefore + 6.0f);
        } else {
            auto* warpSelector = panel.findChild<QComboBox*>("warpKeyformSelector");
            auto* warpTable = panel.findChild<QTableWidget*>("warpKeyformTable");
            QVERIFY(warpSelector && warpTable && warpSelector->count() > 0 && warpTable->rowCount() > 0 && warpTable->isEnabled());
            const auto warpBefore = document.DeformerWarpKeyforms(deformerId);
            const float xBefore = warpBefore.first().positions.first();
            warpTable->item(0, 1)->setText(QString::number(xBefore + 2.5f));
            QCOMPARE(document.DeformerWarpKeyforms(deformerId).first().positions.first(), xBefore + 2.5f);
            panel.Undo();
            QCOMPARE(document.DeformerWarpKeyforms(deformerId).first().positions.first(), xBefore);
            panel.Redo();
            QCOMPARE(document.DeformerWarpKeyforms(deformerId).first().positions.first(), xBefore + 2.5f);
        }
        QString maskedId;
        for (const auto& drawableCandidate : document.Drawables()) {
            if (!drawableCandidate.maskIndices.isEmpty()) { maskedId = drawableCandidate.id; break; }
        }
        if (!maskedId.isEmpty()) {
            panel.SetSelection(maskedId, QStringLiteral("artmesh"));
            auto* maskInverted = panel.findChild<QCheckBox*>("maskInverted");
            QVERIFY(maskInverted && maskInverted->isEnabled());
            const bool beforeMaskInverted = document.DrawableMaskInverted(maskedId);
            maskInverted->setChecked(!beforeMaskInverted);
            QCOMPARE(document.DrawableMaskInverted(maskedId), !beforeMaskInverted);
            panel.Undo();
            QCOMPARE(document.DrawableMaskInverted(maskedId), beforeMaskInverted);
            panel.Redo();
            QCOMPARE(document.DrawableMaskInverted(maskedId), !beforeMaskInverted);
        }
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* mirrorHorizontal = panel.findChild<QPushButton*>("mirrorHorizontal");
        QVERIFY(mirrorHorizontal && mirrorHorizontal->isEnabled());
        const QVector<float> mirrorBefore = document.DrawableKeyformPositions(id, 0);
        float minX = mirrorBefore.first();
        float maxX = minX;
        for (int i = 0; i < mirrorBefore.size(); i += 2) {
            minX = qMin(minX, mirrorBefore[i]);
            maxX = qMax(maxX, mirrorBefore[i]);
        }
        const float mirrorCenter = (minX + maxX) * 0.5f;
        mirrorHorizontal->click();
        const QVector<float> mirrorAfter = document.DrawableKeyformPositions(id, 0);
        QVERIFY(qAbs(mirrorAfter.first() - (2.0f * mirrorCenter - mirrorBefore.first())) < 0.00001f);
        panel.Undo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), mirrorBefore);
        panel.Redo();
        QCOMPARE(document.DrawableKeyformPositions(id, 0), mirrorAfter);
        auto* interpolatePrevious = panel.findChild<QPushButton*>("interpolatePreviousKeyform");
        QVERIFY(interpolatePrevious);
        if (interpolatePrevious->isEnabled() && keyformSelector->count() > 1) {
            keyformSelector->setCurrentIndex(1);
            const QVector<float> current = document.DrawableKeyformPositions(id, 1);
            const QVector<float> previous = document.DrawableKeyformPositions(id, 0);
            QVERIFY(current.size() == previous.size() && !current.isEmpty());
            interpolatePrevious->click();
            const QVector<float> interpolated = document.DrawableKeyformPositions(id, 1);
            QVERIFY(qAbs(interpolated.first() - (current.first() + previous.first()) * 0.5f) < 0.00001f);
            panel.Undo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), current);
            panel.Redo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), interpolated);
            keyformSelector->setCurrentIndex(0);
        }
        auto* copyToNext = panel.findChild<QPushButton*>("copyKeyformToNext");
        auto* resetKeyform = panel.findChild<QPushButton*>("resetKeyform");
        auto* autoTriangulate = panel.findChild<QPushButton*>("autoTriangulate");
        QVERIFY(copyToNext && resetKeyform && autoTriangulate);
        if (copyToNext->isEnabled() && keyformSelector->count() > 1) {
            const QVector<float> nextBefore = document.DrawableKeyformPositions(id, 1);
            const QVector<float> current = document.DrawableKeyformPositions(id, 0);
            copyToNext->click();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), current);
            panel.Undo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), nextBefore);
            panel.Redo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), current);
            keyformSelector->setCurrentIndex(1);
            resetKeyform->click();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), document.Drawables().first().keyforms.at(1).positions);
            panel.Undo();
            QCOMPARE(document.DrawableKeyformPositions(id, 1), current);
            keyformSelector->setCurrentIndex(0);
        }
        const QVector<quint16> topologyBefore = document.DrawableIndices(id);
        QSignalSpy topologyChanges(&panel, &CInspectorPanel::DrawableIndicesChanged);
        QTimer::singleShot(0, &panel, [&] {
            if (auto* dialog = panel.findChild<QMessageBox*>("rebuildTopologyConfirmation"))
                dialog->done(QMessageBox::Yes);
        });
        autoTriangulate->click();
        QVERIFY(topologyChanges.count() > 0);
        const QVector<quint16> topologyAfter = document.DrawableIndices(id);
        QVERIFY(!topologyAfter.isEmpty() && topologyAfter.size() % 3 == 0);
        QVERIFY(topologyAfter != topologyBefore);
        panel.Undo();
        QCOMPARE(document.DrawableIndices(id), topologyBefore);
        panel.Redo();
        QCOMPARE(document.DrawableIndices(id), topologyAfter);
        panel.ClearHistory();

        const QString secondId = document.Drawables().at(1).id;
        const float secondOpacity = document.DrawableOpacity(document.Drawables().at(1));
        const int secondDrawOrder = qRound(document.DrawableDrawOrder(document.Drawables().at(1)));
        const bool secondVisibility = document.Drawables().at(1).visible;
        const int currentDrawOrder = qRound(document.DrawableDrawOrder(document.Drawables().first()));
        const bool firstVisibilityBefore = document.Drawables().first().visible;
        panel.SetSelections({id, secondId}, {QStringLiteral("artmesh"), QStringLiteral("artmesh")});
        const bool targetVisibility = !document.Drawables().first().visible;
        const int visibilityChanges = (document.Drawables().first().visible != targetVisibility ? 1 : 0) +
            (document.Drawables().at(1).visible != targetVisibility ? 1 : 0);
        QVERIFY(visibility->isEnabled());
        visibility->setCheckState(targetVisibility ? Qt::Checked : Qt::Unchecked);
        QCOMPARE(document.Drawables().first().visible, targetVisibility);
        QCOMPARE(document.Drawables().at(1).visible, targetVisibility);
        QVERIFY(visibilityChanges > 0);
        panel.Undo();
        QCOMPARE(document.Drawables().first().visible, firstVisibilityBefore);
        QCOMPARE(document.Drawables().at(1).visible, secondVisibility);
        panel.Redo();
        panel.ClearHistory();

        const float firstOpacityBefore = document.DrawableOpacity(document.Drawables().first());
        const float secondOpacityBefore = document.DrawableOpacity(document.Drawables().at(1));
        opacity->setValue(0.33);
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), 0.33f);
        QCOMPARE(document.DrawableOpacity(document.Drawables().at(1)), 0.33f);
        const int opacityChanges = (qFuzzyCompare(firstOpacityBefore + 1.0f, 1.33f) ? 0 : 1) +
            (qFuzzyCompare(secondOpacityBefore + 1.0f, 1.33f) ? 0 : 1);
        QVERIFY(opacityChanges > 0);
        for (int i = 0; i < opacityChanges; ++i) panel.Undo();
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), firstOpacityBefore);
        QCOMPARE(document.DrawableOpacity(document.Drawables().at(1)), secondOpacity);
        for (int i = 0; i < opacityChanges; ++i) panel.Redo();
        panel.ClearHistory();

        const int firstDrawOrderBefore = qRound(document.DrawableDrawOrder(document.Drawables().first()));
        const int secondDrawOrderBefore = qRound(document.DrawableDrawOrder(document.Drawables().at(1)));
        drawOrder->setValue(321);
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), 321);
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().at(1))), 321);
        const int drawOrderChanges = (firstDrawOrderBefore == 321 ? 0 : 1) +
            (secondDrawOrderBefore == 321 ? 0 : 1);
        QVERIFY(drawOrderChanges > 0);
        for (int i = 0; i < drawOrderChanges; ++i) panel.Undo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), currentDrawOrder);
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().at(1))), secondDrawOrder);
        for (int i = 0; i < drawOrderChanges; ++i) panel.Redo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), 321);
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().at(1))), 321);
        panel.ClearHistory();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), 321);
        panel.SetSelections({id}, {QStringLiteral("artmesh")});
        panel.SetSelections({}, {});
        QVERIFY(!visibility->isEnabled());
        panel.SetSelection(id, QStringLiteral("artmesh"));
        panel.SetLockedState({QStringLiteral("artmesh|") + id});
        QVERIFY(!visibility->isEnabled());
        QVERIFY(!opacity->isEnabled());
        QVERIFY(!drawOrder->isEnabled());
        panel.SetLockedState({});
        QVERIFY(visibility->isEnabled());
    }
};

QTEST_MAIN(InspectorHistoryTests)
#include "inspector_history_tests.moc"
