#include "ui/cuserdatapanel.h"
#include "ui/theme.h"
#include "live2dstruct/cmodeluserdata.h"
#include "std/cmodeljson.h"
#include <QtTest>
#include <QApplication>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

class UserDataPanelTests : public QObject
{
    Q_OBJECT
private:
    void answerPicker(bool accept)
    {
        QTimer::singleShot(0, [accept](){
            auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            if (accept) dialog->accept();
            else dialog->reject();
        });
    }

private slots:
    void editsCreatesAndReloadsModelTags()
    {
        QTemporaryDir temp;
        QFile model(temp.filePath("model.model3.json"));
        QVERIFY(model.open(QIODevice::WriteOnly));
        QVERIFY(model.write(R"({"Version":3,"FileReferences":{"Moc":"model.moc3"}})") > 0);
        model.close();
        CUserDataPanel panel;
        panel.setStyleSheet(Theme::globalStyleSheet());
        panel.SetValidArtMeshIds({"Body", "Eye", "Hair"});
        QString error;
        QVERIFY2(panel.LoadModel(model.fileName(), &error), qPrintable(error));
        panel.resize(640, 280);
        panel.show();
        auto* table = panel.findChild<QTableWidget*>("userDataTable");
        auto* add = panel.findChild<QPushButton*>("addUserData");
        auto* remove = panel.findChild<QPushButton*>("removeUserData");
        auto* save = panel.findChild<QPushButton*>("primaryBtn");
        QVERIFY(table && add && remove && save);
        QVERIFY(add->isEnabled());
        QVERIFY(!remove->isEnabled());
        QSignalSpy saved(&panel, &CUserDataPanel::UserDataSaved);
        answerPicker(false);
        add->click();
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(!panel.HasUnsavedChanges());
        answerPicker(true);
        add->click();
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QStringLiteral("Body"));
        QVERIFY(!(table->item(0, 0)->flags() & Qt::ItemIsEditable));
        auto* editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setText(QStringLiteral("皮肤🌸"));
        QTest::mouseClick(save, Qt::LeftButton);
        QCOMPARE(saved.size(), 1);
        QVERIFY(!panel.HasUnsavedChanges());
        CModelJson manifest;
        manifest.Parse(model.fileName());
        QVERIFY(!manifest.m_UserData.isEmpty());
        live2dstruct::CModelUserData result;
        QVERIFY(result.Load(panel.FilePath()));
        QCOMPARE(result.Entries().first().value, QStringLiteral("皮肤🌸"));
        answerPicker(true);
        add->click();
        table->setFocus();
        QCOMPARE(table->rowCount(), 2);
        table->item(1, 1)->setText(QStringLiteral("眼睛"));
        QVERIFY(panel.SaveUserData(&error));
        const QString path = panel.FilePath();
        QVERIFY(panel.LoadModel(model.fileName(), &error));
        QCOMPARE(panel.FilePath(), path);
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->item(1, 1)->text(), QStringLiteral("眼睛"));
        if (!qEnvironmentVariableIsEmpty("USERDATA_PANEL_SCREENSHOT")) {
            QVERIFY(panel.grab().save(qEnvironmentVariable("USERDATA_PANEL_SCREENSHOT")));
            panel.setStyleSheet(Theme::globalStyleSheet(true));
            QVERIFY(panel.grab().save(qEnvironmentVariable("USERDATA_PANEL_SCREENSHOT") + ".dark.png"));
        }
        table->selectAll();
        remove->click();
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(panel.SaveUserData(&error));
        QVERIFY(result.Load(path));
        QVERIFY(result.Entries().isEmpty());
    }

    void failuresPreserveDraftAndUnknownFields()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("labels.userdata3.json");
        QFile original(path);
        QVERIFY(original.open(QIODevice::WriteOnly));
        const QByteArray bytes = R"({"Version":3,"Extra":true,"Meta":{"Note":"keep"},
            "UserData":[{"Target":"ArtMesh","Id":"Body","Value":"skin","Extra":42}]})";
        QCOMPARE(original.write(bytes), bytes.size());
        original.close();
        CUserDataPanel panel;
        panel.SetValidArtMeshIds({"Body"});
        QString error;
        QVERIFY(panel.LoadUserData(path, &error));
        auto* table = panel.findChild<QTableWidget*>("userDataTable");
        QSignalSpy saved(&panel, &CUserDataPanel::UserDataSaved);
        table->item(0, 1)->setText(QStringLiteral("草稿"));
        QVERIFY(panel.HasUnsavedChanges());
        QVERIFY(!panel.LoadUserData(temp.filePath("missing.json"), &error));
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("草稿"));
        QCOMPARE(panel.FilePath(), path);
        QVERIFY(panel.HasUnsavedChanges());
        panel.SetValidArtMeshIds({});
        QVERIFY(!panel.SaveUserData(&error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(saved.size(), 0);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), bytes);
        original.close();
        panel.SetValidArtMeshIds({"Body"});
        QVERIFY(panel.SaveUserData(&error));
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(original.readAll()).object();
        original.close();
        QVERIFY(root.value("Extra").toBool());
        QCOMPARE(root.value("Meta").toObject().value("Note").toString(), QStringLiteral("keep"));
        QCOMPARE(root.value("UserData").toArray().first().toObject().value("Extra").toInt(), 42);
        QCOMPARE(root.value("UserData").toArray().first().toObject().value("Value").toString(), QStringLiteral("草稿"));
        table->item(0, 1)->setText(QStringLiteral("新草稿"));
        QVERIFY(QFile::remove(path));
        QVERIFY(QDir().mkdir(path));
        QVERIFY(!panel.SaveUserData(&error));
        QVERIFY(panel.HasUnsavedChanges());
        QCOMPARE(saved.size(), 1);
    }

    void recognizesAndSavesUncommittedInput()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("labels.userdata3.json");
        live2dstruct::CModelUserData data;
        data.SetEntries({{"ArtMesh", "Body", "old", {}}});
        QVERIFY(data.Save(path));
        CUserDataPanel panel;
        panel.SetValidArtMeshIds({"Body"});
        QVERIFY(panel.LoadUserData(path));
        panel.show();
        auto* table = panel.findChild<QTableWidget*>("userDataTable");
        table->setCurrentCell(0, 1);
        table->editItem(table->item(0, 1));
        auto* editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setText(QStringLiteral("まだ入力中"));
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("old"));
        QVERIFY(panel.HasUnsavedChanges());
        QVERIFY(panel.SaveUserData());
        QVERIFY(!panel.HasUnsavedChanges());
        QVERIFY(data.Load(path));
        QCOMPARE(data.Entries().first().value, QStringLiteral("まだ入力中"));
    }
};

QTEST_MAIN(UserDataPanelTests)
#include "userdata_panel_tests.moc"
