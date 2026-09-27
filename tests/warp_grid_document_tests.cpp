#include "warp_grid_fixture.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmodelsavefingerprint.h"
#include <QtTest>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <stdexcept>

using namespace live2dstruct;
using Raw = CMocDeformerStructureBuilder;
using Controller = CKeyformEditController;
using Pairs = CGeometryObjectPairs;
namespace Fixture = WarpGridFixture;

namespace {
QByteArray preview(const CModelDocument& document)
{
    QByteArray result;
    document.BuildPreviewMoc(result);
    return result;
}

QByteArray fingerprint(const CModelDocument& document)
{
    QByteArray result;
    CModelSaveFingerprint::Capture(document, result);
    return result;
}

CDeformerStructureRequest anchored(
    const CModelDocument& document, decltype(CDeformerStructureRequest::operation) operation)
{
    return { std::move(operation), document.SourceDigest(),
        QCryptographicHash::hash(preview(document), QCryptographicHash::Sha256) };
}

Raw::ResizeWarpRequest resizeFor(const CModelDocument& document, bool blend = false)
{
    auto reader = CReader::FromBytes(preview(document));
    CSectionTable table(reader);
    const int index = Fixture::target(table, blend);
    if (index < 0)
        return { };
    const auto* source = table.warp_deformers[table.deformers[index]->specific_sources_indices];
    return { table.deformers[index]->ids, source->rows + 1, source->columns + 2 };
}

CModelSession capture(const CModelDocument& document)
{
    CModelSession result;
    result.version = 13;
    result.sourceMoc = document.SourcePath();
    result.sourceDigest = document.SourceDigest();
    result.structure = document.ParameterStructure();
    result.meshStructure = document.HasMeshStructure();
    result.materialStructure = document.HasMaterialStructure();
    result.drawableMaterials = document.DrawableMaterialOverrides();
    result.deformerStructure = document.HasDeformerStructure();
    if (result.deformerStructure)
        result.sourceDeformerIds = document.SourceDeformerIds();
    result.deformerDisplayNames = document.DeformerDisplayNames();
    result.parameterMetadata = document.ParameterMetadataOverrides();
    for (auto value = document.ParameterValues().cbegin(); value != document.ParameterValues().cend(); ++value)
        result.parameters.insert(value.key(), value.value());
    result.parameterOrganization = document.ParameterOrganization();
    result.geometryObjectPairs = document.GeometryObjectPairs();
    result.visibility = document.VisibilityState();
    for (const auto& key : document.LockedKeys())
        result.locked.insert(key, true);
    result.drawableOpacity = document.DrawableOpacityOverrides();
    result.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    result.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    result.drawableIndices = document.DrawableIndexOverrides();
    result.drawableMaskIndices = document.DrawableMaskIndexOverrides();
    result.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    result.deformerParentIndices = document.DeformerParentOverrides();
    result.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    result.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    return result;
}

QMap<QString, float> values(const CModelDocument& document)
{
    QMap<QString, float> result;
    for (const auto& parameter : document.Parameters())
        result.insert(parameter.id, document.ParameterValue(parameter.id));
    return result;
}

bool insertWarp(CModelDocument& document, const QString& id, QString* error, const QString& parent = { }, int part = -1)
{
    Raw::InsertRequest request;
    request.id = id;
    request.parentId = parent;
    request.partIndex = part;
    request.shape = Raw::Warp { };
    CModelEdit command;
    return document.MakeDeformerStructureEdit(anchored(document, request), command, error)
        && document.ExecuteEdit(command);
}

struct Runtime
{
    QByteArray bytes;
    Controller::EditorState editor;
    bool reject = false, throwing = false;
    int calls = 0;
    void connect(Controller& controller)
    {
        controller.SetPublisher(
            [this](const CModelDocument& document, const auto& pose, bool, QString* error) {
                QByteArray candidate;
                if (!document.BuildRuntimeMoc(candidate, error))
                    return false;
                bytes.swap(candidate);
                editor.pose = pose;
                return true;
            },
            [](QString*) { return true; });
        controller.SetDeformerPublisher(
            [this](const CModelDocument& document, const CModelEdit& command, bool forward, QString* error) {
                ++calls;
                Controller::EditorState planned;
                if (!Controller::ReadDeformerEditorState(command, forward, planned, error))
                    return false;
                if (reject || throwing) {
                    command.runtime->before = QStringLiteral("publisher mutation");
                    command.runtime->afterEditor = QStringLiteral("publisher mutation");
                    if (throwing)
                        throw std::runtime_error("publication failed");
                    if (error)
                        *error = QStringLiteral("publication rejected");
                    return false;
                }
                QByteArray candidate;
                if (!document.BuildRuntimeMoc(candidate, error))
                    return false;
                (forward ? command.runtime->before : command.runtime->after) = bytes;
                (forward ? command.runtime->after : command.runtime->before) = candidate;
                bytes.swap(candidate);
                editor = planned;
                return true;
            });
    }
};
}

