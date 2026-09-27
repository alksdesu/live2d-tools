#include "ui/cparameterorganizationdialog.h"
#include "ui/para/ccidjson.h"
#include "ui/theme.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QTimer>
#include <stdexcept>

using namespace live2dstruct;
using Organization = CParameterOrganization;
namespace {
constexpr int kindRole = Qt::UserRole, idRole = Qt::UserRole + 1, colorRole = Qt::UserRole + 2;
QStringList ids(const CModelDocument& document)
{
    QStringList result; for (const auto& parameter : document.Parameters()) result.append(parameter.id); return result;
}
Organization::State organized(const CModelDocument& document, bool pairs)
{
    const auto names = ids(document); Organization::State result; result.configured = true;
    result.nodes.append({Organization::Node::Folder, names[0], {}, "Folder", "#112233"});
    for (int index = 0; index < names.size(); ++index)
        result.nodes.append({Organization::Node::Parameter, names[index], index < 2 ? names[0] : QString(), {}, index == 0 ? "#445566" : QString()});
    if (pairs) result.pairs = {{names[0], names[1], false, true}, {names[2], names[3], false, true}};
    return result;
}
QTreeWidgetItem* item(QTreeWidget* tree, Organization::Node::Kind kind, const QString& id)
{
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, kindRole).toInt() == kind && (*it)->data(0, idRole).toString() == id) return *it;
    return nullptr;
}
Organization::State snapshot(CParameterOrganizationDialog& dialog)
{
    Organization::State result;
    dialog.SetSubmissionHandler([&](const auto& state, QString* error) { result = state; *error = "kept open"; return false; });
    dialog.accept(); dialog.SetSubmissionHandler({}); return result;
}
void select(QTreeWidget* tree, QTreeWidgetItem* current)
{
    tree->clearSelection(); tree->setCurrentItem(current); current->setSelected(true);
}
}
class ParameterOrganizationDialogTests : public QObject
{
    Q_OBJECT
    CModelDocument m_original;
private slots:
    void initTestCase()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QVERIFY(m_original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3")); QVERIFY(m_original.Parameters().size() >= 6);
    }

    void initialStateMergesCdiGroupsAndKeepsTrailingValidParameter()
    {
        const auto parameters = ids(m_original); QTemporaryDir directory; QFile file(directory.filePath("names.cdi3.json"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QJsonObject source{{"ParameterGroups", QJsonArray{QJsonObject{{"Id", "A"}, {"Name", "First"}}, QJsonObject{{"Id", "B"}, {"Name", "Second"}}}},
            {"Parameters", QJsonArray{QJsonObject{{"Id", parameters[0]}, {"GroupId", "A"}}, QJsonObject{{"Id", parameters[1]}, {"GroupId", "B"}},
                QJsonObject{{"Id", parameters[2]}, {"GroupId", "A"}}, QJsonObject{}}}};
        QVERIFY(file.write(QJsonDocument(source).toJson()) > 0); file.close(); CCidJson cdi; QVERIFY(cdi.Parse(file.fileName()));
        const auto state = CParameterOrganizationDialog::InitialState(m_original, &cdi); QSet<QString> parametersFound, folders;
        for (const auto& node : state.nodes) {
            if (node.kind == Organization::Node::Folder) { QVERIFY(!folders.contains(node.id)); folders.insert(node.id); }
            else { QVERIFY(!parametersFound.contains(node.id)); parametersFound.insert(node.id); }
        }
        QCOMPARE(folders, QSet<QString>({"A", "B"})); QCOMPARE(parametersFound, QSet<QString>(parameters.cbegin(), parameters.cend()));
        for (const auto& node : state.nodes) if (node.kind == Organization::Node::Parameter && node.id == parameters[2]) QCOMPARE(node.parentFolder, QString("A"));
    }

    void validPairDraftIsSavedByOverallApplyBeforeSubmission()
    {
        auto document = m_original; QVERIFY(document.SetParameterOrganization(organized(document, true)));
        const auto before = document.ParameterOrganization(); const auto moc = document.SourceBytes();
        CParameterOrganizationDialog dialog(document, nullptr); dialog.show();
        auto* reversed = dialog.findChild<QCheckBox*>("reverseParameterPair"); reversed->setChecked(true);
        int calls = 0; bool reject = true; Organization::State proposal;
        dialog.SetSubmissionHandler([&](const auto& state, QString* error) {
            ++calls; proposal = state;
            if (reject) { *error = "controlled rejection"; return false; }
            CModelEdit edit; return document.MakeParameterOrganizationEdit(state, edit, error) && document.ExecuteEdit(edit);
        });
        dialog.accept(); QCOMPARE(calls, 1); QVERIFY(dialog.isVisible()); QVERIFY(proposal.pairs[0].reversed);
        QCOMPARE(document.ParameterOrganization(), before); QCOMPARE(dialog.Result(), before); QVERIFY(reversed->isChecked());
        QCOMPARE(dialog.findChild<QLabel*>("parameterOrganizationError")->text(), QString("controlled rejection"));
        reject = false; QSignalSpy accepted(&dialog, &QDialog::accepted); dialog.accept();
        QCOMPARE(calls, 2); QCOMPARE(accepted.size(), 1); QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(document.ParameterOrganization(), proposal); QCOMPARE(dialog.Result(), proposal); QCOMPARE(document.SourceBytes(), moc);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.ParameterOrganization(), before); QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.RedoEdit()); QCOMPARE(document.ParameterOrganization(), proposal);
    }

