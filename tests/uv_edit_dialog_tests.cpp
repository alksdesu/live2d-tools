#include "ui/cuveditdialog.h"
#include "ui/cuvcanvasview.h"
#include "ui/theme.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QWheelEvent>
#include <stdexcept>

using namespace live2dstruct;
namespace {
CUvEditDraft::Snapshot snapshot()
{
    return {QByteArray(32,'a'),7,QByteArray(32,'b'),
        {{{"Face",0,{.2f,.2f,.4f,.2f,.2f,.4f,.7f,.7f,.9f,.7f,.7f,.9f}},{0,1,2,3,4,5},true,{}},
         {{"Locked",1,{0,0,1,0,0,1}},{0,1,2},false,"对象已锁定"}},{{0,QSize(100,100)},{1,QSize(200,100)}}};
}
QMap<int,QImage> images()
{
    QImage a(100,100,QImage::Format_ARGB32),b(200,100,QImage::Format_ARGB32);
    a.fill(QColor(245,219,167)); b.fill(QColor(167,219,245)); return {{0,a},{1,b}};
}
void connectPreview(CUvEditDialog& dialog, CModelMaterialState& published, int* calls=nullptr)
{
    dialog.SetPreviewHandler([&published,calls](const auto& state,QString*) { published=state; if(calls) ++*calls; return true; });
    dialog.SetCancellationHandler([](QString*) { return true; });
}
void mouse(CUvCanvasView* view,QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons,Qt::KeyboardModifiers modifiers={})
{
    QMouseEvent event(type,point,view->mapToGlobal(point.toPoint()),button,buttons,modifiers); QApplication::sendEvent(view,&event);
}
void show(CUvEditDialog& dialog)
{
    dialog.resize(980,720); dialog.show(); auto* canvas=dialog.findChild<CUvCanvasView*>();
    QTRY_VERIFY(std::abs((canvas->PixelToViewport({100,100})-canvas->PixelToViewport({0,0})).x()-
        (std::min(canvas->width(),canvas->height())-canvas->fontMetrics().height()*2))<1);
}
}
class UvEditDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void parentedLargeAtlasFitsOnFirstShow_data()
    {
        QTest::addColumn<bool>("dark"); QTest::addColumn<int>("width");
        for (bool dark : {false, true}) for (int width : {620, 1100})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(width))) << dark << width;
    }
    void parentedLargeAtlasFitsOnFirstShow()
    {
        QFETCH(bool, dark); QFETCH(int, width);
        QImage atlas(QString(TEST_RESOURCES) + "/Mark/Mark.2048/texture_00.png"); QVERIFY(!atlas.isNull()); QCOMPARE(atlas.size(), QSize(2048,2048));
        auto source=snapshot(); source.atlasSizes[0]=atlas.size(); auto textures=images(); textures[0]=atlas;
        QWidget parent; parent.resize(1200,900); parent.show();
        CUvEditDialog dialog(source,textures,"Face",&parent); dialog.setStyleSheet(Theme::globalStyleSheet(dark)); dialog.resize(width,800); dialog.show();
        auto* canvas=dialog.findChild<CUvCanvasView*>();
        QTRY_VERIFY(canvas->isVisible() && std::min(canvas->width(),canvas->height())>canvas->fontMetrics().height()*2+100 &&
            std::abs((canvas->PixelToViewport({2048,2048})-canvas->PixelToViewport({0,0})).x()-
                (std::min(canvas->width(),canvas->height())-canvas->fontMetrics().height()*2))<1);
        if(qEnvironmentVariableIsSet("LIVE2D_UV_SCREENSHOTS")) {
            const QString path=qEnvironmentVariable("LIVE2D_UV_SCREENSHOTS")+QString("/first-show-%1-%2.png").arg(dark?"dark":"light").arg(width); QVERIFY(dialog.grab().save(path));
        }
        const double margin=canvas->fontMetrics().height();
        const double expected=std::min(canvas->width()-margin*2,canvas->height()-margin*2);
        const auto extent=canvas->PixelToViewport({2048,2048})-canvas->PixelToViewport({0,0});
        QVERIFY2(std::abs(extent.x()-expected)<1, qPrintable(QString("atlas width %1, expected %2 in viewport %3x%4").arg(extent.x()).arg(expected).arg(canvas->width()).arg(canvas->height())));
        QVERIFY(extent.x()>100); QCOMPARE(extent.x(),extent.y());
        const auto origin=canvas->PixelToViewport({0,0}); QVERIFY(origin.x()>=0); QVERIFY(origin.y()>=0);
        QVERIFY(origin.x()+extent.x()<=canvas->width()); QVERIFY(origin.y()+extent.y()<=canvas->height());
    }
    void pendingFitSettlesAfterVisibleGeometryAndSameMeshSnapshotChanges()
    {
        QWidget parent; parent.resize(800,600); auto* canvas=new CUvCanvasView(&parent); canvas->resize(160,120);
        auto source=snapshot(); canvas->SetSnapshot(source,images()); canvas->show(); QTest::qWait(20);
        QCOMPARE(canvas->PixelToViewport({100,100})-canvas->PixelToViewport({0,0}),QPointF(100,100));
        parent.show(); canvas->resize(640,480);
        const auto expected=[&] { return std::min(canvas->width(),canvas->height())-canvas->fontMetrics().height()*2; };
        QTRY_VERIFY(std::abs((canvas->PixelToViewport({100,100})-canvas->PixelToViewport({0,0})).x()-expected())<1);
        source.atlasSizes[0]=QSize(2048,1024); auto textures=images(); textures[0]=QImage(source.atlasSizes[0],QImage::Format_ARGB32); textures[0].fill(Qt::transparent);
        canvas->SetSnapshot(source,textures);
        QTRY_VERIFY(std::abs((canvas->PixelToViewport({2048,0})-canvas->PixelToViewport({0,0})).x()-(canvas->width()-canvas->fontMetrics().height()*2))<1);
    }
    void userNavigationCancelsQueuedFitAndSurvivesReshow_data()
    {
        QTest::addColumn<bool>("pan"); QTest::newRow("pan")<<true; QTest::newRow("zoom")<<false;
    }
    void userNavigationCancelsQueuedFitAndSurvivesReshow()
    {
        QFETCH(bool,pan);
        CUvCanvasView canvas; canvas.resize(640,480); canvas.SetSnapshot(snapshot(),images()); canvas.show();
        if(pan) {
            mouse(&canvas,QEvent::MouseButtonPress,{80,80},Qt::MiddleButton,Qt::MiddleButton);
            mouse(&canvas,QEvent::MouseMove,{120,100},Qt::NoButton,Qt::MiddleButton);
            mouse(&canvas,QEvent::MouseButtonRelease,{120,100},Qt::MiddleButton,{});
        } else {
            QWheelEvent event({200,160},canvas.mapToGlobal(QPointF(200,160)),{},QPoint(0,120),{},Qt::NoModifier,Qt::NoScrollPhase,false);
            QApplication::sendEvent(&canvas,&event);
        }
        const auto origin=canvas.PixelToViewport({0,0}), unit=canvas.PixelToViewport({1,1})-origin;
        QTest::qWait(30); QCOMPARE(canvas.PixelToViewport({0,0}),origin); QCOMPARE(canvas.PixelToViewport({1,1})-origin,unit);
        canvas.hide(); canvas.show(); QTest::qWait(30); QCOMPARE(canvas.PixelToViewport({0,0}),origin); QCOMPARE(canvas.PixelToViewport({1,1})-origin,unit);
        canvas.resize(700,540); QTest::qWait(20); QCOMPARE(canvas.PixelToViewport({1,1})-canvas.PixelToViewport({0,0}),unit);
        canvas.FitAtlas(); QTRY_VERIFY(std::abs((canvas.PixelToViewport({100,100})-canvas.PixelToViewport({0,0})).x()-(540-canvas.fontMetrics().height()*2))<1);
    }
    void missingOrMismatchedAtlasIsRejected()
    {
        auto wrong=images(); wrong[0]=QImage(4,4,QImage::Format_ARGB32); CUvEditDialog dialog(snapshot(),wrong);
        QVERIFY(!dialog.IsValid()); QVERIFY(!dialog.Error().isEmpty()); QVERIFY(!dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
    }
    void numericPointPreviewThenOneSubmission()
    {
        CUvEditDialog dialog(snapshot(),images()); QVERIFY(dialog.IsValid()); auto published=dialog.Result(); const auto original=published; int previews=0,commits=0;
        bool committedCurrent=false; connectPreview(dialog,published,&previews); dialog.SetSubmissionHandler([&](const auto& state,QString*) { ++commits; committedCurrent=state==published; return true; });
        auto* canvas=dialog.findChild<CUvCanvasView*>(); canvas->SetSelectedVertices({0});
        dialog.findChild<QDoubleSpinBox*>("uvPointX")->setValue(27.25); dialog.findChild<QDoubleSpinBox*>("uvPointY")->setValue(31.5);
        dialog.findChild<QPushButton*>("uvSetPoint")->click(); QCOMPARE(previews,1); QVERIFY(std::abs(published[0].uvs[0]-.2725f)<1e-7f); QCOMPARE(published[1],original[1]);
        dialog.accept(); QCOMPARE(commits,1); QVERIFY(committedCurrent); QCOMPARE(dialog.result(),int(QDialog::Accepted));
    }
    void previewRejectionAndExceptionPreserveAcceptedDraftAndHistory()
    {
        CUvEditDialog dialog(snapshot(),images()); const auto original=dialog.Result(); auto* canvas=dialog.findChild<CUvCanvasView*>(); canvas->SetSelectedVertices({0});
        dialog.findChild<QDoubleSpinBox*>("uvTranslateX")->setValue(10);
        for(int mode=0;mode<2;++mode) {
            dialog.SetPreviewHandler([mode](const auto&,QString* error)->bool { if(mode) throw std::runtime_error("controlled"); *error="rejected"; return false; });
            dialog.findChild<QPushButton*>("uvApplyTransform")->click(); QCOMPARE(dialog.Result(),original); QVERIFY(!dialog.Error().isEmpty()); QVERIFY(!dialog.findChild<QPushButton*>("uvUndo")->isEnabled());
        }
    }
    void rejectionOnSaveRetainsWindowAndCanRetry()
    {
        CUvEditDialog dialog(snapshot(),images()); show(dialog); bool reject=true; int calls=0;
        dialog.SetSubmissionHandler([&](const auto&,QString* error) { ++calls; if(reject) { *error="save rejected"; return false; } return true; });
        dialog.accept(); QVERIFY(dialog.isVisible()); QCOMPARE(dialog.Error(),QString("save rejected")); reject=false; dialog.accept(); QCOMPARE(calls,2); QCOMPARE(dialog.result(),int(QDialog::Accepted));
    }
    void cancellationFailureKeepsPreviewAndWindowUntilRecoverySucceeds()
    {
        CUvEditDialog dialog(snapshot(),images()); auto published=dialog.Result(); const auto original=published; connectPreview(dialog,published); show(dialog);
        dialog.findChild<CUvCanvasView*>()->SetSelectedVertices({0}); dialog.findChild<QDoubleSpinBox*>("uvTranslateX")->setValue(10); dialog.findChild<QPushButton*>("uvApplyTransform")->click(); const auto changed=dialog.Result(); QVERIFY(changed!=original);
        bool fail=true; dialog.SetCancellationHandler([&](QString* error) { if(fail) { *error="restore rejected"; return false; } published=original; return true; });
        dialog.reject(); QVERIFY(dialog.isVisible()); QCOMPARE(dialog.Result(),changed); QCOMPARE(published,changed); QCOMPARE(dialog.Error(),QString("restore rejected"));
        fail=false; dialog.reject(); QVERIFY(!dialog.isVisible()); QCOMPARE(published,original); QCOMPARE(dialog.Result(),original);
    }
    void gestureHasOneLocalUndoAndOriginRestoresPreciseFloat()
    {
        auto source=snapshot(); source.meshes[0].material.uvs[0]=.23456789f;
        CUvEditDialog dialog(source,images()); auto published=dialog.Result(); const auto original=published; int previews=0; connectPreview(dialog,published,&previews); show(dialog);
        auto* canvas=dialog.findChild<CUvCanvasView*>(); canvas->setFocus(); QApplication::processEvents();
        const auto start=canvas->PixelToViewport({double(original[0].uvs[0])*100,20});
        mouse(canvas,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseMove,start+QPointF(45,20),Qt::NoButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseMove,start+QPointF(75,30),Qt::NoButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseButtonRelease,start+QPointF(75,30),Qt::LeftButton,{});
        QVERIFY(previews>=2); QVERIFY(dialog.Result()!=original); dialog.findChild<QPushButton*>("uvUndo")->click(); QCOMPARE(dialog.Result(),original); QVERIFY(!dialog.findChild<QPushButton*>("uvUndo")->isEnabled());
        canvas->setFocus(); mouse(canvas,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseMove,start+QPointF(60,30),Qt::NoButton,Qt::LeftButton); mouse(canvas,QEvent::MouseMove,start,Qt::NoButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseButtonRelease,start,Qt::LeftButton,{}); QCOMPARE(dialog.Result(),original); QVERIFY(!dialog.findChild<QPushButton*>("uvUndo")->isEnabled());
    }
    void escapeCancelsGestureAndAxisConstraintKeepsOtherCoordinate()
    {
        CUvEditDialog dialog(snapshot(),images()); auto published=dialog.Result(); const auto original=published; connectPreview(dialog,published); show(dialog);
        auto* canvas=dialog.findChild<CUvCanvasView*>(); canvas->setFocus(); const auto start=canvas->PixelToViewport({20,20});
        mouse(canvas,QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
        QTest::keyPress(canvas,Qt::Key_X); mouse(canvas,QEvent::MouseMove,start+QPointF(60,40),Qt::NoButton,Qt::LeftButton);
        QCOMPARE(dialog.Result()[0].uvs[1],original[0].uvs[1]); QVERIFY(dialog.Result()[0].uvs[0]!=original[0].uvs[0]);
        QTest::keyClick(canvas,Qt::Key_Escape); QCOMPARE(dialog.Result(),original); QVERIFY(dialog.isVisible()); QVERIFY(!dialog.findChild<QPushButton*>("uvUndo")->isEnabled());
    }
    void islandAndBoxSelectionsKeepDisconnectedComponentsDistinct()
    {
        CUvEditDialog dialog(snapshot(),images()); show(dialog); auto* canvas=dialog.findChild<CUvCanvasView*>();
        auto point=canvas->PixelToViewport({25,25}); mouse(canvas,QEvent::MouseButtonDblClick,point,Qt::LeftButton,Qt::LeftButton);
        QCOMPARE(canvas->SelectedVertices(),QSet<int>({0,1,2}));
        auto a=canvas->PixelToViewport({65,65}),b=canvas->PixelToViewport({95,95});
        mouse(canvas,QEvent::MouseButtonPress,a,Qt::LeftButton,Qt::LeftButton,Qt::ShiftModifier); mouse(canvas,QEvent::MouseMove,b,Qt::NoButton,Qt::LeftButton,Qt::ShiftModifier); mouse(canvas,QEvent::MouseButtonRelease,b,Qt::LeftButton,{},Qt::ShiftModifier);
        QCOMPARE(canvas->SelectedVertices().size(),6);
        mouse(canvas,QEvent::MouseButtonDblClick,point,Qt::LeftButton,Qt::LeftButton,Qt::AltModifier); QCOMPARE(canvas->SelectedVertices(),QSet<int>({3,4,5}));
    }
    void textureAssignmentAndResetUseSameUndoPipeline()
    {
        CUvEditDialog dialog(snapshot(),images()); auto published=dialog.Result(); const auto original=published; connectPreview(dialog,published);
        auto* slot=dialog.findChild<QComboBox*>("uvDestinationTexture"); slot->setCurrentIndex(slot->findData(1));
        dialog.findChild<QComboBox*>("uvTextureCoordinates")->setCurrentIndex(1); dialog.findChild<QPushButton*>("uvAssignTexture")->click();
        QCOMPARE(dialog.Result()[0].textureIndex,1); QCOMPARE(dialog.Result()[0].uvs[0],.1f); dialog.findChild<QPushButton*>("uvResetAll")->click(); QCOMPARE(dialog.Result(),original);
        dialog.findChild<QPushButton*>("uvUndo")->click(); QCOMPARE(dialog.Result()[0].textureIndex,1); dialog.findChild<QPushButton*>("uvUndo")->click(); QCOMPARE(dialog.Result(),original);
        dialog.findChild<QPushButton*>("uvRedo")->click(); QCOMPARE(dialog.Result()[0].textureIndex,1);
    }
    void keyboardMovementAndPanningDoNotShareEditHistory()
    {
        CUvEditDialog dialog(snapshot(),images()); auto published=dialog.Result(); connectPreview(dialog,published); show(dialog);
        auto* canvas=dialog.findChild<CUvCanvasView*>(); canvas->SetSelectedVertices({0}); canvas->setFocus(); QTest::keyClick(canvas,Qt::Key_Right);
        QVERIFY(std::abs(dialog.Result()[0].uvs[0]-.21f)<1e-7f); const auto edited=dialog.Result();
        const auto before=canvas->PixelToViewport({20,20}); mouse(canvas,QEvent::MouseButtonPress,{100,100},Qt::MiddleButton,Qt::MiddleButton); mouse(canvas,QEvent::MouseMove,{140,120},Qt::NoButton,Qt::MiddleButton); mouse(canvas,QEvent::MouseButtonRelease,{140,120},Qt::MiddleButton,{});
        QCOMPARE(dialog.Result(),edited); QCOMPARE(canvas->PixelToViewport({20,20})-before,QPointF(40,20));
    }
    void closedOrReenteredPublisherDoesNotMutateDestroyedOrBusyDialog()
    {
        QPointer<CUvEditDialog> dialog=new CUvEditDialog(snapshot(),images()); auto* canvas=dialog->findChild<CUvCanvasView*>(); canvas->SetSelectedVertices({0});
        int calls=0; dialog->SetPreviewHandler([&](const auto&,QString*) { ++calls; dialog->reject(); dialog->accept(); delete dialog.data(); return true; });
        dialog->findChild<QDoubleSpinBox*>("uvTranslateX")->setValue(10); dialog->findChild<QPushButton*>("uvApplyTransform")->click(); QVERIFY(!dialog); QCOMPARE(calls,1);
    }
    void pendingNumericInputIsRetainedAcrossRejectionAndBlocksSilentDiscard()
    {
        CUvEditDialog dialog(snapshot(),images()); auto published=dialog.Result(); connectPreview(dialog,published); show(dialog);
        auto* canvas=dialog.findChild<CUvCanvasView*>(); canvas->SetSelectedVertices({0});
        dialog.findChild<QDoubleSpinBox*>("uvPointX")->setValue(28.75);
        int saves=0; dialog.SetSubmissionHandler([&](const auto&,QString*) { ++saves; return true; });
        dialog.accept(); QCOMPARE(saves,0); QVERIFY(dialog.isVisible()); QCOMPARE(dialog.findChild<QDoubleSpinBox*>("uvPointX")->value(),28.75);
        canvas->SetSelectedVertices({1}); QCOMPARE(canvas->SelectedVertices(),QSet<int>({0}));
        dialog.findChild<QComboBox*>("uvMesh")->setCurrentIndex(1); QCOMPARE(canvas->Mesh(),QString("Face")); QCOMPARE(dialog.findChild<QComboBox*>("uvMesh")->currentIndex(),0);
        dialog.SetPreviewHandler([](const auto&,QString* error) { *error="try again"; return false; }); dialog.findChild<QPushButton*>("uvSetPoint")->click();
        QCOMPARE(dialog.findChild<QDoubleSpinBox*>("uvPointX")->value(),28.75); dialog.accept(); QCOMPARE(saves,0);
        connectPreview(dialog,published); dialog.findChild<QPushButton*>("uvSetPoint")->click(); dialog.accept(); QCOMPARE(saves,1); QVERIFY(std::abs(published[0].uvs[0]-.2875f)<1e-7f);
    }
    void selectingAnIslandAllowsDraggingItsInteriorAndDiscardClearsOnlyPendingInput()
    {
        CUvEditDialog dialog(snapshot(),images()); auto published=dialog.Result(); const auto original=published; connectPreview(dialog,published); show(dialog);
        auto* canvas=dialog.findChild<CUvCanvasView*>(); canvas->SetSelectedVertices({0,1,2});
        const auto position=canvas->PixelToViewport({25,25}); mouse(canvas,QEvent::MouseButtonPress,position,Qt::LeftButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseMove,position+QPointF(60,0),Qt::NoButton,Qt::LeftButton); mouse(canvas,QEvent::MouseButtonRelease,position+QPointF(60,0),Qt::LeftButton,{});
        for(int vertex=0;vertex<3;++vertex) QVERIFY(dialog.Result()[0].uvs[vertex*2]>original[0].uvs[vertex*2]);
        QCOMPARE(dialog.Result()[0].uvs[6],original[0].uvs[6]); const auto accepted=dialog.Result();
        dialog.findChild<QDoubleSpinBox*>("uvTranslateX")->setValue(5); dialog.findChild<QPushButton*>("uvDiscardInput")->click(); QCOMPARE(dialog.Result(),accepted);
        dialog.findChild<QPushButton*>("uvUndo")->click(); QCOMPARE(dialog.Result(),original);
    }
    void pointEditingPreservesUntouchedAxisAndSavingFlushesPendingText()
    {
        auto source=snapshot(); source.meshes[0].material.uvs[1]=1.2345678e-9f;
        CUvEditDialog dialog(source,images()); auto published=dialog.Result(); const auto original=published; connectPreview(dialog,published); show(dialog);
        dialog.findChild<CUvCanvasView*>()->SetSelectedVertices({0}); auto* x=dialog.findChild<QDoubleSpinBox*>("uvPointX");
        x->setValue(30); dialog.findChild<QPushButton*>("uvSetPoint")->click(); QCOMPARE(dialog.Result()[0].uvs[1],original[0].uvs[1]);
        int saves=0; dialog.SetSubmissionHandler([&](const auto&,QString*) { ++saves; return true; });
        x->findChild<QLineEdit*>()->setText("31.25 px"); dialog.accept(); QCOMPARE(saves,0); QCOMPARE(x->value(),31.25); QVERIFY(dialog.isVisible());
        dialog.findChild<QPushButton*>("uvSetPoint")->click(); dialog.accept(); QCOMPARE(saves,1); QCOMPARE(dialog.Result()[0].uvs[0],.3125f);
    }
    void narrowAndWideLayoutsRetainReachableControls()
    {
        for(bool dark:{false,true}) {
            CUvEditDialog dialog(snapshot(),images()); dialog.setStyleSheet(Theme::globalStyleSheet(dark)); show(dialog);
            for(const auto size:{QSize(620,720),QSize(1100,800)}) {
                dialog.resize(size); QApplication::processEvents();
                const auto expected=size.width()<dialog.fontMetrics().averageCharWidth()*105 ? Qt::Vertical:Qt::Horizontal;
                QCOMPARE(dialog.findChild<QSplitter*>()->orientation(),expected); auto* scroll=dialog.findChild<QScrollArea*>(); QVERIFY(scroll->widgetResizable()); QVERIFY(scroll->viewport()->height()>0); QVERIFY(dialog.findChild<CUvCanvasView*>()->height()>0);
                if(qEnvironmentVariableIsSet("LIVE2D_UV_SCREENSHOTS")) {
                    const QString path=qEnvironmentVariable("LIVE2D_UV_SCREENSHOTS")+QString("/uv-%1-%2.png").arg(dark?"dark":"light").arg(size.width()); QVERIFY(dialog.grab().save(path));
                }
            }
        }
    }
};
QTEST_MAIN(UvEditDialogTests)
#include "uv_edit_dialog_tests.moc"
