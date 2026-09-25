#include "cmo3/ccaffarchive.h"
#include "cmo3/ccmo3builder.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"
#include "ui/cinspectorpanel.h"

#include <QtTest>
#include <QCheckBox>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <stdexcept>

using namespace live2dstruct;

namespace {
struct RawClipping
{
    QStringList ids;
    QVector<quint8> flags;
    QVector<QVector<int>> masks;

    bool read(const QByteArray& bytes)
    {
        *this = {};
        const auto integer = [&bytes](qint64 offset, int& value) {
            if (offset < 0 || offset > bytes.size() - 4) return false;
            value = qFromLittleEndian<qint32>(bytes.constData() + offset);
            return true;
        };
        int counts, count, poolCount, idOffset, flagOffset, beginOffset, countOffset, poolOffset;
        if (bytes.left(4) != "MOC3" || !integer(0x40, counts) ||
            !integer(qint64(counts) + 16, count) || !integer(qint64(counts) + 68, poolCount) ||
            count < 0 || count > 1000000 || poolCount < 0 ||
            !integer(0xc4, idOffset) || !integer(0xe8, flagOffset) ||
            !integer(0xfc, beginOffset) || !integer(0x100, countOffset) || !integer(0x180, poolOffset)) return false;
        for (int index = 0; index < count; ++index) {
            const qint64 idPosition = qint64(idOffset) + index * 64LL;
            const qint64 flagPosition = qint64(flagOffset) + index;
            int begin, length;
            if (idPosition < 0 || idPosition > bytes.size() - 64 || flagPosition < 0 || flagPosition >= bytes.size() ||
                !integer(qint64(beginOffset) + index * 4LL, begin) ||
                !integer(qint64(countOffset) + index * 4LL, length) || length < 0 ||
                (length && (begin < 0 || begin > poolCount || length > poolCount - begin))) return false;
            const QByteArray id = bytes.mid(idPosition, 64);
            ids.append(QString::fromUtf8(id.constData(), id.indexOf('\0') < 0 ? 64 : id.indexOf('\0')));
            flags.append(static_cast<quint8>(bytes[flagPosition]));
            QVector<int> indices;
            for (int mask = 0; mask < length; ++mask) {
                int value;
                if (!integer(qint64(poolOffset) + (qint64(begin) + mask) * 4, value) || value < 0 || value >= count) return false;
                indices.append(value);
            }
            masks.append(indices);
        }
        return true;
    }
};

struct XmlNode
{
    QString tag, text;
    QMap<QString, QString> attributes;
    QVector<int> children;
};

struct ClippingXml
{
    QVector<XmlNode> nodes;
    QHash<QString, int> definitions;
    QHash<QString, int> sources;

    QString attribute(int index, const QString& name) const
    {
        return index >= 0 && index < nodes.size() ? nodes[index].attributes.value(name) : QString();
    }
    int field(int index, const QString& name) const
    {
        if (index >= 0 && index < nodes.size())
            for (int child : nodes[index].children)
                if (attribute(child, "xs.n") == name) return child;
        return -1;
    }
    bool read(const QByteArray& bytes)
    {
        QXmlStreamReader xml(bytes);
        xml.setNamespaceProcessing(false);
        QVector<int> parents;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                XmlNode node;
                node.tag = xml.name().toString();
                for (const auto& attribute : xml.attributes())
                    node.attributes.insert(attribute.qualifiedName().toString(), attribute.value().toString());
                const int index = nodes.size();
                if (!parents.isEmpty()) nodes[parents.last()].children.append(index);
                const auto id = node.attributes.value("xs.id");
                if (!id.isEmpty()) {
                    if (definitions.contains(id)) return false;
                    definitions.insert(id, index);
                }
                nodes.append(node);
                parents.append(index);
            } else if (xml.isCharacters() && !parents.isEmpty()) nodes[parents.last()].text += xml.text();
            else if (xml.isEndElement()) parents.removeLast();
        }
        if (xml.hasError()) return false;
        for (int index = 0; index < nodes.size(); ++index) {
            if (nodes[index].tag != QLatin1String("CArtMeshSource") || attribute(index, "xs.id").isEmpty()) continue;
            const auto id = attribute(field(field(index, "super"), "id"), "idstr");
            if (id.isEmpty() || sources.contains(id)) return false;
            sources.insert(id, index);
        }
        return true;
    }
};

struct Editor
{
    CModelDocument document;
    CKeyformEditController controller;
    CInspectorPanel inspector;
    QMap<QString, float> current;
    QString path;
    int attempts = 0, publications = 0, activations = 0, mode = 0;
    QByteArray publishedBytes;
    QMap<QString, float> publishedPose;
    bool publishedEditing = false;

    bool load(const QString& model, QString* error = nullptr)
    {
        path = QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3";
        if (!document.Load(path, error)) return false;
        for (const auto& parameter : document.Parameters())
            current.insert(parameter.id, float(double(parameter.minimum) +
                (double(parameter.maximum) - parameter.minimum) * 0.37));
        controller.SetDocument(&document);
        controller.SetPublisher([this](const CModelDocument& candidate, const QMap<QString, float>& pose,
                                       bool editing, QString* error) {
            ++attempts;
            QByteArray bytes;
            if (!candidate.BuildPreviewMoc(bytes, error)) return false;
            auto reader = CReader::FromBytes(bytes);
            CSectionTable table(reader);
            if (!table.IsValid()) { if (error) *error = reader.Error(); return false; }
            if (mode == 2) throw std::runtime_error("controlled clipping publication failure");
            if (mode == 1) { if (error) *error = QStringLiteral("controlled clipping rejection"); return false; }
            ++publications;
            publishedBytes = bytes;
            publishedPose = pose;
            publishedEditing = editing;
            return true;
        }, [](QString*) { return true; });
        inspector.SetDocument(&document);
        inspector.SetKeyformHandler([this](const QString& id, int keyform, QString* error) {
            ++activations;
            return controller.Activate(id, keyform, current, error);
        });
        inspector.SetEditHandler([this](const CModelEdit& edit, QString* error) {
            if (!controller.Submit(edit, current, inspector.GetActiveKeyformIndex(), error)) return false;
            refresh();
            return true;
        });
        inspector.SetHistoryHandler([this](bool undo, CModelEdit& edit, QString* error) {
            if (!controller.History(undo, edit, error)) return false;
            refresh();
            return true;
        });
        return true;
    }
    void refresh()
    {
        const QSignalBlocker blocker(&inspector);
        inspector.RefreshSelection();
        inspector.SyncHistoryState();
    }
};

