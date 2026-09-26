#include "ui/cdeformercreatedialog.h"
#include "ui/theme.h"
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <limits>
#include <stdexcept>

using Dialog = CDeformerCreateDialog;
using Builder = Dialog::Builder;
namespace {
Dialog::Context context()
{
    Dialog::Context value;
    value.suggestedId = "FaceControl"; value.existingIds = {"Existing"}; value.partIndex = 2;
    value.parentId = "FaceWarp"; value.parentLabel = QStringLiteral("脸部（FaceWarp）"); value.partLabel = QStringLiteral("面部");
    value.children = {{Builder::Owner::ArtMesh, "EyeL"}, {Builder::Owner::Warp, "EyeWarp"}};
    value.mocVersion = 5; value.warpBounds = QRectF(-1, -2, 3, 4);
    value.rotation.originX = .25f; value.rotation.originY = -.5f;
    return value;
}
Dialog::Prepare ready()
{
    return [](const auto&, QString* summary, QString*) { *summary = QStringLiteral("可创建。选中对象的形状与现有动画将保持一致。"); return true; };
}
Dialog::Submit success() { return [](const auto&, QString*) { return true; }; }
QPushButton* create(Dialog& dialog) { return dialog.findChild<QDialogButtonBox*>("deformerCreateButtons")->button(QDialogButtonBox::Ok); }
QLineEdit* id(Dialog& dialog) { return dialog.findChild<QLineEdit*>("deformerCreateId"); }
QComboBox* type(Dialog& dialog) { return dialog.findChild<QComboBox*>("deformerCreateType"); }
QString error(Dialog& dialog) { return dialog.findChild<QLabel*>("deformerCreateError")->text(); }
}

class DeformerCreateDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void boundsAndTypedSelectionArePreserved()
    {
        auto input = context();
        Dialog dialog(input, ready(), success());
        Builder::InsertRequest request; QString message;
        QVERIFY2(dialog.BuildRequest(request, &message), qPrintable(message));
        QCOMPARE(request.id, input.suggestedId); QCOMPARE(request.partIndex, input.partIndex);
        QCOMPARE(request.parentId, input.parentId); QVERIFY(request.children == input.children);
        const auto warp = std::get<Builder::Warp>(request.shape);
        QCOMPARE(warp.rows, 2); QCOMPARE(warp.columns, 2); QVERIFY(!warp.quad);
        QCOMPARE(warp.positions, QVector<float>({-1,-2,.5f,-2,2,-2,-1,0,.5f,0,2,0,-1,2,.5f,2,2,2}));
        type(dialog)->setCurrentIndex(1);
        QVERIFY(dialog.BuildRequest(request, &message));
        const auto& rotation = std::get<Builder::Rotation>(request.shape);
        QCOMPARE(rotation.originX, input.rotation.originX); QCOMPARE(rotation.originY, input.rotation.originY);
        QCOMPARE(rotation.scale, 1.f); QVERIFY(request.children == input.children);
        type(dialog)->setCurrentIndex(0); QVERIFY(dialog.BuildRequest(request));
        QCOMPARE(std::get<Builder::Warp>(request.shape).positions, warp.positions);
    }

    void invalidDraftDoesNotReplaceOutput_data()
    {
        QTest::addColumn<int>("fault");
        const char* names[]{"empty-id","duplicate-id","utf8-limit","embedded-null","missing-bounds","zero-bounds","infinite-bounds","float-overflow","invalid-grid","huge-grid","duplicate-child","wrong-child-kind","bad-part","bad-version","invalid-rotation","pending-row"};
        for (int index = 0; index < int(std::size(names)); ++index) QTest::newRow(names[index]) << index;
    }
    void invalidDraftDoesNotReplaceOutput()
    {
        QFETCH(int, fault); auto input = context();
        if (fault == 0) input.suggestedId = "  ";
        if (fault == 1) input.suggestedId = "Existing";
        if (fault == 2) input.suggestedId = QString(22, QChar(0x4e2d));
        if (fault == 3) input.suggestedId = QString("a") + QChar(0) + 'b';
        if (fault == 4) input.warpBounds.reset();
        if (fault == 5) input.warpBounds = QRectF(0, 0, 0, 1);
        if (fault == 6) input.warpBounds = QRectF(0, 0, std::numeric_limits<double>::infinity(), 1);
        if (fault == 7) input.warpBounds = QRectF(0, 0, 1e100, 1);
        if (fault == 8) input.warp.rows = 0;
        if (fault == 9) { input.warp.rows = 999999; input.warp.columns = 999999; }
        if (fault == 10) input.children.append(input.children.front());
        if (fault == 11) input.children[0].type = static_cast<Builder::Owner::Type>(9);
        if (fault == 12) input.partIndex = -2;
        if (fault == 13) input.mocVersion = 0;
        if (fault == 14) { input.startWithRotation = true; input.rotation.angle = std::numeric_limits<float>::quiet_NaN(); }
        int prepared = 0;
        Dialog dialog(input, [&](const auto&, QString*, QString*) { ++prepared; return true; }, success());
        if (fault == 15) dialog.findChild<QSpinBox*>("deformerCreateRows")->findChild<QLineEdit*>()->clear();
        Builder::InsertRequest request; request.id = "keep"; QString message;
        QVERIFY(!dialog.BuildRequest(request, &message)); QVERIFY(!message.isEmpty()); QCOMPARE(request.id, QString("keep"));
        dialog.RefreshAvailability(); QVERIFY(!create(dialog)->isEnabled()); QCOMPARE(prepared, 0);
        dialog.accept(); QCOMPARE(prepared, 0); QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }

    void oldVersionAndLockedGridStayExplicit_data()
    {
        QTest::addColumn<bool>("oldVersion"); QTest::addColumn<bool>("quad");
        for (bool oldVersion : {false,true}) for (bool quad : {false,true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(oldVersion).arg(quad))) << oldVersion << quad;
    }
    void oldVersionAndLockedGridStayExplicit()
    {
        QFETCH(bool, oldVersion); QFETCH(bool, quad); auto input = context();
        input.mocVersion = oldVersion ? 1 : 5; input.warp.quad = quad;
        input.lockWarpGrid = !oldVersion; input.warp.rows = 3; input.warp.columns = 4;
        input.warpGridHint = QStringLiteral("沿用父网格，保持原有动作。");
        Dialog dialog(input, ready(), success());
        auto* check = dialog.findChild<QCheckBox*>("deformerCreateQuad");
        QCOMPARE(check->isChecked(), quad); QVERIFY(!check->isEnabled());
        Builder::InsertRequest request; QString message;
        QCOMPARE(dialog.BuildRequest(request, &message), !(oldVersion && quad));
        if (oldVersion && quad) {
            QVERIFY(message.contains(QStringLiteral("四边形")));
            type(dialog)->setCurrentIndex(1); QVERIFY(dialog.BuildRequest(request));
            type(dialog)->setCurrentIndex(0); QVERIFY(!dialog.BuildRequest(request)); QVERIFY(check->isChecked());
        }
        if (!oldVersion) {
            QVERIFY(!dialog.findChild<QSpinBox*>("deformerCreateRows")->isEnabled());
            QCOMPARE(std::get<Builder::Warp>(request.shape).rows, 3);
            dialog.findChild<QSpinBox*>("deformerCreateRows")->setValue(5);
            QVERIFY(!dialog.BuildRequest(request, &message)); QVERIFY(message.contains(QStringLiteral("锁定")));
        }
    }

    void missingCallbacksNeverEnableCreate_data()
    {
        QTest::addColumn<bool>("missingPrepare"); QTest::newRow("prepare") << true; QTest::newRow("submit") << false;
    }
    void missingCallbacksNeverEnableCreate()
    {
        QFETCH(bool, missingPrepare);
        Dialog dialog(context(), missingPrepare ? Dialog::Prepare{} : ready(), missingPrepare ? success() : Dialog::Submit{});
        dialog.RefreshAvailability(); QVERIFY(!create(dialog)->isEnabled()); QVERIFY(!error(dialog).isEmpty());
        dialog.accept(); QCOMPARE(dialog.result(), int(QDialog::Rejected));
    }

    void prepareCapabilityFollowsTypeAndRechecksOnSubmit()
    {
        int prepares = 0, submits = 0; bool stale = false;
        Dialog dialog(context(), [&](const auto& request, QString* summary, QString* message) {
            ++prepares;
            if (stale || std::holds_alternative<Builder::Rotation>(request.shape)) { *message = QStringLiteral("选区已变化，请重新选择。"); return false; }
            *summary = QStringLiteral("保留当前动画。"); return true;
        }, [&](const auto&, QString*) { ++submits; return true; });
        QTRY_VERIFY(create(dialog)->isEnabled());
        type(dialog)->setCurrentIndex(1); QTRY_VERIFY(!error(dialog).isEmpty()); QVERIFY(!create(dialog)->isEnabled());
        type(dialog)->setCurrentIndex(0); QTRY_VERIFY(create(dialog)->isEnabled());
        stale = true; dialog.accept(); QCOMPARE(submits, 0); QVERIFY(!create(dialog)->isEnabled());
        QVERIFY(prepares >= 4); QVERIFY(error(dialog).contains(QStringLiteral("选区")));
    }

    void rejectedAndThrowingCallbacksKeepDraft_data()
    {
        QTest::addColumn<bool>("prepareFailure"); QTest::addColumn<int>("kind");
        for (bool prepareFailure : {false,true}) for (int kind : {0,1,2})
            QTest::newRow(qPrintable(QString("%1-%2").arg(prepareFailure).arg(kind))) << prepareFailure << kind;
    }
    void rejectedAndThrowingCallbacksKeepDraft()
    {
        QFETCH(bool, prepareFailure); QFETCH(int, kind); bool reject = false; int submits = 0;
        const auto failure = [&](QString* error) {
            if (kind == 1) throw std::runtime_error("controlled");
            if (kind == 2) throw 42;
            *error = QStringLiteral("请重试"); return false;
        };
        Dialog dialog(context(), [&](const auto&, QString*, QString* error) { return reject && prepareFailure ? failure(error) : true; },
                      [&](const auto&, QString* error) { ++submits; return reject && !prepareFailure ? failure(error) : true; });
        id(dialog)->setText("RetainedDraft"); QTRY_VERIFY(create(dialog)->isEnabled());
        reject = true; dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected)); QCOMPARE(id(dialog)->text(), QString("RetainedDraft")); QVERIFY(!error(dialog).isEmpty());
        QCOMPARE(submits, prepareFailure ? 0 : 1);
        QVERIFY(id(dialog)->isEnabled());
        reject = false;
        if (prepareFailure) {
            auto* retry = dialog.findChild<QPushButton*>("deformerCreateRetry"); QVERIFY(!retry->isHidden()); retry->click();
        } else dialog.RefreshAvailability();
        QVERIFY(create(dialog)->isEnabled()); dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted)); QCOMPARE(id(dialog)->text(), QString("RetainedDraft"));
    }

    void callbacksCannotReenterOrDismissSubmission()
    {
        Dialog* pointer = nullptr; int prepares = 0, submits = 0; bool inputsDisabled = false;
        Dialog dialog(context(), [&](const auto&, QString*, QString*) {
            ++prepares; pointer->accept(); pointer->reject(); pointer->RefreshAvailability(); return true;
        }, [&](const auto&, QString*) {
            ++submits; inputsDisabled = !id(*pointer)->isEnabled();
            pointer->accept(); pointer->reject(); pointer->RefreshAvailability(); return true;
        });
        pointer = &dialog; QTRY_VERIFY(create(dialog)->isEnabled()); QCOMPARE(prepares, 1);
        QSignalSpy accepted(&dialog, &QDialog::accepted); dialog.accept();
        QCOMPARE(prepares, 2); QCOMPARE(submits, 1); QCOMPARE(accepted.count(), 1); QVERIFY(inputsDisabled);
        QCoreApplication::processEvents(); QCOMPARE(prepares, 2); QCOMPARE(submits, 1);
    }

    void draftChangeInsidePrepareDiscardsOldCapability()
    {
        Dialog* pointer = nullptr; QStringList prepared; QString committed;
        Dialog dialog(context(), [&](const auto& request, QString*, QString*) {
            prepared.append(request.id); if (prepared.size() == 1) id(*pointer)->setText("LatestDraft"); return true;
        }, [&](const auto& request, QString*) { committed = request.id; return true; });
        pointer = &dialog; QTRY_VERIFY(create(dialog)->isEnabled());
        QCOMPARE(prepared, QStringList({"FaceControl","LatestDraft"}));
        dialog.accept(); QCOMPARE(committed, QString("LatestDraft"));
    }

    void keyboardAcceptAndEscapeUseRealCallbacks_data()
    {
        QTest::addColumn<bool>("accept"); QTest::newRow("enter") << true; QTest::newRow("escape") << false;
    }
    void keyboardAcceptAndEscapeUseRealCallbacks()
    {
        QFETCH(bool, accept); int submitted = 0;
        Dialog dialog(context(), ready(), [&](const auto&, QString*) { ++submitted; return true; });
        dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog)); QTRY_VERIFY(create(dialog)->isEnabled());
        id(dialog)->setFocus(); QTest::keyClick(id(dialog), accept ? Qt::Key_Return : Qt::Key_Escape);
        QTRY_VERIFY(!dialog.isVisible()); QCOMPARE(submitted, int(accept));
        QCOMPARE(dialog.result(), int(accept ? QDialog::Accepted : QDialog::Rejected));
        QCoreApplication::processEvents(); QCOMPARE(submitted, int(accept));
    }

    void callbackDestructionDoesNotAccessFreedDialog_data()
    {
        QTest::addColumn<bool>("duringPrepare"); QTest::addColumn<bool>("parent"); QTest::addColumn<bool>("result");
        for (bool duringPrepare : {false,true}) for (bool parent : {false,true}) for (bool result : {false,true})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(duringPrepare).arg(parent).arg(result))) << duringPrepare << parent << result;
    }
    void callbackDestructionDoesNotAccessFreedDialog()
    {
        QFETCH(bool, duringPrepare); QFETCH(bool, parent); QFETCH(bool, result);
        auto* owner = new QWidget; Dialog* pointer = nullptr; bool destroy = false;
        const auto remove = [&] { if (parent) { delete owner; owner = nullptr; } else { delete pointer; } return result; };
        pointer = new Dialog(context(), [&](const auto&, QString*, QString*) { return destroy && duringPrepare ? remove() : true; },
                             [&](const auto&, QString*) { return remove(); }, owner);
        QPointer<Dialog> alive(pointer); QTRY_VERIFY(create(*pointer)->isEnabled()); destroy = true;
        if (duringPrepare) pointer->RefreshAvailability(); else pointer->accept();
        QVERIFY(alive.isNull()); delete owner; QCoreApplication::processEvents();
    }

    void cancelBeforePrepareAndParentCloseAreSafe()
    {
        int calls = 0; auto* parent = new QWidget;
        auto* dialog = new Dialog(context(), [&](const auto&, QString*, QString*) { ++calls; return true; }, success(), parent);
        QPointer<Dialog> alive(dialog); dialog->reject(); QCoreApplication::processEvents(); QCOMPARE(calls, 0);
        delete parent; QVERIFY(alive.isNull());
    }

    void nativeLayoutAndThemes_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false,true}) for (int width : {340,640})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark).arg(width))) << dark << width;
    }
    void nativeLayoutAndThemes()
    {
        QFETCH(bool, dark); QFETCH(int, width);
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        auto input = context(); input.lockWarpGrid = true;
        input.selectionSummary = QStringLiteral("选中左眼、右眼和轮廓的 3 个对象；<b>此处按原文显示</b>。");
        input.parentLabel = QStringLiteral("脸部变形器（FaceWarpWithALongReadableIdentifier）");
        Dialog dialog(input, ready(), success()); dialog.resize(width, 460); dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog)); QTRY_VERIFY(create(dialog)->isEnabled());
        QCOMPARE(create(dialog)->palette().color(QPalette::Button), QColor(Theme::tokens(dark).value("@accentFill@")));
        QCOMPARE(create(dialog)->palette().color(QPalette::ButtonText), QColor(Theme::tokens(dark).value("@accentText@")));
        auto* scroll = dialog.findChild<QScrollArea*>("deformerCreateScroll");
        QVERIFY(scroll); QVERIFY(scroll->widget()->width() <= scroll->viewport()->width());
        QVERIFY(dialog.rect().contains(create(dialog)->mapTo(&dialog, QPoint(0, 0))));
        QVERIFY(dialog.rect().contains(create(dialog)->mapTo(&dialog, create(dialog)->rect().bottomRight())));
        QCOMPARE(dialog.findChild<QLabel*>("deformerCreateSelection")->textFormat(), Qt::PlainText);
        QCOMPARE(dialog.findChild<QLabel*>("deformerCreateSelection")->text(), input.selectionSummary);
        for (auto* field : {static_cast<QWidget*>(id(dialog)), static_cast<QWidget*>(type(dialog)), static_cast<QWidget*>(dialog.findChild<QSpinBox*>("deformerCreateRows"))}) {
            const auto position = field->mapTo(scroll->widget(), QPoint(0, 0));
            QVERIFY2(position.x() >= 0 && position.x() + field->width() <= scroll->widget()->width(), qPrintable(field->objectName()));
        }
        const auto output = qEnvironmentVariable("LIVE2D_DIALOG_SCREENSHOTS");
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            QVERIFY(dialog.grab().save(output + QString("/deformer-create-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
        }
        dialog.reject();
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings isolated(QSettings::IniFormat, QSettings::UserScope, "live2d-tests", "deformer-create");
    if (isolated.format() != QSettings::IniFormat || !isolated.fileName().startsWith(settings.path())) return 3;
    qApp->setStyleSheet(Theme::globalStyleSheet(false));
    DeformerCreateDialogTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "deformer_create_dialog_tests.moc"
