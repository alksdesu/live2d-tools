#include "ui/catlaseditdialog.h"
#include "ui/catlascanvasview.h"
#include "ui/theme.h"
#include "ui/cdraftdialoglifecycle.h"
#include <QtTest>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QWheelEvent>
#include <stdexcept>
#include <QScopeGuard>
#include <QTimer>

using namespace live2dstruct;
namespace {
CAtlasRepackPlanner::Snapshot snapshot(bool locked = false, QSize size = {100, 60})
{
    CAtlasRepackPlanner::Snapshot result;
    result.sourceDigest = QByteArray(32, 'a'); result.topologyDigest = QByteArray(32, 'b'); result.documentRevision = 7;
    for (int slot = 0; slot < 2; ++slot) {
        QImage image(slot ? QSize(200, 100) : size, QImage::Format_RGBA8888);
        image.fill(slot ? QColor(90, 70, 140, 67) : QColor(190, 110, 50, 203));
        if (!slot) for (int y = 0; y < image.height(); ++y) {
            auto* row = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                row[x * 4] = uchar(x % 256); row[x * 4 + 1] = uchar(y % 256);
                row[x * 4 + 2] = uchar((x + y) % 256); row[x * 4 + 3] = uchar((x * 17 + y * 29) % 256);
            }
        }
        QBuffer buffer; buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG");
        CTextureAssets::Source source; CTextureAssets::MakeSource(QString("/tmp/atlas-ui-source-%1.png").arg(slot), buffer.data(), source); result.sources.insert(slot, source);
    }
    CTextureWorkspace::Initialize(result.sources, {}, result.workspace);
    result.meshes = {{{"Face", 0, {.1f, .2f, .25f, .2f, .1f, .4f}}, {0, 1, 2}, true, {}},
                     {{"Hair", 0, {.7f, .5f, .85f, .5f, .7f, .8f}}, {0, 1, 2}, !locked, locked ? QStringLiteral("所属部件已锁定：头发") : QString()}};
    return result;
}
void ready(CAtlasEditDialog& dialog)
{
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 20000);
    QVERIFY2(dialog.IsValid() && dialog.IsReady(), qPrintable(dialog.Error()));
}
void wait(CAtlasEditDialog& dialog) { QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 20000); }
void show(CAtlasEditDialog& dialog, int width = 1100)
{
    dialog.resize(width, 800); dialog.show(); ready(dialog);
    auto* canvas = dialog.findChild<CAtlasCanvasView*>();
    QTRY_VERIFY(canvas->isVisible() && canvas->PixelToViewport({0, 0}).x() >= 0);
}
void connectPreview(CAtlasEditDialog& dialog, CModelAtlasState& runtime, int* calls = nullptr)
{
    dialog.SetPreviewHandler([&runtime, calls](const auto& plan, QString*) { runtime = plan.state; if (calls) ++*calls; return true; });
    dialog.SetCancellationHandler([](QString*) { return true; });
}
void move(CAtlasEditDialog& dialog, int x = 30, int y = 28)
{
    dialog.findChild<QSpinBox*>("atlasPositionX")->setValue(x); dialog.findChild<QSpinBox*>("atlasPositionY")->setValue(y);
    dialog.findChild<QPushButton*>("atlasMove")->click(); wait(dialog);
}
void mouse(CAtlasCanvasView* canvas, QEvent::Type type, QPointF position, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, position, canvas->mapToGlobal(position.toPoint()), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(canvas, &event);
}
}
class AtlasEditDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void displayNamesFilterWithoutChangingMeshIdentity()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); QString error;
        QVERIFY(dialog.SetMeshDisplayNames({{"Face", QStringLiteral("<b>脸部</b>")}, {"Hair", QStringLiteral("头发")}}, &error));
        ready(dialog); auto* list = dialog.findChild<QListWidget*>("atlasObjects");
        if (!list) for (auto* candidate : dialog.findChildren<QListWidget*>()) if (candidate->count() && candidate->item(0)->data(Qt::UserRole).toString() == "Face") list = candidate;
        QVERIFY(list); QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QString("Face"));
        QVERIFY(list->item(0)->text().contains(QStringLiteral("<b>脸部</b> · Face"))); QVERIFY(list->item(0)->toolTip().contains("&lt;b&gt;"));
        auto* search = dialog.findChild<QLineEdit*>("atlasObjectSearch"); search->setText(QStringLiteral("脸部"));
        QVERIFY(!list->item(0)->isHidden()); QVERIFY(list->item(1)->isHidden());
        search->setText("Face"); QVERIFY(!list->item(0)->isHidden());
        QCOMPARE(dialog.Result().state.materials.first().id, QString("Face"));
        QVERIFY(!dialog.SetMeshDisplayNames({{"Face", QString(QChar(0))}}, &error));
        QVERIFY(list->item(0)->text().contains(QStringLiteral("脸部")));
    }
    void recoveredDialogClosesAndCanBeOpenedAgain()
    {
        QWidget owner; QPointer<QDialog> current; bool active = false, blocked = true; int releases = 0, reports = 0;
        CDraftDialogLifecycle::Completion settle;
        const auto open = [&] {
            if (current) { current->raise(); current->activateWindow(); return; }
            current = new QDialog(&owner); const QPointer<QDialog> retained = current; active = true;
            settle = CDraftDialogLifecycle::Attach(current, &owner, [&](QString* error) {
                if (blocked) { *error = "restore blocked"; return false; } active = false; return true;
            }, [&, retained] { if (current && current != retained) return; current.clear(); ++releases; },
               [&](const QString& error) { QVERIFY(error.contains("blocked")); ++reports; });
        };
        open(); const QPointer<QDialog> first = current;
        {
            const auto cleanup = qScopeGuard(settle);
            QTimer::singleShot(0, current, [retained = current] { retained->done(QDialog::Rejected); });
            current->exec();
        }
        QTRY_VERIFY(first && first->isVisible()); QVERIFY(active); QVERIFY(reports > 0); QCOMPARE(releases, 0);
        blocked = false; first->reject(); QVERIFY(!active); QVERIFY(!current); QCOMPARE(releases, 1);
        QVERIFY(first); QVERIFY(!first->isVisible());
        open(); QVERIFY(current); QVERIFY(current != first); current->show(); QVERIFY(current->isVisible());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!first);
        current->reject(); QCOMPARE(releases, 2); QVERIFY(!current);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void recoveredDialogReleaseWaitsForTheClosingStack_data()
    {
        QTest::addColumn<int>("result"); QTest::newRow("accepted") << int(QDialog::Accepted); QTest::newRow("rejected") << int(QDialog::Rejected);
    }
    void recoveredDialogReleaseWaitsForTheClosingStack()
    {
        QFETCH(int, result); QWidget owner; QPointer<QDialog> dialog = new QDialog(&owner); bool inside = false, destroyedInside = false; int releases = 0;
        connect(dialog, &QObject::destroyed, &owner, [&] { destroyedInside = inside; });
        const auto settle = CDraftDialogLifecycle::Attach(dialog, &owner, [](QString*) { return true; }, [&] { ++releases; }, {});
        dialog->show(); inside = true; dialog->done(result); inside = false;
        QCOMPARE(releases, 1); QVERIFY(dialog); settle(); QCOMPARE(releases, 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!dialog); QVERIFY(!destroyedInside);
    }
    void pendingRecoveryShowCannotResurrectAReleasedDialog()
    {
        QWidget owner; QPointer<QDialog> dialog = new QDialog(&owner); bool blocked = true; int releases = 0;
        CDraftDialogLifecycle::Attach(dialog, &owner, [&](QString*) { return !blocked; }, [&] { ++releases; }, {});
        dialog->show(); dialog->done(QDialog::Rejected); QVERIFY(!dialog->isVisible());
        blocked = false; dialog->done(QDialog::Rejected); QCOMPARE(releases, 1);
        QCoreApplication::processEvents(); if (dialog) QVERIFY(!dialog->isVisible());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!dialog);
    }
    void parentDestructionDoesNotCallRecoveryFromChildDestructors()
    {
        auto owner = std::make_unique<QWidget>(); QPointer<QDialog> dialog = new QDialog(owner.get()); int recoveries = 0, releases = 0;
        CDraftDialogLifecycle::Attach(dialog, owner.get(), [&](QString*) { ++recoveries; return false; }, [&] { ++releases; }, {});
        dialog->show(); dialog->done(QDialog::Rejected); QCOMPARE(recoveries, 1);
        owner.reset(); QVERIFY(!dialog); QCoreApplication::processEvents();
        QCOMPARE(recoveries, 1); QCOMPARE(releases, 0);
    }
    void destroyedChildIsRecoveredAsynchronouslyWithoutLosingRelease()
    {
        QWidget owner; QPointer<QDialog> current = new QDialog(&owner); const QPointer<QDialog> retained = current;
        bool active = true; int releases = 0, recoveries = 0;
        CDraftDialogLifecycle::Attach(current, &owner, [&](QString*) { ++recoveries; active = false; return true; },
            [&, retained] { if (current && current != retained) return; current.clear(); ++releases; }, {});
        delete current.data(); QVERIFY(!current); QVERIFY(active); QCOMPARE(recoveries, 0);
        QTRY_COMPARE(releases, 1); QCOMPARE(recoveries, 1); QVERIFY(!active);
    }
    void oldDestroyedDialogDoesNotCancelAReplacementDraft()
    {
        QWidget owner; QPointer<QDialog> current = new QDialog(&owner); const QPointer<QDialog> retained = current;
        bool active = true; int releases = 0;
        CDraftDialogLifecycle::Attach(current, &owner, [&, retained](QString*) {
            if (current && current != retained) return true; active = false; return true;
        }, [&, retained] { if (current && current != retained) return; current.clear(); ++releases; }, {});
        delete current.data(); current = new QDialog(&owner); current->show(); const auto replacement = current;
        QCoreApplication::processEvents(); QVERIFY(active); QCOMPARE(current, replacement); QVERIFY(current->isVisible()); QCOMPARE(releases, 0);
    }
    void lifecycleCallbacksCanDestroyTheirOwner_data()
    {
        QTest::addColumn<int>("phase"); for (int phase = 0; phase < 3; ++phase) QTest::newRow(qPrintable(QString::number(phase))) << phase;
    }
    void lifecycleCallbacksCanDestroyTheirOwner()
    {
        QFETCH(int, phase); QPointer<QWidget> owner = new QWidget; QPointer<QDialog> dialog = new QDialog(owner); int reports = 0, releases = 0;
        const auto settle = CDraftDialogLifecycle::Attach(dialog, owner, [&](QString*) {
            if (phase == 0) delete owner.data(); return phase == 2;
        }, [&] { ++releases; delete owner.data(); }, [&](const QString&) { ++reports; delete owner.data(); });
        settle(); QVERIFY(!owner); QVERIFY(!dialog);
        QCOMPARE(reports, int(phase == 1)); QCOMPARE(releases, int(phase == 2));
        QCoreApplication::processEvents(); settle();
    }
    void forcedClosureDuringRecoveryDoesNotReenterTheRecoveryCallback()
    {
        QWidget owner; QPointer<QDialog> dialog = new QDialog(&owner); bool blocked = true; int recoveries = 0, releases = 0;
        const auto settle = CDraftDialogLifecycle::Attach(dialog, &owner, [&](QString*) {
            ++recoveries; if (blocked) dialog->done(QDialog::Rejected); return !blocked;
        }, [&] { ++releases; }, {});
        settle(); QCOMPARE(recoveries, 1); QCOMPARE(releases, 0); QTRY_VERIFY(dialog->isVisible());
        blocked = false; dialog->reject(); QCOMPARE(recoveries, 2); QCOMPARE(releases, 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!dialog);
    }
    void firstShowFitsParentedAtlasWithoutExplicitFit_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {650, 1180})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark).arg(width))) << dark << width;
    }
    void firstShowFitsParentedAtlasWithoutExplicitFit()
    {
        QFETCH(bool, dark); QFETCH(int, width);
        QWidget parent; parent.resize(1200, 900); parent.show();
        CAtlasEditDialog dialog(snapshot(false, {2048, 1024}), {"Face"}, &parent);
        dialog.setStyleSheet(Theme::globalStyleSheet(dark)); dialog.resize(width, 850); dialog.show(); ready(dialog);
        auto* canvas = dialog.findChild<CAtlasCanvasView*>();
        const auto expected = [&] { const int margin = canvas->fontMetrics().height() * 2; return std::min((canvas->width() - margin) / 2048.0, (canvas->height() - margin) / 1024.0); };
        QTRY_VERIFY(canvas->isVisible() && canvas->height() > canvas->fontMetrics().height() * 2 + 30 &&
            std::abs((canvas->PixelToViewport({2048, 0}) - canvas->PixelToViewport({0, 0})).x() - expected() * 2048) < 1);
        const auto first = canvas->PixelToViewport({0, 0}), last = canvas->PixelToViewport({2048, 1024});
        QVERIFY(first.x() >= 0 && first.y() >= 0); QVERIFY(last.x() <= canvas->width() && last.y() <= canvas->height());
        QCOMPARE(dialog.findChild<QSplitter*>("atlasEditorSplitter")->orientation(), width == 650 ? Qt::Vertical : Qt::Horizontal);
        if (qEnvironmentVariableIsSet("LIVE2D_ATLAS_SCREENSHOTS")) {
            QVERIFY(dialog.grab().save(qEnvironmentVariable("LIVE2D_ATLAS_SCREENSHOTS") + QString("/atlas-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
        }
    }
    void initializeIsAsynchronousAndRejectsInvalidSourceWithoutPublishing()
    {
        auto source = snapshot(); source.workspace.sourceFingerprint[0] ^= 1;
        CAtlasEditDialog dialog(source, {"Face"}); int publications = 0;
        dialog.SetPreviewHandler([&](const auto&, QString*) { ++publications; return true; });
        QVERIFY(dialog.IsBusy()); wait(dialog); QVERIFY(!dialog.IsValid()); QVERIFY(!dialog.IsReady()); QCOMPARE(publications, 0);
        QVERIFY(!dialog.Error().isEmpty()); QVERIFY(!dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
    }
    void numericMovePublishesPixelsAndUvsThenSavesOnce()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); ready(dialog); const auto original = dialog.Result().state; auto runtime = original;
        int previews = 0, submissions = 0; connectPreview(dialog, runtime, &previews);
        dialog.SetSubmissionHandler([&](const auto& plan, QString*) { ++submissions; return plan.state == runtime; });
        move(dialog); QVERIFY2(dialog.Error().isEmpty(), qPrintable(dialog.Error())); QCOMPARE(previews, 1);
        QVERIFY(runtime.materials != original.materials); QVERIFY(runtime.workspace != original.workspace); QCOMPARE(dialog.Result().state, runtime);
        dialog.accept(); QCOMPARE(submissions, 1); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    void failureAndExceptionRetainPlanInputsAndHistory_data()
    {
        QTest::addColumn<bool>("exception"); QTest::newRow("failure") << false; QTest::newRow("exception") << true;
    }
    void failureAndExceptionRetainPlanInputsAndHistory()
    {
        QFETCH(bool, exception); CAtlasEditDialog dialog(snapshot(), {"Face"}); ready(dialog); const auto original = dialog.Result().state;
        dialog.SetPreviewHandler([exception](const auto&, QString* error) -> bool { if (exception) throw std::runtime_error("controlled"); *error = "GPU rejected"; return false; });
        move(dialog); QCOMPARE(dialog.Result().state, original); QVERIFY(!dialog.Error().isEmpty());
        QCOMPARE(dialog.findChild<QSpinBox*>("atlasPositionX")->value(), 30); QVERIFY(!dialog.findChild<QPushButton*>("atlasUndo")->isEnabled());
        QVERIFY(dialog.findChild<QPushButton*>("atlasDiscardInput")->isEnabled());
    }
    void localUndoRedoAndCancelRestoreCombinedState()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); show(dialog); const auto original = dialog.Result().state; auto runtime = original; connectPreview(dialog, runtime);
        move(dialog); QVERIFY2(dialog.Error().isEmpty(), qPrintable(dialog.Error())); const auto changed = dialog.Result().state;
        dialog.findChild<QPushButton*>("atlasUndo")->click(); QCOMPARE(dialog.Result().state, original); QCOMPARE(runtime, original);
        dialog.findChild<QPushButton*>("atlasRedo")->click(); QCOMPARE(dialog.Result().state, changed); QCOMPARE(runtime, changed);
        bool fail = true; dialog.SetCancellationHandler([&](QString* error) { if (fail) { *error = "restore rejected"; return false; } runtime = original; return true; });
        dialog.reject(); QVERIFY(dialog.isVisible()); QCOMPARE(dialog.Result().state, changed); QCOMPARE(runtime, changed);
        fail = false; dialog.reject(); QVERIFY(!dialog.isVisible()); QCOMPARE(runtime, original); QCOMPARE(dialog.Result().state, original);
    }
    void pendingInputsCannotBeSilentlySavedOrDiscardedBySelection()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); ready(dialog); auto runtime = dialog.Result().state; connectPreview(dialog, runtime); int saved = 0;
        dialog.SetSubmissionHandler([&](const auto&, QString*) { ++saved; return true; });
        dialog.findChild<QSpinBox*>("atlasPositionX")->setValue(30); dialog.accept(); QCOMPARE(saved, 0);
        dialog.findChild<CAtlasCanvasView*>()->SetSelectedMeshes({"Hair"});
        QCOMPARE(dialog.findChild<CAtlasCanvasView*>()->SelectedMeshes(), QSet<QString>{"Face"});
        dialog.findChild<QPushButton*>("atlasDiscardInput")->click(); dialog.accept(); QCOMPARE(saved, 1);
    }
    void resizeListsAndRespectsUnselectedLockedObjects()
    {
        CAtlasEditDialog dialog(snapshot(true), {"Face"}); ready(dialog); const auto original = dialog.Result().state; auto runtime = original; int calls = 0; connectPreview(dialog, runtime, &calls);
        const auto impact = dialog.findChild<QLabel*>("atlasResizeImpact")->text(); QVERIFY(impact.contains("Face")); QVERIFY(impact.contains("Hair")); QVERIFY(impact.contains(QStringLiteral("已锁定")));
        dialog.findChild<QSpinBox*>("atlasWidth")->setValue(120); dialog.findChild<QPushButton*>("atlasResize")->click(); wait(dialog);
        QVERIFY(!dialog.Error().isEmpty()); QCOMPARE(calls, 0); QCOMPARE(dialog.Result().state, original); QCOMPARE(runtime, original);
    }
    void newAtlasCrossSlotMoveRotateAndRemovalAreReachable()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); ready(dialog); auto runtime = dialog.Result().state; connectPreview(dialog, runtime);
        dialog.findChild<QSpinBox*>("atlasNewWidth")->setValue(128); dialog.findChild<QSpinBox*>("atlasNewHeight")->setValue(64);
        dialog.findChild<QLineEdit*>("atlasNewName")->setText(QStringLiteral("配件")); dialog.findChild<QPushButton*>("atlasAdd")->click(); wait(dialog);
        QVERIFY2(dialog.Error().isEmpty(), qPrintable(dialog.Error())); QCOMPARE(dialog.Result().state.workspace.atlases.size(), 3);
        auto* target = dialog.findChild<QComboBox*>("atlasDestinationSlot"); target->setCurrentIndex(target->findData(2));
        move(dialog, 8, 9); QVERIFY2(dialog.Error().isEmpty(), qPrintable(dialog.Error())); QCOMPARE(dialog.Result().state.materials[0].textureIndex, 2);
        auto* slot = dialog.findChild<QComboBox*>("atlasCurrentSlot"); slot->setCurrentIndex(slot->findData(2));
        dialog.findChild<QPushButton*>("atlasRotate90")->click(); wait(dialog); QVERIFY2(dialog.Error().isEmpty(), qPrintable(dialog.Error()));
        dialog.findChild<QPushButton*>("atlasRemove")->click(); wait(dialog); QVERIFY(!dialog.Error().isEmpty()); QCOMPARE(dialog.Result().state.workspace.atlases.size(), 3);
    }
    void automaticPackingUsesSelectedTargets()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face", "Hair"}); ready(dialog); auto runtime = dialog.Result().state; connectPreview(dialog, runtime);
        auto* targets = dialog.findChild<QListWidget*>("atlasPackTargets");
        for (int index = 0; index < targets->count(); ++index) targets->item(index)->setCheckState(targets->item(index)->data(Qt::UserRole).toInt() == 1 ? Qt::Checked : Qt::Unchecked);
        dialog.findChild<QPushButton*>("atlasPack")->click(); wait(dialog); QVERIFY2(dialog.Error().isEmpty(), qPrintable(dialog.Error()));
        for (const auto& material : dialog.Result().state.materials) QCOMPARE(material.textureIndex, 1);
    }
    void draggingPublishesOnlyAtReleaseAndEscapeDiscardsGhost()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); show(dialog); auto runtime = dialog.Result().state; int calls = 0; connectPreview(dialog, runtime, &calls);
        auto* canvas = dialog.findChild<CAtlasCanvasView*>();
        QTRY_VERIFY(canvas->PixelToViewport({100, 60}).x() <= canvas->width());
        const auto press = canvas->PixelToViewport({15, 17}), end = canvas->PixelToViewport({35, 32});
        mouse(canvas, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton); QCOMPARE(calls, 0);
        QTest::keyClick(canvas, Qt::Key_Escape); QCOMPARE(calls, 0); QVERIFY(!canvas->HasGesture());
        mouse(canvas, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton); QCOMPARE(calls, 0);
        mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, {}); wait(dialog);
        QVERIFY2(dialog.Error().isEmpty(), qPrintable(dialog.Error())); QCOMPARE(calls, 1);
    }
    void stoppingCalculationAndClosingInitializationCannotPublish()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); int calls = 0, cancellations = 0;
        dialog.SetPreviewHandler([&](const auto&, QString*) { ++calls; return true; });
        dialog.SetCancellationHandler([&](QString*) { ++cancellations; return true; });
        dialog.show(); dialog.reject(); QTRY_VERIFY_WITH_TIMEOUT(!dialog.IsBusy(), 20000);
        QCOMPARE(calls, 0); QCOMPARE(cancellations, 1); QVERIFY(!dialog.isVisible());
    }
    void stopDiscardsEvenAnAlreadyComputedQueuedResult()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); ready(dialog); const auto original = dialog.Result().state; auto runtime = original; int calls = 0; connectPreview(dialog, runtime, &calls);
        dialog.findChild<QSpinBox*>("atlasNewWidth")->setValue(128); dialog.findChild<QSpinBox*>("atlasNewHeight")->setValue(64);
        dialog.findChild<QPushButton*>("atlasAdd")->click(); QVERIFY(dialog.IsBusy());
        dialog.findChild<QPushButton*>("atlasStop")->click(); wait(dialog);
        QCOMPARE(calls, 0); QCOMPARE(dialog.Result().state, original); QCOMPARE(runtime, original);
        QVERIFY(!dialog.findChild<QPushButton*>("atlasUndo")->isEnabled()); QVERIFY(!dialog.Error().isEmpty());
    }
    void doneCannotBypassSaveOrCancellationFailures()
    {
        CAtlasEditDialog dialog(snapshot(), {"Face"}); show(dialog); auto runtime = dialog.Result().state; connectPreview(dialog, runtime); move(dialog);
        const auto changed = dialog.Result().state; bool fail = true;
        dialog.SetSubmissionHandler([&](const auto&, QString* error) { *error = "save rejected"; return !fail; });
        dialog.SetCancellationHandler([&](QString* error) { *error = "restore rejected"; return !fail; });
        dialog.done(QDialog::Accepted); QVERIFY(dialog.isVisible()); QCOMPARE(dialog.Result().state, changed); QCOMPARE(dialog.Error(), QString("save rejected"));
        dialog.done(QDialog::Rejected); QVERIFY(dialog.isVisible()); QCOMPARE(dialog.Result().state, changed); QCOMPARE(dialog.Error(), QString("restore rejected"));
        fail = false; dialog.done(QDialog::Rejected); QVERIFY(!dialog.isVisible());
    }
    void callbacksCanDestroyDialog_data()
    {
        QTest::addColumn<int>("phase"); for (int phase = 0; phase < 3; ++phase) QTest::newRow(qPrintable(QString::number(phase))) << phase;
    }
    void callbacksCanDestroyDialog()
    {
        QFETCH(int, phase); QPointer<CAtlasEditDialog> dialog = new CAtlasEditDialog(snapshot(), {"Face"}); ready(*dialog);
        if (phase == 0) {
            dialog->SetPreviewHandler([&](const auto&, QString*) { delete dialog.data(); return true; });
            dialog->findChild<QSpinBox*>("atlasPositionX")->setValue(30); dialog->findChild<QSpinBox*>("atlasPositionY")->setValue(28);
            dialog->findChild<QPushButton*>("atlasMove")->click(); QTRY_VERIFY_WITH_TIMEOUT(!dialog, 20000);
        } else if (phase == 1) {
            dialog->SetSubmissionHandler([&](const auto&, QString*) { delete dialog.data(); return true; }); dialog->accept(); QVERIFY(!dialog);
        } else {
            dialog->SetCancellationHandler([&](QString*) { delete dialog.data(); return true; }); dialog->reject(); QVERIFY(!dialog);
        }
    }
    void destroyingWhileWorkerRunsDoesNotPublish()
    {
        int calls = 0; auto* dialog = new CAtlasEditDialog(snapshot(false, {2048, 1024}), {"Face"});
        dialog->SetPreviewHandler([&](const auto&, QString*) { ++calls; return true; });
        delete dialog; QTest::qWait(200); QCOMPARE(calls, 0);
    }
    void canvasNavigationCancelsDeferredFit()
    {
        CAtlasCanvasView canvas; canvas.resize(640, 480);
        QImage image(2048, 1024, QImage::Format_RGBA8888); image.fill(Qt::transparent);
        QVERIFY(canvas.SetContent({{0, image}}, {{"Face", 0, {{10, 10}, {20, 10}, {10, 20}}, {0, 1, 2}, true, {}}})); canvas.show();
        QWheelEvent event({200, 160}, canvas.mapToGlobal(QPointF(200, 160)), {}, QPoint(0, 120), {}, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&canvas, &event); const auto origin = canvas.PixelToViewport({0, 0}), unit = canvas.PixelToViewport({1, 1}) - origin;
        QTest::qWait(30); QCOMPARE(canvas.PixelToViewport({0, 0}), origin); QCOMPARE(canvas.PixelToViewport({1, 1}) - origin, unit);
        canvas.hide(); canvas.show(); QTest::qWait(30); QCOMPARE(canvas.PixelToViewport({0, 0}), origin);
    }
};
QTEST_MAIN(AtlasEditDialogTests)
#include "atlas_edit_dialog_tests.moc"
