#include "ui/cphysicspanel.h"
#include "ui/cphysicsgraph.h"
#include "cmo3/ccmo3builder.h"
#include "cmo3/ccaffarchive.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QPushButton>
#include <QInputDialog>
#include <QTimer>
#include <QXmlStreamReader>
#include <QMessageBox>
#include <QLineEdit>
#include <type_traits>
#include <stdexcept>

namespace {
QJsonObject physicsFixture()
{
    return QJsonDocument::fromJson(R"({
        "Version": 3,
        "Meta": {"Fps": 30, "PhysicsSettingCount": 2, "TotalInputCount": 2,
            "TotalOutputCount": 0, "VertexCount": 2,
            "EffectiveForces": {"Gravity": {"X": 1.25, "Y": -1}, "Wind": {"X": 0.1234, "Y": 0}},
            "PhysicsDictionary": [{"Id": "hair", "Name": "头发"}, {"Id": "tail", "Name": "尾巴"}],
            "CustomMeta": "keep"},
        "PhysicsSettings": [
            {"Id": "hair", "Input": [{"Source": {"Target": "Parameter", "Id": "ParamHair"},
                "Type": "X", "Weight": 100, "Reflect": false}], "Output": [],
             "Vertices": [{"Position": {"X": 0, "Y": 0}, "Mobility": 1, "Delay": 1, "Acceleration": 1, "Radius": 0}],
             "Normalization": {"Position": {"Minimum": -10, "Default": 0, "Maximum": 10},
                               "Angle": {"Minimum": -30, "Default": 0, "Maximum": 30}}},
            {"Id": "tail", "Input": [{"Source": {"Target": "Parameter", "Id": "ParamTail"},
                "Type": "Angle", "Weight": 100, "Reflect": false}], "Output": [],
             "Vertices": [{"Position": {"X": 0, "Y": 0}, "Mobility": 1, "Delay": 1, "Acceleration": 1, "Radius": 0}],
             "Normalization": {"Position": {"Minimum": -10, "Default": 0, "Maximum": 10},
                               "Angle": {"Minimum": -30, "Default": 0, "Maximum": 30}}}
        ]
    })").object();
}

bool writePhysics(const QString& path, const QJsonObject& root)
{
    QFile file(path);
    const QByteArray data = QJsonDocument(root).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QJsonObject readPhysics(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool answerTextDialogs(QPushButton* button, const QStringList& answers, bool cancelLast = false)
{
    if (!button || !button->isEnabled()) return false;
    QTimer timer;
    QElapsedTimer elapsed;
    int answered = 0;
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, [&](){
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (elapsed.elapsed() > 2000) {
            timedOut = true;
            if (modal) modal->reject();
            return;
        }
        auto* dialog = qobject_cast<QInputDialog*>(modal);
        if (!dialog || answered >= answers.size()) return;
        dialog->setTextValue(answers.at(answered++));
        if (cancelLast && answered == answers.size()) dialog->reject();
        else dialog->accept();
    });
    elapsed.start();
    timer.start(5);
    button->click();
    return !timedOut && answered == answers.size();
}
}

class PhysicsPanelTests : public QObject
{
    Q_OBJECT
private slots:
    void sessionSaveKeepsSourceAndHistoryBaseline_data()
    {
        QTest::addColumn<int>("result");
        QTest::newRow("success") << 0;
        QTest::newRow("refusal") << 1;
        QTest::newRow("exception") << 2;
        QTest::newRow("nonstandard-exception") << 3;
    }

