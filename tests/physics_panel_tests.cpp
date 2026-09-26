#include "ui/cphysicspanel.h"
#include "ui/cphysicsgraph.h"
#include "ui/theme.h"
#include <QScrollArea>
#include <QScrollBar>
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
#include <QComboBox>
#include <limits>
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
    void topObservationGroupSynchronizesKeyboardAndTableWithoutEditing()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); panel.resize(460, 500); panel.show();
        auto* picker = panel.findChild<QComboBox*>("physicsObservationGroup"); auto* table = panel.findChild<QTableWidget*>("physicsGroupTable");
        QVERIFY(picker); QCOMPARE(picker->count(), 2); QCOMPARE(picker->currentText(), QStringLiteral("头发（hair）"));
        const auto before = panel.DocumentSnapshot(); QSignalSpy changed(&panel, &CPhysicsPanel::ObservationGroupChanged);
        picker->setFocus(); QTest::keyClick(picker, Qt::Key_Down);
        QCOMPARE(picker->currentIndex(), 1); QCOMPARE(table->currentRow(), 1); QCOMPARE(panel.SelectedGroupIndex(), 1);
        QCOMPARE(changed.size(), 1); QCOMPARE(changed.last()[0].toInt(), 1);
        table->setCurrentCell(0, 0); QCOMPARE(picker->currentIndex(), 0); QCOMPARE(panel.SelectedGroupIndex(), 0); QCOMPARE(changed.size(), 2);
        picker->setCurrentIndex(-1); QCOMPARE(table->currentRow(), -1); QCOMPARE(panel.SelectedGroupIndex(), -1); QCOMPARE(changed.last()[0].toInt(), -1);
        CPhysicsPanel::Snapshot current; QVERIFY(panel.CaptureSnapshot(current)); auto target = current; target.selectedGroupRow = 1; target.selectedGroupId = "tail";
        CPhysicsPanel::PreparedSnapshot prepared; QVERIFY(panel.PrepareSnapshot(current, target, prepared));
        QVERIFY(panel.CommitPreparedSnapshot(prepared)); panel.NotifySnapshotCommitted();
        QCOMPARE(picker->currentIndex(), 1); QCOMPARE(table->currentRow(), 1); QCOMPARE(picker->currentText(), QStringLiteral("尾巴（tail）"));
        QCOMPARE(panel.DocumentSnapshot(), before); QVERIFY(!panel.CanUndo()); QVERIFY(!panel.CanRedo()); QVERIFY(!panel.HasUnsavedChanges());
    }

    void topObservationGroupRejectsPendingAndPublishingWithoutLosingText()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); panel.resize(800, 700); panel.show();
        auto* picker = panel.findChild<QComboBox*>("physicsObservationGroup"); auto* groups = panel.findChild<QTableWidget*>("physicsGroupTable");
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable");
        const auto before = panel.DocumentSnapshot(); QSignalSpy rejected(&panel, &CPhysicsPanel::EditRejected), selected(&panel, &CPhysicsPanel::ObservationGroupChanged);
        inputs->setCurrentCell(0, 2); inputs->editItem(inputs->item(0, 2)); QTRY_VERIFY(inputs->findChild<QLineEdit*>());
        QPointer<QLineEdit> editor = inputs->findChild<QLineEdit*>(); editor->setText("invalid weight");
        picker->setCurrentIndex(1); QCOMPARE(picker->currentIndex(), 0); QCOMPARE(groups->currentRow(), 0); QCOMPARE(panel.SelectedGroupIndex(), 0);
        QVERIFY(editor); QCOMPARE(editor->text(), QString("invalid weight")); QCOMPARE(panel.DocumentSnapshot(), before); QCOMPARE(selected.size(), 0); QVERIFY(!rejected.isEmpty());
        groups->setCurrentCell(1, 0); QCOMPARE(picker->currentIndex(), 0); QCOMPARE(groups->currentRow(), 0); QCOMPARE(editor->text(), QString("invalid weight"));
        QTest::keyClick(editor, Qt::Key_Escape); QTRY_VERIFY(!panel.HasUnsavedChanges());
        panel.SetEditPublisher([&](const auto&, const auto&, QString* error) {
            picker->setCurrentIndex(1); if (error) *error = "rejected candidate"; return false;
        });
        panel.findChild<QPushButton*>("addPhysicsVertex")->click();
        QCOMPARE(picker->currentIndex(), 0); QCOMPARE(groups->currentRow(), 0); QCOMPARE(panel.DocumentSnapshot(), before);
        QVERIFY(!panel.CanUndo()); QVERIFY(!panel.HasUnsavedChanges()); QCOMPARE(selected.size(), 0);
    }

    void topObservationGroupLabelsFollowRenameAddDeleteAndUndo()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); auto* picker = panel.findChild<QComboBox*>("physicsObservationGroup");
        QVERIFY(answerTextDialogs(panel.findChild<QPushButton*>("renamePhysicsGroup"), {QStringLiteral("发梢摆动")}));
        QCOMPARE(picker->currentText(), QStringLiteral("发梢摆动（hair）")); QVERIFY(panel.Undo()); QCOMPARE(picker->currentText(), QStringLiteral("头发（hair）"));
        QVERIFY(panel.Redo()); QCOMPARE(picker->currentText(), QStringLiteral("发梢摆动（hair）"));
        QVERIFY(answerTextDialogs(panel.findChild<QPushButton*>("addPhysicsGroup"), {"extra", "extra"}));
        QCOMPARE(picker->count(), 3); QCOMPARE(picker->currentText(), QString("extra")); QCOMPARE(picker->currentIndex(), 2);
        QVERIFY(panel.Undo()); QCOMPARE(picker->count(), 2); QCOMPARE(picker->currentIndex(), 0);
        QVERIFY(panel.Redo()); QCOMPARE(picker->count(), 3); QCOMPARE(picker->currentIndex(), 2);
        panel.findChild<QPushButton*>("removePhysicsGroup")->click(); QCOMPARE(picker->count(), 2); QCOMPARE(picker->currentIndex(), 1);
        QVERIFY(panel.Undo()); QCOMPARE(picker->count(), 3); QCOMPARE(picker->currentText(), QString("extra"));
        CPhysicsPanel empty; auto* emptyPicker = empty.findChild<QComboBox*>("physicsObservationGroup");
        QCOMPARE(emptyPicker->currentIndex(), -1); QVERIFY(!emptyPicker->isEnabled());
        QVERIFY(empty.NewPhysics()); QCOMPARE(emptyPicker->currentIndex(), -1); QVERIFY(!emptyPicker->isEnabled());
    }

    void topObservationGroupIsVisibleInShortThemedWindows_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void topObservationGroupIsVisibleInShortThemedWindows()
    {
        QFETCH(bool, dark); QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; panel.setStyleSheet(Theme::globalStyleSheet(dark)); QVERIFY(panel.LoadPhysics(path)); panel.resize(460, 500); panel.show();
        auto* picker = panel.findChild<QComboBox*>("physicsObservationGroup"); auto* scroll = panel.findChild<QScrollArea*>("physicsWorkspaceScroll");
        QTRY_VERIFY(picker->isVisible()); QTRY_COMPARE(scroll->verticalScrollBar()->value(), 0);
        const QRect position(picker->mapTo(scroll->viewport(), QPoint()), picker->size());
        QVERIFY(scroll->viewport()->rect().contains(position)); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        auto* status = panel.findChild<QLabel*>("physicsObservationStatus"); QVERIFY(picker->mapTo(scroll->viewport(), QPoint()).y() < status->mapTo(scroll->viewport(), QPoint()).y());
        picker->setFocus(); QTest::keyClick(picker, Qt::Key_Down); QCOMPARE(panel.SelectedGroupIndex(), 1);
        QCOMPARE(scroll->verticalScrollBar()->value(), 0); QVERIFY(!panel.HasUnsavedChanges()); QVERIFY(!panel.CanUndo());
    }

    void observationSamplesDoNotChangeTheDocumentOrHistory()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json");
        QVERIFY(writePhysics(path, physicsFixture())); CPhysicsPanel panel;
        QSignalSpy groups(&panel, &CPhysicsPanel::ObservationGroupChanged);
        QVERIFY(panel.LoadPhysics(path)); QCOMPARE(panel.SelectedGroupIndex(), 0); QVERIFY(!groups.isEmpty());
        QCOMPARE(groups.last()[0].toInt(), 0);
        const auto before = panel.DocumentSnapshot(); QSignalSpy changed(&panel, &CPhysicsPanel::PhysicsConfigChanged);
        QSignalSpy drag(&panel, &CPhysicsPanel::PhysicsDragParamsChanged);
        Live2D::Cubism::Framework::PhysicsObservation sample;
        sample.groupIndex = 0; sample.groupId = "hair"; sample.sourceDigest = QByteArray(32, 'a'); sample.generation = 1;
        sample.sequence = 1; sample.time = .1; sample.sampled = true; sample.particles = {{0, 0}, {.3, 1}};
        Live2D::Cubism::Framework::PhysicsInputObservation input;
        input.id = "ParamHair"; input.type = "X"; input.active = input.sampled = input.hasRange = true;
        input.minimum = -30; input.maximum = 30; input.value = 17; sample.inputs = {input};
        Live2D::Cubism::Framework::PhysicsOutputObservation output;
        output.id = "<ParamOutput>"; output.type = "Angle"; output.active = output.sampled = output.hasRange = true;
        output.minimum = -1; output.maximum = 1; output.rawValue = 2; output.clampedValue = 1; output.value = .7; output.clipped = true;
        sample.outputs = {output}; panel.SetObservation(sample);
        auto* table = panel.findChild<QTableWidget*>("physicsObservationTable"); auto* graph = panel.findChild<CPhysicsGraphWidget*>();
        QCOMPARE(table->rowCount(), 2); QCOMPARE(table->item(0, 2)->text(), QString("17"));
        QCOMPARE(table->item(1, 1)->text(), QString("<ParamOutput>")); QVERIFY(table->item(1, 1)->toolTip().contains("&lt;ParamOutput&gt;"));
        QCOMPARE(table->item(1, 2)->text(), QString("0.7")); QCOMPARE(table->item(1, 4)->text(), QStringLiteral("2 → 1"));
        QCOMPARE(table->item(1, 5)->text(), QStringLiteral("超限，已裁剪"));
        QCOMPARE(graph->HistorySize(), 1); QCOMPARE(graph->History().last(), QPointF(.1, double(output.value)));
        panel.SetObservation(sample, QStringLiteral("已暂停")); QCOMPARE(graph->HistorySize(), 1);
        QCOMPARE(panel.findChild<QLabel*>("physicsObservationStatus")->text(), QStringLiteral("已暂停"));
        sample.sequence = 0; sample.time = .05; sample.outputs[0].value = -.9; panel.SetObservation(sample);
        QCOMPARE(table->item(1, 2)->text(), QString("0.7")); QCOMPARE(graph->HistorySize(), 1);
        sample.sequence = 2; sample.time = .2; sample.outputs[0].active = false; panel.SetObservation(sample);
        QCOMPARE(table->item(1, 2)->text(), QStringLiteral("—")); QCOMPARE(table->item(1, 5)->text(), QStringLiteral("已屏蔽"));
        QCOMPARE(graph->HistorySize(), 1);
        sample.sequence = 3; sample.sampled = false; panel.SetObservation(sample);
        QVERIFY(!graph->HasObservation()); QCOMPARE(graph->HistorySize(), 0);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("—"));
        QCOMPARE(panel.DocumentSnapshot(), before); QCOMPARE(changed.size(), 0); QCOMPARE(drag.size(), 0);
        QVERIFY(!panel.CanUndo()); QVERIFY(!panel.CanRedo()); QVERIFY(!panel.HasUnsavedChanges());
    }

    void observationHistoryIsBoundedAndResetsForItsActualIdentity()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        Live2D::Cubism::Framework::PhysicsObservation sample;
        sample.groupIndex = 0; sample.groupId = "hair"; sample.sourceDigest = QByteArray(32, 'a'); sample.generation = 1; sample.sampled = true;
        Live2D::Cubism::Framework::PhysicsInputObservation row; row.id = "ParamHair"; row.type = "X"; row.active = row.sampled = true;
        sample.inputs = {row}; sample.particles = {{0, 0}, {0, 1}};
        auto* graph = panel.findChild<CPhysicsGraphWidget*>();
        for (int i = 1; i <= CPhysicsGraphWidget::HistoryCapacity + 30; ++i) {
            sample.sequence = i; sample.time = i / 30.0; sample.inputs[0].value = i;
            panel.SetObservation(sample);
        }
        QCOMPARE(graph->HistorySize(), CPhysicsGraphWidget::HistoryCapacity);
        QCOMPARE(graph->History().first().y(), 31.0);
        ++sample.generation; sample.sequence = 1; sample.time = .1; panel.SetObservation(sample); QCOMPARE(graph->HistorySize(), 1);
        sample.sourceDigest = QByteArray(32, 'b'); panel.SetObservation(sample); QCOMPARE(graph->HistorySize(), 1);
        ++sample.sequence; sample.time = .01; panel.SetObservation(sample); QCOMPARE(graph->HistorySize(), 1);
        sample.groupIndex = 1; sample.groupId = "tail"; ++sample.sequence; panel.SetObservation(sample); QCOMPARE(graph->HistorySize(), 1);
        auto* groups = panel.findChild<QTableWidget*>("physicsGroupTable"); QSignalSpy selection(&panel, &CPhysicsPanel::ObservationGroupChanged);
        groups->setCurrentCell(1, 0); QCOMPARE(selection.size(), 1); QCOMPARE(selection.first()[0].toInt(), 1); QCOMPARE(graph->HistorySize(), 0);
        QCOMPARE(panel.SelectedGroupIndex(), 1); panel.SetObservation(sample); QCOMPARE(graph->HistorySize(), 1);
        sample.inputs[0].value = std::numeric_limits<float>::infinity(); ++sample.sequence; panel.SetObservation(sample);
        QCOMPARE(graph->HistorySize(), 0); QVERIFY(!graph->HasObservation());
        QVERIFY(panel.findChild<QLabel*>("physicsObservationStatus")->text().contains(QStringLiteral("无效")));
        panel.ClearObservation(QStringLiteral("编辑姿态已暂停物理")); QCOMPARE(graph->HistorySize(), 0);
        QCOMPARE(panel.findChild<QTableWidget*>("physicsObservationTable")->rowCount(), 0);
        QCOMPARE(panel.findChild<QLabel*>("physicsObservationStatus")->text(), QStringLiteral("编辑姿态已暂停物理"));
    }

    void observationChannelAndBakeButtonAreUserControlled()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); panel.resize(650, 700); panel.show();
        Live2D::Cubism::Framework::PhysicsObservation sample;
        sample.groupIndex = 0; sample.groupId = "hair"; sample.sequence = 1; sample.time = .1; sample.sampled = true;
        Live2D::Cubism::Framework::PhysicsInputObservation input; input.id = "in"; input.type = "X"; input.active = input.sampled = true; input.value = 9;
        Live2D::Cubism::Framework::PhysicsOutputObservation output; output.id = "out"; output.type = "Y"; output.active = output.sampled = true; output.value = 3;
        sample.inputs = {input}; sample.outputs = {output}; panel.SetObservation(sample);
        auto* channels = panel.findChild<QComboBox*>("physicsObservationChannel"); auto* graph = panel.findChild<CPhysicsGraphWidget*>();
        QCOMPARE(channels->count(), 2); QCOMPARE(graph->History().last().y(), 3.0);
        channels->setFocus(); QTest::keyClick(channels, Qt::Key_Down); QCOMPARE(channels->currentIndex(), 1); QCOMPARE(graph->HistorySize(), 0);
        sample.sequence = 2; sample.time = .2; panel.SetObservation(sample); QCOMPARE(graph->History().last().y(), 9.0);
        auto* button = panel.findChild<QPushButton*>("bakePhysicsButton"); QSignalSpy baked(&panel, &CPhysicsPanel::BakeRequested);
        QVERIFY(!button->isEnabled()); panel.SetBakeAvailable(true);
        auto* scroll = panel.findChild<QScrollArea*>("physicsWorkspaceScroll"); scroll->ensureWidgetVisible(button); QCoreApplication::processEvents();
        QTest::mouseClick(button, Qt::LeftButton); QCOMPARE(baked.size(), 1);
        panel.SetBakeAvailable(false); QTest::mouseClick(button, Qt::LeftButton); QCOMPARE(baked.size(), 1);
        QVERIFY(!panel.CanUndo()); QVERIFY(!panel.HasUnsavedChanges());
    }

    void particlePresetsUseOneAtomicHistoryEntry_data()
    {
        QTest::addColumn<int>("preset"); QTest::addColumn<double>("mobility"); QTest::addColumn<double>("delay"); QTest::addColumn<double>("acceleration");
        QTest::newRow("light") << 0 << .85 << .1 << .7;
        QTest::newRow("standard") << 1 << .8 << .2 << 1.0;
        QTest::newRow("heavy") << 2 << .7 << .4 << 1.3;
    }
    void particlePresetsUseOneAtomicHistoryEntry()
    {
        QFETCH(int, preset); QFETCH(double, mobility); QFETCH(double, delay); QFETCH(double, acceleration);
        auto source = physicsFixture(); auto groups = source["PhysicsSettings"].toArray(); auto group = groups[0].toObject();
        auto vertices = group["Vertices"].toArray();
        auto vertex = vertices[0].toObject(); vertex["Position"] = QJsonObject{{"X", .3}, {"Y", 2.0}}; vertex["Radius"] = 2;
        vertex["Extension"] = QJsonObject{{"Keep", true}}; vertices.append(vertex); vertices.append(vertex);
        group["Vertices"] = vertices; group["Extension"] = "kept"; groups[0] = group; source["PhysicsSettings"] = groups;
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, source));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); const auto before = panel.DocumentSnapshot();
        int calls = 0; bool accept = false;
        panel.SetEditPublisher([&](const auto&, const auto&, QString* error) { ++calls; if (!accept) *error = "runtime rejected"; return accept; });
        auto* picker = panel.findChild<QComboBox*>("physicsParticlePreset"); picker->setCurrentIndex(preset);
        auto* button = panel.findChild<QPushButton*>("applyPhysicsPreset"); QVERIFY(button->isEnabled()); button->click();
        QCOMPARE(calls, 1); QCOMPARE(panel.DocumentSnapshot(), before); QVERIFY(!panel.CanUndo()); QVERIFY(!panel.HasUnsavedChanges());
        accept = true; button->click(); QCOMPARE(calls, 2); QVERIFY(panel.CanUndo()); QVERIFY(panel.HasUnsavedChanges());
        const auto after = panel.DocumentSnapshot(); auto afterGroups = after["PhysicsSettings"].toArray();
        QCOMPARE(afterGroups[1], groups[1]); auto actual = afterGroups[0].toObject(); auto expected = group;
        auto expectedVertices = vertices;
        for (int i = 1; i < expectedVertices.size(); ++i) { auto v = expectedVertices[i].toObject(); v["Mobility"] = mobility; v["Delay"] = delay; v["Acceleration"] = acceleration; expectedVertices[i] = v; }
        expected["Vertices"] = expectedVertices; QCOMPARE(actual, expected);
        QCOMPARE(actual["Vertices"].toArray()[0], vertices[0]);
        button->click(); QCOMPARE(calls, 2);
        QVERIFY(panel.Undo()); QCOMPARE(panel.DocumentSnapshot(), before); QVERIFY(!panel.CanUndo()); QVERIFY(panel.CanRedo());
        QVERIFY(panel.Redo()); QCOMPARE(panel.DocumentSnapshot(), after); QCOMPARE(readPhysics(path), source);
    }

    void physicsPositiveYHangsBelowTheFixedRoot()
    {
        CPhysicsGraphWidget graph; graph.setStyleSheet(Theme::globalStyleSheet(false)); graph.resize(440, 260); graph.show();
        Live2D::Cubism::Framework::PhysicsObservation sample; sample.sampled = true; sample.sequence = 1;
        sample.particles = {{0, 0}, {0, 1}}; graph.SetObservation(sample);
        const auto image = graph.grab().toImage(); const auto accent = graph.palette().color(QPalette::Highlight);
        const double dpr = image.devicePixelRatio(); QVector<int> runs; int length = 0;
        for (int y = qRound(30 * dpr); y < qRound(160 * dpr); ++y) {
            bool ink = false;
            for (int x = image.width() / 2 - 1; x <= image.width() / 2 + 1; ++x) {
                const auto color = image.pixelColor(x, y);
                ink = ink || (std::abs(color.red() - accent.red()) + std::abs(color.green() - accent.green()) + std::abs(color.blue() - accent.blue()) < 15);
            }
            if (ink) ++length;
            else if (length) { runs.append(length); length = 0; }
        }
        if (length) runs.append(length);
        QVERIFY(runs.size() >= 2);
        QVERIFY2(runs.first() < runs.last(), "The hollow fixed root must be above the filled particle in positive physics Y");
    }

    void observedLargeParticleGraphsRenderWithRealPositions_data()
    {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void observedLargeParticleGraphsRenderWithRealPositions()
    {
        QFETCH(bool, dark); CPhysicsGraphWidget graph; graph.setStyleSheet(Theme::globalStyleSheet(dark)); graph.resize(440, 260); graph.show();
        Live2D::Cubism::Framework::PhysicsObservation sample; sample.sampled = true; sample.sequence = 1;
        for (int i = 0; i < 10000; ++i) sample.particles.append(QPointF(double(i) / 10000, std::sin(double(i) / 600)));
        graph.SetObservation(sample); QVERIFY(graph.HasObservation()); const auto first = graph.grab().toImage(); QVERIFY(!first.isNull());
        ++sample.sequence; for (auto& point : sample.particles) point.setX(-point.x()); graph.SetObservation(sample);
        const auto second = graph.grab().toImage(); QVERIFY(first != second);
        graph.ClearObservation(); QVERIFY(!graph.HasObservation()); QVERIFY(graph.grab().toImage() != second);
    }
    void missingAndPartialFieldsUseRuntimeDefaults_data()
    {
        QTest::addColumn<bool>("partial");
        QTest::newRow("missing-forces-and-fps") << false;
        QTest::newRow("partial-force-components") << true;
    }

    void missingAndPartialFieldsUseRuntimeDefaults()
    {
        QFETCH(bool, partial);
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json");
        auto source = physicsFixture(); auto meta = source.value("Meta").toObject(); meta.remove("Fps"); meta.remove("EffectiveForces");
        if (partial) meta.insert("EffectiveForces", QJsonObject{{"Gravity", QJsonObject{{"X", 1.25}}}, {"Wind", QJsonObject{{"Y", 0.5}}}});
        source.insert("Meta", meta); QVERIFY(writePhysics(path, source)); CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        auto* fps = panel.findChild<QLineEdit*>("physicsFps"); QCOMPARE(fps->text(), QString("0"));
        auto* gravityY = panel.findChild<QLineEdit*>("physicsForceValue1"); QCOMPARE(gravityY->text(), QString("-1"));
        QCOMPARE(panel.findChild<QSlider*>("physicsGravityY")->value(), -100);
        QCOMPARE(panel.findChild<QLineEdit*>("physicsForceValue0")->text().toDouble(), partial ? 1.25 : 0.0);
        QCOMPARE(panel.findChild<QLineEdit*>("physicsForceValue2")->text().toDouble(), 0.0);
        QCOMPARE(panel.findChild<QLineEdit*>("physicsForceValue3")->text().toDouble(), partial ? 0.5 : 0.0);
        fps->setText("0.000"); QTest::keyClick(fps, Qt::Key_Return); gravityY->setText("-1.0"); QTest::keyClick(gravityY, Qt::Key_Return);
        QCOMPARE(panel.DocumentSnapshot(), source); QVERIFY(!panel.CanUndo()); QVERIFY(!panel.HasUnsavedChanges());
        QSignalSpy gravity(&panel, &CPhysicsPanel::GravityChanged);
        auto* gravityX = panel.findChild<QLineEdit*>("physicsForceValue0"); gravityX->setText("0.75"); QTest::keyClick(gravityX, Qt::Key_Return);
        QCOMPARE(gravity.size(), 1); QCOMPARE(gravity.last()[0].toFloat(), 0.75f); QCOMPARE(gravity.last()[1].toFloat(), -1.0f);
        const auto afterMeta = panel.DocumentSnapshot().value("Meta").toObject(); QVERIFY(!afterMeta.contains("Fps"));
        QVERIFY(!afterMeta.value("EffectiveForces").toObject().value("Gravity").toObject().contains("Y"));
        QCOMPARE(gravityY->text(), QString("-1")); QVERIFY(panel.Undo()); QCOMPARE(panel.DocumentSnapshot(), source);
        QCOMPARE(gravityY->text(), QString("-1")); QCOMPARE(fps->text(), QString("0")); QVERIFY(panel.Redo());
        QCOMPARE(panel.findChild<QSlider*>("physicsGravityY")->value(), -100);
    }
    void committingTableCellDoesNotDiscardAnotherPendingControl()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); panel.resize(900, 1400); panel.show();
        int calls = 0; panel.SetEditPublisher([&](const auto&, const auto&, QString*) { ++calls; return true; });
        auto* fps = panel.findChild<QLineEdit*>("physicsFps"); fps->setText("invalid"); QTest::keyClick(fps, Qt::Key_Return);
        auto* table = panel.findChild<QTableWidget*>("physicsInputTable"); table->setCurrentCell(0, 0); table->editItem(table->item(0, 0));
        QTRY_VERIFY(table->findChild<QLineEdit*>()); QPointer<QLineEdit> editor = table->findChild<QLineEdit*>();
        editor->setText("ParamNotYetAccepted"); QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(calls, 0); QCOMPARE(fps->text(), QString("invalid")); QCOMPARE(table->item(0, 0)->text(), QString("ParamHair"));
        QVERIFY(editor); QCOMPARE(editor->text(), QString("ParamNotYetAccepted")); QCOMPARE(panel.DocumentSnapshot(), physicsFixture());
        fps->setText("30"); QTest::keyClick(editor, Qt::Key_Return);
        QTRY_COMPARE(table->item(0, 0)->text(), QString("ParamNotYetAccepted")); QCOMPARE(calls, 1); QVERIFY(panel.CanUndo());
    }
    void legacyTopLevelDictionarySurvivesEditingAndHistory()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json");
        auto source = physicsFixture(); auto meta = source.value("Meta").toObject();
        const auto dictionary = meta.take("PhysicsDictionary").toArray(); source.insert("Meta", meta); source.insert("PhysicsDictionary", dictionary);
        QVERIFY(writePhysics(path, source)); CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        auto* groups = panel.findChild<QTableWidget*>("physicsGroupTable"); QCOMPARE(groups->item(0, 1)->text(), QStringLiteral("头发"));
        panel.findChild<QPushButton*>("addPhysicsVertex")->click();
        QCOMPARE(panel.DocumentSnapshot().value("Meta").toObject().value("PhysicsDictionary").toArray(), dictionary);
        QVERIFY(!panel.DocumentSnapshot().contains("PhysicsDictionary")); QVERIFY(panel.Undo());
        QCOMPARE(panel.DocumentSnapshot(), source); QCOMPARE(groups->item(0, 1)->text(), QStringLiteral("头发"));
        QVERIFY(panel.Redo()); QCOMPARE(groups->item(0, 1)->text(), QStringLiteral("头发"));
        QCOMPARE(readPhysics(path), source);
    }
    void narrowThemedWorkspaceKeepsFirstRowsAndControlsReachable_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("height");
        QTest::addColumn<bool>("largeFont");
        for (bool dark : {false, true}) for (int height : {400, 850}) for (bool largeFont : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark ? "dark" : "light").arg(height).arg(largeFont)))
                << dark << height << largeFont;
    }

    void narrowThemedWorkspaceKeepsFirstRowsAndControlsReachable()
    {
        QFETCH(bool, dark); QFETCH(int, height); QFETCH(bool, largeFont);
        CPhysicsPanel panel; panel.setStyleSheet(Theme::globalStyleSheet(dark));
        if (largeFont) { auto font = panel.font(); font.setPointSizeF(font.pointSizeF() * 1.5); panel.setFont(font); }
        QVERIFY(panel.LoadPhysics(QString(TEST_RESOURCES) + "/Wanko/Wanko.physics3.json"));
        panel.resize(280, height); panel.show();
        auto* scroll = panel.findChild<QScrollArea*>("physicsWorkspaceScroll"); QVERIFY(scroll);
        QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        for (const char* name : {"physicsGroupTable", "physicsInputTable", "physicsOutputTable", "physicsVertexTable"}) {
            auto* table = panel.findChild<QTableWidget*>(name); QVERIFY(table); QVERIFY(table->rowCount() > 0);
            QTRY_VERIFY2(table->visualItemRect(table->item(0, 0)).bottom() < table->viewport()->height(), name);
            scroll->ensureWidgetVisible(table);
            QVERIFY(scroll->viewport()->rect().intersects(QRect(table->mapTo(scroll->viewport(), QPoint()), table->size())));
        }
        QVERIFY(!panel.grab().isNull()); QVERIFY(!panel.HasUnsavedChanges());
    }

    void refusedForceTextRemainsCorrectableAndSliderKeepsAcceptedValue()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        bool accepted = false; int calls = 0;
        panel.SetEditPublisher([&](const auto&, const auto&, QString* error) { ++calls; if (!accepted) *error = "rig rejected"; return accepted; });
        auto* edit = panel.findChild<QLineEdit*>("physicsForceValue3"); auto* slider = panel.findChild<QSlider*>("physicsWindY");
        edit->setText("0.7"); QTest::keyClick(edit, Qt::Key_Return);
        QCOMPARE(calls, 1); QCOMPARE(edit->text(), QString("0.7")); QCOMPARE(slider->value(), 0);
        QCOMPARE(panel.DocumentSnapshot(), physicsFixture()); QVERIFY(!panel.CanUndo()); QVERIFY(panel.HasUnsavedChanges());
        accepted = true; QTest::keyClick(edit, Qt::Key_Return); QCOMPARE(calls, 2); QCOMPARE(slider->value(), 70); QVERIFY(panel.CanUndo());
        QVERIFY(panel.Undo()); QCOMPARE(panel.DocumentSnapshot(), physicsFixture());
    }

    void destroyingFocusedNumericAndTableEditorsDoesNotPublish()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        for (int field = 0; field < 3; ++field) {
            auto panel = std::make_unique<CPhysicsPanel>(); QVERIFY(panel->LoadPhysics(path));
            panel->resize(900, 1300); panel->show(); panel->activateWindow();
            int calls = 0; panel->SetEditPublisher([&](const auto&, const auto&, QString*) { ++calls; return true; });
            if (field < 2) {
                auto* edit = panel->findChild<QLineEdit*>(field == 0 ? "physicsFps" : "physicsForceValue0");
                edit->setFocus(); QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(edit, "42");
            } else {
                auto* table = panel->findChild<QTableWidget*>("physicsInputTable"); table->setCurrentCell(0, 0); table->editItem(table->item(0, 0));
                QTRY_VERIFY(table->findChild<QLineEdit*>()); table->findChild<QLineEdit*>()->setText("ParamNotCommitted");
            }
            panel.reset(); QCoreApplication::processEvents(); QCOMPARE(calls, 0);
        }
    }
    void boundsAndNoopPreservePrecisionAndAvoidPublication()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); auto source = physicsFixture();
        auto settings = source.value("PhysicsSettings").toArray(); auto group = settings[0].toObject();
        auto vertices = group.value("Vertices").toArray();
        vertices.append(QJsonObject{{"Position", QJsonObject{{"X", 0}, {"Y", 1}}}, {"Radius", 1.123456789012345}, {"Mobility", 0.5}, {"Delay", 0.2}, {"Acceleration", 0.3}});
        group.insert("Vertices", vertices);
        group.insert("Output", QJsonArray{QJsonObject{{"Destination", QJsonObject{{"Id", "ParamHair"}, {"Target", "Parameter"}}}, {"Type", "X"}, {"Weight", 100}, {"Scale", 1.23456789012345}, {"VertexIndex", 1}}});
        settings[0] = group; source.insert("PhysicsSettings", settings); QVERIFY(writePhysics(path, source));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); int calls = 0;
        panel.SetEditPublisher([&](const auto&, const auto&, QString*) { ++calls; return true; });
        auto* outputs = panel.findChild<QTableWidget*>("physicsOutputTable"); auto* pendulums = panel.findChild<QTableWidget*>("physicsVertexTable");
        QCOMPARE(outputs->item(0, 3)->text().toDouble(), 1.23456789012345);
        QCOMPARE(pendulums->item(0, 1)->text().toDouble(), 1.123456789012345);
        outputs->item(0, 3)->setText("100001"); pendulums->item(0, 3)->setText("100001"); pendulums->item(0, 4)->setText("inf");
        QCOMPARE(calls, 0); QCOMPARE(panel.DocumentSnapshot(), source); QVERIFY(!panel.CanUndo());
        outputs->item(0, 3)->setText("1.2345678901234500"); QCOMPARE(calls, 0); QVERIFY(!panel.CanUndo());
        auto* exact = panel.findChild<QLineEdit*>("physicsForceValue0"); exact->setText("100000"); QTest::keyClick(exact, Qt::Key_Return);
        QCOMPARE(calls, 1); QCOMPARE(panel.findChild<QSlider*>("physicsGravityX")->value(), 10000000);
        QVERIFY(panel.Undo()); QCOMPARE(panel.DocumentSnapshot(), source);
    }

    void pendingControlTextBlocksSnapshotsAndExportWithoutDiscardingText()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        auto* fps = panel.findChild<QLineEdit*>("physicsFps"); fps->setText("not-a-number"); QTest::keyClick(fps, Qt::Key_Return);
        CPhysicsPanel::Snapshot current; QString error; QVERIFY(!panel.CaptureSnapshot(current, &error));
        const auto output = directory.filePath("output.physics3.json"); QVERIFY(!panel.ExportPhysics(output, &error)); QVERIFY(!QFile::exists(output));
        QCOMPARE(fps->text(), QString("not-a-number")); QVERIFY(panel.HasUnsavedChanges());
        fps->setText("30.000"); QTest::keyClick(fps, Qt::Key_Return);
        QVERIFY(panel.CaptureSnapshot(current, &error)); QVERIFY(!panel.HasUnsavedChanges()); QVERIFY(!panel.CanUndo()); QCOMPARE(fps->text(), QString("30"));
    }

    void savedMarkerChangedAfterPrepareIsAppliedAtCommit()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        CPhysicsPanel::Snapshot initial; QVERIFY(panel.CaptureSnapshot(initial));
        panel.findChild<QSlider*>("physicsWindY")->setValue(20);
        CPhysicsPanel::Snapshot edited; QVERIFY(panel.CaptureSnapshot(edited));
        CPhysicsPanel::PreparedSnapshot restore; QVERIFY(panel.PrepareSnapshot(edited, initial, restore)); QVERIFY(!restore.Result().dirty);
        panel.MarkSessionSaved(); QVERIFY(!panel.HasUnsavedChanges());
        QVERIFY(panel.CommitPreparedSnapshot(restore)); panel.NotifySnapshotCommitted(); QVERIFY(panel.HasUnsavedChanges());
        QCOMPARE(panel.DocumentSnapshot(), initial.document);
    }

    void newDraftHistoryCanReturnToNoPhysicsAfterSaving()
    {
        CPhysicsPanel panel; QVERIFY(panel.NewPhysics()); panel.MarkSessionSaved(); QVERIFY(!panel.HasUnsavedChanges());
        QVERIFY(panel.Undo()); QVERIFY(panel.DocumentSnapshot().isEmpty()); QVERIFY(panel.HasUnsavedChanges()); QVERIFY(panel.CanRedo());
        QVERIFY(panel.Redo()); QVERIFY(!panel.HasUnsavedChanges());
    }

    void failedPublisherCannotCommitAnotherPreparedSnapshot()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); CPhysicsPanel::Snapshot original; QVERIFY(panel.CaptureSnapshot(original));
        CPhysicsPanel::PreparedSnapshot prepared; QVERIFY(panel.PrepareReferenceRemoval(original, {"ParamHair"}, prepared));
        bool nested = true;
        panel.SetEditPublisher([&](const auto&, const auto&, QString*) { nested = panel.CommitPreparedSnapshot(prepared); return false; });
        panel.findChild<QSlider*>("physicsWindY")->setValue(20);
        QVERIFY(!nested); QCOMPARE(panel.DocumentSnapshot(), original.document); QVERIFY(!panel.CanUndo());
        QVERIFY(panel.ValidatePreparedSnapshot(prepared));
    }

    void publisherAcceptsOnlyCompleteValidatedCandidatesAndHistoryIsAtomic()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        int calls = 0; int rejection = 1; QJsonObject runtime = panel.DocumentSnapshot();
        panel.SetEditPublisher([&](const CPhysicsPanel::Snapshot& before, const CPhysicsPanel::Snapshot& after, QString* error) {
            ++calls; if (before.document != runtime) { *error = "runtime source mismatch"; return false; }
            if (rejection == 2) throw std::runtime_error("rig rejected");
            if (rejection) { *error = "unknown parameter"; return false; }
            runtime = after.document; return true;
        });
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable");
        const auto initial = panel.DocumentSnapshot(); QSignalSpy rejected(&panel, &CPhysicsPanel::EditRejected);
        inputs->item(0, 0)->setText("ParamRejected");
        QCOMPARE(panel.DocumentSnapshot(), initial); QCOMPARE(inputs->item(0, 0)->text(), QString("ParamHair"));
        QVERIFY(!panel.CanUndo()); QVERIFY(!panel.HasUnsavedChanges()); QCOMPARE(calls, 1); QCOMPARE(rejected.size(), 1);
        rejection = 2; inputs->item(0, 0)->setText("ParamThrow");
        QCOMPARE(panel.DocumentSnapshot(), initial); QVERIFY(!panel.CanUndo()); QCOMPARE(calls, 2);
        rejection = 0; inputs->item(0, 0)->setText("ParamAccepted");
        QVERIFY(panel.CanUndo()); QCOMPARE(panel.DocumentSnapshot(), runtime); QCOMPARE(calls, 3);
        const auto accepted = runtime;
        rejection = 1; QString error; QVERIFY(!panel.Undo(&error)); QVERIFY(!error.isEmpty());
        QVERIFY(panel.CanUndo()); QVERIFY(!panel.CanRedo()); QCOMPARE(panel.DocumentSnapshot(), accepted);
        rejection = 0; QVERIFY(panel.Undo(&error)); QCOMPARE(panel.DocumentSnapshot(), initial); QVERIFY(panel.CanRedo());
        rejection = 2; QVERIFY(!panel.Redo(&error)); QCOMPARE(panel.DocumentSnapshot(), initial); QVERIFY(panel.CanRedo());
        rejection = 0; QVERIFY(panel.Redo(&error)); QCOMPARE(panel.DocumentSnapshot(), accepted);
        QCOMPARE(readPhysics(path), initial);
    }

    void cellReturnRejectsWithoutPublishingTheTextAndCanBeCorrected()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); panel.resize(900, 1300); panel.show();
        bool accept = false; int calls = 0;
        panel.SetEditPublisher([&](const auto&, const auto&, QString* error) { ++calls; if (!accept) *error = "rig refusal"; return accept; });
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable");
        inputs->setCurrentCell(0, 0); inputs->editItem(inputs->item(0, 0));
        QTRY_VERIFY(inputs->findChild<QLineEdit*>());
        QPointer<QLineEdit> editor = inputs->findChild<QLineEdit*>();
        editor->setText("ParamCorrectable"); QTest::keyClick(editor, Qt::Key_Return);
        QCOMPARE(calls, 1); QVERIFY(editor); QVERIFY(editor->isVisible()); QCOMPARE(editor->text(), QString("ParamCorrectable"));
        QCOMPARE(inputs->item(0, 0)->text(), QString("ParamHair")); QVERIFY(!panel.CanUndo());
        auto* error = panel.findChild<QLabel*>("physicsEditError"); QVERIFY(error->isVisible()); QCOMPARE(error->text(), QString("rig refusal"));
        CPhysicsPanel::Snapshot ignored; QString detail; QVERIFY(!panel.CaptureSnapshot(ignored, &detail));
        accept = true; QTest::keyClick(editor, Qt::Key_Return);
        QTRY_COMPARE(inputs->item(0, 0)->text(), QString("ParamCorrectable")); QCOMPARE(calls, 2); QVERIFY(panel.CanUndo());
        QVERIFY(panel.Undo()); QCOMPARE(inputs->item(0, 0)->text(), QString("ParamHair"));
    }

    void invalidNumericCellIsRejectedBeforePublisherAndEscapeRestoresInput()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); panel.resize(900, 1300); panel.show();
        int calls = 0; panel.SetEditPublisher([&](const auto&, const auto&, QString*) { ++calls; return true; });
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable"); inputs->setCurrentCell(0, 2); inputs->editItem(inputs->item(0, 2));
        QTRY_VERIFY(inputs->findChild<QLineEdit*>()); QPointer<QLineEdit> editor = inputs->findChild<QLineEdit*>();
        editor->setText("101"); QTest::keyClick(editor, Qt::Key_Tab);
        QCOMPARE(calls, 0); QVERIFY(editor); QCOMPARE(editor->text(), QString("101")); QCOMPARE(inputs->item(0, 2)->text(), QString("100"));
        QTest::keyClick(editor, Qt::Key_Escape); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        CPhysicsPanel::Snapshot current; QVERIFY(panel.CaptureSnapshot(current)); QVERIFY(!panel.CanUndo()); QVERIFY(!panel.HasUnsavedChanges());
    }

    void referenceRemovalIsolatesHistoryAndRestoresItsCheckpoint()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        panel.findChild<QSlider*>("physicsWindY")->setValue(20); QVERIFY(panel.CanUndo());
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before));
        CPhysicsPanel::PreparedSnapshot removal; QVERIFY(panel.PrepareReferenceRemoval(before, {"ParamHair"}, removal));
        const auto removed = removal.Result(); QVERIFY(panel.CommitPreparedSnapshot(removal)); panel.NotifySnapshotCommitted();
        QVERIFY(!panel.CanUndo()); QVERIFY(!panel.CanRedo());
        panel.MarkSessionSaved(); QVERIFY(!panel.HasUnsavedChanges());
        CPhysicsPanel::PreparedSnapshot restore; QVERIFY(panel.PrepareSnapshot(removed, before, restore));
        QVERIFY(restore.Result().dirty); QVERIFY(panel.CommitPreparedSnapshot(restore)); panel.NotifySnapshotCommitted();
        QVERIFY(panel.CanUndo()); QVERIFY(panel.HasUnsavedChanges());
        QVERIFY(panel.Undo()); QCOMPARE(panel.DocumentSnapshot(), physicsFixture());
        QVERIFY(panel.HasUnsavedChanges());
    }

    void historyOnlyReferencesCannotReappearAfterRemoval()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        auto* inputs = panel.findChild<QTableWidget*>("physicsInputTable"); inputs->setCurrentCell(0, 0);
        panel.findChild<QPushButton*>("removePhysicsInput")->click(); QVERIFY(panel.CanUndo());
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before));
        CPhysicsPanel::PreparedSnapshot removal; QVERIFY(panel.PrepareReferenceRemoval(before, {"ParamHair"}, removal));
        QVERIFY(panel.CommitPreparedSnapshot(removal)); panel.NotifySnapshotCommitted();
        QVERIFY(!panel.CanUndo()); QCOMPARE(panel.DocumentSnapshot(), before.document);
        CPhysicsPanel::Snapshot removed; QVERIFY(panel.CaptureSnapshot(removed)); QVERIFY(removed.revision > before.revision);
        CPhysicsPanel::PreparedSnapshot restore; QVERIFY(panel.PrepareSnapshot(removed, before, restore));
        QVERIFY(panel.CommitPreparedSnapshot(restore)); panel.NotifySnapshotCommitted(); QVERIFY(panel.Undo());
        QCOMPARE(inputs->item(0, 0)->text(), QString("ParamHair"));
    }

    void independentPhysicsEditKeepsStructureUndoConflictProtection()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        CPhysicsPanel::Snapshot before; QVERIFY(panel.CaptureSnapshot(before));
        CPhysicsPanel::PreparedSnapshot removal; QVERIFY(panel.PrepareReferenceRemoval(before, {"ParamHair"}, removal));
        const auto removed = removal.Result(); QVERIFY(panel.CommitPreparedSnapshot(removal)); panel.NotifySnapshotCommitted();
        panel.findChild<QSlider*>("physicsWindY")->setValue(20); QVERIFY(panel.Undo());
        QCOMPARE(panel.DocumentSnapshot(), removed.document);
        CPhysicsPanel::PreparedSnapshot restore; QString error; QVERIFY(!panel.PrepareSnapshot(removed, before, restore, &error));
        QVERIFY(!error.isEmpty());
    }

    void coalescedForceGestureAndSaveMarkersPreserveHistory()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        auto* slider = panel.findChild<QSlider*>("physicsWindY");
        slider->setSliderDown(true); slider->setValue(20); slider->setValue(40); slider->setValue(60); slider->setSliderDown(false);
        QVERIFY(panel.CanUndo()); panel.MarkSessionSaved(); QVERIFY(!panel.HasUnsavedChanges());
        QVERIFY(panel.Undo()); QVERIFY(!panel.CanUndo()); QVERIFY(panel.HasUnsavedChanges()); QCOMPARE(panel.DocumentSnapshot(), physicsFixture());
        QVERIFY(panel.Redo()); QVERIFY(!panel.HasUnsavedChanges());
        QVERIFY(panel.SavePhysics(path)); QVERIFY(panel.CanUndo()); QVERIFY(panel.Undo()); QVERIFY(panel.HasUnsavedChanges());
    }

    void newDraftFpsNormalizationAndExportUseThePublisher()
    {
        CPhysicsPanel panel; int calls = 0; bool accepted = false;
        panel.SetEditPublisher([&](const auto&, const auto&, QString* error) { ++calls; if (!accepted) *error = "refused"; return accepted; });
        QString error; QVERIFY(!panel.NewPhysics(&error)); QVERIFY(panel.DocumentSnapshot().isEmpty()); QVERIFY(!panel.CanUndo());
        accepted = true; QVERIFY(panel.NewPhysics(&error)); QVERIFY(panel.FilePath().isEmpty()); QVERIFY(panel.HasUnsavedChanges());
        QVERIFY(answerTextDialogs(panel.findChild<QPushButton*>("addPhysicsGroup"), {"hair", "头发"}));
        auto* fps = panel.findChild<QLineEdit*>("physicsFps"); fps->setText("29.97"); QTest::keyClick(fps, Qt::Key_Return);
        QCOMPARE(panel.DocumentSnapshot().value("Meta").toObject().value("Fps").toDouble(), 29.97);
        fps->setText("0"); QTest::keyClick(fps, Qt::Key_Return);
        QCOMPARE(panel.DocumentSnapshot().value("Meta").toObject().value("Fps").toDouble(), 0.0);
        auto* minimum = panel.findChild<QLineEdit*>("physicsNormalization0"); minimum->setText("20");
        const auto before = panel.DocumentSnapshot(); const int beforeCalls = calls;
        panel.findChild<QPushButton*>("applyPhysicsNormalization")->click();
        QCOMPARE(panel.DocumentSnapshot(), before); QCOMPARE(calls, beforeCalls); QCOMPARE(minimum->text(), QString("20"));
        minimum->setText("-20"); panel.findChild<QPushButton*>("applyPhysicsNormalization")->click();
        QCOMPARE(panel.DocumentSnapshot().value("PhysicsSettings").toArray()[0].toObject().value("Normalization").toObject().value("Position").toObject().value("Minimum").toDouble(), -20.0);
        QTemporaryDir directory; CPhysicsPanel::Snapshot snapshot; QVERIFY(panel.CaptureSnapshot(snapshot));
        QSignalSpy saved(&panel, &CPhysicsPanel::PhysicsSaved);
        const auto path = directory.filePath("export.physics3.json"); QVERIFY(panel.ExportPhysics(path, &error));
        QCOMPARE(readPhysics(path), snapshot.document); CPhysicsPanel::Snapshot after; QVERIFY(panel.CaptureSnapshot(after));
        QVERIFY(after.SameDocument(snapshot)); QCOMPARE(after.dirty, snapshot.dirty); QCOMPARE(after.history, snapshot.history); QCOMPARE(saved.size(), 0);
        QVERIFY(panel.Undo()); QVERIFY(panel.Redo());
    }

    void copiedGroupIsIndependentAndPreservesUnknownFields()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); auto source = physicsFixture();
        auto settings = source.value("PhysicsSettings").toArray(); auto group = settings[0].toObject();
        group.insert("Unknown", QJsonObject{{"Keep", 8}}); settings[0] = group; source.insert("PhysicsSettings", settings); QVERIFY(writePhysics(path, source));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path));
        QVERIFY(answerTextDialogs(panel.findChild<QPushButton*>("copyPhysicsGroup"), {"hairCopy"}));
        auto actual = panel.DocumentSnapshot().value("PhysicsSettings").toArray(); QCOMPARE(actual.size(), 3);
        auto copy = actual[2].toObject(); copy.insert("Id", "hair"); QCOMPARE(copy, group); QCOMPARE(actual[0].toObject(), group);
        QVERIFY(panel.Undo()); QCOMPARE(panel.DocumentSnapshot(), source); QVERIFY(panel.Redo());
        panel.findChild<QTableWidget*>("physicsInputTable")->item(0, 2)->setText("25");
        actual = panel.DocumentSnapshot().value("PhysicsSettings").toArray(); QCOMPARE(actual[0].toObject(), group);
        QCOMPARE(actual[2].toObject().value("Input").toArray()[0].toObject().value("Weight").toDouble(), 25.0);
    }

    void publisherReentryCannotChangeDraftHistoryOrSource()
    {
        QTemporaryDir directory; const auto path = directory.filePath("source.physics3.json"); QVERIFY(writePhysics(path, physicsFixture()));
        CPhysicsPanel panel; QVERIFY(panel.LoadPhysics(path)); int calls = 0;
        panel.SetEditPublisher([&](const auto&, const auto&, QString*) {
            ++calls; QString error;
            if (panel.LoadPhysics(path, &error) || panel.SavePhysics(path, &error) || panel.Undo(&error) || panel.NewPhysics(&error)) return false;
            return true;
        });
        panel.findChild<QSlider*>("physicsWindY")->setValue(20); QCOMPARE(calls, 1); QVERIFY(panel.CanUndo());
        QCOMPARE(readPhysics(path), physicsFixture());
    }

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
        QCOMPARE(output->item(0, 3)->text(), QStringLiteral("3.4"));
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