class WarpGridDocumentTests : public QObject
{
    Q_OBJECT
    CModelDocument m_original;

    void verifyRoundTrip(const CModelDocument& expected, const CModelDocument& loaded)
    {
        const auto session = capture(expected);
        const auto bytes = session.ToJson();
        QVERIFY(!bytes.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(bytes).object()["version"].toInt(), 13);
        CModelSession decoded;
        QString error;
        QVERIFY2(CModelSession::FromJson(bytes, &decoded, &error), qPrintable(error));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY2(decoded.Save(directory.filePath("grid.session"), &error), qPrintable(error));
        QVERIFY2(CModelSession::Load(directory.filePath("grid.session"), &decoded, &error), qPrintable(error));
        for (const auto* source : { &loaded, &expected }) {
            CModelDocument restored;
            QVERIFY2(decoded.RestoreDocument(*source, restored, &error), qPrintable(error));
            QCOMPARE(restored.AuthoringStructureState(), expected.AuthoringStructureState());
            QCOMPARE(restored.DeformerDisplayNames(), expected.DeformerDisplayNames());
            QCOMPARE(restored.SourceDeformerIds(), expected.SourceDeformerIds());
            QCOMPARE(preview(restored), preview(expected));
            QCOMPARE(fingerprint(restored), fingerprint(expected));
            QCOMPARE(restored.SourceBytes(), loaded.SourceBytes());
            QVERIFY(!restored.CanUndoEdit());
            QVERIFY(!restored.CanRedoEdit());
        }
    }

private slots:
    void initTestCase()
    {
        QString error;
        QVERIFY2(m_original.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
    }

    void allFormsNamesAndHistoryRoundTrip_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<bool>("blend");
        QTest::newRow("Mark-ordinary") << QString("Mark") << false;
        QTest::newRow("Rice-ordinary") << QString("Rice") << false;
        if (!qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty())
            QTest::newRow("Mao-blend") << QString("Mao") << true;
    }

