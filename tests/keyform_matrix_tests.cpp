#include "ui/ckeyformmatrix.h"
#include "ui/theme.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTableView>
#include <QtEndian>
#include <QtTest>

using namespace live2dstruct;

namespace {
QString source(const QString& model) { return QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3"; }
template<class T> T* widget(CKeyformMatrix& matrix, const char* name) { return matrix.findChild<T*>(QLatin1String(name)); }
QTableView* table(CKeyformMatrix& matrix) { return widget<QTableView>(matrix, "keyformMatrixTable"); }
QComboBox* combo(CKeyformMatrix& matrix, const char* name) { return widget<QComboBox>(matrix, name); }
qint64 keyform(CKeyformMatrix& matrix, int row, int column)
{
    return table(matrix)->model()->index(row, column).data(Qt::UserRole).toLongLong();
}
void click(CKeyformMatrix& matrix, int row, int column)
{
    auto* view = table(matrix);
    const auto index = view->model()->index(row, column);
    view->scrollTo(index);
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->visualRect(index).center());
}
bool visible(CKeyformMatrix& matrix)
{
    matrix.resize(300, 420);
    if (const auto* screen = QGuiApplication::primaryScreen())
        matrix.move(screen->availableGeometry().center() - QPoint(matrix.width() / 2, matrix.height() / 2));
    matrix.show(); matrix.activateWindow();
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(&matrix);
    QT_WARNING_POP
    return QTest::qWaitForWindowExposed(&matrix);
}
}

class KeyformMatrixTests : public QObject
{
    Q_OBJECT
private slots:
    void cleanup() { qApp->setStyleSheet({}); }

    void floatLabelsRoundTripAndSmallMatricesStayCompact()
    {
        qApp->setStyleSheet(Theme::globalStyleSheet(false));
        CModelDocument document;
        QVERIFY(document.Load(source("Natori")));
        CKeyformGridSnapshot grid;
        QVERIFY(document.DescribeKeyformGrid("ArtMesh24", "artmesh", grid));
        CKeyformMatrix matrix;
        matrix.SetGrid(grid, 0, false);
        QVERIFY(visible(matrix));
        auto* view = table(matrix);
        QCOMPARE(view->model()->rowCount(), 2);
        QCOMPARE(view->model()->columnCount(), 4);
        QCOMPARE(view->model()->headerData(2, Qt::Horizontal).toString(), QString("1.2"));
        QCOMPARE(view->model()->headerData(3, Qt::Horizontal).toString(), QString("1.3"));
        for (int column = 0; column < view->model()->columnCount(); ++column) {
            const auto text = view->model()->headerData(column, Qt::Horizontal).toString();
            bool valid = false;
            const float value = text.toFloat(&valid);
            QVERIFY(valid);
            QCOMPARE(value, grid.axes[0].keys[column]);
        }
        QTRY_COMPARE(view->horizontalScrollBar()->maximum(), 0);
        QVERIFY(view->viewport()->height() < view->rowHeight(0) * 4);
        QVERIFY(view->visualRect(view->model()->index(1, 3)).right() < view->viewport()->width());
    }