    void changingRowsStoresValidDraftAndRetainsInvalidDraftUntilDiscard()
    {
        auto document = m_original; QVERIFY(document.SetParameterOrganization(organized(document, true)));
        CParameterOrganizationDialog dialog(document, nullptr); dialog.show();
        auto* table = dialog.findChild<QTableWidget*>("parameterPairTable"); auto* reversed = dialog.findChild<QCheckBox*>("reverseParameterPair");
        reversed->setChecked(true); table->setCurrentCell(1, 0);
        QCOMPARE(table->currentRow(), 1); QVERIFY(table->item(0, 2)->data(Qt::UserRole).toBool()); QVERIFY(!reversed->isChecked());
        auto* first = dialog.findChild<QComboBox*>("pairedParameterA"); first->setEditText("unknown parameter");
        table->setCurrentCell(0, 0); QCOMPARE(table->currentRow(), 1); QCOMPARE(first->currentText(), QString("unknown parameter"));
        int calls = 0; dialog.SetSubmissionHandler([&](const auto&, QString*) { ++calls; return true; });
        dialog.accept(); QCOMPARE(calls, 0); QVERIFY(dialog.isVisible()); QVERIFY(!dialog.findChild<QLabel*>("parameterOrganizationError")->text().isEmpty());
        dialog.findChild<QPushButton*>("discardParameterPairDraft")->click();
        QCOMPARE(first->currentData().toString(), document.ParameterOrganization().pairs[1].first);
        table->setCurrentCell(0, 0); QCOMPARE(table->currentRow(), 0); QVERIFY(reversed->isChecked());
        dialog.accept(); QCOMPARE(calls, 1); QVERIFY(dialog.Result().pairs[0].reversed);
    }

    void newPairDraftIsNotLostAndUntouchedDefaultsDoNotCreatePairs()
    {
        CParameterOrganizationDialog untouched(m_original, nullptr); untouched.accept(); QVERIFY(untouched.Result().pairs.isEmpty());
        CParameterOrganizationDialog dialog(m_original, nullptr); dialog.show(); const auto parameters = ids(m_original);
        auto* first = dialog.findChild<QComboBox*>("pairedParameterA"); auto* second = dialog.findChild<QComboBox*>("pairedParameterB");
        first->setCurrentIndex(first->findData(parameters[2])); second->setCurrentIndex(second->findData(parameters[3]));
        dialog.findChild<QCheckBox*>("enableParameterPair")->setChecked(false);
        dialog.accept(); QCOMPARE(dialog.Result().pairs.size(), 1); QCOMPARE(dialog.Result().pairs[0].first, parameters[2]);
        QCOMPARE(dialog.Result().pairs[0].second, parameters[3]); QVERIFY(!dialog.Result().pairs[0].enabled);
    }