    void allFormsNamesAndHistoryRoundTrip()
    {
        QFETCH(QString, model);
        QFETCH(bool, blend);
        QString error;
        CModelDocument document;
        const auto source = DeformerReplayFixture::read(model);
        QVERIFY2(document.LoadBytes(source, model + ".moc3", &error), qPrintable(error));
        const auto loaded = document;
        const auto request = resizeFor(document, blend);
        QVERIFY(!request.id.isEmpty());
        CModelEdit name;
        QVERIFY(document.MakeDeformerDisplayNameEdit(request.id, QStringLiteral("网格 ✦ 名称"), name, &error));
        QVERIFY(document.ExecuteEdit(name));
        const auto names = document.DeformerDisplayNames();
        const auto sourceIds = document.SourceDeformerIds();
        const auto beforeState = document.AuthoringStructureState();
        const auto beforeBytes = preview(document);
        const auto beforeFingerprint = fingerprint(document);
        auto reader = CReader::FromBytes(beforeBytes);
        CSectionTable before(reader);
        const int index = Fixture::target(before, blend);
        const int specific = before.deformers[index]->specific_sources_indices;
        const auto oldRows = Fixture::formRows(before, specific);
        QVERIFY(!oldRows.isEmpty());
        if (blend)
            QVERIFY(std::any_of(
                oldRows.cbegin(), oldRows.cend(), [](const auto& row) { return row.first.startsWith("blend/"); }));
        CModelEdit command;
        const auto revision = document.Revision();
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, request), command, &error), qPrintable(error));
        QCOMPARE(document.AuthoringStructureState(), beforeState);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(document.ExecuteEdit(command));
        QCOMPARE(document.DeformerDisplayNames(), names);
        QCOMPARE(document.SourceDeformerIds(), sourceIds);
        const auto afterBytes = preview(document);
        const auto afterState = document.AuthoringStructureState();
        auto nextReader = CReader::FromBytes(afterBytes);
        CSectionTable after(nextReader);
        QVERIFY(after.IsValid());
        const int nextSpecific = after.deformers[index]->specific_sources_indices;
        const auto nextRows = Fixture::formRows(after, nextSpecific);
        const auto* warp = after.warp_deformers[nextSpecific];
        QCOMPARE(warp->rows, request.rows);
        QCOMPARE(warp->columns, request.columns);
        QCOMPARE(nextRows.size(), oldRows.size());
        for (int form = 0; form < oldRows.size(); ++form) {
            QCOMPARE(nextRows[form].first, oldRows[form].first);
            const auto expected
                = Fixture::oracle(before, specific, oldRows[form].second, request.rows, request.columns);
            const auto actual = Fixture::positions(after, nextRows[form].second, warp->vertex_counts);
            QVERIFY2(Fixture::close(actual, expected), qPrintable(oldRows[form].first));
            QCOMPARE(after.warp_deformer_keyforms[nextRows[form].second]->opacities,
                before.warp_deformer_keyforms[oldRows[form].second]->opacities);
        }
        QCOMPARE(Fixture::bindingMetadata(after, nextSpecific), Fixture::bindingMetadata(before, specific));
        QCOMPARE(Fixture::siblingPositions(after, request.id), Fixture::siblingPositions(before, request.id));
        QCOMPARE(Fixture::fixedColumns(afterBytes, after), Fixture::fixedColumns(beforeBytes, before));
        verifyRoundTrip(document, loaded);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), beforeState);
        QCOMPARE(preview(document), beforeBytes);
        QCOMPARE(fingerprint(document), beforeFingerprint);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.AuthoringStructureState(), afterState);
        QCOMPARE(preview(document), afterBytes);
        QCOMPARE(document.SourceBytes(), source);
    }

    void existingGeometryOverridesAreBakedBeforeSampling()
    {
        auto document = m_original;
        const auto request = resizeFor(document);
        auto forms = document.DeformerWarpKeyforms(request.id);
        QVERIFY(!forms.isEmpty());
        for (int form = 0; form < forms.size(); ++form) {
            forms[form].positions[0] += float(form + 1) * .1375f;
            forms[form].opacity = .625f;
        }
        QVERIFY(document.SetDeformerWarpKeyforms(request.id, forms));
        const auto mesh = document.Drawables().front().id;
        auto meshForm = document.DrawableKeyformPositions(mesh, 0);
        meshForm[0] += .375f;
        QVERIFY(document.SetDrawableKeyformPositions(mesh, 0, meshForm));
        const auto beforeBytes = preview(document);
        const auto beforeState = document.AuthoringStructureState();
        auto reader = CReader::FromBytes(beforeBytes);
        CSectionTable before(reader);
        const int index = Fixture::target(before), specific = before.deformers[index]->specific_sources_indices;
        CModelEdit edit;
        QString error;
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, request), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit));
        const auto changed = document.DeformerWarpKeyforms(request.id);
        QCOMPARE(changed.size(), forms.size());
        for (int form = 0; form < changed.size(); ++form) {
            const int row = before.warp_deformers[specific]->keyform_sources_begin_indices + form;
            QVERIFY(Fixture::close(
                changed[form].positions, Fixture::oracle(before, specific, row, request.rows, request.columns)));
            QCOMPARE(changed[form].opacity, .625f);
        }
        QCOMPARE(document.DrawableKeyformPositions(mesh, 0), meshForm);
        QVERIFY(document.DeformerWarpKeyformOverrides().isEmpty());
        QVERIFY(document.DrawableKeyformPositionOverrides().isEmpty());
        verifyRoundTrip(document, m_original);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), beforeState);
        QCOMPARE(document.DeformerWarpKeyformOverrides().value(request.id), forms);
        QCOMPARE(preview(document), beforeBytes);
        QVERIFY(document.RedoEdit());
    }

    void changedPairLayoutRequiresReviewAndUndoRestoresIt()
    {
        auto document = m_original;
        QString error;
        QVERIFY(insertWarp(document, "GridLeft", &error));
        QVERIFY(insertWarp(document, "GridRight", &error));
        Pairs::Pair pair;
        pair.id = "warp-pair";
        QVERIFY(Pairs::DescribeEndpoint(document, { Pairs::Type::Warp, "GridLeft" }, pair.first, &error));
        QVERIFY(Pairs::DescribeEndpoint(document, { Pairs::Type::Warp, "GridRight" }, pair.second, &error));
        for (int index = 0; index < pair.first.vertexCount; ++index)
            pair.firstVertexToSecondVertex.append(index);
        QVERIFY2(document.SetGeometryObjectPairs({ document.SourceDigest(), { pair } }, &error), qPrintable(error));
        const auto pairs = document.GeometryObjectPairs();
        CModelEdit edit;
        QVERIFY2(document.MakeDeformerStructureEdit(
                     anchored(document, Raw::ResizeWarpRequest { "GridLeft", 3, 4 }), edit, &error),
            qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit));
        const auto& changed = document.GeometryObjectPairs().pairs.front();
        QVERIFY(changed.requiresReview);
        QCOMPARE(changed.first, pairs.pairs.front().first);
        QCOMPARE(changed.firstVertexToSecondVertex, pairs.pairs.front().firstVertexToSecondVertex);
        int pairedKey = -1;
        QVERIFY(!Pairs::ResolveKeyform(changed, 0, true, pairedKey, &error));
        Pairs::Endpoint current;
        QVERIFY(Pairs::DescribeEndpoint(document, changed.first.object, current, &error));
        QCOMPARE(current.rows, 3);
        QCOMPARE(current.columns, 4);
        verifyRoundTrip(document, m_original);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.GeometryObjectPairs(), pairs);
        QVERIFY(document.RedoEdit());
        QVERIFY(document.GeometryObjectPairs().pairs.front().requiresReview);
    }

    void lockedTargetAndAncestorsRejectAtomically_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("target") << 0;
        QTest::newRow("deformer-parent") << 1;
        QTest::newRow("own-part") << 2;
        QTest::newRow("parent-part") << 3;
    }

    void lockedTargetAndAncestorsRejectAtomically()
    {
        QFETCH(int, kind);
        auto document = m_original;
        QString error;
        int childPart = -1;
        for (int index = 0; index < document.Parts().size(); ++index)
            if (document.Parts()[index].parentIndex >= 0) {
                childPart = index;
                break;
            }
        QVERIFY(childPart >= 0);
        QVERIFY(insertWarp(document, "GridParent", &error, { }, childPart));
        QVERIFY(insertWarp(document, "GridChild", &error, "GridParent", childPart));
        if (kind < 2)
            document.SetLocked("warp", kind == 0 ? "GridChild" : "GridParent", true);
        else
            document.SetLocked(
                "part", document.Parts()[kind == 2 ? childPart : document.Parts()[childPart].parentIndex].id, true);
        const auto state = document.AuthoringStructureState();
        const auto revision = document.Revision();
        const auto bytes = preview(document);
        CModelEdit sentinel;
        sentinel.name = "keep";
        QVERIFY(!document.MakeDeformerStructureEdit(
            anchored(document, Raw::ResizeWarpRequest { "GridChild", 3, 4 }), sentinel, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(sentinel.name, QString("keep"));
        QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(preview(document), bytes);
    }

    void staleRequestAndChangedCommandRejectAtomically_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("source-digest") << 0;
        QTest::newRow("authoring-digest") << 1;
        QTest::newRow("stale-command") << 2;
        QTest::newRow("changed-grid-request") << 3;
        QTest::newRow("changed-grid-payload") << 4;
    }

    void staleRequestAndChangedCommandRejectAtomically()
    {
        QFETCH(int, fault);
        auto document = m_original;
        auto request = anchored(document, resizeFor(document));
        CModelEdit edit;
        edit.name = "keep";
        QString error;
        if (fault == 0)
            request.sourceDigest.fill('x');
        if (fault == 1)
            request.expectedAuthoringDigest.fill('x');
        if (fault < 2) {
            QVERIFY(!document.MakeDeformerStructureEdit(request, edit, &error));
            QCOMPARE(edit.name, QString("keep"));
            QCOMPARE(document.AuthoringStructureState(), m_original.AuthoringStructureState());
            return;
        }
        QVERIFY(document.MakeDeformerStructureEdit(request, edit, &error));
        if (fault == 2) {
            auto forms = document.DeformerWarpKeyforms(edit.name);
            QVERIFY(!forms.isEmpty());
            forms[0].positions[0] += .5f;
            QVERIFY(document.SetDeformerWarpKeyforms(edit.name, forms));
        } else if (fault == 3) {
            auto changed = *edit.deformerRequest;
            ++std::get<Raw::ResizeWarpRequest>(changed.operation).rows;
            edit.deformerRequest = std::make_shared<const CDeformerStructureRequest>(changed);
        } else {
            CModelEdit another;
            ++std::get<Raw::ResizeWarpRequest>(request.operation).columns;
            QVERIFY(document.MakeDeformerStructureEdit(request, another, &error));
            edit.after = another.after;
        }
        const auto state = document.AuthoringStructureState();
        const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(edit));
        QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void borrowedRequestIsOwnedBeforeReturningCommand()
    {
        auto document = m_original;
        const auto resize = resizeFor(document);
        auto idStorage = resize.id;
        auto sourceStorage = document.SourceDigest();
        auto authorStorage = QCryptographicHash::hash(preview(document), QCryptographicHash::Sha256);
        auto* idData = idStorage.data();
        auto* sourceData = sourceStorage.data();
        auto* authorData = authorStorage.data();
        Raw::ResizeWarpRequest borrowed { QString::fromRawData(idData, idStorage.size()), resize.rows, resize.columns };
        CDeformerStructureRequest request { borrowed, QByteArray::fromRawData(sourceData, sourceStorage.size()),
            QByteArray::fromRawData(authorData, authorStorage.size()) };
        CModelEdit command;
        QString error;
        QVERIFY2(document.MakeDeformerStructureEdit(request, command, &error), qPrintable(error));
        idData[0] = QChar('!');
        sourceData[0] = '!';
        authorData[0] = '!';
        QCOMPARE(std::get<Raw::ResizeWarpRequest>(command.deformerRequest->operation).id, resize.id);
        QCOMPARE(command.deformerRequest->sourceDigest, document.SourceDigest());
        QVERIFY(document.ExecuteEdit(command));
        QVERIFY(document.UndoEdit());
        QVERIFY(document.RedoEdit());
    }

    void sameGridIsNoOpAndPreservesRedo()
    {
        auto document = m_original;
        QString error;
        auto resize = resizeFor(document);
        for (const auto& node : document.Deformers())
            if (node.id == resize.id) {
                resize.rows = node.rows;
                resize.columns = node.columns;
            }
        CModelEdit name;
        QVERIFY(document.MakeDeformerDisplayNameEdit(resize.id, "temporary name", name, &error));
        QVERIFY(document.ExecuteEdit(name));
        QVERIFY(document.UndoEdit());
        const auto state = document.AuthoringStructureState();
        const auto revision = document.Revision();
        CModelEdit noOp;
        QVERIFY(document.MakeDeformerStructureEdit(anchored(document, resize), noOp, &error));
        QCOMPARE(noOp.before, noOp.after);
        QVERIFY(!document.ExecuteEdit(noOp));
        QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.CanRedoEdit());
    }

    void resizedGridRemainsEditableAcrossTwoStructureCommands()
    {
        auto document = m_original;
        const auto initial = document.AuthoringStructureState();
        const auto first = resizeFor(document);
        QString error;
        CModelEdit edit;
        QVERIFY(document.MakeDeformerStructureEdit(anchored(document, first), edit, &error));
        QVERIFY(document.ExecuteEdit(edit));
        const auto resized = document.AuthoringStructureState();
        const auto previousForms = document.DeformerWarpKeyforms(first.id);
        auto changedForms = previousForms;
        QVERIFY(!changedForms.isEmpty());
        changedForms.front().positions.back() += .375f;
        edit = { first.id, QStringLiteral("warp"), CModelEdit::WarpKeyforms, QVariant::fromValue(previousForms),
            QVariant::fromValue(changedForms), -1, false };
        QVERIFY(document.ExecuteEdit(edit));
        const auto sculpted = document.AuthoringStructureState();
        const auto previousBytes = preview(document);
        auto reader = CReader::FromBytes(previousBytes);
        CSectionTable table(reader);
        int specific = -1;
        for (const auto* node : table.deformers)
            if (node->ids == first.id)
                specific = node->specific_sources_indices;
        QVERIFY(specific >= 0);
        const Raw::ResizeWarpRequest second { first.id, 2, 3 };
        QVERIFY(document.MakeDeformerStructureEdit(anchored(document, second), edit, &error));
        QVERIFY(document.ExecuteEdit(edit));
        const auto finalState = document.AuthoringStructureState();
        const auto finalForms = document.DeformerWarpKeyforms(first.id);
        for (int form = 0; form < finalForms.size(); ++form)
            QVERIFY(Fixture::close(finalForms[form].positions,
                Fixture::oracle(table, specific, table.warp_deformers[specific]->keyform_sources_begin_indices + form,
                    second.rows, second.columns)));
        verifyRoundTrip(document, m_original);
        for (const auto& state : { sculpted, resized, initial }) {
            QVERIFY(document.UndoEdit());
            QCOMPARE(document.AuthoringStructureState(), state);
        }
        for (const auto& state : { resized, sculpted, finalState }) {
            QVERIFY(document.RedoEdit());
            QCOMPARE(document.AuthoringStructureState(), state);
        }
    }

    void invalidResizedSessionLeavesOutputUnchanged_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("grid-count") << 0;
        QTest::newRow("unknown-name") << 1;
        QTest::newRow("unknown-source-id") << 2;
    }

    void invalidResizedSessionLeavesOutputUnchanged()
    {
        QFETCH(int, fault);
        auto document = m_original;
        const auto request = resizeFor(document);
        CModelEdit edit;
        QString error;
        QVERIFY(document.MakeDeformerStructureEdit(anchored(document, request), edit, &error));
        QVERIFY(document.ExecuteEdit(edit));
        auto session = capture(document);
        if (fault == 0) {
            const auto bytes = preview(document);
            DeformerReplayFixture::RawColumns raw(bytes);
            auto reader = CReader::FromBytes(bytes);
            CSectionTable table(reader);
            int specific = -1;
            for (const auto* node : table.deformers)
                if (node->ids == request.id)
                    specific = node->specific_sources_indices;
            QVERIFY(specific >= 0);
            const auto span = raw.spans.value("warp_deformers.vertex_counts");
            QVERIFY(session.structure.columns.contains(span.slot));
            auto& column = session.structure.columns[span.slot];
            qToLittleEndian(qint32(1), column.data() + specific * 4);
        } else if (fault == 1) {
            session.deformerDisplayNames->insert("unknown", "unknown display name");
        } else {
            session.sourceDeformerIds.insert("unknown-source");
        }
        auto output = m_original;
        const auto state = output.AuthoringStructureState();
        const auto revision = output.Revision();
        QVERIFY(!session.RestoreDocument(m_original, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(output.AuthoringStructureState(), state);
        QCOMPARE(output.Revision(), revision);
        QCOMPARE(preview(output), preview(m_original));
    }

    void invalidWarpReferencesRejectThroughEveryStructureEntry_data()
    {
        QTest::addColumn<int>("fault");
        QTest::addColumn<bool>("blend");
        for (const auto& item : { qMakePair("grid-count", 0), qMakePair("ordinary-form", 1),
                 qMakePair("ordinary-position", 2), qMakePair("ordinary-nan", 3) })
            QTest::newRow(item.first) << item.second << false;
        if (!qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty())
            for (const auto& item : { qMakePair("blend-target", 4), qMakePair("blend-binding", 5),
                     qMakePair("blend-form", 6), qMakePair("blend-position", 7), qMakePair("blend-nan", 8) })
                QTest::newRow(item.first) << item.second << true;
    }

    void invalidWarpReferencesRejectThroughEveryStructureEntry()
    {
        QFETCH(int, fault);
        QFETCH(bool, blend);
        const auto source = DeformerReplayFixture::read(blend ? "Mao" : "Mark");
        CModelDocument document;
        QString error;
        QVERIFY(document.LoadBytes(source, "reference-fixture.moc3", &error));
        auto reader = CReader::FromBytes(source);
        CSectionTable table(reader);
        const int target = Fixture::target(table, blend);
        QVERIFY(target >= 0);
        const int specific = table.deformers[target]->specific_sources_indices;
        const auto* warp = table.warp_deformers[specific];
        int form = warp->keyform_sources_begin_indices;
        DeformerReplayFixture::RawColumns raw(source);
        if (fault == 0)
            raw.set("warp_deformers.vertex_counts", specific, 1);
        else if (fault == 1)
            raw.set("warp_deformers.keyform_sources_begin_indices", specific, table.warp_deformer_keyforms.size());
        else if (fault == 2)
            raw.set("warp_deformer_keyforms.keyform_position_sources_begin_indices", form,
                table.keyform_positions->xys.size());
        else if (fault == 3)
            raw.setFloat("keyform_positions.xys",
                table.warp_deformer_keyforms[form]->keyform_position_sources_begin_indices,
                std::numeric_limits<float>::quiet_NaN());
        else {
            int shapeIndex = -1;
            for (int index = 0; index < table.blend_shapes_warp_deformers.size(); ++index)
                if (table.blend_shapes_warp_deformers[index]->target_indices == specific
                    && table.blend_shapes_warp_deformers[index]->blend_shape_keyform_binding_sources_counts > 0) {
                    shapeIndex = index;
                    break;
                }
            QVERIFY(shapeIndex >= 0);
            const int binding
                = table.blend_shapes_warp_deformers[shapeIndex]->blend_shape_keyform_binding_sources_begin_indices;
            form = table.blend_shape_keyform_bindings[binding]->keyform_sources_blend_shape_indices;
            QVERIFY(table.blend_shape_keyform_bindings[binding]->keyform_sources_blend_shape_counts > 0);
            if (fault == 4)
                raw.set("blend_shapes_warp_deformers.target_indices", shapeIndex, table.warp_deformers.size());
            else if (fault == 5)
                raw.set("blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_begin_indices", shapeIndex,
                    table.blend_shape_keyform_bindings.size());
            else if (fault == 6)
                raw.set("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", binding,
                    table.warp_deformer_keyforms.size());
            else if (fault == 7)
                raw.set("warp_deformer_keyforms.keyform_position_sources_begin_indices", form,
                    table.keyform_positions->xys.size());
            else
                raw.setFloat("keyform_positions.xys",
                    table.warp_deformer_keyforms[form]->keyform_position_sources_begin_indices,
                    std::numeric_limits<float>::quiet_NaN());
        }
        const auto state = document.AuthoringStructureState();
        const auto revision = document.Revision();
        for (int entry = 0; entry < 4; ++entry) {
            auto output = document;
            bool accepted = false;
            if (entry == 0)
                accepted = output.SetDeformerStructure(raw.state, false, document.SourceDeformerIds(), &error);
            else if (entry == 1)
                accepted = output.SetMaterialStructure(raw.state, false, true, document.SourceDeformerIds(), &error);
            else if (entry == 2)
                accepted = output.SetParameterStructure(raw.state, &error);
            else {
                auto session = capture(document);
                session.structure = raw.state;
                session.deformerStructure = true;
                session.sourceDeformerIds = document.SourceDeformerIds();
                accepted = session.RestoreDocument(document, output, &error);
            }
            QVERIFY2(!accepted, qPrintable(QString("entry %1 accepted invalid Warp data").arg(entry)));
            QVERIFY(!error.isEmpty());
            QCOMPARE(output.AuthoringStructureState(), state);
            QCOMPARE(output.Revision(), revision);
            QCOMPARE(output.SourceBytes(), source);
        }
    }

    void emptyBlendFormRangeRemainsRestorable()
    {
        if (qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty())
            return;
        const auto source = DeformerReplayFixture::read("Mao");
        auto reader = CReader::FromBytes(source);
        CSectionTable table(reader);
        const int index = Fixture::target(table, true);
        QVERIFY(index >= 0);
        const int specific = table.deformers[index]->specific_sources_indices;
        DeformerReplayFixture::RawColumns raw(source);
        bool changed = false;
        for (const auto* shape : table.blend_shapes_warp_deformers)
            if (shape->target_indices == specific && shape->blend_shape_keyform_binding_sources_counts > 0) {
                const int binding = shape->blend_shape_keyform_binding_sources_begin_indices;
                raw.set("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", binding, -1);
                raw.set("blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", binding, 0);
                changed = true;
                break;
            }
        QVERIFY(changed);
        const auto bytes = raw.encode();
        QVERIFY(!bytes.isEmpty());
        CModelDocument document;
        QString error;
        QVERIFY(document.LoadBytes(bytes, "empty-blend-range.moc3", &error));
        const auto loaded = document;
        CModelEdit command;
        QVERIFY2(document.MakeDeformerStructureEdit(anchored(document, resizeFor(document, true)), command, &error),
            qPrintable(error));
        QVERIFY(document.ExecuteEdit(command));
        verifyRoundTrip(document, loaded);
        QVERIFY(document.UndoEdit());
        QVERIFY(document.RedoEdit());
    }

    void zeroOrdinaryFormsKeepLegacyStructureRestorable()
    {
        if (qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC").isEmpty())
            return;
        const auto source = DeformerReplayFixture::read("Mao");
        auto reader = CReader::FromBytes(source);
        CSectionTable table(reader);
        const int index = Fixture::target(table, true);
        QVERIFY(index >= 0);
        const int specific = table.deformers[index]->specific_sources_indices;
        DeformerReplayFixture::RawColumns raw(source);
        raw.set("warp_deformers.keyform_sources_begin_indices", specific, -1);
        raw.set("warp_deformers.keyform_sources_counts", specific, 0);
        raw.set("warp_deformers.keyform_binding_sources_indices", specific, -1);
        raw.set("deformers.keyform_binding_sources_indices", index, -1);
        const auto bytes = raw.encode();
        QVERIFY(!bytes.isEmpty());
        CModelDocument original;
        QString error;
        QVERIFY(original.LoadBytes(bytes, "blend-only-legacy.moc3", &error));
        QVERIFY(original.DeformerWarpKeyforms(table.deformers[index]->ids).isEmpty());
        for (bool material : { false, true }) {
            auto document = original;
            QVERIFY2(material ? document.SetMaterialStructure({ }, false, false, { }, &error)
                              : document.SetParameterStructure({ }, &error),
                qPrintable(error));
            auto session = capture(document);
            CModelDocument restored;
            QVERIFY2(session.RestoreDocument(original, restored, &error), qPrintable(error));
            QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
            QCOMPARE(restored.SourceBytes(), bytes);
        }
    }

    void controllerPublicationFailureAndActiveKeyformRoundTrip_data()
    {
        QTest::addColumn<bool>("throwing");
        QTest::newRow("false") << false;
        QTest::newRow("exception") << true;
    }

    void controllerPublicationFailureAndActiveKeyformRoundTrip()
    {
        QFETCH(bool, throwing);
        auto document = m_original;
        Controller controller;
        controller.SetDocument(&document);
        Runtime runtime;
        runtime.connect(controller);
        QString error;
        Raw::ResizeWarpRequest request;
        for (const auto& node : document.Deformers()) {
            if (node.type != CModelDeformer::Warp || node.warpKeyforms.size() < 2)
                continue;
            if (controller.Activate(node.id, "warp", 1, values(document), &error)) {
                request = { node.id, node.rows + 1, node.columns + 2 };
                break;
            }
        }
        QVERIFY2(!request.id.isEmpty(), qPrintable(error));
        const auto pose = controller.Pose();
        const auto state = document.AuthoringStructureState();
        const auto bytes = runtime.bytes;
        const auto revision = document.Revision();
        CModelEdit edit;
        QVERIFY(document.MakeDeformerStructureEdit(anchored(document, request), edit, &error));
        runtime.reject = !throwing;
        runtime.throwing = throwing;
        QVERIFY(!controller.Submit(edit, &error));
        QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(runtime.bytes, bytes);
        QCOMPARE(controller.Target(), request.id);
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(controller.Pose(), pose);
        QVERIFY(!document.CanUndoEdit());
        runtime.reject = runtime.throwing = false;
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        const auto changedState = document.AuthoringStructureState();
        const auto changedBytes = runtime.bytes;
        QCOMPARE(controller.Target(), request.id);
        QCOMPARE(controller.Type(), QString("warp"));
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(controller.Pose(), pose);
        QCOMPARE(runtime.editor.target, request.id);
        CModelEdit history;
        QVERIFY2(controller.History(true, history, &error), qPrintable(error));
        QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(runtime.bytes, bytes);
        QCOMPARE(controller.Target(), request.id);
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(controller.Pose(), pose);
        runtime.reject = !throwing;
        runtime.throwing = throwing;
        const auto undoRevision = document.Revision();
        QVERIFY(!controller.History(false, history, &error));
        QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(document.Revision(), undoRevision);
        QCOMPARE(runtime.bytes, bytes);
        QVERIFY(document.CanRedoEdit());
        runtime.reject = runtime.throwing = false;
        QVERIFY2(controller.History(false, history, &error), qPrintable(error));
        QCOMPARE(document.AuthoringStructureState(), changedState);
        QCOMPARE(runtime.bytes, changedBytes);
        QCOMPARE(controller.Keyform(), 1);
        QCOMPARE(controller.Pose(), pose);
    }
};

QTEST_GUILESS_MAIN(WarpGridDocumentTests)
#include "warp_grid_document_tests.moc"
