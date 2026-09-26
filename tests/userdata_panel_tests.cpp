#include "ui/cuserdatapanel.h"
#include "ui/theme.h"
#include "live2dstruct/cmodeluserdata.h"
#include "live2dstruct/cmodeldocument.h"
#include "std/cmodeljson.h"
#include "cmo3/ccmo3builder.h"
#include "cmo3/ccaffarchive.h"
#include "cmo3_userdata.h"
#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QInputDialog>
#include <QJsonArray>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

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
    void destroyingFocusedFloatingEditorDoesNotPublish_data()
    {
        QTest::addColumn<int>("destruction");
        QTest::newRow("panel") << 0;
        QTest::newRow("parent") << 1;
        QTest::newRow("remove-dock") << 2;
    }

    void destroyingFocusedFloatingEditorDoesNotPublish()
    {
        QFETCH(int, destruction);
        live2dstruct::CModelDocument document;
        QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        QTemporaryDir directory;
        const auto path = directory.filePath("labels.userdata3.json");
        live2dstruct::CModelUserData data;
        data.SetEntries({{QStringLiteral("ArtMesh"), document.Drawables().first().id, QStringLiteral("old"), {}}});
        QVERIFY(data.Save(path));
        auto window = std::make_unique<QMainWindow>();
        QPointer<CUserDataPanel> panel = new CUserDataPanel(window.get());
        panel->SetValidArtMeshIds({document.Drawables().first().id});
        QVERIFY(panel->LoadUserData(path));
        window->addDockWidget(Qt::RightDockWidgetArea, panel);
        window->show();
        panel->setFloating(true);
        panel->resize(700, 500);
        panel->show();
        panel->activateWindow();
        auto* table = panel->findChild<QTableWidget*>("userDataTable");
        QVERIFY(table);
        table->setCurrentCell(0, 1);
        table->editItem(table->item(0, 1));
        auto* editor = table->findChild<QLineEdit*>();
        QVERIFY(editor);
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());
        editor->selectAll();
        QTest::keyClicks(editor, "pending label");
        QCOMPARE(table->item(0, 1)->text(), QStringLiteral("old"));
        QVERIFY(panel->HasUnsavedChanges());
        QSignalSpy dirty(panel, &CUserDataPanel::DirtyChanged);
        QSignalSpy saved(panel, &CUserDataPanel::UserDataSaved);
        if (destruction == 2) {
            window->removeDockWidget(panel);
            QCOMPARE(dirty.size(), 1);
            QCOMPARE(table->item(0, 1)->text(), QStringLiteral("pending label"));
        }
        const auto beforeDestruction = dirty.size();
        if (destruction == 1) window.reset();
        else delete panel.data();
        QCoreApplication::processEvents();
        QVERIFY(!panel);
        QCOMPARE(dirty.size(), beforeDestruction);
        QCOMPARE(saved.size(), 0);
        live2dstruct::CModelUserData savedData;
        QVERIFY(savedData.Load(path));
        QCOMPARE(savedData.Entries().first().value, QStringLiteral("old"));
    }

    void firstSaveRejectsChangedManifestAndPreservesDraft_data()
    {
        QTest::addColumn<QString>("change");
        for (const char* change : {"moc", "textures", "groups", "layout", "extension", "userdata", "whitespace", "newlines", "malformed"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }

    void firstSaveRejectsChangedManifestAndPreservesDraft()
    {
        QFETCH(QString, change);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto path = temp.filePath("model.model3.json");
        const QByteArray original = "{\r\n\"Version\":3,\"FileReferences\":{\"Moc\":\"model.moc3\"},\r\n\"Custom\":42}\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(original), original.size()); file.close();
        CUserDataPanel panel; panel.SetValidArtMeshIds({"Body"}); QVERIFY(panel.LoadModel(path)); panel.show();
        auto* add = panel.findChild<QPushButton*>("addUserData"); auto* table = panel.findChild<QTableWidget*>("userDataTable");
        QVERIFY(add && table); answerPicker(true); add->click(); QCOMPARE(table->rowCount(), 1);
        auto* editor = table->findChild<QLineEdit*>(); QVERIFY(editor); editor->setText(QStringLiteral("保留的草稿"));
        QTest::keyClick(editor, Qt::Key_Return); QVERIFY(panel.HasUnsavedChanges());
        QTRY_COMPARE(table->item(0, 1)->text(), QStringLiteral("保留的草稿"));
        QByteArray draft; QVERIFY(panel.CaptureUserDataBytes(draft));
        const auto sourcePath = panel.SourcePath(); const auto sourceDigest = panel.SourceDigest();
        auto root = QJsonDocument::fromJson(original).object(); auto files = root.value("FileReferences").toObject();
        if (change == "moc") files["Moc"] = "different.moc3";
        else if (change == "textures") files["Textures"] = QJsonArray{"different.png"};
        else if (change == "userdata") files["UserData"] = "registered.json";
        else if (change == "groups") root["Groups"] = QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Ids", QJsonArray{"ParamA"}}}};
        else if (change == "layout") root["Layout"] = QJsonObject{{"width", 3}};
        else if (change == "extension") root["Custom"] = 43;
        root["FileReferences"] = files;
        QByteArray updated;
        if (change == "whitespace") updated = original + ' ';
        else if (change == "newlines") { updated = original; updated.replace("\r\n", "\n"); }
        else if (change == "malformed") updated = "{";
        else updated = QJsonDocument(root).toJson();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(updated), updated.size()); file.close();
        const auto entries = QDir(temp.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        QSignalSpy saved(&panel, &CUserDataPanel::UserDataSaved), dirty(&panel, &CUserDataPanel::DirtyChanged);
        QString error; QVERIFY(!panel.SaveUserData(&error)); QVERIFY(!error.isEmpty());
        QCOMPARE(saved.size(), 0); QCOMPARE(dirty.size(), 0); QVERIFY(panel.HasUnsavedChanges());
        QCOMPARE(panel.SourcePath(), sourcePath); QCOMPARE(panel.SourceDigest(), sourceDigest);
        QByteArray after; QVERIFY(panel.CaptureUserDataBytes(after)); QCOMPARE(after, draft);
        QCOMPARE(QDir(temp.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot), entries);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), updated); file.close();
        QVERIFY(!panel.LoadModel(temp.filePath("missing"), &error)); QVERIFY(panel.HasUnsavedChanges());
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(original), original.size()); file.close();
        QVERIFY2(panel.SaveUserData(&error), qPrintable(error)); QCOMPARE(saved.size(), 1); QVERIFY(!panel.HasUnsavedChanges());
        QFile output(panel.SourcePath()); QVERIFY(output.open(QIODevice::ReadOnly));
        const auto bytes = output.readAll(); QCOMPARE(bytes, draft);
        QCOMPARE(panel.SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
    }

    void firstSaveRetainsAbsoluteModelIdentityAcrossWorkingDirectoryChanges()
    {
        QTemporaryDir modelDirectory, unrelated; QVERIFY(modelDirectory.isValid() && unrelated.isValid());
        const auto path = modelDirectory.filePath("model.model3.json");
        const QByteArray raw = "{\r\n\"Version\":3,\"FileReferences\":{\"Moc\":\"model.moc3\",\"UserData\":\"\"}}\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(raw), raw.size()); file.close();
        CUserDataPanel panel; QVERIFY(panel.LoadModel(QDir::current().relativeFilePath(path)));
        const auto previous = QDir::currentPath(); const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
        QVERIFY(QDir::setCurrent(unrelated.path())); QString error;
        QVERIFY2(panel.SaveUserData(&error), qPrintable(error));
        QCOMPARE(QFileInfo(panel.SourcePath()).absolutePath(), modelDirectory.path());
        QCOMPARE(QDir(unrelated.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot), QStringList());
        CModelJson loaded; loaded.Parse(path); QVERIFY(loaded.IsOpen()); QVERIFY(!loaded.m_UserData.isEmpty());
        QFile output(panel.SourcePath()); QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(panel.SourceDigest(), QCryptographicHash::hash(output.readAll(), QCryptographicHash::Sha256));
    }

    void existingUserDataSaveDoesNotRewriteChangedManifest()
    {
        QTemporaryDir temp; const auto path = temp.filePath("model.model3.json");
        live2dstruct::CModelUserData data; data.SetEntries({{"ArtMesh", "Body", "old", {}}});
        QVERIFY(data.Save(temp.filePath("labels.json")));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray original = R"({"Version":3,"FileReferences":{"Moc":"model.moc3","UserData":"labels.json"}})";
        QCOMPARE(file.write(original), original.size()); file.close();
        CUserDataPanel panel; panel.SetValidArtMeshIds({"Body"}); QVERIFY(panel.LoadModel(path));
        auto* table = panel.findChild<QTableWidget*>("userDataTable"); QVERIFY(table); table->item(0, 1)->setText("new");
        const auto updated = original + ' ';
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(updated), updated.size()); file.close();
        QString error; QVERIFY2(panel.SaveUserData(&error), qPrintable(error));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), updated);
        QVERIFY(data.Load(temp.filePath("labels.json"))); QCOMPARE(data.Entries().first().value, QString("new"));
    }

    void sourceFingerprintOnlyChangesAfterSuccessfulLoadOrSave()
    {
        QTemporaryDir temp; const auto path = temp.filePath("labels.json");
        const QByteArray raw = "{\r\n\"Version\":3,\"UserData\":[{\"Target\":\"ArtMesh\",\"Id\":\"Body\",\"Value\":\"old\"}]}\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(raw), raw.size()); file.close();
        CUserDataPanel panel; panel.SetValidArtMeshIds({"Body"});
        QVERIFY(panel.SourcePath().isEmpty()); QVERIFY(panel.SourceDigest().isEmpty());
        QVERIFY(panel.LoadUserData(QDir::current().relativeFilePath(path)));
        QCOMPARE(panel.SourcePath(), path); const auto originalDigest = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
        QCOMPARE(panel.SourceDigest(), originalDigest);
        auto* table = panel.findChild<QTableWidget*>("userDataTable"); QVERIFY(table); table->item(0, 1)->setText("draft");
        QByteArray output; QVERIFY(panel.CaptureUserDataBytes(output));
        QVERIFY(QCryptographicHash::hash(output, QCryptographicHash::Sha256) != panel.SourceDigest());
        QVERIFY(!panel.LoadUserData(temp.filePath("missing"))); QCOMPARE(panel.SourcePath(), path); QCOMPARE(panel.SourceDigest(), originalDigest);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write("{"), 1); file.close();
        QVERIFY(!panel.LoadUserData(path)); QCOMPARE(panel.SourceDigest(), originalDigest); QCOMPARE(table->item(0, 1)->text(), QString("draft"));
        QVERIFY(QFile::remove(path)); QVERIFY(QDir().mkdir(path)); QVERIFY(!panel.SaveUserData());
        QCOMPARE(panel.SourcePath(), path); QCOMPARE(panel.SourceDigest(), originalDigest); QVERIFY(panel.HasUnsavedChanges());
        QVERIFY(QDir().rmdir(path)); QVERIFY(panel.SaveUserData()); QVERIFY(!panel.HasUnsavedChanges());
        QVERIFY(file.open(QIODevice::ReadOnly)); const auto saved = file.readAll(); file.close();
        QCOMPARE(panel.SourceDigest(), QCryptographicHash::hash(saved, QCryptographicHash::Sha256));
        const auto savedDigest = panel.SourceDigest(); QVERIFY(savedDigest != originalDigest);
        QVERIFY(QFile::remove(path)); QVERIFY(panel.CaptureUserDataBytes(output)); QCOMPARE(panel.SourceDigest(), savedDigest);
        QVERIFY(panel.LoadUserData({})); QVERIFY(panel.SourcePath().isEmpty()); QVERIFY(panel.SourceDigest().isEmpty());
    }

    void capturesUnsavedBytesWithoutFilesOrSignals()
    {
        QTemporaryDir temp;
        const auto path = temp.filePath("source.userdata3.json");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray source = R"({"Version":3,"Extension":{"keep":true},"Meta":{"UserDataCount":99,"Note":"keep"},
            "UserData":[{"Target":"ArtMesh","Id":"Body","Value":"old","EntryExtra":17}]})";
        QCOMPARE(file.write(source), source.size()); file.close();
        CUserDataPanel panel; panel.SetValidArtMeshIds({"Body"}); QVERIFY(panel.LoadUserData(path));
        auto* table = panel.findChild<QTableWidget*>("userDataTable"); QVERIFY(table);
        table->item(0, 1)->setText(QStringLiteral("草稿🌸")); table->setCurrentCell(0, 1); table->selectRow(0);
        QVERIFY(panel.HasUnsavedChanges());
        const auto current = table->currentIndex();
        const auto selected = table->selectionModel()->selectedIndexes();
        QSignalSpy dirty(&panel, &CUserDataPanel::DirtyChanged), saved(&panel, &CUserDataPanel::UserDataSaved);
        QByteArray bytes("sentinel"); QString error;
        QVERIFY2(panel.CaptureUserDataBytes(bytes, &error), qPrintable(error)); QVERIFY(error.isEmpty());
        const auto root = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(root.value("Extension"), QJsonValue(QJsonObject{{"keep", true}}));
        QCOMPARE(root.value("Meta").toObject().value("Note"), QJsonValue("keep"));
        QCOMPARE(root.value("Meta").toObject().value("UserDataCount").toInt(), 1);
        QCOMPARE(root.value("Meta").toObject().value("TotalUserDataSize").toInt(), QStringLiteral("草稿🌸").toUtf8().size());
        QCOMPARE(root.value("UserData").toArray().first().toObject().value("EntryExtra").toInt(), 17);
        QCOMPARE(root.value("UserData").toArray().first().toObject().value("Value").toString(), QStringLiteral("草稿🌸"));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), source); file.close();
        QVERIFY(QFile::remove(path)); QByteArray afterRemoval;
        QVERIFY(panel.CaptureUserDataBytes(afterRemoval)); QCOMPARE(afterRemoval, bytes);
        QVERIFY(!QFileInfo::exists(path)); QVERIFY(panel.HasUnsavedChanges()); QCOMPARE(panel.FilePath(), path);
        QCOMPARE(table->currentIndex(), current); QCOMPARE(table->selectionModel()->selectedIndexes(), selected);
        QCOMPARE(dirty.size(), 0); QCOMPARE(saved.size(), 0);
        panel.SetValidArtMeshIds({}); const auto originalBytes = bytes;
        QVERIFY(!panel.CaptureUserDataBytes(bytes, &error)); QCOMPARE(bytes, originalBytes); QVERIFY(!error.isEmpty());
        QCOMPARE(dirty.size(), 0); QCOMPARE(saved.size(), 0);
    }

    void captureRejectsPendingInputWithoutCommittingOrClosingIt()
    {
        QTemporaryDir temp; const auto path = temp.filePath("data.json");
        live2dstruct::CModelUserData data; data.SetEntries({{"ArtMesh", "Body", "old", {}}}); QVERIFY(data.Save(path));
        CUserDataPanel panel; panel.SetValidArtMeshIds({"Body"}); QVERIFY(panel.LoadUserData(path));
        panel.show(); auto* table = panel.findChild<QTableWidget*>("userDataTable"); QVERIFY(table);
        table->setCurrentCell(0, 1); table->editItem(table->item(0, 1));
        auto* editor = table->findChild<QLineEdit*>(); QVERIFY(editor); editor->setText("not committed");
        QSignalSpy dirty(&panel, &CUserDataPanel::DirtyChanged), saved(&panel, &CUserDataPanel::UserDataSaved);
        QByteArray output("unchanged"); QString error;
        QVERIFY(!panel.CaptureUserDataBytes(output, &error)); QCOMPARE(output, QByteArray("unchanged")); QVERIFY(!error.isEmpty());
        QCOMPARE(editor->text(), QString("not committed")); QCOMPARE(table->item(0, 1)->text(), QString("old"));
        QVERIFY(panel.HasUnsavedChanges()); QCOMPARE(dirty.size(), 0); QCOMPARE(saved.size(), 0);
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(panel.CaptureUserDataBytes(output)); QVERIFY(!panel.HasUnsavedChanges());
        table->item(0, 1)->setText(QString("invalid") + QChar::Null);
        const auto before = output; QVERIFY(!panel.CaptureUserDataBytes(output, &error)); QCOMPARE(output, before);
        QVERIFY(panel.HasUnsavedChanges());
    }

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
        QCOMPARE(panel.SourcePath(), panel.FilePath());
        QVERIFY(QFileInfo(panel.SourcePath()).isAbsolute());
        QFile savedSource(panel.SourcePath()); QVERIFY(savedSource.open(QIODevice::ReadOnly));
        QCOMPARE(panel.SourceDigest(), QCryptographicHash::hash(savedSource.readAll(), QCryptographicHash::Sha256));
        savedSource.close();
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

    void savedPanelEditsReachCmo3()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("labels.userdata3.json");
        live2dstruct::CModelUserData data;
        data.SetEntries({{"ArtMesh", "Hair1", "old", {}}});
        QVERIFY(data.Save(path));
        CUserDataPanel panel;
        panel.SetValidArtMeshIds({"Hair1"});
        QVERIFY(panel.LoadUserData(path));
        auto* table = panel.findChild<QTableWidget*>("userDataTable");
        table->item(0, 1)->setText(QStringLiteral("标签 & 导出🌸"));
        QString error;
        QVERIFY(panel.SaveUserData(&error));
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        options.userDataPath = panel.FilePath();
        options.outPath = temp.filePath("labels.cmo3");
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY(archive.Load(options.outPath, &error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        QMap<QString, QString> tags;
        QVERIFY2(readMeshUserData(main->content, &tags, &error), qPrintable(error));
        QCOMPARE(tags.value("Hair1"), QStringLiteral("标签 & 导出🌸"));
    }
};

QTEST_MAIN(UserDataPanelTests)
#include "userdata_panel_tests.moc"