    void newPairSelectionStoresPendingPairAndDuplicatePairCanBeCorrected()
    {
        auto document = m_original; QVERIFY(document.SetParameterOrganization(organized(document, true)));
        CParameterOrganizationDialog dialog(document, nullptr); dialog.show(); const auto parameters = ids(document);
        auto* table = dialog.findChild<QTableWidget*>("parameterPairTable"); auto* first = dialog.findChild<QComboBox*>("pairedParameterA");
        auto* second = dialog.findChild<QComboBox*>("pairedParameterB");
        dialog.findChild<QPushButton*>("newParameterPair")->click(); QCOMPARE(table->currentRow(), -1);
        dialog.findChild<QCheckBox*>("reverseParameterPair")->setChecked(true);
        table->setCurrentCell(0, 0); QCOMPARE(table->rowCount(), 3); QVERIFY(table->item(2, 2)->data(Qt::UserRole).toBool());
        first->setCurrentIndex(first->findData(parameters[2])); second->setCurrentIndex(second->findData(parameters[3]));
        dialog.accept(); QVERIFY(dialog.isVisible()); QCOMPARE(table->rowCount(), 3);
        dialog.findChild<QPushButton*>("discardParameterPairDraft")->click();
        dialog.findChild<QPushButton*>("removeParameterPair")->click(); QCOMPARE(table->rowCount(), 2);
        const auto saved = snapshot(dialog); QCOMPARE(saved.pairs.size(), 2); QCOMPARE(saved.nodes.size(), document.ParameterOrganization().nodes.size());
    }

    void typedIdsAndRootFolderMixedOrderingSurviveMoveRenameAndDissolve()
    {
        auto document = m_original; QVERIFY(document.SetParameterOrganization(organized(document, false))); const auto parameters = ids(document);
        CParameterOrganizationDialog dialog(document, nullptr); auto* tree = dialog.findChild<QTreeWidget*>("parameterOrganizationTree");
        auto* folder = item(tree, Organization::Node::Folder, parameters[0]); auto* parameter = item(tree, Organization::Node::Parameter, parameters[0]);
        QVERIFY(folder && parameter && folder != parameter); QCOMPARE(parameter->parent(), folder);
        select(tree, parameter); auto* destination = dialog.findChild<QComboBox*>("parameterFolderDestination"); destination->setCurrentIndex(destination->findData(QString()));
        dialog.findChild<QPushButton*>("moveParametersToFolder")->click(); QVERIFY(!parameter->parent());
        QCOMPARE(folder->data(0, colorRole).toString(), QString("#112233")); QCOMPARE(parameter->data(0, colorRole).toString(), QString("#445566"));
        select(tree, folder); folder->setText(0, "Renamed folder"); dialog.findChild<QAction*>("moveParameterDown")->trigger();
        const auto moved = snapshot(dialog); QCOMPARE(moved.nodes[0].kind, Organization::Node::Parameter); QCOMPARE(moved.nodes[1].kind, Organization::Node::Folder);
        QCOMPARE(moved.nodes[1].name, QString("Renamed folder"));
        auto reopenedDocument = m_original; QVERIFY(reopenedDocument.SetParameterOrganization(moved));
        CParameterOrganizationDialog reopened(reopenedDocument, nullptr); QCOMPARE(snapshot(reopened), moved);
        select(tree, folder); const auto child = folder->child(0)->data(0, idRole).toString();
        dialog.findChild<QAction*>("dissolveParameterFolder")->trigger(); QVERIFY(!item(tree, Organization::Node::Folder, parameters[0]));
        QVERIFY(item(tree, Organization::Node::Parameter, parameters[0])); QVERIFY(!item(tree, Organization::Node::Parameter, child)->parent());
        const auto dissolved = snapshot(dialog); QCOMPARE(dissolved.nodes.size(), parameters.size());
    }

    void editedFolderNameIsCommittedBeforeSubmission()
    {
        auto document = m_original; QVERIFY(document.SetParameterOrganization(organized(document, false)));
        CParameterOrganizationDialog dialog(document, nullptr); dialog.show(); auto* tree = dialog.findChild<QTreeWidget*>("parameterOrganizationTree");
        auto* folder = tree->topLevelItem(0); tree->editItem(folder, 0);
        auto* editor = tree->findChild<QLineEdit*>(); QVERIFY(editor); editor->setText("Edited inline");
        dialog.accept(); QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(dialog.Result().nodes[0].name, QString("Edited inline"));
        QCOMPARE(folder->toolTip(0), QString("Edited inline"));
    }

