#include "ui/cblendshapepanel.h"
#include "ui/theme.h"
#include "ui/para/ccidjson.h"
#include "live2dstruct/cmodeldocument.h"
#include "blend_shape_v5_fixture.h"

#include <QtTest>
#include <QFile>
#include <QAccessible>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QPointer>
#include <QSettings>
#include <QSplitter>
#include <QStyleFactory>
#include <QStyleOption>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace {
bool save(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

struct Fixture
{
    QTemporaryDir directory;
    CCidJson cid;
    QString id, path;

    bool create(float minimum, float maximum, float defaultValue)
    {
        QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        if (!directory.isValid() || !file.open(QIODevice::ReadOnly)) return false;
        QByteArray bytes = file.readAll();
        const auto word = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
        id = QString::fromUtf8(bytes.mid(word(0x108), 64).split('\0').first());
        for (const auto& field : {std::pair{0x110, minimum}, {0x10c, maximum}, {0x114, defaultValue}}) {
            quint32 bits;
            std::memcpy(&bits, &field.second, sizeof(bits));
            qToLittleEndian(bits, bytes.data() + word(field.first));
        }
        path = directory.filePath("range.moc3");
        const QString cidPath = directory.filePath("range.cdi3.json");
        const QJsonDocument json(QJsonObject{{"CombinedParameters", QJsonArray{QJsonArray{id}}}});
        return save(path, bytes) && save(cidPath, json.toJson()) && cid.Parse(cidPath);
    }
};

QWidget* row(CBlendShapePanel& panel, const QString& id)
{
    auto* tree = panel.findChild<QTreeWidget*>("blendParameterTree");
    if (!tree) return nullptr;
    for (QTreeWidgetItemIterator item(tree); *item; ++item)
        if ((*item)->data(0, Qt::UserRole).toString() == id) return tree->itemWidget(*item, 1);
    return nullptr;
}

int word(const QByteArray& bytes, int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); }
void put(QByteArray& bytes, int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); }
void cell(QByteArray& bytes, int slot, int row, int value) { put(bytes, word(bytes, slot) + row * 4, value); }
int bits(float value) { int result; std::memcpy(&result, &value, 4); return result; }
void array(QByteArray& bytes, int slot, const QVector<int>& values)
{
    const int offset = bytes.size();
    bytes.append(QByteArray(values.size() * 4, '\0'));
    for (int index = 0; index < values.size(); ++index) put(bytes, offset + index * 4, values[index]);
    put(bytes, slot, offset);
}
void name(QByteArray& bytes, int slot, int row, const QByteArray& id)
{
    bytes.replace(word(bytes, slot) + row * 64, 64, id.leftJustified(64, '\0'));
}

QByteArray blendBytes()
{
    QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto bytes = file.readAll();
    const int counts = word(bytes, 0x40);
    for (int index = 0; index < 32; ++index) put(bytes, counts + index * 4, 0);
    for (const auto& count : {std::pair{1, 3}, {2, 1}, {3, 1}, {4, 1}, {5, 3}, {7, 1}, {8, 1}, {9, 1},
                             {14, 2}, {25, 1}, {26, 1}, {27, 1}, {28, 1}, {29, 1}, {30, 1}, {31, 2}})
        put(bytes, counts + count.first * 4, count.second);
    const QList<QByteArray> parameters{"Driver", "Limiter", "Unbound"};
    for (int index = 0; index < parameters.size(); ++index) {
        name(bytes, 0x108, index, parameters[index]);
        cell(bytes, 0x10c, index, bits(1));
        cell(bytes, 0x110, index, bits(-1));
        cell(bytes, 0x114, index, bits(0));
        cell(bytes, 0x120, index, 0);
        cell(bytes, 0x124, index, 0);
    }
    array(bytes, 0x208, {1, 0, 1});
    array(bytes, 0x20c, {0, 0, -1});
    array(bytes, 0x210, {1, 0, 0});
    array(bytes, 0x174, {bits(-1), bits(1)});
    array(bytes, 0x214, {0});
    array(bytes, 0x218, {2});
    array(bytes, 0x21c, {0});
    array(bytes, 0x220, {0});
    array(bytes, 0x224, {0});
    array(bytes, 0x228, {1});
    array(bytes, 0x22c, {0});
    array(bytes, 0x230, {1});
    for (int slot : {0x234, 0x238, 0x240, 0x244, 0x24c, 0x254}) array(bytes, slot, {0});
    for (int slot : {0x23c, 0x248, 0x250}) array(bytes, slot, {1});
    array(bytes, 0x258, {2});
    array(bytes, 0x25c, {bits(-1), bits(1)});
    array(bytes, 0x260, {bits(0.2f), bits(0.8f)});
    const QList<QByteArray> targets{"Shared", "Rotation", "OtherOwner"};
    for (int index = 0; index < targets.size(); ++index) {
        name(bytes, 0x6c, index, targets[index]);
        cell(bytes, 0x84, index, index == 1 ? 1 : 0);
        cell(bytes, 0x88, index, 0);
    }
    name(bytes, 0xc4, 0, "Shared");
    return bytes;
}

QTreeWidgetItem* findItem(QTreeWidget* tree, const QString& kind, const QString& id = {}, const QString& type = {})
{
    for (QTreeWidgetItemIterator item(tree); *item; ++item)
        if ((*item)->data(0, Qt::UserRole + 1).toString() == kind &&
            (id.isEmpty() || (*item)->data(0, Qt::UserRole).toString() == id) &&
            (type.isEmpty() || (*item)->data(0, Qt::UserRole + 2).toString() == type)) return *item;
    return nullptr;
}

QRect disclosureRect(QTreeWidget* tree, QTreeWidgetItem* item)
{
    const auto rect = tree->visualItemRect(item);
    QStyleOption option;
    option.initFrom(tree);
    option.rect = QRect(tree->isRightToLeft() ? rect.right() + 1 : rect.left() - tree->indentation(),
                        rect.top(), tree->indentation(), rect.height());
    return tree->style()->subElementRect(QStyle::SE_TreeViewDisclosureItem, &option, tree);
}

double visibleContrast(const QImage& image)
{
    if (image.isNull()) return 1.0;
    const auto luminance = [](QColor color) {
        const auto channel = [](double value) { return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4); };
        return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
    };
    const double background = luminance(image.pixelColor(0, 0));
    double result = 1.0;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const double foreground = luminance(image.pixelColor(x, y));
        result = std::max(result, (std::max(background, foreground) + 0.05) / (std::min(background, foreground) + 0.05));
    }
    return result;
}

class LayoutEventCounter : public QObject
{
public:
    int count = 0;
protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest) ++count;
        return false;
    }
};

double splitterRatio(QSplitter* splitter)
{
    const auto sizes = splitter->sizes();
    const double total = double(sizes.value(0)) + sizes.value(1);
    return sizes.size() == 2 && total > 0 ? sizes[0] / total : -1;
}