bool editDialog(CInspectorPanel& panel, const std::function<void(QDialog&, QListWidget&)>& edit, bool accept = true)
{
    auto* button = panel.findChild<QPushButton*>("editMasks");
    if (!button || !button->isEnabled()) return false;
    bool visited = false;
    QTimer::singleShot(0, &panel, [&] {
        auto* dialog = panel.findChild<QDialog*>();
        if (!dialog) return;
        auto* list = dialog->findChild<QListWidget*>();
        if (!list) { dialog->reject(); return; }
        edit(*dialog, *list);
        visited = true;
        if (accept) dialog->accept();
        else dialog->reject();
    });
    button->click();
    return visited;
}

bool setMasks(CInspectorPanel& panel, const QVector<int>& masks)
{
    return editDialog(panel, [&masks](QDialog&, QListWidget& list) {
        for (int row = 0; row < list.count(); ++row)
            list.item(row)->setCheckState(masks.contains(list.item(row)->data(Qt::UserRole).toInt()) ? Qt::Checked : Qt::Unchecked);
    });
}

void models()
{
    QTest::addColumn<QString>("model");
    for (const char* model : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(model) << QString::fromLatin1(model);
}
}

class ClippingEditorTests : public QObject
{
    Q_OBJECT

    static void verifyBytes(const Editor& editor)
    {
        RawClipping source, preview;
        QVERIFY(source.read(editor.document.SourceBytes()));
        QVERIFY(preview.read(editor.publishedBytes));
        QCOMPARE(preview.ids, source.ids);
        for (int index = 0; index < source.ids.size(); ++index) {
            const auto& id = source.ids[index];
            QCOMPARE(preview.masks[index], editor.document.DrawableMaskIndices(id));
            QCOMPARE(preview.flags[index] & 0xf7, source.flags[index] & 0xf7);
            QCOMPARE(bool(preview.flags[index] & 8), editor.document.DrawableMaskInverted(id));
        }
    }

    static void verifyExport(const Editor& editor)
    {
        RawClipping source;
        QVERIFY(source.read(editor.document.SourceBytes()));
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        cmo3::CCmo3Builder::Options options;
        options.mocPath = editor.path;
        options.outPath = temporary.filePath("clipping.cmo3");
        options.drawableMaskIndexOverrides = editor.document.DrawableMaskIndexOverrides();
        options.drawableMaskInverted = editor.document.DrawableMaskInvertedOverrides();
        QString error;
        QVERIFY2(cmo3::CCmo3Builder::Build(options, &error), qPrintable(error));
        cmo3::CCaffArchive archive;
        QVERIFY2(archive.Load(options.outPath, &error), qPrintable(error));
        const auto* main = archive.FindByPath("main.xml");
        QVERIFY(main);
        ClippingXml xml;
        QVERIFY(xml.read(main->content));
        QCOMPARE(xml.sources.size(), source.ids.size());
        for (int index = 0; index < source.ids.size(); ++index) {
            const QString id = source.ids[index];
            const int base = xml.field(xml.sources.value(id, -1), "super");
            const int list = xml.field(base, "clipGuidList");
            const int inverse = xml.field(base, "invertClippingMask");
            QVERIFY(list >= 0 && inverse >= 0);
            const auto masks = options.drawableMaskIndexOverrides.value(id, source.masks[index]);
            QCOMPARE(xml.nodes[list].children.size(), masks.size());
            QCOMPARE(xml.attribute(list, "count").toInt(), masks.size());
            for (int mask = 0; mask < masks.size(); ++mask) {
                const int guid = xml.definitions.value(xml.attribute(xml.nodes[list].children[mask], "xs.ref"), -1);
                QCOMPARE(xml.attribute(guid, "note"), source.ids[masks[mask]]);
            }
            const bool expected = !masks.isEmpty() && options.drawableMaskInverted.value(id, bool(source.flags[index] & 8));
            QCOMPARE(xml.nodes[inverse].text.trimmed(), expected ? QStringLiteral("true") : QStringLiteral("false"));
        }
    }

private slots:
    void unchangedNativeOrderDoesNotPublishOrCreateHistory()
    {
        Editor editor;
        QVERIFY(editor.load("Natori"));
        RawClipping raw;
        QVERIFY(raw.read(editor.document.SourceBytes()));
        const int target = raw.ids.indexOf("ArtMesh3");
        QVERIFY(target >= 0);
        QCOMPARE(raw.masks[target], (QVector<int>{149, 3}));
        editor.inspector.SetSelection(raw.ids[target], "artmesh");
        const auto revision = editor.document.Revision();
        QSignalSpy history(&editor.inspector, &CInspectorPanel::HistoryChanged);
        QSignalSpy changed(&editor.inspector, &CInspectorPanel::DrawableMaskIndicesChanged);
        QVERIFY(editDialog(editor.inspector, [](QDialog&, QListWidget&) {}));
        QCOMPARE(editor.document.DrawableMaskIndices(raw.ids[target]), raw.masks[target]);
        QCOMPARE(editor.document.Revision(), revision);
        QCOMPARE(editor.attempts, 0);
        QCOMPARE(editor.activations, 0);
        QCOMPARE(history.size(), 0);
        QCOMPARE(changed.size(), 0);
        QVERIFY(!editor.document.CanUndoEdit());
    }

