#include "ui/cparampanel.h"
#include "ui/cparameterorganizationdialog.h"
#include "ui/para/ccidjson.h"
#include "ui/theme.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

using namespace live2dstruct;
using Organization = CParameterOrganization;
namespace {
Organization::State organized(const CModelDocument& document)
{
    auto state = CParameterOrganizationDialog::InitialState(document, nullptr);
    state.nodes.prepend({Organization::Node::Folder, "group", {}, "Eyes & face", "#198A71"});
    for (int index = 1; index < 3; ++index) state.nodes[index].parentFolder = "group";
    return state;
}
QWidget* row(CParamPanel& panel, const QString& id)
{
    for (auto* widget : panel.findChildren<QWidget*>()) if (widget->property("parameterId").toString() == id) return widget;
    return nullptr;
}
QToolButton* folder(CParamPanel& panel)
{
    for (auto* button : panel.findChildren<QToolButton*>()) if (button->property("parameterGroupId").toString() == "group") return button;
    return nullptr;
}
QTreeWidgetItem* folder(CParameterOrganizationDialog& dialog)
{
    auto* tree = dialog.findChild<QTreeWidget*>("parameterOrganizationTree");
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
        if (tree->topLevelItem(index)->data(0, Qt::UserRole + 1).toString() == "group") return tree->topLevelItem(index);
    return nullptr;
}
}
class ParameterOrganizationPanelTests : public QObject
{
    Q_OBJECT
    CModelDocument m_original;
private slots:
    void initTestCase()
    {
        QVERIFY(m_original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3")); QVERIFY(m_original.Parameters().size() > 3);
    }
    void foldersColorAndRelatedFilterPreserveValues()
    {
        auto document = m_original; QVERIFY(document.SetParameterOrganization(organized(document)));
        CParamPanel panel; panel.SetDocument(&document); panel.BuildParams(); panel.resize(460, 600); panel.show();
        const auto first = document.Parameters()[0].id, second = document.Parameters()[1].id, third = document.Parameters()[2].id;
        QVERIFY(row(panel, first)); QVERIFY(folder(panel));
        QCOMPARE(row(panel, first)->parentWidget(), row(panel, second)->parentWidget());
        QVERIFY(row(panel, first)->parentWidget() != row(panel, third)->parentWidget());
        QCOMPARE(folder(panel)->property("parameterLabelColor").toString(), QString("#198A71"));
        QCOMPARE(folder(panel)->accessibleName(), QString("Eyes & face"));
        const auto values = panel.CurrentValues(); const auto revision = document.Revision();
        panel.SetRelatedParameterIds({second}); panel.findChild<QCheckBox*>("onlyRelatedParameters")->setChecked(true);
        QVERIFY(row(panel, first)->isHidden()); QVERIFY(!row(panel, second)->isHidden()); QVERIFY(row(panel, third)->isHidden());
        QCOMPARE(panel.CurrentValues(), values); QCOMPARE(document.Revision(), revision);
        panel.SetRelatedParameterIds({third}); QVERIFY(folder(panel)->isHidden());
        QVERIFY(panel.FocusParameter(first)); QVERIFY(!panel.findChild<QCheckBox*>("onlyRelatedParameters")->isChecked());
        QVERIFY(!row(panel, first)->isHidden()); QVERIFY(!folder(panel)->isHidden());
    }
    void submissionFailureKeepsDialogAndAllowsRetry()
    {
        auto document = m_original; QVERIFY(document.SetParameterOrganization(organized(document)));
        CParamPanel panel; panel.SetDocument(&document); panel.BuildParams(); panel.resize(800, 650); panel.show();
        const auto before = document.ParameterOrganization(); bool allow = false; int attempts = 0;
        panel.SetOrganizationEditHandler([&](const auto& edit, QString* error) {
            ++attempts; if (!allow) { *error = "publisher rejected"; return false; }
            return document.ExecuteEdit(edit);
        });
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<CParameterOrganizationDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            folder(*dialog)->setText(0, "Renamed folder"); dialog->accept();
            QVERIFY(dialog->isVisible()); QCOMPARE(document.ParameterOrganization(), before);
            QCOMPARE(dialog->findChild<QLabel*>("parameterOrganizationError")->text(), QString("publisher rejected"));
            allow = true; dialog->accept();
        });
        panel.EditOrganization(); QCOMPARE(attempts, 2); QCOMPARE(folder(panel)->text(), QString("Renamed folder"));
        QVERIFY(document.CanUndoEdit()); panel.Undo(); QCOMPARE(document.ParameterOrganization(), before);
        QCOMPARE(folder(panel)->accessibleName(), QString("Eyes & face"));
        panel.Redo(); QCOMPARE(folder(panel)->text(), QString("Renamed folder"));
    }
    void staleDocumentRejectsWithoutDiscardingDraft()
    {
        auto document = m_original; CParamPanel panel; panel.SetDocument(&document); panel.BuildParams(); panel.show();
        int called = 0; panel.SetOrganizationEditHandler([&](const auto&, QString*) { ++called; return true; });
        QTimer::singleShot(0, [&] {
            auto* dialog = qobject_cast<CParameterOrganizationDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            const auto parameter = document.Parameters()[0];
            QVERIFY(document.SetParameterValue(parameter.id, parameter.maximum));
            dialog->accept(); QVERIFY(dialog->isVisible()); QVERIFY(!dialog->findChild<QLabel*>("parameterOrganizationError")->text().isEmpty());
            dialog->reject();
        });
        panel.EditOrganization(); QCOMPARE(called, 0); QVERIFY(!document.ParameterOrganization().configured);
    }
    void hiddenPartnerUpdatesAsOneHistoryEntry()
    {
        auto document = m_original; auto state = organized(document);
        const auto first = document.Parameters()[0], second = document.Parameters()[1];
        state.pairs.append({first.id, second.id, true, true}); QVERIFY(document.SetParameterOrganization(state));
        CParamPanel panel; panel.SetDocument(&document); panel.BuildParams();
        panel.SetRelatedParameterIds({first.id}); panel.findChild<QCheckBox*>("onlyRelatedParameters")->setChecked(true);
        QVERIFY(row(panel, second.id)->isHidden()); QSignalSpy changed(&panel, &CParamPanel::UserValuesEdited);
        QVERIFY(panel.ApplyUserValues({{first.id, first.maximum}})); QCOMPARE(panel.CurrentValues().value(second.id), second.minimum);
        QCOMPARE(changed.size(), 1); const auto edited = changed[0][0].value<QMap<QString, float>>(); QCOMPARE(edited.size(), 2);
        panel.Undo(); QCOMPARE(document.ParameterValue(first.id), first.defaultValue); QCOMPARE(document.ParameterValue(second.id), second.defaultValue);
        QVERIFY(!document.CanUndoEdit()); QVERIFY(row(panel, second.id)->isHidden());
    }
    void cdiExtensionRestoresStateAndRejectsFutureWithoutPartialMutation()
    {
        auto state = organized(m_original); const auto parameters = m_original.Parameters();
        state.pairs.append({parameters[0].id, parameters[1].id, false, true});
        QJsonObject encoded; QVERIFY(Organization::ToJson(state, encoded));
        auto root = QJsonObject{{"Parameters", QJsonArray{}}, {"Live2DToolsParameterOrganization", QJsonObject{{"Version", 1}, {"State", encoded}}}};
        QTemporaryDir directory; const auto path = directory.filePath("model.cdi3.json");
        const auto write = [&] { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(root).toJson()) > 0; };
        QVERIFY(write()); CCidJson display; QVERIFY(display.Parse(path)); QVERIFY(display.ParameterOrganization()); QCOMPARE(*display.ParameterOrganization(), state);
        const auto digest = display.SourceDigest(); root["Live2DToolsParameterOrganization"] = QJsonObject{{"Version", 2}, {"State", encoded}};
        QVERIFY(write()); QString error; QVERIFY(!display.Parse(path, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(display.SourceDigest(), digest); QCOMPARE(*display.ParameterOrganization(), state);
        root = QJsonObject{{"Parameters", QJsonArray{}}}; QVERIFY(write()); QVERIFY(display.Parse(path)); QVERIFY(!display.ParameterOrganization());
    }
};
QTEST_MAIN(ParameterOrganizationPanelTests)
#include "parameter_organization_panel_tests.moc"
