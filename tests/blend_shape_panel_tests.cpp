#include "ui/cblendshapepanel.h"
#include "ui/para/ccidjson.h"
#include "live2dstruct/cmodeldocument.h"

#include <QtTest>
#include <QFile>
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
};

QTEST_MAIN(BlendShapePanelTests)
#include "blend_shape_panel_tests.moc"