    void clippingTransactionsRemainIndependentOfKeyformPose_data() { models(); }
    void clippingTransactionsRemainIndependentOfKeyformPose()
    {
        QFETCH(QString, model);
        Editor editor;
        QVERIFY(editor.load(model));
        RawClipping raw;
        QVERIFY(raw.read(editor.document.SourceBytes()));
        QVERIFY(raw.ids.size() >= 4);
        const int target = 0;
        const QString id = raw.ids[target];
        editor.inspector.SetSelection(id, "artmesh");
        const int selectedKeyform = editor.inspector.GetActiveKeyformIndex();
        const auto valuesBefore = editor.document.ParameterValues();
        const auto currentBefore = editor.current;
        const auto revision = editor.document.Revision();
        QSignalSpy history(&editor.inspector, &CInspectorPanel::HistoryChanged);
        QSignalSpy rejected(&editor.inspector, &CInspectorPanel::EditRejected);
        QSignalSpy changed(&editor.inspector, &CInspectorPanel::DrawableMaskIndicesChanged);
        const QVector<int> after{1, 2, 3};
        QVERIFY(setMasks(editor.inspector, after));
        QCOMPARE(editor.document.DrawableMaskIndices(id), after);
        QCOMPARE(editor.document.Revision(), revision + 1);
        QCOMPARE(editor.publications, 1);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(history.size(), 1);
        QCOMPARE(rejected.size(), 0);
        QCOMPARE(editor.activations, 0);
        QVERIFY(!editor.controller.IsActive());
        QVERIFY(!editor.publishedEditing);
        QCOMPARE(editor.inspector.GetActiveKeyformIndex(), selectedKeyform);
        QCOMPARE(editor.document.ParameterValues(), valuesBefore);
        QCOMPARE(editor.current, currentBefore);
        verifyBytes(editor);
        auto* inverted = editor.inspector.findChild<QCheckBox*>("maskInverted");
        QVERIFY(inverted && inverted->isEnabled());
        const bool before = inverted->isChecked();
        inverted->click();
        QCOMPARE(editor.document.DrawableMaskInverted(id), !before);
        QCOMPARE(editor.publications, 2);
        QCOMPARE(history.size(), 2);
        verifyBytes(editor);
        editor.inspector.Undo();
        QCOMPARE(editor.document.DrawableMaskInverted(id), before);
        editor.inspector.Undo();
        QCOMPARE(editor.document.DrawableMaskIndices(id), raw.masks[target]);
        QVERIFY(!editor.document.CanUndoEdit());
        editor.inspector.Redo();
        editor.inspector.Redo();
        QCOMPARE(editor.document.DrawableMaskIndices(id), after);
        QCOMPARE(editor.document.DrawableMaskInverted(id), !before);
        QCOMPARE(editor.publications, 6);
        QCOMPARE(history.size(), 6);
        QCOMPARE(editor.activations, 0);
        QVERIFY(!editor.controller.IsActive());
        QCOMPARE(editor.inspector.GetActiveKeyformIndex(), selectedKeyform);
        QCOMPARE(editor.document.ParameterValues(), valuesBefore);
        verifyBytes(editor);
        verifyExport(editor);
    }

    void rejectedAndThrowingPublishersPreserveControlsAndHistory_data()
    {
        QTest::addColumn<bool>("inverse");
        QTest::addColumn<int>("mode");
        for (int mode : {1, 2}) {
            QTest::newRow(qPrintable(QStringLiteral("list-%1").arg(mode))) << false << mode;
            QTest::newRow(qPrintable(QStringLiteral("inverse-%1").arg(mode))) << true << mode;
        }
    }

    void rejectedAndThrowingPublishersPreserveControlsAndHistory()
    {
        QFETCH(bool, inverse);
        QFETCH(int, mode);
        Editor editor;
        QVERIFY(editor.load("Rice"));
        const QString id = QStringLiteral("ArtMesh9");
        editor.inspector.SetSelection(id, "artmesh");
        auto* inverted = editor.inspector.findChild<QCheckBox*>("maskInverted");
        QVERIFY(inverted && inverted->isEnabled());
        const auto before = editor.document.DrawableMaskIndices(id);
        const bool originalInverse = editor.document.DrawableMaskInverted(id);
        const auto revision = editor.document.Revision();
        QSignalSpy history(&editor.inspector, &CInspectorPanel::HistoryChanged);
        QSignalSpy changed(&editor.inspector, &CInspectorPanel::DrawableMaskIndicesChanged);
        QSignalSpy rejected(&editor.inspector, &CInspectorPanel::EditRejected);
        editor.mode = mode;
        if (inverse) inverted->click();
        else QVERIFY(setMasks(editor.inspector, {}));
        QCOMPARE(editor.attempts, 1);
        QCOMPARE(editor.publications, 0);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(history.size(), 0);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(editor.document.Revision(), revision);
        QCOMPARE(editor.document.DrawableMaskIndices(id), before);
        QCOMPARE(editor.document.DrawableMaskInverted(id), originalInverse);
        QCOMPARE(inverted->isChecked(), originalInverse);
        QVERIFY(inverted->isEnabled());
        QVERIFY(!editor.document.CanUndoEdit());
        QVERIFY(!editor.document.CanRedoEdit());
        editor.mode = 0;
        if (inverse) inverted->click();
        else QVERIFY(setMasks(editor.inspector, {}));
        QCOMPARE(editor.publications, 1);
        const auto acceptedBytes = editor.publishedBytes;
        const auto acceptedRevision = editor.document.Revision();
        editor.mode = mode;
        editor.inspector.Undo();
        QCOMPARE(editor.document.Revision(), acceptedRevision);
        QCOMPARE(editor.publishedBytes, acceptedBytes);
        QCOMPARE(editor.publications, 1);
        QCOMPARE(history.size(), 1);
        QCOMPARE(rejected.size(), 2);
        QVERIFY(editor.document.CanUndoEdit());
        QVERIFY(!editor.document.CanRedoEdit());
        editor.mode = 0;
        editor.inspector.Undo();
        QCOMPARE(editor.document.DrawableMaskIndices(id), before);
        QCOMPARE(editor.document.DrawableMaskInverted(id), originalInverse);
        QCOMPARE(editor.publications, 2);
        editor.mode = mode;
        editor.inspector.Redo();
        QCOMPARE(editor.document.DrawableMaskIndices(id), before);
        QCOMPARE(editor.document.DrawableMaskInverted(id), originalInverse);
        QCOMPARE(editor.publications, 2);
        QCOMPARE(history.size(), 2);
        QVERIFY(!editor.document.CanUndoEdit());
        QVERIFY(editor.document.CanRedoEdit());
        editor.mode = 0;
        editor.inspector.Redo();
        QCOMPARE(editor.publications, 3);
        verifyBytes(editor);
    }