    void movingMultipleParametersPreservesVisualOrder()
    {
        auto document = m_original; QVERIFY(document.SetParameterOrganization(organized(document, false)));
        const auto parameters = ids(document); CParameterOrganizationDialog dialog(document, nullptr);
        auto* tree = dialog.findChild<QTreeWidget*>("parameterOrganizationTree");
        auto* folder = item(tree, Organization::Node::Folder, parameters[0]);
        auto* first = item(tree, Organization::Node::Parameter, parameters[2]);
        auto* second = item(tree, Organization::Node::Parameter, parameters[3]);
        select(tree, second); first->setSelected(true);
        auto* destination = dialog.findChild<QComboBox*>("parameterFolderDestination");
        destination->setCurrentIndex(destination->findData(parameters[0]));
        dialog.findChild<QPushButton*>("moveParametersToFolder")->click();
        QCOMPARE(folder->childCount(), 4); QCOMPARE(folder->child(2), first); QCOMPARE(folder->child(3), second);
        QCOMPARE(snapshot(dialog).nodes.size(), document.ParameterOrganization().nodes.size());
    }

    void newFolderAndCustomColorPreserveSelectedParameters()
    {
        CParameterOrganizationDialog dialog(m_original, nullptr); dialog.show(); auto* tree = dialog.findChild<QTreeWidget*>("parameterOrganizationTree");
        const auto id = m_original.Parameters()[0].id; auto* parameter = item(tree, Organization::Node::Parameter, id); QVERIFY(parameter); select(tree, parameter);
        QTimer::singleShot(0, &dialog, [&] { auto* input = dialog.findChild<QInputDialog*>("parameterFolderNameDialog"); QVERIFY(input); input->setTextValue("New folder"); input->accept(); });
        dialog.findChild<QAction*>("addParameterFolder")->trigger(); QVERIFY(parameter->parent()); auto* folder = parameter->parent(); QCOMPARE(folder->text(0), QString("New folder"));
        select(tree, parameter);
        QTimer::singleShot(0, &dialog, [&] { auto* picker = dialog.findChild<QColorDialog*>("parameterLabelColorDialog"); QVERIFY(picker); picker->setCurrentColor(QColor("#17A0C3")); picker->accept(); });
        dialog.findChild<QAction*>("parameterLabelColor")->trigger(); QCOMPARE(parameter->data(0, colorRole).toString(), QString("#17A0C3")); QVERIFY(!parameter->icon(0).isNull());
        const auto state = snapshot(dialog); bool found = false;
        for (const auto& node : state.nodes) if (node.kind == Organization::Node::Parameter && node.id == id) { found = true; QCOMPARE(node.color, QString("#17A0C3")); QCOMPARE(node.parentFolder, folder->data(0, idRole).toString()); }
        QVERIFY(found);
        QTimer::singleShot(0, &dialog, [&] { auto* picker = dialog.findChild<QColorDialog*>("parameterLabelColorDialog"); QVERIFY(picker); QCOMPARE(picker->currentColor(), QColor("#17A0C3")); picker->reject(); });
        dialog.findChild<QAction*>("parameterLabelColor")->trigger(); QCOMPARE(parameter->data(0, colorRole).toString(), QString("#17A0C3"));
        dialog.findChild<QAction*>("clearParameterLabelColor")->trigger(); QVERIFY(parameter->data(0, colorRole).toString().isEmpty());
    }

    void rejectionExceptionAndReentryKeepDialogEditable()
    {
        CParameterOrganizationDialog dialog(m_original, nullptr); dialog.show(); int calls = 0;
        QSignalSpy accepted(&dialog, &QDialog::accepted), rejected(&dialog, &QDialog::rejected);
        dialog.SetSubmissionHandler([&](const auto&, QString*) -> bool { ++calls; dialog.accept(); dialog.reject(); dialog.SetSubmissionHandler({}); throw std::runtime_error("failure"); });
        dialog.accept(); QCOMPARE(calls, 1); QCOMPARE(accepted.size(), 0); QCOMPARE(rejected.size(), 0); QVERIFY(dialog.isVisible());
        QVERIFY(dialog.findChild<QComboBox*>("pairedParameterA")->isEnabled());
        dialog.accept(); QCOMPARE(calls, 2); QVERIFY(!dialog.findChild<QLabel*>("parameterOrganizationError")->text().isEmpty());
        dialog.SetSubmissionHandler([](const auto&, QString*) { return true; }); dialog.accept(); QCOMPARE(accepted.size(), 1);
    }