void dragSplitter(QSplitter* splitter, int delta)
{
    auto* handle = splitter->handle(1);
    const QPoint start = handle->rect().center();
    const QPoint end = handle->mapToGlobal(start) + QPoint(0, delta);
    QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
    const QPoint local = handle->mapFromGlobal(end);
    QMouseEvent move(QEvent::MouseMove, local, handle->window()->mapFromGlobal(end), end,
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, handle->mapFromGlobal(end));
}

QStringList treeText(QTreeWidget* tree)
{
    QStringList result;
    for (QTreeWidgetItemIterator item(tree); *item; ++item) result.append((*item)->text(0) + '|' + (*item)->text(1));
    return result;
}
}

class BlendShapePanelTests : public QObject
{
    Q_OBJECT

    static void labelMatches(QLabel* label, float value)
    {
        QVERIFY(label);
        QCOMPARE(label->text(), QString::number(value, 'g', 9));
        bool converted = false;
        const double number = label->text().toDouble(&converted);
        QVERIFY(converted && std::isfinite(number));
        const double tolerance = std::max(std::abs(double(value)) * 5e-9,
                                          double(std::numeric_limits<float>::denorm_min()) * 0.5);
        QVERIFY(std::abs(number - double(value)) <= tolerance);
        QVERIFY(label->maximumWidth() >= label->fontMetrics().horizontalAdvance(label->text()));
    }

private slots:
    void disclosureUsesThemeColorsAndNativeInteraction_data()
    {
        QTest::addColumn<QString>("styleName");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("rtl");
        QStringList styles{QStringLiteral("Fusion"), QApplication::style()->objectName()};
        styles.removeDuplicates();
        for (const auto& style : styles) for (bool dark : {false, true}) for (bool rtl : {false, true}) {
            const auto name = QStringLiteral("%1-%2-%3").arg(style, dark ? "dark" : "light", rtl ? "rtl" : "ltr").toLatin1();
            QTest::newRow(name.constData()) << style << dark << rtl;
        }
    }