    void realAxesKeepSourceOrder_data()
    {
        QTest::addColumn<QString>("model"); QTest::addColumn<QString>("id"); QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("x"); QTest::addColumn<QString>("y");
        QTest::newRow("mesh") << "Mark" << "EyeL4" << "artmesh" << "ParamEyeBallX" << "ParamEyeBallY";
        QTest::newRow("warp") << "Mark" << "Warp6" << "warp" << "ParamAngleX" << "ParamAngleY";
        QTest::newRow("reversed-warp") << "Wanko" << "B_EAR_01" << "warp" << "PARAM_ANGLE_Y" << "PARAM_ANGLE_X";
        QTest::newRow("rotation") << "Natori" << "Rotation2" << "rotation" << "ParamAllX" << "ParamAllY";
    }
    void realAxesKeepSourceOrder()
    {
        QFETCH(QString, model); QFETCH(QString, id); QFETCH(QString, type); QFETCH(QString, x); QFETCH(QString, y);
        CModelDocument document; QString error;
        QVERIFY2(document.Load(source(model), &error), qPrintable(error));
        CKeyformGridSnapshot grid; QVERIFY2(document.DescribeKeyformGrid(id, type, grid, &error), qPrintable(error));
        CKeyformMatrix matrix; matrix.SetGrid(grid, 4, false);
        QCOMPARE(combo(matrix, "keyformMatrixXAxis")->currentText(), x);
        QCOMPARE(combo(matrix, "keyformMatrixYAxis")->currentText(), y);
        QCOMPARE(table(matrix)->model()->rowCount(), 3);
        QCOMPARE(table(matrix)->model()->columnCount(), 3);
        QCOMPARE(keyform(matrix, 0, 1), 1); QCOMPARE(keyform(matrix, 1, 0), 3); QCOMPARE(keyform(matrix, 2, 2), 8);
        QMap<QString, float> pose; QVERIFY(grid.Pose(int(keyform(matrix, 0, 1)), pose, &error));
        QCOMPARE(pose[x], grid.axes[0].keys[1]); QCOMPARE(pose[y], grid.axes[1].keys[0]);
        QCOMPARE(matrix.ActiveKeyform(), 4);
        QCOMPARE(table(matrix)->model()->index(1, 1).data().toString(), QStringLiteral("#4"));
        QVERIFY(table(matrix)->model()->index(1, 1).data(Qt::ToolTipRole).toString().contains(QStringLiteral("关键形 #4")));
        QVERIFY(widget<QLabel>(matrix, "keyformMatrixStatus")->text().contains(QStringLiteral("当前形 #4")));
        QVERIFY(!widget<QLabel>(matrix, "keyformMatrixStatus")->text().contains(QStringLiteral("当前预览")));
    }

    void requestDoesNotPublishUntilAccepted()
    {
        CModelDocument document; QVERIFY(document.Load(source("Mark")));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", grid));
        CKeyformMatrix matrix; matrix.SetGrid(grid, 4, false); QVERIFY(visible(matrix));
        QSignalSpy requested(&matrix, &CKeyformMatrix::KeyformRequested);
        click(matrix, 0, 1);
        QCOMPARE(requested.count(), 1); QCOMPARE(requested.last().first().toInt(), 1);
        QCOMPARE(matrix.ActiveKeyform(), 4);
        QCOMPARE(table(matrix)->currentIndex(), table(matrix)->model()->index(0, 1));
        QVERIFY(table(matrix)->model()->index(1, 1).data(Qt::UserRole + 1).toBool());
        QVERIFY(!table(matrix)->currentIndex().data(Qt::UserRole + 1).toBool());
        connect(&matrix, &CKeyformMatrix::KeyformRequested, &matrix, &CKeyformMatrix::SetActiveKeyform);
        click(matrix, 1, 0);
        QCOMPARE(requested.count(), 2); QCOMPARE(matrix.ActiveKeyform(), 3);
        const int before = requested.count();
        matrix.SetActiveKeyform(8); QCOMPARE(requested.count(), before);
        QCOMPARE(matrix.ActiveKeyform(), 8);
    }