    void clearingMasksRetainsInversionThroughSession_data()
    {
        QTest::addColumn<bool>("sourceInverted");
        QTest::newRow("source-inverse") << true;
        QTest::newRow("user-inverse") << false;
    }

    void clearingMasksRetainsInversionThroughSession()
    {
        QFETCH(bool, sourceInverted);
        Editor editor;
        QVERIFY(editor.load("Rice"));
        RawClipping raw;
        QVERIFY(raw.read(editor.document.SourceBytes()));
        int target = -1;
        for (int index = 0; index < raw.ids.size(); ++index)
            if (!raw.masks[index].isEmpty() && bool(raw.flags[index] & 8) == sourceInverted) { target = index; break; }
        QVERIFY(target >= 0);
        const QString id = raw.ids[target];
        editor.inspector.SetSelection(id, "artmesh");
        auto* inverted = editor.inspector.findChild<QCheckBox*>("maskInverted");
        QVERIFY(inverted && inverted->isEnabled());
        if (!sourceInverted) inverted->click();
        QVERIFY(inverted->isChecked());
        const int before = editor.publications;
        QVERIFY(setMasks(editor.inspector, {}));
        QCOMPARE(editor.publications, before + 1);
        QVERIFY(editor.document.DrawableMaskIndices(id).isEmpty());
        QVERIFY(editor.document.DrawableMaskInverted(id));
        QVERIFY(inverted->isChecked() && !inverted->isEnabled());
        CModelEdit pending;
        QVERIFY(editor.document.PeekUndoEdit(&pending));
        QCOMPARE(pending.field, CModelEdit::MaskIndices);
        editor.inspector.Undo();
        QCOMPARE(editor.document.DrawableMaskIndices(id), raw.masks[target]);
        QVERIFY(inverted->isChecked() && inverted->isEnabled());
        editor.inspector.Redo();
        verifyBytes(editor);
        for (const auto& diagnostic : editor.document.Validate()) QVERIFY(diagnostic.code != QLatin1String("mask_inverted_id"));
        CModelSession session;
        session.sourceMoc = editor.path;
        session.sourceDigest = CModelSession::ComputeSourceDigest(editor.path);
        session.drawableMaskIndices = editor.document.DrawableMaskIndexOverrides();
        session.drawableMaskInverted = editor.document.DrawableMaskInvertedOverrides();
        CModelSession decoded;
        QString error;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        CModelDocument original, restored;
        QVERIFY(original.Load(editor.path));
        QVERIFY2(decoded.RestoreDocument(original, restored, &error), qPrintable(error));
        QVERIFY(restored.DrawableMaskIndices(id).isEmpty());
        QVERIFY(restored.DrawableMaskInverted(id));
        QVERIFY(!restored.CanUndoEdit());
        QVERIFY2(editor.controller.Restore(std::move(restored), &error), qPrintable(error));
        editor.refresh();
        QVERIFY(inverted->isChecked() && !inverted->isEnabled());
        verifyBytes(editor);
        verifyExport(editor);
        QVERIFY(setMasks(editor.inspector, raw.masks[target]));
        QVERIFY(inverted->isChecked() && inverted->isEnabled());
        verifyBytes(editor);
        verifyExport(editor);
    }

    void selectingOtherObjectsAndClearingSelectionDisableClipping_data()
    {
        QTest::addColumn<QString>("type");
        for (const char* type : {"part", "warp", "rotation", "empty", "multiple"}) QTest::newRow(type) << QString::fromLatin1(type);
    }

    void selectingOtherObjectsAndClearingSelectionDisableClipping()
    {
        QFETCH(QString, type);
        Editor editor;
        QVERIFY(editor.load("Rice"));
        editor.inspector.SetSelection("ArtMesh9", "artmesh");
        auto* inverted = editor.inspector.findChild<QCheckBox*>("maskInverted");
        auto* masks = editor.inspector.findChild<QPushButton*>("editMasks");
        QVERIFY(inverted && masks && inverted->isEnabled() && inverted->isChecked() && masks->isEnabled());
        if (type == QLatin1String("empty")) editor.inspector.ClearSelection();
        else if (type == QLatin1String("multiple")) editor.inspector.SetSelections({"ArtMesh9", "ArtMesh165"}, {"artmesh", "artmesh"});
        else if (type == QLatin1String("part")) editor.inspector.SetSelection(editor.document.Parts().front().id, type);
        else {
            const auto kind = type == QLatin1String("warp") ? CModelDeformer::Warp : CModelDeformer::Rotation;
            const auto found = std::find_if(editor.document.Deformers().cbegin(), editor.document.Deformers().cend(),
                                            [kind](const auto& deformer) { return deformer.type == kind; });
            QVERIFY(found != editor.document.Deformers().cend());
            editor.inspector.SetSelection(found->id, type);
        }
        QVERIFY(!inverted->isEnabled());
        QVERIFY(!inverted->isChecked());
        QVERIFY(!masks->isEnabled());
        QCOMPARE(editor.attempts, 0);
        QCOMPARE(editor.activations, 0);
        QVERIFY(!editor.document.CanUndoEdit());
    }