    void submissionCallbackCanCloseParentAsynchronously()
    {
        auto* parent = new QWidget; auto* dialog = new CParameterOrganizationDialog(m_original, nullptr, parent);
        QPointer<CParameterOrganizationDialog> alive(dialog); QPointer<QWidget> parentAlive(parent); dialog->show();
        dialog->SetSubmissionHandler([parent](const auto&, QString*) { parent->deleteLater(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); return true; });
        dialog->accept(); QVERIFY(alive.isNull()); QVERIFY(parentAlive.isNull());
    }

    void closingParentDuringChildDialogIsSafe_data()
    {
        QTest::addColumn<bool>("color"); QTest::newRow("folder") << false; QTest::newRow("color") << true;
    }
    void closingParentDuringChildDialogIsSafe()
    {
        QFETCH(bool, color); auto* parent = new QWidget; auto* dialog = new CParameterOrganizationDialog(m_original, nullptr, parent);
        QPointer<CParameterOrganizationDialog> alive(dialog); dialog->show(); auto* tree = dialog->findChild<QTreeWidget*>("parameterOrganizationTree"); select(tree, tree->topLevelItem(0));
        auto* action = dialog->findChild<QAction*>(color ? "parameterLabelColor" : "addParameterFolder"); QVERIFY(action);
        QTimer::singleShot(0, qApp, [parent] { parent->deleteLater(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); });
        action->trigger(); QVERIFY(alive.isNull());
    }

    void narrowAndWideThemesKeepLargeLabelsWithinDialog_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {360, 900}) QTest::newRow(qPrintable(QString("%1-%2").arg(dark).arg(width))) << dark << width;
    }
    void narrowAndWideThemesKeepLargeLabelsWithinDialog()
    {
        QFETCH(bool, dark); QFETCH(int, width); qApp->setStyleSheet(Theme::globalStyleSheet(dark)); auto document = m_original;
        auto metadata = document.ParameterMetadata(document.Parameters()[0].id); metadata.displayName = QString(512, QLatin1Char('W'));
        QVERIFY(document.SetParameterMetadata(document.Parameters()[0].id, metadata)); QVERIFY(document.SetParameterOrganization(organized(document, true)));
        CParameterOrganizationDialog dialog(document, nullptr); dialog.resize(width, 600); dialog.show(); QTest::qWait(20);
        auto* tabs = dialog.findChild<QTabWidget*>("parameterOrganizationTabs"); tabs->setCurrentIndex(1); QTest::qWait(20);
        QVERIFY2(dialog.width() <= width, qPrintable(QString("actual width %1 exceeds %2").arg(dialog.width()).arg(width)));
        for (const auto* combo : dialog.findChildren<QComboBox*>()) QVERIFY(combo->minimumSizeHint().width() < 150);
        for (const auto* combo : {dialog.findChild<QComboBox*>("pairedParameterA"), dialog.findChild<QComboBox*>("pairedParameterB")}) {
            QVERIFY(combo->width() > width / 2); QCOMPARE(combo->toolTip(), combo->currentText());
        }
        auto* scroll = dialog.findChild<QScrollArea*>("parameterOrganizationScroll"); QVERIFY(scroll); QVERIFY(scroll->viewport()->width() > 0);
        for (const auto* button : dialog.findChildren<QToolButton*>()) QVERIFY(button->focusPolicy() != Qt::NoFocus);
        const auto destination = qEnvironmentVariable("PARAMETER_ORGANIZATION_SCREENSHOT_DIR");
        if (!destination.isEmpty()) {
            QDir().mkpath(destination); QVERIFY(dialog.grab().save(destination + QString("/pair-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
            tabs->setCurrentIndex(0); QTest::qWait(20);
            QVERIFY(dialog.grab().save(destination + QString("/folders-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
        }
        qApp->setStyleSheet({});
    }
};
QTEST_MAIN(ParameterOrganizationDialogTests)
#include "parameter_organization_dialog_tests.moc"