    void sessionSaveKeepsSourceAndHistoryBaseline()
    {
        QFETCH(int, result);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("source.physics3.json"); const auto source = physicsFixture(); QVERIFY(writePhysics(path, source));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        panel.findChild<QSlider*>("physicsWindY")->setValue(20);
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before));
        QVERIFY(before.dirty);
        int calls = 0;
        panel.SetSessionSaveHandler([&](QString* error) {
            ++calls;
            if (result == 2) throw std::runtime_error("session failure");
            if (result == 3) throw 3;
            if (result == 1) { if (error) *error = "session refusal"; return false; }
            return true;
        });
        QCOMPARE(panel.findChild<QPushButton*>("savePhysicsButton")->text(), QStringLiteral("保存到会话"));
        QSignalSpy saved(&panel, &CPhysicsPanel::PhysicsSaved);
        QSignalSpy changed(&panel, &CPhysicsPanel::PhysicsConfigChanged);
        QString error;
        QCOMPARE(panel.SavePhysics({}, &error), result == 0); QCOMPARE(calls, 1);
        QCOMPARE(error.isEmpty(), result == 0);
        CPhysicsPanel::Snapshot after; QVERIFY(panel.CaptureSnapshot(after)); QVERIFY(after.SameDocument(before));
        QCOMPARE(readPhysics(path), source); QCOMPARE(saved.size(), 0); QCOMPARE(changed.size(), 0);
    }

    void explicitExportAndClearedHandlerKeepLegacySaveBehavior()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto sourcePath = directory.filePath("source.physics3.json"); const auto output = directory.filePath("export.physics3.json");
        const auto source = physicsFixture(); QVERIFY(writePhysics(sourcePath, source));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(sourcePath));
        panel.findChild<QSlider*>("physicsWindY")->setValue(20);
        int calls = 0;
        panel.SetSessionSaveHandler([&](QString*) { ++calls; return true; });
        QSignalSpy saved(&panel, &CPhysicsPanel::PhysicsSaved);
        QVERIFY(panel.SavePhysics(output)); QCOMPARE(calls, 0); QCOMPARE(saved.size(), 1);
        QCOMPARE(readPhysics(sourcePath), source); QVERIFY(readPhysics(output) != source); QVERIFY(!panel.HasUnsavedChanges());
        panel.SetSessionSaveHandler({});
        QCOMPARE(panel.findChild<QPushButton*>("savePhysicsButton")->text(), QStringLiteral("保存物理配置"));
        panel.findChild<QSlider*>("physicsWindY")->setValue(30);
        QVERIFY(panel.SavePhysics()); QCOMPARE(calls, 0); QCOMPARE(saved.size(), 2);
        QCOMPARE(readPhysics(output).value("Meta").toObject().value("EffectiveForces").toObject().value("Wind").toObject().value("Y").toDouble(), 0.3);
        QCOMPARE(readPhysics(sourcePath), source);
    }

    void preparedReferenceRemovalIsSilentAndReversible()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("draft.physics3.json");
        const auto source = physicsFixture(); QVERIFY(writePhysics(path, source));
        CPhysicsPanel panel; QString error; QVERIFY(panel.LoadPhysics(path, &error));
        auto* groups = panel.findChild<QTableWidget*>("physicsGroupTable");
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable");
        auto* drag = panel.findChild<QCheckBox*>("physicsDragMode");
        groups->setCurrentCell(1, 0); drag->setChecked(true); inputs->setCurrentCell(0, 2);
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before, &error));
        QSignalSpy changed(&panel, &CPhysicsPanel::PhysicsConfigChanged);
        QSignalSpy saved(&panel, &CPhysicsPanel::PhysicsSaved);
        QSignalSpy driven(&panel, &CPhysicsPanel::PhysicsDragParamsChanged);
        QSignalSpy gravity(&panel, &CPhysicsPanel::GravityChanged);
        QSignalSpy wind(&panel, &CPhysicsPanel::WindChanged);
        CPhysicsPanel::PreparedSnapshot prepared;
        QVERIFY2(panel.PrepareReferenceRemoval(before, {"ParamTail"}, prepared, &error), qPrintable(error));
        QCOMPARE(panel.DocumentSnapshot(), before.document); QVERIFY(!panel.HasUnsavedChanges());
        QCOMPARE(groups->currentRow(), 1); QCOMPARE(inputs->rowCount(), 1);
        QVERIFY(panel.ValidatePreparedSnapshot(prepared, &error));
        const auto after = prepared.Result();
        QVERIFY(panel.CommitPreparedSnapshot(prepared, &error));
        QVERIFY(panel.HasUnsavedChanges());
        QCOMPARE(panel.FilePath(), path); QCOMPARE(groups->currentRow(), 1); QVERIFY(drag->isChecked());
        QCOMPARE(inputs->rowCount(), 1); QCOMPARE(changed.size(), 0); QCOMPARE(saved.size(), 0); QCOMPARE(driven.size(), 0);
        QCOMPARE(gravity.size(), 0); QCOMPARE(wind.size(), 0); QCOMPARE(readPhysics(path), source);
        CPhysicsPanel::Snapshot committed; QVERIFY(panel.CaptureSnapshot(committed)); QVERIFY(committed.SameDocument(after));
        panel.NotifySnapshotCommitted();
        QCOMPARE(inputs->rowCount(), 0);
        QCOMPARE(changed.size(), 1); QCOMPARE(driven.size(), 1); QVERIFY(driven.last()[0].toStringList().isEmpty());
        panel.NotifySnapshotCommitted(); QCOMPARE(changed.size(), 1); QCOMPARE(driven.size(), 1);
        QVERIFY(!panel.CommitPreparedSnapshot(prepared, &error)); QVERIFY(!panel.ValidatePreparedSnapshot(prepared, &error));
        CPhysicsPanel::PreparedSnapshot undo;
        QVERIFY2(panel.PrepareSnapshot(after, before, undo, &error), qPrintable(error));
        QVERIFY(panel.CommitPreparedSnapshot(undo, &error));
        QCOMPARE(panel.DocumentSnapshot(), before.document); QCOMPARE(panel.HasUnsavedChanges(), before.dirty);
        QCOMPARE(inputs->rowCount(), 0);
        QVERIFY(drag->isChecked()); QCOMPARE(saved.size(), 0); QCOMPARE(changed.size(), 1);
        panel.NotifySnapshotCommitted();
        QCOMPARE(groups->currentRow(), before.selectedGroupRow); QCOMPARE(inputs->currentRow(), 0); QCOMPARE(inputs->currentColumn(), 2);
        QCOMPARE(changed.size(), 2); QCOMPARE(driven.last()[0].toStringList(), QStringList{"ParamTail"});
        QCOMPARE(readPhysics(path), source);
    }

    void UIOnlyChangesDoNotConflictAndRemovalUsesLatestSelection()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("draft.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        CPhysicsPanel::Snapshot expected; QVERIFY(panel.CaptureSnapshot(expected));
        auto* groups = panel.findChild<QTableWidget*>("physicsGroupTable");
        auto* drag = panel.findChild<QCheckBox*>("physicsDragMode");
        groups->setCurrentCell(1, 0); drag->setChecked(true);
        CPhysicsPanel::PreparedSnapshot prepared; QString error;
        QVERIFY2(panel.PrepareReferenceRemoval(expected, {"ParamHair"}, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.Result().selectedGroupId, QString("tail")); QVERIFY(prepared.Result().dragEnabled);
        groups->setCurrentCell(0, 0); drag->setChecked(false);
        QVERIFY(panel.ValidatePreparedSnapshot(prepared, &error));
        QVERIFY(panel.CommitPreparedSnapshot(prepared, &error));
        QCOMPARE(groups->currentRow(), 0); QVERIFY(!drag->isChecked());
        panel.NotifySnapshotCommitted();
        QCOMPARE(groups->currentRow(), 1); QVERIFY(drag->isChecked());
        QCOMPARE(panel.findChild<QTableWidget*>("physicsInputTable")->item(0, 0)->text(), QString("ParamTail"));
    }

    void independentDraftChangesAndRevisionABARejectPreparedSnapshot()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("draft.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        auto* wind = panel.findChild<QSlider*>("physicsWindY");
        wind->setValue(20);
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before));
        CPhysicsPanel::PreparedSnapshot prepared; QString error;
        QVERIFY(panel.PrepareReferenceRemoval(before, {"ParamHair"}, prepared, &error));
        wind->setValue(30); wind->setValue(20);
        CPhysicsPanel::Snapshot current; QVERIFY(panel.CaptureSnapshot(current));
        QCOMPARE(current.document, before.document); QCOMPARE(current.dirty, before.dirty); QVERIFY(current.revision > before.revision);
        QVERIFY(!panel.ValidatePreparedSnapshot(prepared, &error));
        QVERIFY(!panel.CommitPreparedSnapshot(prepared, &error));
        QVERIFY(!panel.PrepareSnapshot(before, before, prepared, &error));
        CPhysicsPanel::Snapshot after; QVERIFY(panel.CaptureSnapshot(after)); QVERIFY(after.SameDocument(current));
        QVERIFY(error.contains(QStringLiteral("修改")));
    }

    void pendingCellTextRejectsCapturePrepareLoadAndSave()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("draft.physics3.json"); const auto source = physicsFixture(); QVERIFY(writePhysics(path, source));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); panel.show();
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before));
        CPhysicsPanel::PreparedSnapshot prepared; QString error;
        QVERIFY(panel.PrepareReferenceRemoval(before, {"ParamHair"}, prepared, &error));
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable");
        inputs->setCurrentCell(0, 0); inputs->editItem(inputs->item(0, 0));
        QTRY_VERIFY(inputs->findChild<QLineEdit*>());
        auto* editor = inputs->findChild<QLineEdit*>(); editor->setText("ParamUncommitted");
        QVERIFY(panel.HasUnsavedChanges());
        auto unchanged = before;
        QVERIFY(!panel.CaptureSnapshot(unchanged, &error)); QVERIFY(error.contains(QStringLiteral("单元格")));
        QVERIFY(unchanged.SameDocument(before));
        QVERIFY(!panel.PrepareSnapshot(before, before, prepared, &error));
        QVERIFY(!panel.PrepareReferenceRemoval(before, {"ParamHair"}, prepared, &error));
        QVERIFY(!panel.ValidatePreparedSnapshot(prepared, &error));
        QVERIFY(!panel.CommitPreparedSnapshot(prepared, &error));
        QVERIFY(!panel.LoadPhysics(path, &error));
        int sessionCalls = 0;
        panel.SetSessionSaveHandler([&](QString*) { ++sessionCalls; return true; });
        QVERIFY(!panel.SavePhysics({}, &error)); QCOMPARE(sessionCalls, 0);
        QVERIFY(!panel.SavePhysics(path, &error));
        QCOMPARE(editor->text(), QString("ParamUncommitted"));
        QCOMPARE(panel.DocumentSnapshot(), before.document); QCOMPARE(readPhysics(path), source);
        QTest::keyClick(editor, Qt::Key_Escape);
        QTRY_VERIFY(!panel.HasUnsavedChanges());
        QVERIFY(panel.ValidatePreparedSnapshot(prepared, &error));
    }

    void invalidTargetsAndForeignCandidatesRemainAtomic()
    {
        static_assert(!std::is_copy_constructible_v<CPhysicsPanel::PreparedSnapshot>);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("draft.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel, other; QVERIFY(panel.LoadPhysics(path)); QVERIFY(other.LoadPhysics(path));
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before));
        CPhysicsPanel::PreparedSnapshot valid; QString error;
        QVERIFY(panel.PrepareReferenceRemoval(before, {"ParamHair"}, valid, &error));
        const auto planned = valid.Result();
        auto invalid = before;
        auto groups = invalid.document.value("PhysicsSettings").toArray(); auto group = groups.first().toObject();
        auto vertices = group.value("Vertices").toArray(); auto vertex = vertices.first().toObject(); vertex["Radius"] = -1;
        vertices[0] = vertex; group["Vertices"] = vertices; groups[0] = group; invalid.document["PhysicsSettings"] = groups;
        QVERIFY(!panel.PrepareSnapshot(before, invalid, valid, &error));
        QVERIFY(valid.Result().SameDocument(planned));
        QVERIFY(!other.ValidatePreparedSnapshot(valid, &error)); QVERIFY(!other.CommitPreparedSnapshot(valid, &error));
        CPhysicsPanel::PreparedSnapshot moved = std::move(valid);
        QVERIFY(!panel.ValidatePreparedSnapshot(valid, &error));
        QVERIFY(panel.ValidatePreparedSnapshot(moved, &error));
        QVERIFY(panel.CommitPreparedSnapshot(moved, &error));
        QCOMPARE(other.DocumentSnapshot(), before.document);
    }

    void snapshotRestoreUsesGroupIdentityAfterReordering()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("draft.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        auto* groups = panel.findChild<QTableWidget*>("physicsGroupTable"); groups->setCurrentCell(1, 0);
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before));
        auto target = before; auto values = target.document.value("PhysicsSettings").toArray();
        target.document["PhysicsSettings"] = QJsonArray{values[1], values[0]}; target.dirty = true;
        CPhysicsPanel::PreparedSnapshot prepared; QString error;
        QVERIFY2(panel.PrepareSnapshot(before, target, prepared, &error), qPrintable(error));
        QCOMPARE(prepared.Result().selectedGroupRow, 0);
        QVERIFY(panel.CommitPreparedSnapshot(prepared, &error));
        QCOMPARE(groups->currentRow(), 1);
        panel.NotifySnapshotCommitted();
        QCOMPARE(groups->currentRow(), 0); QCOMPARE(groups->item(0, 0)->text(), QString("tail"));
    }

    void editedGroupNameReachesCmo3()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        CPhysicsPanel panel;
        QString error;
        QVERIFY2(panel.LoadPhysics(QString(TEST_RESOURCES) + "/Wanko/Wanko.physics3.json", &error), qPrintable(error));
        const QString name = QStringLiteral("测试组 & 摆动");
        QVERIFY(answerTextDialogs(panel.findChild<QPushButton*>("renamePhysicsGroup"), {name}));
        cmo3::CCmo3Builder::Options options;
        options.mocPath = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        options.physicsPath = temp.filePath("edited.physics3.json");
        options.outPath = temp.filePath("edited.cmo3");
        QVERIFY(panel.SavePhysics(options.physicsPath));
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* entry = archive.FindByPath("main.xml");
        QVERIFY(entry);
        QXmlStreamReader xml(entry->content);
        QString firstName;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement() || xml.name() != QLatin1String("CPhysicsSettingsSource")) continue;
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("s") && xml.attributes().value("xs.n") == QLatin1String("name")) {
                    firstName = xml.readElementText();
                    break;
                }
                xml.skipCurrentElement();
            }
            break;
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(firstName, name);
    }

    void groupNamesSurviveSaveAndReload_data()
    {
        QTest::addColumn<bool>("hasDictionaryEntry");
        QTest::newRow("existing-name") << true;
        QTest::newRow("missing-name") << false;
    }

    void groupNamesSurviveSaveAndReload()
    {
        QFETCH(bool, hasDictionaryEntry);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("groups.physics3.json");
        QJsonObject root = physicsFixture();
        root.insert(QStringLiteral("PhysicsDictionary"), QJsonArray{QJsonObject{{"Id", "hair"}, {"Name", "旧副本"}}});
        if (!hasDictionaryEntry) {
            QJsonObject meta = root.value("Meta").toObject();
            QJsonArray dictionary = meta.value("PhysicsDictionary").toArray();
            dictionary.removeAt(0);
            meta.insert("PhysicsDictionary", dictionary);
            root.insert("Meta", meta);
        }
        QVERIFY(writePhysics(path, root));
        CPhysicsPanel panel;
        QString error;
        QVERIFY2(panel.LoadPhysics(path, &error), qPrintable(error));
        QVERIFY(answerTextDialogs(panel.findChild<QPushButton*>("renamePhysicsGroup"), {QStringLiteral("发梢摆动")}));
        QVERIFY(panel.SavePhysics());
        const QJsonObject saved = readPhysics(path);
        QVERIFY(!saved.contains("PhysicsDictionary"));
        const QJsonArray dictionary = saved.value("Meta").toObject().value("PhysicsDictionary").toArray();
        QString name;
        for (const auto& entry : dictionary)
            if (entry.toObject().value("Id") == "hair") name = entry.toObject().value("Name").toString();
        QCOMPARE(name, QStringLiteral("发梢摆动"));
        QCOMPARE(saved.value("PhysicsSettings"), root.value("PhysicsSettings"));
        CPhysicsPanel reloaded;
        QVERIFY2(reloaded.LoadPhysics(path, &error), qPrintable(error));
        QCOMPARE(reloaded.findChild<QTableWidget*>("physicsGroupTable")->item(0, 1)->text(), name);
    }

    void addedGroupAndCancelledDialogRoundTrip()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("groups.physics3.json");
        QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel;
        QVERIFY(panel.LoadPhysics(path));
        auto* add = panel.findChild<QPushButton*>("addPhysicsGroup");
        QVERIFY(answerTextDialogs(add, {"extra", QStringLiteral("追加组")}));
        QVERIFY(panel.SavePhysics());
        const QJsonObject saved = readPhysics(path);
        QVERIFY(!saved.contains("PhysicsDictionary"));
        const auto settings = saved.value("PhysicsSettings").toArray();
        QCOMPARE(settings.size(), 3);
        QCOMPARE(settings.last().toObject().value("Id").toString(), QStringLiteral("extra"));
        QCOMPARE(saved.value("Meta").toObject().value("PhysicsDictionary").toArray().last().toObject().value("Name").toString(), QStringLiteral("追加组"));
        CPhysicsPanel reloaded;
        QVERIFY(reloaded.LoadPhysics(path));
        auto* groups = reloaded.findChild<QTableWidget*>("physicsGroupTable");
        QCOMPARE(groups->item(2, 1)->text(), QStringLiteral("追加组"));
        QVERIFY(answerTextDialogs(reloaded.findChild<QPushButton*>("addPhysicsGroup"), {"cancelled", "cancel"}, true));
        QCOMPARE(groups->rowCount(), 3);
        QVERIFY(!reloaded.HasUnsavedChanges());
        QVERIFY(reloaded.SavePhysics());
        QCOMPARE(readPhysics(path), saved);
    }

    void deletingCurrentGroupRefreshesDetailsAndDrag()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("groups.physics3.json");
        QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel;
        QVERIFY(panel.LoadPhysics(path));
        auto* groups = panel.findChild<QTableWidget*>("physicsGroupTable");
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable");
        auto* drag = panel.findChild<QCheckBox*>();
        auto* remove = panel.findChild<QPushButton*>("removePhysicsGroup");
        QVERIFY(groups && inputs && drag && remove);
        QSignalSpy parameters(&panel, &CPhysicsPanel::PhysicsDragParamsChanged);
        drag->setChecked(true);
        QCOMPARE(parameters.last().at(0).toStringList(), QStringList{"ParamHair"});
        parameters.clear();
        remove->click();
        QCOMPARE(groups->item(0, 0)->text(), QStringLiteral("tail"));
        QCOMPARE(inputs->item(0, 0)->text(), QStringLiteral("ParamTail"));
        QVERIFY(!parameters.isEmpty());
        QCOMPARE(parameters.last().at(0).toStringList(), QStringList{"ParamTail"});
        QVERIFY(panel.SavePhysics());
        const auto dictionary = readPhysics(path).value("Meta").toObject().value("PhysicsDictionary").toArray();
        QCOMPARE(dictionary.size(), 1);
        QCOMPARE(dictionary.first().toObject().value("Id").toString(), QStringLiteral("tail"));
        parameters.clear();
        remove->click();
        QCOMPARE(inputs->rowCount(), 0);
        QVERIFY(!parameters.isEmpty());
        QVERIFY(parameters.last().at(0).toStringList().isEmpty());
        QVERIFY(!panel.findChild<QPushButton*>("addPhysicsInput")->isEnabled());
        QVERIFY(!panel.findChild<QPushButton*>("addPhysicsOutput")->isEnabled());
        QVERIFY(!panel.findChild<QPushButton*>("addPhysicsVertex")->isEnabled());
        QVERIFY(panel.findChild<QPushButton*>("addPhysicsGroup")->isEnabled());
        QVERIFY(panel.SavePhysics());
        CPhysicsPanel empty;
        QVERIFY(empty.LoadPhysics(path));
        QCOMPARE(empty.findChild<CPhysicsGraphWidget*>()->InputCount(), 0);
        QVERIFY(!empty.findChild<QPushButton*>("addPhysicsInput")->isEnabled());
    }

    void savingPreservesUneditedForces()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("forces.physics3.json");
        const QJsonObject source = physicsFixture();
        QVERIFY(writePhysics(path, source));
        CPhysicsPanel panel;
        QVERIFY(panel.LoadPhysics(path));
        QVERIFY(panel.SavePhysics());
        QCOMPARE(readPhysics(path), source);
    }

    void editingOneForcePreservesOtherComponents()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("forces.physics3.json");
        QJsonObject expected = physicsFixture();
        QVERIFY(writePhysics(path, expected));
        CPhysicsPanel panel;
        QVERIFY(panel.LoadPhysics(path));
        QSignalSpy force(&panel, &CPhysicsPanel::GravityChanged);
        auto* slider = panel.findChild<QSlider*>("physicsGravityY");
        QVERIFY(slider);
        slider->setValue(-50);
        QCOMPARE(force.size(), 1);
        QCOMPARE(force.first().at(0).toFloat(), 1.25f);
        QCOMPARE(force.first().at(1).toFloat(), -0.5f);
        QJsonObject meta = expected.value("Meta").toObject();
        QJsonObject forces = meta.value("EffectiveForces").toObject();
        QJsonObject gravity = forces.value("Gravity").toObject();
        gravity.insert("Y", -0.5);
        forces.insert("Gravity", gravity);
        meta.insert("EffectiveForces", forces);
        expected.insert("Meta", meta);
        QVERIFY(panel.SavePhysics());
        QCOMPARE(readPhysics(path), expected);
    }

    void removingInputClearsActiveDragParameters()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("topology.physics3.json");
        QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel;
        QVERIFY(panel.LoadPhysics(path));
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable");
        auto* drag = panel.findChild<QCheckBox*>("physicsDragMode");
        auto* remove = panel.findChild<QPushButton*>("removePhysicsInput");
        QVERIFY(inputs && drag && remove);
        drag->setChecked(true);
        inputs->selectRow(0);
        QSignalSpy parameters(&panel, &CPhysicsPanel::PhysicsDragParamsChanged);
        remove->click();
        QCOMPARE(inputs->rowCount(), 0);
        QVERIFY(!parameters.isEmpty());
        QVERIFY(parameters.last().at(0).toStringList().isEmpty());
        QVERIFY(panel.SavePhysics());
        CPhysicsPanel reloaded;
        QVERIFY(reloaded.LoadPhysics(path));
        QCOMPARE(reloaded.findChild<QTableWidget*>("physicsInputTable")->rowCount(), 0);
    }

    void failedSavePreservesFileAndDraft()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("original.physics3.json");
        const QJsonObject source = physicsFixture();
        QVERIFY(writePhysics(path, source));
        CPhysicsPanel panel;
        QVERIFY(panel.LoadPhysics(path));
        QVERIFY(answerTextDialogs(panel.findChild<QPushButton*>("renamePhysicsGroup"), {QStringLiteral("待保存名称")}));
        QTimer dismiss;
        int warnings = 0;
        QObject::connect(&dismiss, &QTimer::timeout, [&](){
            auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (message) { ++warnings; message->accept(); }
        });
        dismiss.start(5);
        QVERIFY(!panel.SavePhysics(temp.path()));
        dismiss.stop();
        QCOMPARE(warnings, 1);
        QVERIFY(panel.HasUnsavedChanges());
        QCOMPARE(readPhysics(path), source);
        QVERIFY(panel.SavePhysics());
        CPhysicsPanel reloaded;
        QVERIFY(reloaded.LoadPhysics(path));
        QCOMPARE(reloaded.findChild<QTableWidget*>("physicsGroupTable")->item(0, 1)->text(), QStringLiteral("待保存名称"));
    }

    void failedLoadPreservesSelectionAndUnsavedChanges()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("original.physics3.json");
        const QString invalidPath = temp.filePath("invalid.physics3.json");
        QVERIFY(writePhysics(path, physicsFixture()));
        QVERIFY(writePhysics(invalidPath, QJsonObject{{"PhysicsSettings", "invalid"}}));
        CPhysicsPanel panel;
        QVERIFY(panel.LoadPhysics(path));
        auto* groups = panel.findChild<QTableWidget*>("physicsGroupTable");
        groups->selectRow(1);
        panel.findChild<QCheckBox*>("physicsDragMode")->setChecked(true);
        QVERIFY(answerTextDialogs(panel.findChild<QPushButton*>("renamePhysicsGroup"), {QStringLiteral("编辑中的尾巴")}));
        QSignalSpy parameters(&panel, &CPhysicsPanel::PhysicsDragParamsChanged);
        QString error;
        QVERIFY(!panel.LoadPhysics(invalidPath, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(panel.HasUnsavedChanges());
        QCOMPARE(groups->currentRow(), 1);
        QCOMPARE(groups->item(1, 1)->text(), QStringLiteral("编辑中的尾巴"));
        QVERIFY(parameters.isEmpty());
        QVERIFY(panel.SavePhysics());
        CPhysicsPanel reloaded;
        QVERIFY(reloaded.LoadPhysics(path));
        QCOMPARE(reloaded.findChild<QTableWidget*>("physicsGroupTable")->item(1, 1)->text(), QStringLiteral("编辑中的尾巴"));
    }

    void rejectsInvalidJson()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("broken.physics3.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{broken") > 0);
        file.close();

        CPhysicsPanel panel;
        QString error;
        QVERIFY(!panel.LoadPhysics(path, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!panel.HasUnsavedChanges());
        auto* groups = panel.findChild<QTableWidget*>(QStringLiteral("physicsGroupTable"));
        QVERIFY(groups);
        QVERIFY(panel.findChild<CPhysicsGraphWidget*>(QStringLiteral("physicsGraph")));
        QCOMPARE(groups->rowCount(), 0);
    }

    void editsAndSavesPhysics()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("model.physics3.json"));
        const QByteArray source = R"({
            "Meta": {"Fps": 30, "EffectiveForces": {"Gravity": {"X": 0, "Y": -1}, "Wind": {"X": 0, "Y": 0}},
                     "PhysicsDictionary": [{"Id": "ParamAngleX", "Name": "角度"}]},
            "PhysicsSettings": [{"Id": "hair", "Input": [{"Source": {"Id": "ParamAngleX"}, "Type": "X", "Weight": 0.5, "Reflect": false}],
                                  "Output": [{"Destination": {"Id": "ParamAngleY"}, "VertexIndex": 1, "Type": "Y", "Weight": 0.7, "Scale": 1.2, "Reflect": false}],
                                  "Vertices": [{"Position": {"X": 0, "Y": 0}}, {"Position": {"X": 1, "Y": 0}, "Radius": 1, "Mobility": 0.5, "Delay": 0.2, "Acceleration": 0.3}]}]
        })";
        QJsonObject sourceRoot = QJsonDocument::fromJson(source).object();
        QJsonArray settings = sourceRoot.value(QStringLiteral("PhysicsSettings")).toArray();
        QJsonObject second = settings.first().toObject();
        second.insert(QStringLiteral("Id"), QStringLiteral("tail"));
        QJsonArray secondOutputs = second.value(QStringLiteral("Output")).toArray();
        QJsonObject secondOutput = secondOutputs.first().toObject();
        secondOutput.insert(QStringLiteral("Scale"), 3.4);
        secondOutputs[0] = secondOutput;
        second.insert(QStringLiteral("Output"), secondOutputs);
        settings.append(second);
        sourceRoot.insert(QStringLiteral("PhysicsSettings"), settings);
        const QByteArray sourceWithSecondGroup = QJsonDocument(sourceRoot).toJson(QJsonDocument::Compact);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(sourceWithSecondGroup) == sourceWithSecondGroup.size());
        file.close();

        CPhysicsPanel panel;
        panel.LoadPhysics(path);
        QVERIFY(!panel.HasUnsavedChanges());
        auto* input = panel.findChild<QTableWidget*>(QStringLiteral("physicsInputTable"));
        auto* output = panel.findChild<QTableWidget*>(QStringLiteral("physicsOutputTable"));
        auto* vertex = panel.findChild<QTableWidget*>(QStringLiteral("physicsVertexTable"));
        auto* groups = panel.findChild<QTableWidget*>(QStringLiteral("physicsGroupTable"));
        QVERIFY(input && output && vertex && groups);
        auto* graph = panel.findChild<CPhysicsGraphWidget*>(QStringLiteral("physicsGraph"));
        QVERIFY(graph);
        QCOMPARE(groups->rowCount(), 2);
        QCOMPARE(input->rowCount(), 1);
        QCOMPARE(output->rowCount(), 1);
        QCOMPARE(vertex->rowCount(), 1);
        QCOMPARE(graph->InputCount(), 1);
        QCOMPARE(graph->OutputCount(), 1);
        QCOMPARE(graph->VertexCount(), 2);
        auto* addVertex = panel.findChild<QPushButton*>(QStringLiteral("addPhysicsVertex"));
        auto* removeVertex = panel.findChild<QPushButton*>(QStringLiteral("removePhysicsVertex"));
        QVERIFY(addVertex && removeVertex && addVertex->isEnabled());
        addVertex->click();
        QCOMPARE(vertex->rowCount(), 2);
        vertex->selectRow(1);
        QVERIFY(removeVertex->isEnabled());
        removeVertex->click();
        QCOMPARE(vertex->rowCount(), 1);
        QSignalSpy configChanges(&panel, &CPhysicsPanel::PhysicsConfigChanged);
        panel.LoadPhysics(path);
        QCOMPARE(configChanges.count(), 0);

        groups->selectRow(1);
        output->item(0, 3)->setText(QStringLiteral("bad"));
        QCOMPARE(output->item(0, 3)->text(), QStringLiteral("3.40"));
        groups->selectRow(0);

        input->item(0, 2)->setText(QStringLiteral("0.8"));
        QCOMPARE(input->item(0, 2)->text(), QStringLiteral("0.8"));
        input->item(0, 0)->setText(QStringLiteral("ParamEditedIn"));
        input->item(0, 1)->setText(QStringLiteral("Angle"));
        QCOMPARE(input->item(0, 0)->text(), QStringLiteral("ParamEditedIn"));
        QCOMPARE(input->item(0, 1)->text(), QStringLiteral("Angle"));
        input->item(0, 1)->setText(QStringLiteral("Invalid"));
        QCOMPARE(input->item(0, 1)->text(), QStringLiteral("Angle"));
        output->item(0, 0)->setText(QStringLiteral("ParamEditedOut"));
        output->item(0, 1)->setText(QStringLiteral("Angle"));
        output->item(0, 5)->setText(QStringLiteral("0"));
        QCOMPARE(output->item(0, 0)->text(), QStringLiteral("ParamEditedOut"));
        QCOMPARE(output->item(0, 1)->text(), QStringLiteral("Angle"));
        QCOMPARE(output->item(0, 5)->text(), QStringLiteral("0"));
        output->item(0, 5)->setText(QStringLiteral("99"));
        QCOMPARE(output->item(0, 5)->text(), QStringLiteral("0"));
        QVERIFY(panel.HasUnsavedChanges());
        input->item(0, 2)->setText(QStringLiteral("101.0"));
        QVERIFY(input->item(0, 2)->text() != QStringLiteral("101.0"));
        output->item(0, 3)->setText(QStringLiteral("2.5"));
        vertex->item(0, 2)->setText(QStringLiteral("0.75"));
        QVERIFY(panel.SavePhysics());
        QVERIFY(!panel.HasUnsavedChanges());

        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(saved.readAll()).object();
        const QJsonObject setting = root.value(QStringLiteral("PhysicsSettings")).toArray().first().toObject();
        const QJsonObject meta = root.value(QStringLiteral("Meta")).toObject();
        QCOMPARE(meta.value(QStringLiteral("PhysicsSettingCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("TotalInputCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("TotalOutputCount")).toInt(), 2);
        QCOMPARE(meta.value(QStringLiteral("VertexCount")).toInt(), 4);
        const QJsonObject savedInput = setting.value(QStringLiteral("Input")).toArray().first().toObject();
        const QJsonObject savedOutput = setting.value(QStringLiteral("Output")).toArray().first().toObject();
        QCOMPARE(savedInput.value(QStringLiteral("Source")).toObject().value(QStringLiteral("Id")).toString(), QStringLiteral("ParamEditedIn"));
        QCOMPARE(savedInput.value(QStringLiteral("Type")).toString(), QStringLiteral("Angle"));
        QCOMPARE(savedInput.value(QStringLiteral("Weight")).toDouble(), 0.8);
        QCOMPARE(savedOutput.value(QStringLiteral("Destination")).toObject().value(QStringLiteral("Id")).toString(), QStringLiteral("ParamEditedOut"));
        QCOMPARE(savedOutput.value(QStringLiteral("Type")).toString(), QStringLiteral("Angle"));
        QCOMPARE(savedOutput.value(QStringLiteral("VertexIndex")).toInt(), 0);
        QCOMPARE(savedOutput.value(QStringLiteral("Scale")).toDouble(), 2.5);
        QCOMPARE(setting.value(QStringLiteral("Vertices")).toArray().at(1).toObject().value(QStringLiteral("Mobility")).toDouble(), 0.75);

        const QString brokenPath = temp.filePath(QStringLiteral("broken.physics3.json"));
        QFile broken(brokenPath);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        QVERIFY(broken.write("{broken") > 0);
        broken.close();
        QString error;
        QVERIFY(!panel.LoadPhysics(brokenPath, &error));
        QCOMPARE(groups->rowCount(), 2);
        QVERIFY(!panel.HasUnsavedChanges());

        const QString malformedPath = temp.filePath(QStringLiteral("malformed.physics3.json"));
        QFile malformed(malformedPath);
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        QVERIFY(malformed.write(R"({"PhysicsSettings":[{"Input":"bad"}]})") > 0);
        malformed.close();
        QVERIFY(!panel.LoadPhysics(malformedPath, &error));
        QVERIFY(error.contains(QStringLiteral("Input 无效")));
        QCOMPARE(groups->rowCount(), 2);
        auto* removeGroup = panel.findChild<QPushButton*>(QStringLiteral("removePhysicsGroup"));
        QVERIFY(removeGroup);
        groups->selectRow(1);
        removeGroup->click();
        QCOMPARE(groups->rowCount(), 1);
        QVERIFY(panel.HasUnsavedChanges());
    }

    void loadsCubismPercentageWeights()
    {
        CPhysicsPanel panel;
        QString error;
        const QString path = QString(TEST_RESOURCES) + QStringLiteral("/Mark/Mark.physics3.json");
        QVERIFY2(panel.LoadPhysics(path, &error), qPrintable(error));
        auto* groups = panel.findChild<QTableWidget*>(QStringLiteral("physicsGroupTable"));
        auto* input = panel.findChild<QTableWidget*>(QStringLiteral("physicsInputTable"));
        auto* output = panel.findChild<QTableWidget*>(QStringLiteral("physicsOutputTable"));
        QVERIFY(groups && input && output);
        QVERIFY(groups->rowCount() > 0);
        QVERIFY(input->rowCount() > 0);
        QVERIFY(output->rowCount() > 0);
        QVERIFY(input->item(0, 2)->text().toDouble() > 1.0);
        QVERIFY(output->item(0, 2)->text().toDouble() > 1.0);
        QVERIFY(!panel.HasUnsavedChanges());
    }

    void loadsAllBuiltinPhysicsFiles()
    {
        for (const QString& model : {QStringLiteral("Mark"), QStringLiteral("Natori"),
                                     QStringLiteral("Rice"), QStringLiteral("Wanko")}) {
            CPhysicsPanel panel;
            QString error;
            const QString path = QString(TEST_RESOURCES) + QLatin1Char('/') + model + QLatin1Char('/') + model + QStringLiteral(".physics3.json");
            QVERIFY2(panel.LoadPhysics(path, &error), qPrintable(model + QStringLiteral(": ") + error));
            QVERIFY(panel.findChild<QTableWidget*>(QStringLiteral("physicsGroupTable"))->rowCount() > 0);
        }
    }

    void rejectsInvalidPhysicsReferences()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath(QStringLiteral("invalid.physics3.json"));
        const QByteArray source = R"({
            "PhysicsSettings": [{
                "Id": "group",
                "Input": [{"Source": {"Id": ""}, "Type": "Bad", "Weight": 50, "Reflect": false}],
                "Output": [{"Destination": {"Id": "Param"}, "VertexIndex": 2, "Scale": 1, "Weight": 100, "Type": "X", "Reflect": false}],
                "Vertices": [{"Position": {"X": 0, "Y": 0}, "Mobility": 1, "Delay": 1, "Acceleration": 1, "Radius": 0}]
            }]
        })";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(source) == source.size());
        file.close();
        CPhysicsPanel panel;
        QString error;
        QVERIFY(!panel.LoadPhysics(path, &error));
        QVERIFY(error.contains(QStringLiteral("输入权重无效")) || error.contains(QStringLiteral("输出数值无效")));
    }
};

QTEST_MAIN(PhysicsPanelTests)
#include "physics_panel_tests.moc"