    void filteringNeverChangesSelectionAndClearIncludesHiddenItems()
    {
        Editor editor;
        QVERIFY(editor.load("Natori"));
        editor.inspector.SetSelection("ArtMesh3", "artmesh");
        const QVector<int> source{149, 3};
        QCOMPARE(editor.document.DrawableMaskIndices("ArtMesh3"), source);
        auto* count = editor.inspector.findChild<QLabel*>("maskCountLabel");
        QVERIFY(count);
        QVERIFY(count->text().contains('2'));
        bool checkedDialog = false;
        QVERIFY(editDialog(editor.inspector, [&](QDialog& dialog, QListWidget& list) {
            auto* search = dialog.findChild<QLineEdit*>("maskSearch");
            auto* selectedOnly = dialog.findChild<QCheckBox*>("selectedMasksOnly");
            auto* summary = dialog.findChild<QLabel*>("maskSelectionCount");
            auto* clear = dialog.findChild<QPushButton*>("clearMasks");
            QVERIFY(search && selectedOnly && summary && clear);
            QCOMPARE(dialog.objectName(), QStringLiteral("maskEditorDialog"));
            QVERIFY(summary->text().contains('2'));
            selectedOnly->setChecked(true);
            int visible = 0;
            for (int row = 0; row < list.count(); ++row) {
                auto* item = list.item(row);
                if (!item->isHidden()) {
                    ++visible;
                    QVERIFY(source.contains(item->data(Qt::UserRole).toInt()));
                }
            }
            QCOMPARE(visible, 2);
            search->setText(QStringLiteral("this-is-not-a-model-id"));
            for (int row = 0; row < list.count(); ++row) QVERIFY(list.item(row)->isHidden());
            int checked = 0;
            for (int row = 0; row < list.count(); ++row) checked += list.item(row)->checkState() == Qt::Checked;
            QCOMPARE(checked, 2);
            clear->click();
            for (int row = 0; row < list.count(); ++row) QCOMPARE(list.item(row)->checkState(), Qt::Unchecked);
            QVERIFY(summary->text().contains('0'));
            checkedDialog = true;
        }, false));
        QVERIFY(checkedDialog);
        QCOMPARE(editor.document.DrawableMaskIndices("ArtMesh3"), source);
        QCOMPARE(editor.attempts, 0);
        QVERIFY(setMasks(editor.inspector, {}));
        QVERIFY(editor.document.DrawableMaskIndices("ArtMesh3").isEmpty());
        QVERIFY(count->text() == QStringLiteral("无") || count->text().contains('0'));
        editor.inspector.Undo();
        QCOMPARE(editor.document.DrawableMaskIndices("ArtMesh3"), source);
        QVERIFY(count->text().contains('2'));
        bool preservedOrder = false;
        QVERIFY(editDialog(editor.inspector, [&](QDialog&, QListWidget& list) {
            for (int row = 0; row < list.count(); ++row)
                if (list.item(row)->data(Qt::UserRole).toInt() == 2) list.item(row)->setCheckState(Qt::Checked);
            preservedOrder = true;
        }));
        QVERIFY(preservedOrder);
        QCOMPARE(editor.document.DrawableMaskIndices("ArtMesh3"), (QVector<int>{149, 3, 2}));
        QVERIFY(count->text().contains('3'));
        verifyBytes(editor);
    }

    void staleMaskDialogDoesNotOverwriteConcurrentChanges()
    {
        Editor editor;
        QVERIFY(editor.load("Natori"));
        editor.inspector.SetSelection("ArtMesh3", "artmesh");
        QSignalSpy history(&editor.inspector, &CInspectorPanel::HistoryChanged);
        QSignalSpy changed(&editor.inspector, &CInspectorPanel::DrawableMaskIndicesChanged);
        QSignalSpy rejected(&editor.inspector, &CInspectorPanel::EditRejected);
        QVERIFY(editDialog(editor.inspector, [&](QDialog&, QListWidget& list) {
            for (int row = 0; row < list.count(); ++row) list.item(row)->setCheckState(Qt::Unchecked);
            QVERIFY(editor.document.SetDrawableMaskIndices("ArtMesh3", {2, 3}));
        }));
        QCOMPARE(editor.document.DrawableMaskIndices("ArtMesh3"), (QVector<int>{2, 3}));
        QCOMPARE(editor.attempts, 0);
        QCOMPARE(history.size(), 0);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(rejected.size(), 1);
        QVERIFY(!editor.document.CanUndoEdit());
    }

    void replacingTheDocumentWhileTheDialogIsOpenRejectsItsOldSelection()
    {
        Editor editor;
        QVERIFY(editor.load("Natori"));
        CModelDocument replacement;
        QVERIFY(replacement.Load(QString(TEST_RESOURCES) + "/Rice/Rice.moc3"));
        const auto previous = editor.document.DrawableMaskIndices("ArtMesh3");
        const auto replacementRevision = replacement.Revision();
        editor.inspector.SetSelection("ArtMesh3", "artmesh");
        QSignalSpy rejected(&editor.inspector, &CInspectorPanel::EditRejected);
        QSignalSpy changed(&editor.inspector, &CInspectorPanel::DrawableMaskIndicesChanged);
        QVERIFY(editDialog(editor.inspector, [&](QDialog&, QListWidget& list) {
            for (int row = 0; row < list.count(); ++row) list.item(row)->setCheckState(Qt::Unchecked);
            editor.inspector.SetDocument(&replacement);
            editor.inspector.SetSelection("ArtMesh9", "artmesh");
        }));
        QCOMPARE(editor.document.DrawableMaskIndices("ArtMesh3"), previous);
        QCOMPARE(replacement.Revision(), replacementRevision);
        QVERIFY(replacement.DrawableMaskIndexOverrides().isEmpty());
        QCOMPARE(editor.attempts, 0);
        QCOMPARE(rejected.size(), 1);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(editor.inspector.GetSelectionId(), QStringLiteral("ArtMesh9"));
    }

    void lockedClippingTargetsNeverReachThePublisher()
    {
        Editor editor;
        QVERIFY(editor.load("Rice"));
        const QString id = QStringLiteral("ArtMesh9");
        const auto masks = editor.document.DrawableMaskIndices(id);
        const bool inverted = editor.document.DrawableMaskInverted(id);
        editor.document.SetLocked("artmesh", id, true);
        editor.inspector.SetLockedState(editor.document.LockedKeys());
        editor.inspector.SetSelection(id, "artmesh");
        auto* checkbox = editor.inspector.findChild<QCheckBox*>("maskInverted");
        auto* button = editor.inspector.findChild<QPushButton*>("editMasks");
        QVERIFY(checkbox && button && !checkbox->isEnabled() && !button->isEnabled());
        const auto revision = editor.document.Revision();
        QString error;
        QVERIFY(!editor.controller.Submit({id, "artmesh", CModelEdit::MaskIndices,
            QVariant::fromValue(masks), QVariant::fromValue(QVector<int>{})}, editor.current, -1, &error));
        QVERIFY(!editor.controller.Submit({id, "artmesh", CModelEdit::MaskInverted,
            inverted, !inverted}, editor.current, -1, &error));
        QCOMPARE(editor.attempts, 0);
        QCOMPARE(editor.document.Revision(), revision);
        QCOMPARE(editor.document.DrawableMaskIndices(id), masks);
        QCOMPARE(editor.document.DrawableMaskInverted(id), inverted);
        QVERIFY(!editor.document.CanUndoEdit());
    }