    void keyboardCursorIsSeparateFromPreview()
    {
        CModelDocument document; QVERIFY(document.Load(source("Mark")));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("Warp6", "warp", grid));
        CKeyformMatrix matrix; matrix.SetGrid(grid, 4, false); QVERIFY(visible(matrix));
        QSignalSpy requested(&matrix, &CKeyformMatrix::KeyformRequested);
        auto* view = table(matrix); view->setCurrentIndex(view->model()->index(0, 0)); view->setFocus();
        QTest::keyClick(view, Qt::Key_Right); QTest::keyClick(view, Qt::Key_Down);
        QCOMPARE(view->currentIndex(), view->model()->index(1, 1));
        QCOMPARE(requested.count(), 0); QCOMPARE(matrix.ActiveKeyform(), 4);
        QTest::keyClick(view, Qt::Key_Right); QTest::keyClick(view, Qt::Key_Return);
        QCOMPARE(requested.count(), 1); QCOMPARE(requested.first().first().toInt(), 5);
        QCOMPARE(matrix.ActiveKeyform(), 4);
        connect(&matrix, &CKeyformMatrix::KeyformRequested, &matrix, &CKeyformMatrix::SetActiveKeyform);
        QTest::keyClick(view, Qt::Key_Enter);
        QCOMPARE(matrix.ActiveKeyform(), 5);
    }

    void slicesAndAxisSwapsOnlyBrowse()
    {
        CModelDocument document; QVERIFY(document.Load(source("Wanko")));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("D_EFFECT_05", "artmesh", grid));
        CKeyformMatrix matrix; matrix.SetGrid(grid, 0, false); QVERIFY(visible(matrix));
        QSignalSpy requested(&matrix, &CKeyformMatrix::KeyformRequested);
        auto* x = combo(matrix, "keyformMatrixXAxis"); auto* y = combo(matrix, "keyformMatrixYAxis");
        auto* slice = combo(matrix, "keyformMatrixSliceAxis"); auto* key = combo(matrix, "keyformMatrixSliceKey");
        QCOMPARE(slice->currentText(), QStringLiteral("PARAM_EFFECT"));
        key->setCurrentIndex(1);
        QCOMPARE(keyform(matrix, 0, 0), 4); QCOMPARE(keyform(matrix, 1, 1), 7);
        QCOMPARE(matrix.ActiveKeyform(), 0); QCOMPARE(requested.count(), 0);
        QVERIFY(widget<QLabel>(matrix, "keyformMatrixStatus")->text().contains(QStringLiteral("其它切片")));
        x->setCurrentIndex(2);
        QCOMPARE(slice->currentData().toInt(), 0);
        key->setCurrentIndex(1);
        QCOMPARE(keyform(matrix, 0, 0), 1); QCOMPARE(keyform(matrix, 1, 1), 7);
        x->setCurrentIndex(0);
        QCOMPARE(slice->currentData().toInt(), 2); QCOMPARE(key->currentIndex(), 1);
        QCOMPARE(keyform(matrix, 0, 0), 4);
        x->setCurrentIndex(1);
        QCOMPARE(x->currentData().toInt(), 1); QCOMPARE(y->currentData().toInt(), 0);
        QCOMPARE(keyform(matrix, 0, 1), 6); QCOMPARE(keyform(matrix, 1, 0), 5);
        QCOMPARE(requested.count(), 0); QCOMPARE(matrix.ActiveKeyform(), 0);
        QTest::mouseClick(widget<QPushButton>(matrix, "keyformMatrixLocate"), Qt::LeftButton);
        QCOMPARE(key->currentIndex(), 0); QCOMPARE(requested.count(), 0); QCOMPARE(matrix.ActiveKeyform(), 0);
        QCOMPARE(table(matrix)->currentIndex().data(Qt::UserRole).toInt(), 0);
        QVERIFY(!widget<QLabel>(matrix, "keyformMatrixStatus")->text().contains(QStringLiteral("其它切片")));
    }

    void revisionAndActiveUpdatesPreserveBrowsing()
    {
        CModelDocument document; QVERIFY(document.Load(source("Wanko")));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("D_EFFECT_05", "artmesh", grid));
        CKeyformMatrix matrix; matrix.SetGrid(grid, 0, false);
        combo(matrix, "keyformMatrixSliceKey")->setCurrentIndex(1);
        combo(matrix, "keyformMatrixXAxis")->setCurrentIndex(1);
        auto* model = table(matrix)->model(); QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        auto* key = combo(matrix, "keyformMatrixSliceKey");
        ++grid.revision; grid.axes[0].displayName = QStringLiteral("蒸汽一");
        for (auto& axis : grid.axes) axis.bindingIndex += 10;
        matrix.SetGrid(grid, 6, false);
        QCOMPARE(table(matrix)->model(), model); QCOMPARE(resets.count(), 0);
        QCOMPARE(combo(matrix, "keyformMatrixSliceKey"), key); QCOMPARE(key->currentIndex(), 1);
        QCOMPARE(combo(matrix, "keyformMatrixXAxis")->currentData().toInt(), 1);
        QCOMPARE(combo(matrix, "keyformMatrixYAxis")->currentData().toInt(), 0);
        QVERIFY(combo(matrix, "keyformMatrixYAxis")->currentText().contains(QStringLiteral("蒸汽一")));
        QCOMPARE(matrix.ActiveKeyform(), 6);
        grid.sourceDigest = "new source";
        matrix.SetGrid(grid, 0, false);
        QVERIFY(resets.count() > 0); QCOMPARE(key->currentIndex(), 0);
        QCOMPARE(combo(matrix, "keyformMatrixXAxis")->currentData().toInt(), 0);
    }

    void requestReentryAndSnapshotLifetime()
    {
        CKeyformGridSnapshot grid;
        {
            CModelDocument document; QVERIFY(document.Load(source("Mark")));
            QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", grid));
            auto metadata = document.ParameterMetadata("ParamEyeBallX"); metadata.minimum = -.5f; metadata.maximum = .5f;
            QVERIFY(document.SetParameterMetadata("ParamEyeBallX", metadata));
        }
        QMap<QString, float> pose; QVERIFY(grid.Pose(0, pose)); QCOMPARE(pose.value("ParamEyeBallX"), -1.0f);
        CKeyformMatrix matrix; matrix.SetGrid(grid, 4, false); QVERIFY(visible(matrix));
        QSignalSpy requested(&matrix, &CKeyformMatrix::KeyformRequested);
        table(matrix)->setCurrentIndex(table(matrix)->model()->index(0, 0));
        connect(&matrix, &CKeyformMatrix::KeyformRequested, &matrix, [&matrix] {
            QTest::keyClick(table(matrix), Qt::Key_Return);
            matrix.Clear();
        });
        QTest::keyClick(table(matrix), Qt::Key_Return);
        QCOMPARE(requested.count(), 1); QCOMPARE(matrix.ActiveKeyform(), -1);
        QCOMPARE(table(matrix)->model()->rowCount(), 0);
    }

    void zeroAndOneAxis_data()
    {
        QTest::addColumn<QString>("id"); QTest::addColumn<int>("columns");
        QTest::newRow("unbound") << "Hair2" << 1;
        QTest::newRow("one-axis") << "BrowR" << 3;
    }
    void zeroAndOneAxis()
    {
        QFETCH(QString, id); QFETCH(int, columns);
        CModelDocument document; QVERIFY(document.Load(source("Mark")));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid(id, "artmesh", grid));
        CKeyformMatrix matrix; matrix.SetGrid(grid, 0, false); QVERIFY(visible(matrix));
        QCOMPARE(table(matrix)->model()->rowCount(), 1); QCOMPARE(table(matrix)->model()->columnCount(), columns);
        QVERIFY(combo(matrix, "keyformMatrixYAxis")->isHidden());
        QVERIFY(combo(matrix, "keyformMatrixSliceAxis")->isHidden());
        QCOMPARE(combo(matrix, "keyformMatrixXAxis")->isHidden(), columns == 1);
        QSignalSpy requested(&matrix, &CKeyformMatrix::KeyformRequested);
        click(matrix, 0, columns - 1); QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toInt(), columns - 1);
    }

    void rangeGhostAndLockCannotRequest()
    {
        CModelDocument document; QVERIFY(document.Load(source("Mark")));
        auto metadata = document.ParameterMetadata("ParamEyeBallX"); metadata.minimum = -.5f; metadata.maximum = .5f;
        QVERIFY(document.SetParameterMetadata("ParamEyeBallX", metadata));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", grid));
        CKeyformMatrix matrix; matrix.SetGrid(grid, 4, false); QVERIFY(visible(matrix));
        auto* model = table(matrix)->model(); QSignalSpy requested(&matrix, &CKeyformMatrix::KeyformRequested);
        QVERIFY(!(model->flags(model->index(1, 0)) & Qt::ItemIsEnabled));
        QVERIFY(model->index(1, 0).data(Qt::ToolTipRole).toString().contains(QStringLiteral("超出当前范围")));
        QVERIFY(model->flags(model->index(1, 1)) & Qt::ItemIsEnabled);
        table(matrix)->setCurrentIndex(model->index(1, 1));
        click(matrix, 1, 0); QCOMPARE(requested.count(), 0); QCOMPARE(matrix.ActiveKeyform(), 4);
        matrix.SetGrid(grid, 4, true);
        QVERIFY(!(model->flags(model->index(1, 1)) & Qt::ItemIsEnabled));
        QVERIFY(model->index(1, 1).data(Qt::ToolTipRole).toString().contains(QStringLiteral("锁定")));
        click(matrix, 1, 1); QTest::keyClick(table(matrix), Qt::Key_Return);
        QCOMPARE(requested.count(), 0); QCOMPARE(matrix.ActiveKeyform(), 4);
    }

    void missingCornersAndInvalidStructureAreExplicit()
    {
        QFile file(source("Mark")); QVERIFY(file.open(QIODevice::ReadOnly)); auto bytes = file.readAll();
        auto reader = CReader::FromBytes(bytes); CSectionTable original(reader); QVERIFY(original.IsValid());
        int target = -1;
        for (int index = 0; index < original.art_meshes.size(); ++index) if (original.art_meshes[index]->ids == "EyeL4") target = index;
        QVERIFY(target >= 0);
        qToLittleEndian(qint32(8), bytes.data() + original.sectionOffsetTable->art_meshes->keyform_sources_counts + target * 4);
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(bytes, source("Mark"), &error), qPrintable(error));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", grid));
        QCOMPARE(grid.keyformCount, 8); QCOMPARE(grid.expectedKeyformCount, 9);
        QCOMPARE(grid.missingKeyformCount, qint64(1)); QVERIFY(grid.axesValid);
        CKeyformMatrix matrix; matrix.SetGrid(grid, -1, false);
        auto* model = table(matrix)->model();
        QCOMPARE(model->index(2, 2).data().toString(), QStringLiteral("缺失"));
        QVERIFY(model->index(2, 2).data(Qt::ToolTipRole).toString().contains(QStringLiteral("缺失角点")));
        QVERIFY(model->index(2, 2).data(Qt::ToolTipRole).toString().contains(QStringLiteral("1 个")));
        QVERIFY(!(model->flags(model->index(2, 2)) & Qt::ItemIsEnabled));
        QVERIFY(widget<QLabel>(matrix, "keyformMatrixStatus")->text().contains(QStringLiteral("缺失角点：1 个")));
        grid.axesValid = false; grid.diagnostic = QStringLiteral("参数轴引用损坏");
        matrix.SetGrid(grid, -1, false); QCOMPARE(model->rowCount(), 0);
        QVERIFY(widget<QLabel>(matrix, "keyformMatrixStatus")->text().contains(grid.diagnostic));
        matrix.Clear(); QCOMPARE(matrix.ActiveKeyform(), -1); QCOMPARE(model->rowCount(), 0);
        QVERIFY(combo(matrix, "keyformMatrixXAxis")->isHidden());
    }

    void lazyModelAndOverflowAvoidPerCellAllocation()
    {
        CModelDocument document; QVERIFY(document.Load(source("Mark")));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", grid));
        for (auto& axis : grid.axes) { axis.keys.clear(); for (int key = 0; key < 1000; ++key) axis.keys.append(float(key)); }
        grid.expectedKeyformCount = grid.keyformCount = 1000000;
        CKeyformMatrix matrix; matrix.SetGrid(grid, -1, false);
        auto* model = table(matrix)->model(); QCOMPARE(model->rowCount(), 1000); QCOMPARE(model->columnCount(), 1000);
        QCOMPARE(keyform(matrix, 999, 999), 999999);
        QVERIFY(table(matrix)->findChildren<QWidget*>().size() < 30);
        QVERIFY(!table(matrix)->indexWidget(model->index(999, 999)));
        grid.axes.fill(grid.axes.first(), 50); grid.expectedKeyformCount = -1;
        grid.diagnostic = QStringLiteral("关键形组合数量溢出");
        matrix.SetGrid(grid, -1, false);
        QVERIFY(!(model->flags(model->index(0, 0)) & Qt::ItemIsEnabled));
        QVERIFY(model->index(0, 0).data(Qt::ToolTipRole).toString().contains(QStringLiteral("溢出")));
    }

    void modelNamesStayLiteralInTooltips()
    {
        CModelDocument document; QVERIFY(document.Load(source("Mark")));
        auto metadata = document.ParameterMetadata("ParamEyeBallX");
        metadata.displayName = QStringLiteral("<b>眼睛</b> & 左");
        QVERIFY(document.SetParameterMetadata("ParamEyeBallX", metadata));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("EyeL4", "artmesh", grid));
        CKeyformMatrix matrix; matrix.SetGrid(grid, 4, false);
        const auto* model = table(matrix)->model();
        const auto index = model->index(1, 1);
        const auto tip = index.data(Qt::ToolTipRole).toString();
        QVERIFY(tip.startsWith(QStringLiteral("<qt>"))); QVERIFY(tip.contains(QStringLiteral("<br>")));
        QVERIFY(tip.contains(QStringLiteral("&lt;b&gt;眼睛&lt;/b&gt; &amp; 左"))); QVERIFY(!tip.contains(QStringLiteral("<b>")));
        QVERIFY(index.data(Qt::AccessibleDescriptionRole).toString().contains(metadata.displayName));
        QVERIFY(model->headerData(1, Qt::Horizontal, Qt::ToolTipRole).toString().contains(metadata.displayName.toHtmlEscaped()));
        auto* axis = combo(matrix, "keyformMatrixXAxis");
        QVERIFY(axis->currentText().contains(metadata.displayName));
        QVERIFY(axis->toolTip().contains(metadata.displayName.toHtmlEscaped()));
        QVERIFY(axis->itemData(0, Qt::ToolTipRole).toString().contains(metadata.displayName.toHtmlEscaped()));
    }

    void narrowThemeAndKeyboard_data()
    {
        QTest::addColumn<QString>("styleName"); QTest::addColumn<bool>("dark");
        for (const auto& style : QStyleFactory::keys()) if (style == "Fusion"
            || (QGuiApplication::platformName() == "cocoa" && style.compare("macOS", Qt::CaseInsensitive) == 0))
            for (bool dark : {false, true}) QTest::newRow(qPrintable(style + (dark ? "-dark" : "-light"))) << style << dark;
    }
    void narrowThemeAndKeyboard()
    {
        QFETCH(QString, styleName); QFETCH(bool, dark);
        auto* style = QStyleFactory::create(styleName); QVERIFY(style); qApp->setStyle(style); qApp->setPalette(style->standardPalette());
        qApp->setStyleSheet(Theme::globalStyleSheet(dark));
        CModelDocument document; QVERIFY(document.Load(source("Wanko")));
        CKeyformGridSnapshot grid; QVERIFY(document.DescribeKeyformGrid("D_EFFECT_05", "artmesh", grid));
        grid.axes[0].displayName = QStringLiteral("很长的中文蒸汽动作控制参数名称，用于完整下拉与窄栏");
        CKeyformMatrix matrix; matrix.SetGrid(grid, 0, false); QVERIFY(visible(matrix));
        QVERIFY(matrix.minimumSizeHint().width() <= 300); QCOMPARE(matrix.width(), 300);
        for (const auto* name : {"keyformMatrixXAxis", "keyformMatrixYAxis", "keyformMatrixSliceAxis", "keyformMatrixSliceKey"}) {
            auto* control = combo(matrix, name);
            QVERIFY(control->isVisible()); QVERIFY(control->geometry().left() >= 0); QVERIFY(control->geometry().right() < matrix.width());
        }
        QVERIFY(combo(matrix, "keyformMatrixXAxis")->itemData(0, Qt::ToolTipRole).toString().contains(grid.axes[0].displayName));
        auto* view = table(matrix); view->setCurrentIndex(view->model()->index(0, 0)); view->setFocus();
        QSignalSpy requested(&matrix, &CKeyformMatrix::KeyformRequested);
        QTest::keyClick(view, Qt::Key_Right); QCOMPARE(requested.count(), 0);
        QTest::keyClick(view, Qt::Key_Return); QCOMPARE(requested.count(), 1); QCOMPARE(requested.first().first().toInt(), 1);
        const QImage pixels = view->viewport()->grab().toImage();
        const auto highlight = view->palette().color(QPalette::Active, QPalette::Highlight);
        const auto highlightedFraction = [&](const QModelIndex& index) {
            const QRect logical = view->visualRect(index);
            const qreal scale = pixels.devicePixelRatio();
            const QRect region(qRound(logical.x() * scale), qRound(logical.y() * scale),
                               qRound(logical.width() * scale), qRound(logical.height() * scale));
            const auto clipped = region.intersected(pixels.rect());
            int matched = 0;
            for (int y = clipped.top(); y <= clipped.bottom(); ++y)
                for (int x = clipped.left(); x <= clipped.right(); ++x) {
                    const auto actual = pixels.pixelColor(x, y);
                    matched += qAbs(actual.red() - highlight.red()) < 8 && qAbs(actual.green() - highlight.green()) < 8
                        && qAbs(actual.blue() - highlight.blue()) < 8;
                }
            return clipped.isEmpty() ? 0.0 : double(matched) / (clipped.width() * clipped.height());
        };
        QVERIFY(highlightedFraction(view->model()->index(0, 0)) > .5);
        QVERIFY(highlightedFraction(view->model()->index(0, 1)) < .25);
        const QString directory = qEnvironmentVariable("KEYFORM_MATRIX_SCREENSHOTS");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            QVERIFY(matrix.grab().save(directory + '/' + styleName + (dark ? "-dark.png" : "-light.png")));
        }
    }
};

QTEST_MAIN(KeyformMatrixTests)
#include "keyform_matrix_tests.moc"