    void disclosureUsesThemeColorsAndNativeInteraction()
    {
        QFETCH(QString, styleName);
        QFETCH(bool, dark);
        QFETCH(bool, rtl);
        std::unique_ptr<QStyle> style(QStyleFactory::create(styleName));
        QVERIFY(style);
        CBlendShapePanel panel;
        panel.setStyle(style.get());
        panel.setStyleSheet(Theme::globalStyleSheet(dark));
        panel.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
        QVERIFY(panel.BuildBlendShapesBytes(blendBytes(), nullptr));
        panel.resize(640, 760);
        panel.show();
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        QVERIFY(parameters && details);
        for (auto* tree : {parameters, details}) tree->setStyle(style.get());
        QCoreApplication::processEvents();
        auto* group = parameters->topLevelItem(0);
        auto* target = findItem(details, "target", "Shared", "warp");
        QVERIFY(group && target);
        QSignalSpy navigation(&panel, &CBlendShapePanel::ObjectSelected);
        QSignalSpy values(&panel, &CBlendShapePanel::ParamChanged);
        for (const auto& entry : {qMakePair(parameters, group), qMakePair(details, target)}) {
            auto* tree = entry.first;
            auto* item = entry.second;
            tree->scrollToItem(item);
            QCoreApplication::processEvents();
            const bool expanded = item->isExpanded();
            const auto before = tree->viewport()->grab(disclosureRect(tree, item)).toImage();
            QVERIFY2(visibleContrast(before) >= 3.0, "Disclosure must contrast with the actual themed background");
            QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, disclosureRect(tree, item).center());
            QCOMPARE(item->isExpanded(), !expanded);
            QCoreApplication::processEvents();
            const auto after = tree->viewport()->grab(disclosureRect(tree, item)).toImage();
            QVERIFY(visibleContrast(after) >= 3.0);
            QVERIFY(after != before);
            tree->setCurrentItem(item);
            tree->setFocus();
            QTest::keyClick(tree, Qt::Key_Minus);
            QVERIFY(!item->isExpanded());
            QTest::keyClick(tree, Qt::Key_Plus);
            QVERIFY(item->isExpanded());
            auto* accessible = QAccessible::queryAccessibleInterface(tree);
            QVERIFY(accessible && accessible->role() == QAccessible::Tree);
            QVERIFY(accessible->focusChild());
            QVERIFY(accessible->focusChild()->state().expandable);
            QVERIFY(accessible->focusChild()->state().expanded);
        }
        QCOMPARE(target->childCount(), 1);
        QVERIFY(navigation.isEmpty());
        QVERIFY(values.isEmpty());
    }

    void automaticLayoutFitsShortParameterContentAndLeavesRoomForRelations()
    {
        CBlendShapePanel panel;
        panel.setStyleSheet(Theme::globalStyleSheet(false));
        QVERIFY(panel.BuildBlendShapesBytes(blendBytes(), nullptr));
        panel.resize(560, 740);
        panel.show();
        auto* splitter = panel.findChild<QSplitter*>("blendRelationSplitter");
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        QVERIFY(splitter && parameters);
        QTRY_VERIFY(splitter->sizes()[0] < splitter->sizes()[1]);
        auto* last = findItem(parameters, "parameter", "Unbound");
        QVERIFY(last);
        QTRY_VERIFY(parameters->viewport()->rect().contains(parameters->visualItemRect(last)));
        QTRY_COMPARE(parameters->viewport()->height(), parameters->visualItemRect(last).bottom() + 1);
        const auto initial = splitter->sizes();
        const auto state = panel.SaveLayout();
        QCOMPARE(QJsonDocument::fromJson(state).object().value("mode").toString(), QString("auto"));
        panel.resize(560, 940);
        QTRY_COMPARE(splitter->sizes()[0], initial[0]);
        QVERIFY(splitter->sizes()[1] > initial[1]);
        QCOMPARE(panel.SaveLayout(), state);
        panel.findChild<QLineEdit*>("blendParameterSearch")->setText("Unbound");
        QTRY_VERIFY(splitter->sizes()[0] < initial[0]);
        panel.findChild<QLineEdit*>("blendParameterSearch")->clear();
        QTRY_COMPARE(splitter->sizes()[0], initial[0]);
        QCOMPARE(panel.SaveLayout(), state);
    }

    void userSplitterRatioSurvivesReloadResizeAndSettingsRoundTrip()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto bytes = blendBytes();
        CBlendShapePanel panel;
        QVERIFY(panel.BuildBlendShapesBytes(bytes, nullptr));
        panel.resize(560, 740);
        panel.show();
        auto* splitter = panel.findChild<QSplitter*>("blendRelationSplitter");
        QVERIFY(splitter);
        QTRY_VERIFY(splitter->sizes()[0] < splitter->sizes()[1]);
        QSignalSpy moved(splitter, &QSplitter::splitterMoved);
        QSignalSpy values(&panel, &CBlendShapePanel::ParamChanged);
        dragSplitter(splitter, splitter->height() / 5);
        QVERIFY(!moved.isEmpty());
        const auto layout = panel.SaveLayout();
        const auto object = QJsonDocument::fromJson(layout).object();
        QCOMPARE(object.value("version").toInt(), 1);
        QCOMPARE(object.value("mode").toString(), QString("manual"));
        const double ratio = object.value("ratio").toDouble();
        QVERIFY(ratio > 0 && ratio < 1);
        QVERIFY(std::abs(splitterRatio(splitter) - ratio) < 0.005);
        const QPointer<QSplitter> previous = splitter;
        QVERIFY(panel.BuildBlendShapesBytes(bytes, nullptr));
        QVERIFY(previous.isNull());
        splitter = panel.findChild<QSplitter*>("blendRelationSplitter");
        QTRY_VERIFY(std::abs(splitterRatio(splitter) - ratio) < 0.005);
        QCOMPARE(panel.SaveLayout(), layout);
        const auto sizes = splitter->sizes();
        QVERIFY(!panel.BuildBlendShapesBytes("invalid", nullptr));
        QCOMPARE(panel.findChild<QSplitter*>("blendRelationSplitter"), splitter);
        QCOMPARE(splitter->sizes(), sizes);
        QCOMPARE(panel.SaveLayout(), layout);
        panel.resize(560, 400);
        QCoreApplication::processEvents();
        QCOMPARE(panel.SaveLayout(), layout);
        panel.resize(560, 960);
        QTRY_VERIFY(std::abs(splitterRatio(splitter) - ratio) < 0.005);
        panel.hide();
        panel.show();
        QTRY_VERIFY(std::abs(splitterRatio(splitter) - ratio) < 0.005);
        QCOMPARE(panel.SaveLayout(), layout);
        QVERIFY(values.isEmpty());
        const QString path = directory.filePath("layout.ini");
        { QSettings settings(path, QSettings::IniFormat); settings.setValue("blendShapePanelLayout", layout); }
        CBlendShapePanel reopened;
        QSettings settings(path, QSettings::IniFormat);
        QVERIFY(reopened.RestoreLayout(settings.value("blendShapePanelLayout").toByteArray()));
        QCOMPARE(reopened.SaveLayout(), layout);
        QVERIFY(reopened.BuildBlendShapesBytes(bytes, nullptr));
        reopened.resize(620, 800);
        reopened.show();
        auto* restored = reopened.findChild<QSplitter*>("blendRelationSplitter");
        QVERIFY(restored);
        QTRY_VERIFY(std::abs(splitterRatio(restored) - ratio) < 0.005);
        QCOMPARE(reopened.SaveLayout(), layout);
    }

    void constrainedManualLayoutRetainsRatioAndSettles_data()
    {
        QTest::addColumn<double>("ratio");
        QTest::addColumn<bool>("rtl");
        QTest::newRow("near-zero-ltr") << 1e-12 << false;
        QTest::newRow("near-one-rtl") << (1.0 - 1e-12) << true;
        QTest::newRow("middle-rtl") << 0.4 << true;
    }

    void constrainedManualLayoutRetainsRatioAndSettles()
    {
        QFETCH(double, ratio);
        QFETCH(bool, rtl);
        CBlendShapePanel panel;
        panel.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
        QVERIFY(panel.BuildBlendShapesBytes(blendBytes(), nullptr));
        QVERIFY(panel.RestoreLayout(QJsonDocument(QJsonObject{{"version", 1}, {"mode", "manual"}, {"ratio", ratio}}).toJson()));
        const auto saved = panel.SaveLayout();
        panel.resize(480, 180);
        panel.show();
        auto* splitter = panel.findChild<QSplitter*>("blendRelationSplitter");
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        QVERIFY(splitter && parameters && details);
        QTRY_VERIFY(splitter->sizes()[0] > 0 && splitter->sizes()[1] > 0);
        QVERIFY(parameters->viewport()->height() > 0 && details->viewport()->height() > 0);
        QCOMPARE(panel.SaveLayout(), saved);
        auto font = panel.font();
        font.setPixelSize(28);
        panel.setFont(font);
        panel.resize(560, 900);
        QCoreApplication::processEvents();
        QCOMPARE(panel.SaveLayout(), saved);
        for (bool dark : {false, true}) {
            panel.setStyleSheet(Theme::globalStyleSheet(dark));
            QCoreApplication::processEvents();
            QVERIFY(splitter->sizes()[0] > 0 && splitter->sizes()[1] > 0);
            QCOMPARE(panel.SaveLayout(), saved);
        }
        LayoutEventCounter counter;
        for (auto* widget : {static_cast<QWidget*>(splitter), static_cast<QWidget*>(parameters), static_cast<QWidget*>(details)})
            widget->installEventFilter(&counter);
        for (int pass = 0; pass < 8; ++pass) QCoreApplication::processEvents();
        const int settled = counter.count;
        const auto sizes = splitter->sizes();
        for (int pass = 0; pass < 8; ++pass) QCoreApplication::processEvents();
        QCOMPARE(counter.count, settled);
        QCOMPARE(splitter->sizes(), sizes);
        QCOMPARE(panel.SaveLayout(), saved);
    }

    void automaticLayoutRecomputesForLargerFonts()
    {
        CBlendShapePanel panel;
        QVERIFY(panel.BuildBlendShapesBytes(blendBytes(), nullptr));
        panel.resize(600, 900);
        panel.show();
        auto* splitter = panel.findChild<QSplitter*>("blendRelationSplitter");
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        QTRY_VERIFY(splitter->sizes()[0] < splitter->sizes()[1]);
        const int before = splitter->sizes()[0];
        auto font = panel.font();
        font.setPixelSize(28);
        panel.setFont(font);
        QTRY_VERIFY(splitter->sizes()[0] > before);
        auto* last = findItem(parameters, "parameter", "Unbound");
        QVERIFY(last);
        QTRY_VERIFY(parameters->viewport()->rect().contains(parameters->visualItemRect(last)));
        QCOMPARE(QJsonDocument::fromJson(panel.SaveLayout()).object().value("mode").toString(), QString("auto"));
    }

    void malformedLayoutDoesNotChangeStateOrWidgets()
    {
        CBlendShapePanel panel;
        QVERIFY(panel.BuildBlendShapesBytes(blendBytes(), nullptr));
        const QByteArray good = R"({"version":1,"mode":"manual","ratio":0.35})";
        QVERIFY(panel.RestoreLayout(good));
        panel.resize(560, 740);
        panel.show();
        auto* splitter = panel.findChild<QSplitter*>("blendRelationSplitter");
        QTRY_VERIFY(std::abs(splitterRatio(splitter) - 0.35) < 0.005);
        const auto state = panel.SaveLayout();
        const auto sizes = splitter->sizes();
        const QList<QByteArray> invalid{
            {}, "bad", "[]", "{}", R"({"version":2,"mode":"auto"})",
            R"({"version":1.5,"mode":"auto"})", R"({"version":1,"mode":"unknown"})",
            R"({"version":1,"mode":"manual"})", R"({"version":1,"mode":"manual","ratio":null})",
            R"({"version":1,"mode":"manual","ratio":"0.5"})", R"({"version":1,"mode":"manual","ratio":0})",
            R"({"version":1,"mode":"manual","ratio":1})", R"({"version":1,"mode":"manual","ratio":-0.1})",
            R"({"version":1,"mode":"manual","ratio":1e999})", R"({"version":1,"mode":"manual","ratio":NaN})",
            R"({"version":1,"mode":"auto","ratio":0.3})", R"({"version":1,"mode":"auto","extra":1})"};
        for (const auto& bytes : invalid) {
            QVERIFY2(!panel.RestoreLayout(bytes), bytes.constData());
            QCOMPARE(panel.SaveLayout(), state);
            QCOMPARE(panel.findChild<QSplitter*>("blendRelationSplitter"), splitter);
            QCOMPARE(splitter->sizes(), sizes);
        }
        QVERIFY(panel.RestoreLayout(R"({"version":1,"mode":"auto"})"));
        QTRY_VERIFY(splitter->sizes()[0] < splitter->sizes()[1]);
        QCOMPARE(QJsonDocument::fromJson(panel.SaveLayout()).object().value("mode").toString(), QString("auto"));
    }

    void finiteRangesSupportDefaultSliderAndSilentSync_data()
    {
        QTest::addColumn<float>("minimum");
        QTest::addColumn<float>("maximum");
        QTest::addColumn<float>("defaultValue");
        QTest::addColumn<int>("defaultPosition");
        const float maximum = std::numeric_limits<float>::max();
        const float h = std::ldexp(1.0f, 127);
        const float tiny = std::numeric_limits<float>::denorm_min();
        QTest::newRow("full-float-range") << -maximum << maximum << 0.0f << 500;
        QTest::newRow("positive-large") << h << 1.5f * h << 1.25f * h << 500;
        QTest::newRow("negative-large") << -1.5f * h << -h << -1.25f * h << 500;
        QTest::newRow("positive-subnormal") << 0.0f << 16 * tiny << 4 * tiny << 250;
        QTest::newRow("signed-subnormal") << -16 * tiny << 16 * tiny << 0.0f << 500;
        QTest::newRow("fixed-large") << h << h << h << 0;
        QTest::newRow("fixed-subnormal") << tiny << tiny << tiny << 0;
        QTest::newRow("negative-zero-boundary") << 0.0f << 1.0f << -0.0f << 0;
    }

    void finiteRangesSupportDefaultSliderAndSilentSync()
    {
        QFETCH(float, minimum);
        QFETCH(float, maximum);
        QFETCH(float, defaultValue);
        QFETCH(int, defaultPosition);
        Fixture fixture;
        QVERIFY(fixture.create(minimum, maximum, defaultValue));
        CBlendShapePanel panel;
        QSignalSpy changes(&panel, &CBlendShapePanel::ParamChanged);
        panel.BuildBlendShapes(fixture.path, &fixture.cid);
        QVERIFY(!panel.getBlendShapeParamIds().contains(fixture.id));
        auto* widget = row(panel, fixture.id);
        QVERIFY(widget);
        auto* slider = widget->findChild<QSlider*>();
        auto* label = widget->findChild<QLabel*>();
        QVERIFY(slider && label);
        QCOMPARE(slider->value(), defaultPosition);
        QCOMPARE(slider->isEnabled(), minimum != maximum);
        labelMatches(label, defaultValue);
        QCOMPARE(changes.size(), 0);

        for (int position : {0, 250, 500, 750, 1000}) {
            const float expected = float(static_cast<long double>(minimum) +
                (static_cast<long double>(maximum) - minimum) * position / 1000);
            if (minimum != maximum) {
                const int before = slider->value();
                slider->setValue(position);
                QCOMPARE(changes.size(), before == position ? 0 : 1);
                if (!changes.isEmpty()) {
                    const auto event = changes.takeFirst();
                    QCOMPARE(event[0].toString(), fixture.id);
                    QCOMPARE(event[1].toFloat(), expected);
                    QVERIFY(std::isfinite(event[1].toFloat()));
                    QVERIFY(!event[2].toBool());
                }
                labelMatches(label, expected);
            }
            panel.SyncParameterValue(fixture.id, expected);
            QCOMPARE(changes.size(), 0);
            labelMatches(label, expected);
            QVERIFY(slider->value() >= 0 && slider->value() <= 1000);
        }
        panel.SyncParameterValue(fixture.id, std::numeric_limits<float>::max());
        labelMatches(label, maximum);
        QCOMPARE(slider->value(), minimum == maximum ? 0 : 1000);
        panel.SyncParameterValue(fixture.id, std::numeric_limits<float>::lowest());
        labelMatches(label, minimum);
        QCOMPARE(slider->value(), 0);
        panel.SyncParameterValue(fixture.id, defaultValue);
        QCOMPARE(slider->value(), defaultPosition);
        labelMatches(label, defaultValue);
        const auto text = label->text();
        const int previous = slider->value();
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()}) {
            panel.SyncParameterValue(fixture.id, invalid);
            QCOMPARE(slider->value(), previous);
            QCOMPARE(label->text(), text);
        }
        panel.SyncParameterValue("not-a-parameter", 1.0f);
        QCOMPARE(slider->value(), previous);
        QCOMPARE(label->text(), text);
        QCOMPARE(changes.size(), 0);
    }

    void driverRolesAndCombinedHintsStaySeparate()
    {
        const auto bytes = blendBytes();
        QVERIFY(!bytes.isEmpty());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        CCidJson cid;
        const QString path = directory.filePath("names.cdi3.json");
        const QJsonDocument document(QJsonObject{
            {"CombinedParameters", QJsonArray{QJsonArray{"Driver", "Limiter", "Ghost", "Driver"}}},
            {"Parameters", QJsonArray{QJsonObject{{"Id", "Driver"}, {"Name", "驱动名称"}}}}
        });
        QVERIFY(save(path, document.toJson()));
        QVERIFY(cid.Parse(path));
        CBlendShapePanel panel;
        QSignalSpy changed(&panel, &CBlendShapePanel::ParamChanged);
        QString error;
        QVERIFY2(panel.BuildBlendShapesBytes(bytes, &cid, &error), qPrintable(error));
        QCOMPARE(panel.getBlendShapeParamIds(), (QSet<QString>{"Driver", "Unbound"}));
        QVERIFY(row(panel, "Driver"));
        QVERIFY(row(panel, "Limiter"));
        QVERIFY(row(panel, "Unbound"));
        QVERIFY(!row(panel, "Ghost"));
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        QCOMPARE(parameters->topLevelItemCount(), 2);
        QVERIFY(parameters->topLevelItem(0)->text(0).startsWith(QStringLiteral("融合参数")));
        QVERIFY(parameters->topLevelItem(1)->text(0).startsWith(QStringLiteral("组合参数")));
        QCOMPARE(parameters->topLevelItem(0)->childCount(), 2);
        QCOMPARE(parameters->topLevelItem(1)->childCount(), 2);
        auto* driver = findItem(parameters, "parameter", "Driver");
        QCOMPARE(driver->text(0), QStringLiteral("驱动名称"));
        auto* search = panel.findChild<QLineEdit*>("blendParameterSearch");
        search->setText(QStringLiteral("驱动名称"));
        QVERIFY(!driver->isHidden());
        QVERIFY(findItem(parameters, "parameter", "Unbound")->isHidden());
        search->setText(QStringLiteral("unbound"));
        QVERIFY(!findItem(parameters, "parameter", "Unbound")->isHidden());
        search->clear();
        auto* slider = row(panel, "Driver")->findChild<QSlider*>();
        slider->setValue(750);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.first()[1].toFloat(), 0.5f);
        for (QTreeWidgetItemIterator item(parameters); *item; ++item) {
            if ((*item)->data(0, Qt::UserRole).toString() != "Driver") continue;
            auto* control = parameters->itemWidget(*item, 1);
            QVERIFY(control);
            QCOMPARE(control->findChild<QSlider*>()->value(), 750);
        }
    }

    void numericMetadataPreservesCdiNamesAcrossHistoryAndLazyDetails()
    {
        const auto bytes = blendBytes();
        live2dstruct::CModelDocument document;
        QVERIFY(document.LoadBytes(bytes, "blend-names.moc3"));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("names.cdi3.json");
        const QJsonDocument names(QJsonObject{{"CombinedParameters", QJsonArray{QJsonArray{"Driver", "Limiter"}}}, {"Parameters", QJsonArray{
            QJsonObject{{"Id", "Driver"}, {"Name", "驱动名称"}},
            QJsonObject{{"Id", "Limiter"}, {"Name", "限制名称"}}}}});
        QVERIFY(save(path, names.toJson()));
        CCidJson cid;
        QVERIFY(cid.Parse(path));
        CBlendShapePanel panel;
        panel.SetDocument(&document);
        QVERIFY(panel.BuildBlendShapesBytes(bytes, &cid));
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        auto* driver = findItem(parameters, "parameter", "Driver");
        QVERIFY(driver);
        QCOMPARE(driver->text(0), QStringLiteral("驱动名称"));
        parameters->setCurrentItem(driver);
        auto* target = findItem(details, "target", "Shared", "artmesh");
        QVERIFY(target);
        target->setExpanded(true);
        auto* binding = findItem(details, "binding", "Driver");
        QVERIFY(binding);
        QVERIFY(binding->text(0).contains(QStringLiteral("驱动名称")));
        binding->setExpanded(true);
        auto* limiter = findItem(details, "limiter", "Limiter");
        QVERIFY(limiter);
        QVERIFY(limiter->text(0).contains(QStringLiteral("限制名称")));
        auto* controls = row(panel, "Driver");
        QVERIFY(controls);
        auto* slider = controls->findChild<QSlider*>();
        QVERIFY(slider);
        QSignalSpy changed(&panel, &CBlendShapePanel::ParamChanged);
        auto metadata = document.ParameterMetadata("Driver");
        metadata.defaultValue = .5f;
        live2dstruct::CModelEdit edit;
        QVERIFY(document.MakeParameterMetadataEdit("Driver", metadata, edit));
        QVERIFY(document.ExecuteEdit(edit));
        panel.RefreshParameterMetadata();
        QCOMPARE(driver->text(0), QStringLiteral("驱动名称"));
        QCOMPARE(slider->accessibleName(), QStringLiteral("驱动名称"));
        QVERIFY(binding->text(0).contains(QStringLiteral("驱动名称")));
        QCOMPARE(document.ParameterMetadata("Driver").displayName, QStringLiteral("Driver"));
        QVERIFY(document.UndoEdit());
        panel.RefreshParameterMetadata();
        QCOMPARE(driver->text(0), QStringLiteral("驱动名称"));
        QVERIFY(document.RedoEdit());
        panel.RefreshParameterMetadata();
        QCOMPARE(driver->text(0), QStringLiteral("驱动名称"));
        metadata.displayName = QStringLiteral("自定义驱动");
        QVERIFY(document.MakeParameterMetadataEdit("Driver", metadata, edit));
        QVERIFY(document.ExecuteEdit(edit));
        panel.RefreshParameterMetadata();
        QCOMPARE(driver->text(0), QStringLiteral("自定义驱动"));
        QVERIFY(binding->text(0).contains(QStringLiteral("自定义驱动")));
        auto limiterMetadata = document.ParameterMetadata("Limiter");
        limiterMetadata.defaultValue = .5f;
        QVERIFY(document.MakeParameterMetadataEdit("Limiter", limiterMetadata, edit));
        QVERIFY(document.ExecuteEdit(edit));
        panel.RefreshParameterMetadata();
        QVERIFY(limiter->text(0).contains(QStringLiteral("限制名称")));
        QVERIFY(changed.isEmpty());
        QVERIFY(panel.BuildBlendShapesBytes(bytes, &cid));
        parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        driver = findItem(parameters, "parameter", "Driver");
        limiter = findItem(parameters, "parameter", "Limiter");
        QVERIFY(driver && limiter);
        QCOMPARE(driver->text(0), QStringLiteral("自定义驱动"));
        QCOMPARE(limiter->text(0), QStringLiteral("限制名称"));
    }

    void typeOneWithoutConstraintsStillHasControls()
    {
        auto bytes = blendBytes();
        for (int index = 25; index < 32; ++index) put(bytes, word(bytes, 0x40) + index * 4, 0);
        array(bytes, 0x20c, {0, 0, -1});
        array(bytes, 0x210, {0, 0, 0});
        CBlendShapePanel panel;
        QString error;
        QVERIFY2(panel.BuildBlendShapesBytes(bytes, nullptr, &error), qPrintable(error));
        QCOMPARE(panel.getBlendShapeParamIds(), (QSet<QString>{"Driver", "Unbound"}));
        QVERIFY(row(panel, "Driver"));
        QVERIFY(row(panel, "Unbound"));
        QVERIFY(!row(panel, "Limiter"));
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        QVERIFY(!findItem(details, "target"));
        QVERIFY(treeText(details).join('\n').contains(QStringLiteral("没有有效的融合目标")));
    }

    void sharedWarpOwnersAndLimitersLoadOnlyWhenExpanded()
    {
        CBlendShapePanel panel;
        QString error;
        QVERIFY2(panel.BuildBlendShapesBytes(blendBytes(), nullptr, &error), qPrintable(error));
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        auto* driver = findItem(parameters, "parameter", "Driver");
        QVERIFY(driver);
        parameters->setCurrentItem(driver);
        auto* mesh = findItem(details, "target", "Shared", "artmesh");
        auto* warp = findItem(details, "target", "Shared", "warp");
        auto* other = findItem(details, "target", "OtherOwner", "warp");
        QVERIFY(mesh && warp && other);
        QVERIFY(!findItem(details, "target", "Rotation"));
        for (auto* target : {mesh, warp, other}) {
            QCOMPARE(target->childCount(), 0);
            QCOMPARE(target->childIndicatorPolicy(), QTreeWidgetItem::ShowIndicator);
        }
        warp->setExpanded(true);
        QCOMPARE(warp->childCount(), 1);
        QCOMPARE(mesh->childCount(), 0);
        QCOMPARE(other->childCount(), 0);
        auto* binding = warp->child(0);
        QCOMPARE(binding->data(0, Qt::UserRole + 1).toString(), QStringLiteral("binding"));
        QCOMPARE(binding->childCount(), 0);
        binding->setExpanded(true);
        QCOMPARE(binding->childCount(), 1);
        auto* limiter = binding->child(0);
        QCOMPARE(limiter->data(0, Qt::UserRole).toString(), QStringLiteral("Limiter"));
        QVERIFY(limiter->text(0).startsWith(QStringLiteral("限制参数")));
        QCOMPARE(limiter->childCount(), 0);
        limiter->setExpanded(true);
        QCOMPARE(limiter->childCount(), 2);
        QVERIFY(limiter->child(0)->text(0).contains(QStringLiteral("-1")));
        QVERIFY(limiter->child(1)->text(1).contains(QStringLiteral("0.8")));
        warp->setExpanded(false);
        warp->setExpanded(true);
        QCOMPARE(warp->childCount(), 1);
        QCOMPARE(binding->childCount(), 1);
    }

    void versionFivePartAndRotationTargetsNavigateWithIndependentNames()
    {
        QTemporaryDir directory;
        const QJsonObject names{{"Version", 3},
            {"Parts", QJsonArray{QJsonObject{{"Id", "Shared"}, {"Name", QStringLiteral("部件名称")}}}},
            {"Deformers", QJsonArray{QJsonObject{{"Id", "Shared"}, {"Name", QStringLiteral("变形器名称")}},
                                     QJsonObject{{"Id", "RotationTarget"}, {"Name", QStringLiteral("旋转名称")}}}}};
        const auto path = directory.filePath("names.cdi3.json");
        QVERIFY(save(path, QJsonDocument(names).toJson()));
        CCidJson cid;
        QVERIFY(cid.Parse(path));
        CBlendShapePanel panel;
        QString error;
        QVERIFY2(panel.BuildBlendShapesBytes(BlendShapeV5Fixture::Extend(blendBytes()), &cid, &error), qPrintable(error));
        panel.resize(640, 720);
        panel.show();
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        parameters->setCurrentItem(findItem(parameters, "parameter", "Driver"));
        QSignalSpy selections(&panel, &CBlendShapePanel::ObjectSelected);
        QSignalSpy values(&panel, &CBlendShapePanel::ParamChanged);
        auto* warp = findItem(details, "target", "Shared", "warp");
        QVERIFY(warp);
        QCOMPARE(warp->text(0), QStringLiteral("变形器名称"));
        for (const auto& type : {QStringLiteral("rotation"), QStringLiteral("part")}) {
            const QString id = type == "part" ? QStringLiteral("Shared") : QStringLiteral("RotationTarget");
            auto* target = findItem(details, "target", id, type);
            QVERIFY(target);
            QCOMPARE(target->text(0), type == "part" ? QStringLiteral("部件名称") : QStringLiteral("旋转名称"));
            QCOMPARE(target->text(1), type == "part" ? QStringLiteral("部件") : QStringLiteral("旋转变形器"));
            target->setExpanded(true);
            QCOMPARE(target->childCount(), 1);
            target->child(0)->setExpanded(true);
            QCOMPARE(target->child(0)->childCount(), 1);
            QVERIFY(findItem(details, "limiter", "Limiter"));
            details->setCurrentItem(target);
            details->setFocus();
            QTest::keyClick(details, Qt::Key_Return);
            QCOMPARE(selections.size(), 1);
            QCOMPARE(selections.takeFirst(), (QList<QVariant>{id, type}));
        }
        QVERIFY(values.isEmpty());
    }

    void activationNavigatesWithTypedSignalsWithoutEditing()
    {
        CBlendShapePanel panel;
        QVERIFY(panel.BuildBlendShapesBytes(blendBytes(), nullptr));
        panel.resize(600, 700);
        panel.show();
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        auto* driver = findItem(parameters, "parameter", "Driver");
        QSignalSpy values(&panel, &CBlendShapePanel::ParamChanged);
        QSignalSpy parameterSelection(&panel, &CBlendShapePanel::ParameterSelected);
        QSignalSpy objectSelection(&panel, &CBlendShapePanel::ObjectSelected);
        parameters->setCurrentItem(driver);
        QVERIFY(parameterSelection.isEmpty());
        parameters->setFocus();
        for (auto key : {Qt::Key_Return, Qt::Key_Enter}) {
            QTest::keyClick(parameters, key);
            QCOMPARE(parameterSelection.size(), 1);
            QCOMPARE(parameterSelection.takeFirst()[0].toString(), QStringLiteral("Driver"));
        }
        for (const QString& type : {QStringLiteral("artmesh"), QStringLiteral("warp")}) {
            auto* target = findItem(details, "target", "Shared", type);
            QVERIFY(target);
            details->setCurrentItem(target);
            details->setFocus();
            QTest::keyClick(details, Qt::Key_Return);
            QCOMPARE(objectSelection.size(), 1);
            QCOMPARE(objectSelection.takeFirst(), (QList<QVariant>{QStringLiteral("Shared"), type}));
        }
        auto* mesh = findItem(details, "target", "Shared", "artmesh");
        details->scrollToItem(mesh);
        QCoreApplication::processEvents();
        const auto point = details->visualItemRect(mesh).center();
        QTest::mouseClick(details->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseDClick(details->viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(objectSelection.size(), 1);
        QCOMPARE(objectSelection.takeFirst(), (QList<QVariant>{QStringLiteral("Shared"), QStringLiteral("artmesh")}));
        auto* warp = findItem(details, "target", "OtherOwner", "warp");
        warp->setExpanded(true);
        warp->child(0)->setExpanded(true);
        auto* limiter = warp->child(0)->child(0);
        QVERIFY(QMetaObject::invokeMethod(details, "itemActivated", Q_ARG(QTreeWidgetItem*, limiter), Q_ARG(int, 0)));
        QCOMPARE(parameterSelection.size(), 1);
        QCOMPARE(parameterSelection.first()[0].toString(), QStringLiteral("Limiter"));
        QVERIFY(values.isEmpty());
        QCOMPARE(row(panel, "Driver")->findChild<QSlider*>()->value(), 500);
    }

    void malformedRelationsHaveDiagnosticsAndCannotNavigateAmbiguousTargets_data()
    {
        QTest::addColumn<bool>("duplicate");
        QTest::newRow("duplicate-warp-id") << true;
        QTest::newRow("invalid-warp-source") << false;
    }

    void malformedRelationsHaveDiagnosticsAndCannotNavigateAmbiguousTargets()
    {
        QFETCH(bool, duplicate);
        auto bytes = blendBytes();
        if (duplicate) name(bytes, 0x6c, 2, "Shared");
        else cell(bytes, 0x234, 0, std::numeric_limits<int>::max());
        CBlendShapePanel panel;
        QVERIFY(panel.BuildBlendShapesBytes(bytes, nullptr));
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        QVERIFY(findItem(details, "target", "Shared", "artmesh"));
        QVERIFY(!findItem(details, "target", "Shared", "warp"));
        auto* diagnostics = findItem(details, "diagnostics");
        QVERIFY(diagnostics);
        QCOMPARE(diagnostics->childCount(), 0);
        diagnostics->setExpanded(true);
        QVERIFY(diagnostics->childCount() > 0);
        QVERIFY(treeText(details).join('\n').contains(duplicate ? QStringLiteral("目标标识重复") : QStringLiteral("融合目标或关键形绑定区间无效")));
        QSignalSpy objects(&panel, &CBlendShapePanel::ObjectSelected);
        QSignalSpy parameters(&panel, &CBlendShapePanel::ParameterSelected);
        QVERIFY(QMetaObject::invokeMethod(details, "itemActivated", Q_ARG(QTreeWidgetItem*, diagnostics->child(0)), Q_ARG(int, 0)));
        QVERIFY(objects.isEmpty());
        QVERIFY(parameters.isEmpty());
    }

    void ambiguousParameterIdsDoNotCreateControlsOrNavigate()
    {
        auto bytes = blendBytes();
        name(bytes, 0x108, 2, "Driver");
        CBlendShapePanel panel;
        QVERIFY(panel.BuildBlendShapesBytes(bytes, nullptr));
        auto* tree = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* item = findItem(tree, "parameter", "Driver");
        QVERIFY(item);
        QVERIFY(!row(panel, "Driver"));
        QSignalSpy selected(&panel, &CBlendShapePanel::ParameterSelected);
        QSignalSpy changed(&panel, &CBlendShapePanel::ParamChanged);
        QVERIFY(QMetaObject::invokeMethod(tree, "itemActivated", Q_ARG(QTreeWidgetItem*, item), Q_ARG(int, 0)));
        QVERIFY(selected.isEmpty());
        QVERIFY(changed.isEmpty());
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        auto* diagnostics = findItem(details, "diagnostics");
        QVERIFY(diagnostics);
        diagnostics->setExpanded(true);
        QVERIFY(treeText(details).join('\n').contains(QStringLiteral("参数标识重复")));
    }

    void failedReloadPreservesControlsRelationsAndCurrentValues()
    {
        CBlendShapePanel panel;
        const auto bytes = blendBytes();
        QVERIFY(panel.BuildBlendShapesBytes(bytes, nullptr));
        panel.SyncParameterValue("Driver", 0.5f);
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        auto* target = findItem(details, "target", "Shared", "warp");
        target->setExpanded(true);
        const auto before = treeText(details);
        auto* slider = row(panel, "Driver")->findChild<QSlider*>();
        QString error;
        QVERIFY(!panel.BuildBlendShapesBytes("invalid", nullptr, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(panel.findChild<QTreeWidget*>("blendParameterTree"), parameters);
        QCOMPARE(panel.findChild<QTreeWidget*>("blendRelationTree"), details);
        QCOMPARE(treeText(details), before);
        QCOMPARE(row(panel, "Driver")->findChild<QSlider*>(), slider);
        QCOMPARE(slider->value(), 750);
        panel.BuildBlendShapes("/missing/live2d-model.moc3", nullptr);
        QCOMPARE(treeText(details), before);
        QVERIFY(panel.BuildBlendShapesBytes(bytes, nullptr));
        QCOMPARE(row(panel, "Driver")->findChild<QSlider*>()->value(), 750);
        QCOMPARE(panel.getBlendShapeParamIds(), (QSet<QString>{"Driver", "Unbound"}));
    }

    void effectiveBytesKeepDocumentIdentityMetadataAndExistingValues()
    {
        Fixture fixture;
        QVERIFY(fixture.create(-100, 100, 0.25f));
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(fixture.path, &error), qPrintable(error));
        CBlendShapePanel panel;
        panel.SetDocument(&document);
        QVERIFY(panel.BuildBlendShapesBytes(document.SourceBytes(), &fixture.cid));
        panel.SyncParameterValue(fixture.id, 0.75f);
        auto metadata = document.ParameterMetadata(fixture.id);
        metadata.maximum = 200;
        metadata.defaultValue = 2;
        metadata.displayName = QStringLiteral("有效文档名称");
        QVERIFY(document.SetParameterMetadata(fixture.id, metadata));
        QByteArray effective;
        QVERIFY2(document.BuildPreviewMoc(effective, &error), qPrintable(error));
        QVERIFY(effective != document.SourceBytes());
        QSignalSpy values(&panel, &CBlendShapePanel::ParamChanged), selection(&panel, &CBlendShapePanel::ParameterSelected);
        QVERIFY(panel.BuildBlendShapesBytes(effective, &fixture.cid, &error, document.SourceDigest()));
        auto* tree = panel.findChild<QTreeWidget*>("blendParameterTree");
        QVERIFY(tree);
        QCOMPARE(findItem(tree, "parameter", fixture.id)->text(0), metadata.displayName);
        auto* controls = row(panel, fixture.id);
        QVERIFY(controls);
        QCOMPARE(controls->findChild<QLabel*>()->text(), QString("0.75"));
        QCOMPARE(values.size(), 0);
        QCOMPARE(selection.size(), 0);
        metadata.displayName = QStringLiteral("再次编辑名称");
        QVERIFY(document.SetParameterMetadata(fixture.id, metadata));
        panel.RefreshParameterMetadata();
        QCOMPARE(findItem(tree, "parameter", fixture.id)->text(0), metadata.displayName);
        QCOMPARE(controls->findChild<QLabel*>()->text(), QString("0.75"));
    }

    void documentMetadataRefreshIsSilentAndKeepsSourceRanges()
    {
        Fixture fixture;
        QVERIFY(fixture.create(-100, 100, 0.25f));
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(fixture.path, &error), qPrintable(error));
        CBlendShapePanel panel;
        panel.SetDocument(&document);
        QVERIFY(panel.BuildBlendShapesBytes(document.SourceBytes(), &fixture.cid));
        auto* control = row(panel, fixture.id);
        auto* slider = control->findChild<QSlider*>();
        auto* label = control->findChild<QLabel*>();
        auto* parameters = panel.findChild<QTreeWidget*>("blendParameterTree");
        auto* details = panel.findChild<QTreeWidget*>("blendRelationTree");
        const auto sourceDescription = treeText(details);
        QSignalSpy changed(&panel, &CBlendShapePanel::ParamChanged);
        auto metadata = document.ParameterMetadata(fixture.id);
        metadata.minimum = -200;
        metadata.maximum = 200;
        metadata.defaultValue = 2;
        metadata.displayName = QStringLiteral("编辑后的名称");
        QVERIFY(document.SetParameterMetadata(fixture.id, metadata));
        panel.RefreshParameterMetadata();
        QCOMPARE(findItem(parameters, "parameter", fixture.id)->text(0), metadata.displayName);
        QCOMPARE(slider->value(), 505);
        labelMatches(label, 2);
        panel.SyncParameterValue(fixture.id, 0.75f);
        metadata.maximum = 400;
        metadata.defaultValue = 3;
        QVERIFY(document.SetParameterMetadata(fixture.id, metadata));
        panel.RefreshParameterMetadata();
        QCOMPARE(slider->value(), 335);
        labelMatches(label, 0.75f);
        QCOMPARE(treeText(details), sourceDescription);
        QVERIFY(changed.isEmpty());
        QVERIFY(!document.CanUndoEdit());
        live2dstruct::CModelDocument other;
        QVERIFY(other.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        auto foreign = other.ParameterMetadata(fixture.id);
        foreign.displayName = QStringLiteral("其他模型的名称");
        QVERIFY(other.SetParameterMetadata(fixture.id, foreign));
        panel.SetDocument(&other);
        QCOMPARE(findItem(parameters, "parameter", fixture.id)->text(0), fixture.id);
        QCOMPARE(slider->value(), 504);
        labelMatches(label, 0.75f);
        QVERIFY(changed.isEmpty());
    }
    void sliderGestureReturnsOriginalFullPrecisionWithoutQuantization()
    {
        Fixture fixture; QVERIFY(fixture.create(0, 1, 0)); CBlendShapePanel panel;
        panel.BuildBlendShapes(fixture.path, &fixture.cid); const float original = .1234567f;
        panel.SyncParameterValue(fixture.id, original); auto* slider = row(panel, fixture.id)->findChild<QSlider*>(); QVERIFY(slider);
        const int position = slider->value(); QCOMPARE(position, 123);
        QSignalSpy started(&panel, &CBlendShapePanel::ParameterGestureStarted), finished(&panel, &CBlendShapePanel::ParameterGestureFinished);
        QSignalSpy canceled(&panel, &CBlendShapePanel::ParameterGestureCanceled), values(&panel, &CBlendShapePanel::ParamChanged);
        slider->setSliderDown(true); QVERIFY(panel.HasParameterGesture()); slider->setValue(750); slider->setValue(position); slider->setSliderDown(false);
        QCOMPARE(started.size(), 1); QCOMPARE(finished.size(), 1); QCOMPARE(canceled.size(), 0); QCOMPARE(values.size(), 2); QVERIFY(!panel.HasParameterGesture());
        const float captured = started.first()[1].toFloat(), returned = values.last()[1].toFloat();
        QCOMPARE(std::memcmp(&captured, &original, sizeof(float)), 0); QCOMPARE(std::memcmp(&returned, &original, sizeof(float)), 0);
        values.clear(); started.clear(); finished.clear();
        panel.SyncParameterValue(fixture.id, .7f); panel.RefreshParameterMetadata();
        QCOMPARE(values.size(), 0); QCOMPARE(started.size(), 0); QCOMPARE(finished.size(), 0);
    }

    void keyboardGestureFinishesOnceAfterAutoRepeat()
    {
        Fixture fixture; QVERIFY(fixture.create(0, 1, .5f)); CBlendShapePanel panel; panel.BuildBlendShapes(fixture.path, &fixture.cid);
        auto* slider = row(panel, fixture.id)->findChild<QSlider*>(); QVERIFY(slider);
        QSignalSpy started(&panel, &CBlendShapePanel::ParameterGestureStarted), finished(&panel, &CBlendShapePanel::ParameterGestureFinished);
        QSignalSpy canceled(&panel, &CBlendShapePanel::ParameterGestureCanceled), values(&panel, &CBlendShapePanel::ParamChanged);
        QTest::keyPress(slider, Qt::Key_Right);
        QKeyEvent releaseRepeat(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier, {}, true); QApplication::sendEvent(slider, &releaseRepeat);
        QKeyEvent pressRepeat(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier, {}, true); QApplication::sendEvent(slider, &pressRepeat);
        QCOMPARE(started.size(), 1); QCOMPARE(finished.size(), 0); QVERIFY(panel.HasParameterGesture());
        QTest::keyRelease(slider, Qt::Key_Right); QCOMPARE(finished.size(), 1); QCOMPARE(canceled.size(), 0); QCOMPARE(values.size(), 2); QVERIFY(!panel.HasParameterGesture());
    }

    void escapeAndFocusLossCancelWithoutPublishingRestoration_data()
    {
        QTest::addColumn<bool>("escape"); QTest::newRow("escape") << true; QTest::newRow("focus") << false;
    }
    void escapeAndFocusLossCancelWithoutPublishingRestoration()
    {
        QFETCH(bool, escape); Fixture fixture; QVERIFY(fixture.create(0, 1, 0)); CBlendShapePanel panel; panel.BuildBlendShapes(fixture.path, &fixture.cid);
        const float original = .1234567f; panel.SyncParameterValue(fixture.id, original);
        auto* slider = row(panel, fixture.id)->findChild<QSlider*>(); QVERIFY(slider);
        QSignalSpy finished(&panel, &CBlendShapePanel::ParameterGestureFinished), canceled(&panel, &CBlendShapePanel::ParameterGestureCanceled);
        QSignalSpy values(&panel, &CBlendShapePanel::ParamChanged);
        slider->setSliderDown(true); slider->setValue(750); QCOMPARE(values.size(), 1);
        if (escape) QTest::keyClick(slider, Qt::Key_Escape);
        else { QFocusEvent event(QEvent::FocusOut); QApplication::sendEvent(slider, &event); }
        QCOMPARE(canceled.size(), 1); QCOMPARE(finished.size(), 0); QCOMPARE(values.size(), 1); QCOMPARE(slider->value(), 123); QVERIFY(!panel.HasParameterGesture());
        slider->setSliderDown(false); QCOMPARE(finished.size(), 0);
    }

    void rejectedGestureCanBeCanceledFromStartedCallback()
    {
        Fixture fixture; QVERIFY(fixture.create(0, 1, .5f)); CBlendShapePanel panel; panel.BuildBlendShapes(fixture.path, &fixture.cid);
        auto* slider = row(panel, fixture.id)->findChild<QSlider*>(); QVERIFY(slider);
        QSignalSpy values(&panel, &CBlendShapePanel::ParamChanged), canceled(&panel, &CBlendShapePanel::ParameterGestureCanceled);
        connect(&panel, &CBlendShapePanel::ParameterGestureStarted, &panel, &CBlendShapePanel::CancelParameterGesture);
        QTest::keyClick(slider, Qt::Key_Right); QVERIFY(!panel.HasParameterGesture()); QCOMPARE(slider->value(), 500);
        QCOMPARE(canceled.size(), 1); QCOMPARE(values.size(), 0);
    }

};

QTEST_MAIN(BlendShapePanelTests)
#include "blend_shape_panel_tests.moc"