    void clippingChangesPreserveAnActiveEditingPose()
    {
        Editor editor;
        QVERIFY(editor.load("Mark"));
        QString id;
        int selected = -1;
        QString error;
        for (int index = 0; index < editor.document.Drawables().size(); ++index) {
            const auto& drawable = editor.document.Drawables()[index];
            QMap<QString, float> pose;
            if (drawable.keyforms.size() > 1 && editor.document.KeyformParameterValues(drawable.id, "artmesh", 1, pose) &&
                editor.controller.Activate(drawable.id, 1, editor.current, &error)) {
                id = drawable.id;
                selected = index;
                break;
            }
        }
        QVERIFY2(!id.isEmpty(), qPrintable(error));
        editor.inspector.SetSelection(id, "artmesh");
        const auto pose = editor.controller.Pose();
        const auto values = editor.document.ParameterValues();
        const auto target = editor.controller.Target();
        const int keyform = editor.controller.Keyform();
        const int before = editor.publications;
        QVector<int> masks;
        for (int index = 0; masks.size() < 2; ++index) if (index != selected) masks.append(index);
        QVERIFY(setMasks(editor.inspector, masks));
        QCOMPARE(editor.publications, before + 1);
        QCOMPARE(editor.controller.Target(), target);
        QCOMPARE(editor.controller.Keyform(), keyform);
        QCOMPARE(editor.controller.Pose(), pose);
        QCOMPARE(editor.publishedPose, pose);
        QCOMPARE(editor.document.ParameterValues(), values);
        QVERIFY(editor.controller.IsActive() && editor.publishedEditing);
        QCOMPARE(editor.activations, 0);
        editor.inspector.Undo();
        editor.inspector.Redo();
        QCOMPARE(editor.publications, before + 3);
        QCOMPARE(editor.controller.Target(), target);
        QCOMPARE(editor.controller.Keyform(), keyform);
        QCOMPARE(editor.controller.Pose(), pose);
        QCOMPARE(editor.publishedPose, pose);
        QCOMPARE(editor.document.ParameterValues(), values);
        verifyBytes(editor);
    }

    void nativeEmptyInverseRemainsMetadataUntilMasksAreAdded_data()
    {
        QTest::addColumn<QString>("id");
        QTest::newRow("ArtMesh168") << QStringLiteral("ArtMesh168");
        QTest::newRow("ArtMesh169") << QStringLiteral("ArtMesh169");
    }

    void nativeEmptyInverseRemainsMetadataUntilMasksAreAdded()
    {
        QFETCH(QString, id);
        Editor editor;
        QVERIFY(editor.load("Rice"));
        RawClipping raw;
        QVERIFY(raw.read(editor.document.SourceBytes()));
        const int index = raw.ids.indexOf(id);
        QVERIFY(index >= 0);
        QVERIFY(raw.masks[index].isEmpty());
        QCOMPARE(raw.flags[index], quint8(0x08));
        editor.inspector.SetSelection(id, "artmesh");
        auto* inverted = editor.inspector.findChild<QCheckBox*>("maskInverted");
        QVERIFY(inverted && inverted->isChecked() && !inverted->isEnabled());
        QVERIFY(setMasks(editor.inspector, {0, 1}));
        QVERIFY(inverted->isChecked() && inverted->isEnabled());
        verifyBytes(editor);
        editor.inspector.Undo();
        QVERIFY(inverted->isChecked() && !inverted->isEnabled());
        verifyBytes(editor);
    }

    void explicitFalseSurvivesSessionAndOverridesNativeInversion()
    {
        Editor editor;
        QVERIFY(editor.load("Rice"));
        const QString id = QStringLiteral("ArtMesh9");
        editor.inspector.SetSelection(id, "artmesh");
        auto* inverted = editor.inspector.findChild<QCheckBox*>("maskInverted");
        QVERIFY(inverted && inverted->isChecked() && inverted->isEnabled());
        inverted->click();
        QVERIFY(editor.document.DrawableMaskInvertedOverrides().contains(id));
        QVERIFY(!editor.document.DrawableMaskInverted(id));
        CModelSession session;
        session.sourceMoc = editor.path;
        session.sourceDigest = editor.document.SourceDigest();
        session.drawableMaskInverted = editor.document.DrawableMaskInvertedOverrides();
        CModelSession decoded;
        QString error;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        QVERIFY(decoded.drawableMaskInverted.contains(id));
        QVERIFY(!decoded.drawableMaskInverted.value(id));
        CModelDocument source, restored;
        QVERIFY(source.Load(editor.path));
        QVERIFY(source.DrawableMaskInverted(id));
        QVERIFY2(decoded.RestoreDocument(source, restored, &error), qPrintable(error));
        QVERIFY(!restored.DrawableMaskInverted(id));
        QVERIFY2(editor.controller.Restore(std::move(restored), &error), qPrintable(error));
        editor.refresh();
        QVERIFY(!inverted->isChecked() && inverted->isEnabled());
        verifyBytes(editor);
        verifyExport(editor);
    }
    void repairedMaskReferencesClearTheEffectiveDiagnostic()
    {
        Editor editor;
        QVERIFY(editor.load("Mark"));
        int target = -1;
        for (int i = 0; i < editor.document.Drawables().size(); ++i)
            if (!editor.document.Drawables()[i].maskIndices.isEmpty()) { target = i; break; }
        QVERIFY(target >= 0);
        auto bytes = editor.document.SourceBytes();
        const auto word = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
        const int begin = word(word(0xfc) + target * 4);
        const int invalid = editor.document.Drawables().size();
        qToLittleEndian<qint32>(invalid, bytes.data() + word(0x180) + begin * 4);
        CModelDocument damaged;
        QString error;
        QVERIFY2(damaged.LoadBytes(bytes, editor.path, &error), qPrintable(error));
        bool reported = false;
        for (const auto& diagnostic : damaged.Validate())
            reported |= diagnostic.code == QLatin1String("mask_range") && diagnostic.index == target;
        QVERIFY(reported);
        const QString id = damaged.Drawables()[target].id;
        const QVector<int> repaired{target == 0 ? 1 : 0};
        QVERIFY(damaged.SetDrawableMaskIndices(id, repaired));
        for (const auto& diagnostic : damaged.Validate())
            QVERIFY(diagnostic.code != QLatin1String("mask_range") || diagnostic.index != target);
        QByteArray preview;
        QVERIFY2(damaged.BuildPreviewMoc(preview, &error), qPrintable(error));
        RawClipping raw;
        QVERIFY(raw.read(preview));
        QCOMPARE(raw.masks[target], repaired);
    }

    void baselineMaskListsRemainRestorable_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("self") << 0;
        QTest::newRow("duplicate") << 1;
        QTest::newRow("long-duplicate") << 2;
    }

    void originalMaskRowsAreVisibleAndUnchangedConfirmationIsExact_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("self") << 0;
        QTest::newRow("duplicate") << 1;
        QTest::newRow("long-duplicate") << 2;
        QTest::newRow("past-end") << 3;
        QTest::newRow("negative") << 4;
    }

    void originalMaskRowsAreVisibleAndUnchangedConfirmationIsExact()
    {
        QFETCH(int, kind);
        CModelDocument source;
        const QString path = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        QVERIFY(source.Load(path));
        const int meshCount = source.Drawables().size();
        const QVector<int> original = kind == 0 ? QVector<int>{0} : kind == 1 ? QVector<int>{1, 1} :
            kind == 2 ? QVector<int>(meshCount + 1, 1) : kind == 3 ? QVector<int>{meshCount} : QVector<int>{-1};
        QByteArray bytes = source.SourceBytes();
        const auto word = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
        const int pool = (bytes.size() + 63) & ~63;
        bytes.append(QByteArray(pool + original.size() * 4 - bytes.size(), '\0'));
        for (int index = 0; index < original.size(); ++index)
            qToLittleEndian<qint32>(original[index], bytes.data() + pool + index * 4);
        qToLittleEndian<qint32>(pool, bytes.data() + 0x180);
        qToLittleEndian<qint32>(original.size(), bytes.data() + word(0x40) + 17 * 4);
        qToLittleEndian<qint32>(0, bytes.data() + word(0xfc));
        qToLittleEndian<qint32>(original.size(), bytes.data() + word(0x100));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("source.moc3"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        QCOMPARE(sourceFile.write(bytes), bytes.size());
        sourceFile.close();
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(sourcePath, &error), qPrintable(error));
        const QString id = document.Drawables().front().id;
        CKeyformEditController controller;
        controller.SetDocument(&document);
        QByteArray published;
        int publications = 0;
        controller.SetPublisher([&](const CModelDocument& candidate, const QMap<QString, float>&, bool editing, QString* issue) {
            if (editing || !candidate.BuildPreviewMoc(published, issue)) return false;
            ++publications;
            return true;
        }, [](QString*) { return true; });
        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, "artmesh");
        panel.SetEditHandler([&](const CModelEdit& command, QString* issue) { return controller.Submit(command, issue); });
        panel.SetHistoryHandler([&](bool undo, CModelEdit& command, QString* issue) { return controller.History(undo, command, issue); });
        QSignalSpy history(&panel, &CInspectorPanel::HistoryChanged);
        QSignalSpy changes(&panel, &CInspectorPanel::DrawableMaskIndicesChanged);
        QSignalSpy rejected(&panel, &CInspectorPanel::EditRejected);
        const auto revision = document.Revision();
        bool originalVisible = false;
        QVERIFY(editDialog(panel, [&](QDialog& dialog, QListWidget& list) {
            auto* onlySelected = dialog.findChild<QCheckBox*>("selectedMasksOnly");
            QVERIFY(onlySelected);
            onlySelected->setChecked(true);
            int visible = 0, matches = 0;
            for (int row = 0; row < list.count(); ++row) {
                const auto* item = list.item(row);
                if (!item->isHidden()) ++visible;
                if (item->data(Qt::UserRole).toInt() != original.first()) continue;
                ++matches;
                QVERIFY(!item->isHidden());
                QCOMPARE(item->checkState(), Qt::Checked);
                if (kind == 0) QVERIFY(item->text().contains(QStringLiteral("自身")));
                if (kind >= 3) QVERIFY(item->text().contains(QStringLiteral("无效索引")));
            }
            QCOMPARE(matches, 1);
            QCOMPARE(visible, 1);
            originalVisible = true;
        }));
        QVERIFY(originalVisible);
        QCOMPARE(document.DrawableMaskIndices(id), original);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(publications, 0);
        QCOMPARE(history.size(), 0);
        QCOMPARE(changes.size(), 0);
        QCOMPARE(rejected.size(), 0);
        QVERIFY(!document.CanUndoEdit());
        QByteArray unchanged;
        QVERIFY2(document.BuildPreviewMoc(unchanged, &error), qPrintable(error));
        QCOMPARE(unchanged, bytes);

        const auto verifySession = [&](bool cleared) {
            const auto overrides = document.DrawableMaskIndexOverrides();
            QCOMPARE(overrides.contains(id), cleared);
            if (cleared) QVERIFY(overrides.value(id).isEmpty());
            CModelSession session;
            session.sourceMoc = sourcePath;
            session.sourceDigest = document.SourceDigest();
            session.drawableMaskIndices = overrides;
            CModelSession decoded;
            QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
            QCOMPARE(decoded.drawableMaskIndices, overrides);
            const QString sessionPath = directory.filePath(QStringLiteral("clipping.session.json"));
            QVERIFY2(decoded.Save(sessionPath, &error), qPrintable(error));
            CModelSession loaded;
            QVERIFY2(CModelSession::Load(sessionPath, &loaded, &error), qPrintable(error));
            QCOMPARE(loaded.drawableMaskIndices, overrides);
            CModelDocument fresh, restored;
            QVERIFY2(fresh.Load(loaded.sourceMoc, &error), qPrintable(error));
            QCOMPARE(fresh.SourceBytes(), bytes);
            QVERIFY2(loaded.RestoreDocument(fresh, restored, &error), qPrintable(error));
            QCOMPARE(restored.DrawableMaskIndices(id), cleared ? QVector<int>{} : original);
            QCOMPARE(restored.DrawableMaskIndexOverrides().contains(id), cleared);
            QVERIFY(!restored.CanUndoEdit());
            QByteArray preview, expected;
            QVERIFY2(restored.BuildPreviewMoc(preview, &error), qPrintable(error));
            QVERIFY2(document.BuildPreviewMoc(expected, &error), qPrintable(error));
            QCOMPARE(preview, expected);
            if (!cleared) QCOMPARE(preview, bytes);
        };
        for (bool clearButton : {false, true}) {
            bool cleared = false;
            QVERIFY(editDialog(panel, [&](QDialog& dialog, QListWidget& list) {
                if (clearButton) {
                    auto* search = dialog.findChild<QLineEdit*>("maskSearch");
                    auto* clear = dialog.findChild<QPushButton*>("clearMasks");
                    QVERIFY(search && clear && clear->isEnabled());
                    search->setText(QStringLiteral("no-visible-mask-items"));
                    for (int row = 0; row < list.count(); ++row) QVERIFY(list.item(row)->isHidden());
                    clear->click();
                } else {
                    for (int row = 0; row < list.count(); ++row)
                        if (list.item(row)->data(Qt::UserRole).toInt() == original.first())
                            list.item(row)->setCheckState(Qt::Unchecked);
                }
                cleared = true;
            }));
            QVERIFY(cleared);
            QVERIFY(document.DrawableMaskIndices(id).isEmpty());
            QVERIFY(document.CanUndoEdit());
            verifySession(true);
            panel.Undo();
            QCOMPARE(document.DrawableMaskIndices(id), original);
            QCOMPARE(published, bytes);
            QVERIFY(!document.CanUndoEdit());
            verifySession(false);
        }
        QCOMPARE(publications, 4);
        QCOMPARE(history.size(), 4);
        QCOMPARE(changes.size(), 4);
        QCOMPARE(rejected.size(), 0);
    }

    void baselineMaskListsRemainRestorable()
    {
        QFETCH(int, kind);
        CModelDocument source;
        const QString path = QString(TEST_RESOURCES) + "/Wanko/Wanko.moc3";
        QVERIFY(source.Load(path));
        const QVector<int> original = kind == 0 ? QVector<int>{0} :
            kind == 1 ? QVector<int>{1, 1} : QVector<int>(source.Drawables().size() + 1, 1);
        QByteArray bytes = source.SourceBytes();
        const auto word = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
        const int pool = (bytes.size() + 63) & ~63;
        bytes.append(QByteArray(pool + original.size() * 4 - bytes.size(), '\0'));
        for (int i = 0; i < original.size(); ++i) qToLittleEndian<qint32>(original[i], bytes.data() + pool + i * 4);
        qToLittleEndian<qint32>(pool, bytes.data() + 0x180);
        qToLittleEndian<qint32>(original.size(), bytes.data() + word(0x40) + 17 * 4);
        qToLittleEndian<qint32>(0, bytes.data() + word(0xfc));
        qToLittleEndian<qint32>(original.size(), bytes.data() + word(0x100));
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(bytes, path, &error), qPrintable(error));
        const QString id = document.Drawables().front().id;
        QCOMPARE(document.DrawableMaskIndices(id), original);
        CKeyformEditController controller;
        controller.SetDocument(&document);
        QByteArray published;
        int attempts = 0;
        controller.SetPublisher([&](const CModelDocument& candidate, const QMap<QString, float>&, bool editing, QString* issue) {
            ++attempts;
            if (editing) return false;
            return candidate.BuildPreviewMoc(published, issue);
        }, [](QString*) { return true; });
        const CModelEdit clear{id, "artmesh", CModelEdit::MaskIndices, QVariant::fromValue(original),
                              QVariant::fromValue(QVector<int>{}), -1, false};
        QVERIFY2(controller.Submit(clear, &error), qPrintable(error));
        QVERIFY(document.DrawableMaskIndices(id).isEmpty());
        CModelEdit restored;
        QVERIFY2(controller.History(true, restored, &error), qPrintable(error));
        QCOMPARE(document.DrawableMaskIndices(id), original);
        QCOMPARE(published, bytes);
        for (const auto& diagnostic : document.Validate())
            QVERIFY(diagnostic.code != QLatin1String("mask_override_range") && diagnostic.code != QLatin1String("mask_override_duplicate"));
        QVERIFY2(controller.History(false, restored, &error), qPrintable(error));
        QVERIFY(document.DrawableMaskIndices(id).isEmpty());
        const auto revision = document.Revision();
        const QVector<int> invalid = kind == 0 ? QVector<int>{1, 1} : QVector<int>{0};
        QVERIFY(!controller.Submit({id, "artmesh", CModelEdit::MaskIndices, QVariant::fromValue(QVector<int>{}),
                                    QVariant::fromValue(invalid), -1, false}, &error));
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(attempts, 3);
        CModelSession session;
        session.sourceMoc = path;
        session.sourceDigest = document.SourceDigest();
        session.drawableMaskIndices.insert(id, original);
        CModelDocument candidate;
        QVERIFY2(session.RestoreDocument(document, candidate, &error), qPrintable(error));
        QVERIFY2(controller.Restore(std::move(candidate), &error), qPrintable(error));
        QCOMPARE(document.DrawableMaskIndices(id), original);
        QCOMPARE(published, bytes);
    }

};

QTEST_MAIN(ClippingEditorTests)
#include "clipping_editor_tests.moc"
