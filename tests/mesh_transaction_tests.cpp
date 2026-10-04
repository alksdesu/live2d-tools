#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmesheditplanner.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"
#include "wide_mesh_fixture.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <type_traits>

using namespace live2dstruct;
static_assert(std::is_aggregate_v<CModelEdit>);

namespace {
bool load(CModelDocument& document, QString* error)
{
    const QString path = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && document.LoadBytes(file.readAll(), path, error);
}
CMocMeshStructureBuilder::Request insertion(const CModelDocument& document)
{
    const auto& mesh = document.Drawables().first();
    CMocMeshStructureBuilder::MeshEdit edit;
    edit.artMeshId = mesh.id;
    edit.indices = document.DrawableIndices(mesh.id);
    for (int i = 0; i < mesh.vertexCount; ++i) {
        edit.vertices.append({ { i }, { 1 } });
        edit.oldToNew.append(i);
    }
    edit.vertices.append({ { 0, 1 }, { .25, .75 } });
    CMocMeshStructureBuilder::Request request;
    request.meshes = { edit };
    return request;
}
CModelSession savedSession(const CModelDocument& document)
{
    CModelSession session;
    session.version = document.ArtMeshClones().isEmpty() ? 12 : 18;
    session.sourceMoc = document.SourcePath();
    session.sourceDigest = document.SourceDigest();
    session.structure = document.ParameterStructure();
    session.meshStructure = document.HasMeshStructure();
    session.artMeshClones = document.ArtMeshClones();
    session.materialStructure = document.HasMaterialStructure();
    session.drawableMaterials = document.DrawableMaterialOverrides();
    session.deformerStructure = document.HasDeformerStructure();
    if (session.deformerStructure)
        session.sourceDeformerIds = document.SourceDeformerIds();
    for (auto it = document.ParameterValues().cbegin(); it != document.ParameterValues().cend(); ++it)
        session.parameters.insert(it.key(), it.value());
    session.parameterMetadata = document.ParameterMetadataOverrides();
    session.parameterOrganization = document.ParameterOrganization();
    session.geometryObjectPairs = document.GeometryObjectPairs();
    session.visibility = document.VisibilityState();
    for (const auto& key : document.LockedKeys())
        session.locked.insert(key, true);
    session.drawableOpacity = document.DrawableOpacityOverrides();
    session.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    session.drawableKeyformPositions = document.DrawableKeyformPositionOverrides();
    session.drawableIndices = document.DrawableIndexOverrides();
    session.drawableMaskIndices = document.DrawableMaskIndexOverrides();
    session.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    session.deformerParentIndices = document.DeformerParentOverrides();
    session.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
    session.deformerWarpKeyforms = document.DeformerWarpKeyformOverrides();
    session.artwork = document.Artwork();
    session.objectDisplayNames = document.DisplayNames();
    if (document.HasTextureWorkspace()) {
        CTextureWorkspace::Draft draft;
        if (CTextureWorkspace::CaptureDraft(document.TextureSources(), document.TextureWorkspace(), draft))
            session.textureWorkspace = draft;
    }
    return session;
}
bool addWorkspaceAndArtwork(CModelDocument& document, QTemporaryDir& directory, QString* error)
{
    QImage image(16, 8, QImage::Format_RGBA8888);
    image.fill(QColor(20, 70, 120, 180));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return false;
    const auto path = directory.filePath("source.png");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(png) != png.size())
        return false;
    file.close();
    CTextureAssets::Source source;
    if (!CTextureAssets::MakeSource(path, png, source, error)
        || !document.InitializeTextureWorkspace({ { 0, source } }, { }, error))
        return false;
    auto workspace = document.TextureWorkspace();
    workspace.atlases.insert(1, { "generated", "New atlas", -1, image.size(), png });
    if (!document.SetTextureWorkspace(workspace, error))
        return false;
    CModelDrawableMaterial material;
    const auto mesh = document.Drawables().first().id;
    if (!document.CaptureDrawableMaterial(mesh, material, error))
        return false;
    material.textureIndex = 1;
    if (!document.SetDrawableMaterials({ material }, error))
        return false;
    CModelArtwork::State artwork;
    artwork.images.insert(source.digest, { source.size, source.png });
    artwork.families.insert("family", { "family", "Original pixels", CModelArtwork::SourceKind::Png, "revision" });
    CModelArtwork::Revision revision;
    revision.id = "revision";
    revision.familyId = "family";
    revision.path = path;
    revision.fileName = "source.png";
    revision.sourceDigest = source.digest;
    revision.sourceBytes = source.png;
    revision.canvasSize = source.size;
    CModelArtwork::Layer layer;
    layer.id = "layer";
    layer.name = "Pixels";
    layer.sourceIndex = 0;
    layer.bounds = QRect(QPoint(), source.size);
    layer.imageDigest = source.digest;
    revision.layers = { layer };
    artwork.revisions.insert(revision.id, revision);
    artwork.materials.insert("material",
        { "material", "Pixels", "family", "revision", "layer", CModelArtwork::Boundary::Repeat, true, { } });
    artwork.placements.insert("placement",
        { "placement", "material", "generated", QTransform(), QRectF(QPointF(), source.size), 1,
            CModelArtwork::Filter::ExactTexels });
    CModelArtwork::Binding binding;
    binding.meshId = mesh;
    binding.placementId = "placement";
    binding.sampledMaterial = material;
    for (int i = 0; i < material.uvs.size(); i += 2)
        binding.familyCoordinates.append(
            { material.uvs[i] * double(source.size.width()), material.uvs[i + 1] * double(source.size.height()) });
    if (!CModelArtwork::TopologyDigest(
            binding.familyCoordinates.size(), document.DrawableIndices(mesh), binding.topologyDigest, error))
        return false;
    artwork.bindings.insert(mesh, binding);
    return document.SetArtwork(artwork, error);
}

}

class MeshTransactionTests : public QObject
{
    Q_OBJECT
private slots:
    void copiedDocumentsAndUnpreparedCommandsKeepIdenticalResults()
    {
        CModelDocument document;
        QString error;
        QVERIFY2(load(document, &error), qPrintable(error));
        CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(insertion(document), edit, &error), qPrintable(error));
        auto copied = document, legacy = document;
        auto unprepared = edit;
        unprepared.meshVerification = { };
        QVERIFY(copied.ExecuteEdit(edit));
        QVERIFY(legacy.ExecuteEdit(unprepared));
        QCOMPARE(copied.AuthoringStructureState(), legacy.AuthoringStructureState());
        QByteArray first, second;
        QVERIFY(copied.BuildRuntimeMoc(first));
        QVERIFY(legacy.BuildRuntimeMoc(second));
        QCOMPARE(first, second);
        QVERIFY(!document.CanUndoEdit());
    }

    void preparedCommandRejectsEveryMutableAuthorField_data()
    {
        QTest::addColumn<int>("mutation");
        for (int i = 0; i < 18; ++i)
            QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void preparedCommandRejectsEveryMutableAuthorField()
    {
        QFETCH(int, mutation);
        CModelDocument document;
        QString error;
        QVERIFY2(load(document, &error), qPrintable(error));
        CModelEdit original;
        QVERIFY2(document.MakeMeshStructureEdit(insertion(document), original, &error), qPrintable(error));
        const auto before = document.AuthoringStructureState();
        const auto revision = document.Revision();
        auto edit = original;
        switch (mutation) {
        case 0:
            edit.name += "x";
            break;
        case 1:
            edit.type += "x";
            break;
        case 2:
            edit.index = 1;
            break;
        case 3:
            edit.coalesce = true;
            break;
        case 4:
            edit.parameterChange = CModelEdit::ParameterChange::Bind;
            break;
        case 5:
            edit.freezeValue = 0;
            break;
        case 6:
            edit.storage = std::make_shared<CModelEditStorageState>();
            break;
        case 7:
            edit.meshRemaps[0].newToOld.last() = 0;
            break;
        case 8:
            edit.meshRemaps[0].artMeshId += "x";
            break;
        case 9:
            edit.meshRequest.reset();
            break;
        case 10: {
            auto request = *edit.meshRequest;
            request.meshes[0].vertices.last().weights = { .5, .5 };
            edit.meshRequest = std::make_shared<const CMocMeshStructureBuilder::Request>(request);
            break;
        }
        case 11: {
            auto request = *edit.meshRequest;
            request.gluePolicy = CMocMeshStructureBuilder::GlueEndpointPolicy::DropPair;
            edit.meshRequest = std::make_shared<const CMocMeshStructureBuilder::Request>(request);
            break;
        }
        case 12: {
            auto state = edit.after.toMap();
            state.insert("extra", 1);
            edit.after = state;
            break;
        }
        case 13: {
            auto state = edit.after.toMap();
            state.remove("fingerprint");
            edit.after = state;
            break;
        }
        case 14: {
            auto state = edit.after.toMap();
            state["sourcePath"] = QString("/tmp/different");
            edit.after = state;
            break;
        }
        case 15: {
            auto state = edit.after.toMap();
            auto columns = state["columns"].value<QMap<int, QByteArray>>();
            QVERIFY(!columns.isEmpty());
            auto at = columns.begin();
            QVERIFY(!at->isEmpty());
            (*at)[0] = char((*at)[0] ^ 1);
            state["columns"] = QVariant::fromValue(columns);
            edit.after = state;
            break;
        }
        case 16: {
            auto state = edit.before.toMap();
            state["meshStructure"] = true;
            edit.before = state;
            break;
        }
        case 17: {
            auto request = insertion(document);
            request.meshes[0].vertices.last().weights = { .5, .5 };
            CModelEdit other;
            QVERIFY(document.MakeMeshStructureEdit(request, other, &error));
            edit.meshVerification = other.meshVerification;
            break;
        }
        }
        QVERIFY(!document.ExecuteEdit(edit));
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.ExecuteEdit(original));
    }

    void proofCannotBeAttachedToAnotherValidCommandKind()
    {
        CModelDocument document;
        QString error;
        QVERIFY(load(document, &error));
        CModelEdit mesh;
        QVERIFY(document.MakeMeshStructureEdit(insertion(document), mesh, &error));
        const auto parameter = document.Parameters().first();
        const float before = document.ParameterValue(parameter.id);
        const auto metadata = document.ParameterMetadata(parameter.id);
        const float after = before == metadata.maximum ? metadata.minimum : metadata.maximum;
        CModelEdit edit { parameter.id, "parameter", CModelEdit::ParameterValue, before, after, -1, false };
        edit.meshVerification = mesh.meshVerification;
        QVERIFY(!document.ExecuteEdit(edit));
        edit.meshVerification = { };
        QVERIFY(document.ExecuteEdit(edit));
    }

    void sameRevisionCannotSubstituteAnotherAuthorState()
    {
        CModelDocument first;
        QString error;
        QVERIFY(load(first, &error));
        auto other = first;
        const auto parameter = first.Parameters().first();
        const auto metadata = first.ParameterMetadata(parameter.id);
        QVERIFY(first.SetParameterValue(parameter.id, metadata.minimum));
        QVERIFY(other.SetParameterValue(parameter.id, metadata.maximum));
        QCOMPARE(first.Revision(), other.Revision());
        CModelEdit edit;
        QVERIFY(first.MakeMeshStructureEdit(insertion(first), edit, &error));
        QVERIFY(!other.ExecuteEdit(edit));
        QVERIFY(first.ExecuteEdit(edit));
    }

    void sourceRequestAndAcceptedBorrowedValuesBecomeOwned()
    {
        CModelDocument document;
        QString error;
        QVERIFY(load(document, &error));
        const auto id = document.Drawables().first().id;
        QVector<QChar> idBacking(id.cbegin(), id.cend());
        auto request = insertion(document);
        request.meshes[0].artMeshId = QString::fromRawData(idBacking.data(), idBacking.size());
        const auto parameter = document.Parameters().first().id;
        auto metadata = document.ParameterMetadata(parameter);
        metadata.displayName = "Owned label";
        QVERIFY(document.SetParameterMetadata(parameter, metadata));
        CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(request, edit, &error), qPrintable(error));
        idBacking[0] = QChar('X');
        QCOMPARE(edit.meshRequest->meshes[0].artMeshId, id);
        QVector<QChar> textBacking(metadata.displayName.cbegin(), metadata.displayName.cend());
        auto state = edit.after.toMap();
        auto values = state["parameterMetadata"].value<QHash<QString, CModelParameterMetadata>>();
        values[parameter].displayName = QString::fromRawData(textBacking.data(), textBacking.size());
        state["parameterMetadata"] = QVariant::fromValue(values);
        edit.after = state;
        QVERIFY(document.ExecuteEdit(edit));
        textBacking[0] = QChar('X');
        QCOMPARE(document.ParameterMetadata(parameter).displayName, metadata.displayName);
        CModelEdit archived;
        QVERIFY(document.PeekUndoEdit(&archived));
        const auto archivedMetadata
            = archived.after.toMap()["parameterMetadata"].value<QHash<QString, CModelParameterMetadata>>();
        QCOMPARE(archivedMetadata[parameter].displayName, metadata.displayName);
        QVERIFY(archived.meshVerification.IsEmpty());
        QVERIFY(document.UndoEdit());
        QVERIFY(document.RedoEdit());
    }

    void borrowedCurrentStateMutationInvalidatesWithoutRevisionChange()
    {
        CModelDocument document;
        QString error;
        QVERIFY(load(document, &error));
        const auto parameter = document.Parameters().first().id;
        QString label = "Borrowed label";
        QVector<QChar> backing(label.cbegin(), label.cend());
        auto metadata = document.ParameterMetadata(parameter);
        metadata.displayName = QString::fromRawData(backing.data(), backing.size());
        QVERIFY(document.SetParameterMetadata(parameter, metadata));
        CModelEdit edit;
        QVERIFY(document.MakeMeshStructureEdit(insertion(document), edit, &error));
        const auto revision = document.Revision();
        backing[0] = QChar('X');
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.ExecuteEdit(edit));
        backing[0] = label[0];
        QVERIFY(document.ExecuteEdit(edit));
        backing[0] = QChar('X');
        QCOMPARE(document.ParameterMetadata(parameter).displayName, label);
    }

    void signedZeroInScalarStateCannotPassAsExact()
    {
        CModelDocument document;
        QString error;
        QVERIFY(load(document, &error));
        const auto id = document.Drawables().first().id;
        QVERIFY(document.SetDrawableOpacity(id, 0.0f));
        CModelEdit edit;
        QVERIFY(document.MakeMeshStructureEdit(insertion(document), edit, &error));
        auto state = edit.after.toMap();
        auto values = state["opacity"].value<QHash<QString, float>>();
        values[id] = -0.0f;
        state["opacity"] = QVariant::fromValue(values);
        edit.after = state;
        QVERIFY(!document.ExecuteEdit(edit));
    }

    void generatedAtlasAndLinkedArtworkMatchLegacyAndSessionPaths()
    {
        CModelDocument document;
        QString error;
        QTemporaryDir directory;
        QVERIFY(load(document, &error));
        QVERIFY2(addWorkspaceAndArtwork(document, directory, &error), qPrintable(error));
        auto legacy = document, loaded = document;
        CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(insertion(document), edit, &error), qPrintable(error));
        auto unprepared = edit;
        unprepared.meshVerification = { };
        QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(legacy.ExecuteEdit(unprepared));
        QCOMPARE(document.AuthoringStructureState(), legacy.AuthoringStructureState());
        const auto id = document.Drawables().first().id;
        QCOMPARE(document.Artwork().bindings[id].status, CModelArtwork::BindingStatus::Linked);
        QCOMPARE(document.Artwork().bindings[id].familyCoordinates.size(), document.Drawables().first().vertexCount);
        CModelSession parsed;
        QVERIFY2(CModelSession::FromJson(savedSession(document).ToJson(), &parsed, &error), qPrintable(error));
        CModelDocument restored;
        QVERIFY2(parsed.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
        QVERIFY(document.UndoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
    }

    void publicationFailureCanRetryAndReentrantChangeCannotCommit()
    {
        CModelDocument document;
        QString error;
        QVERIFY(load(document, &error));
        CKeyformEditController controller;
        controller.SetDocument(&document);
        CModelEdit edit;
        QVERIFY(document.MakeMeshStructureEdit(insertion(document), edit, &error));
        const auto ownedRequest = edit.meshRequest;
        const auto meshId = ownedRequest->meshes.first().artMeshId;
        QVector<QChar> backing(meshId.cbegin(), meshId.cend());
        auto borrowedRequest = *edit.meshRequest;
        borrowedRequest.meshes[0].artMeshId = QString::fromRawData(backing.data(), backing.size());
        edit.meshRequest = std::make_shared<const CMocMeshStructureBuilder::Request>(borrowedRequest);
        const auto before = document.AuthoringStructureState();
        bool accept = false, canonical = true;
        int calls = 0;
        controller.SetMeshPublisher([&](const CModelDocument& next, const CModelEdit& command, bool forward, QString*) {
            ++calls;
            canonical = canonical && forward && command.meshVerification.IsEmpty()
                && command.meshRequest.get() == ownedRequest.get() && next.CanUndoEdit();
            backing[0] = QChar('X');
            canonical = canonical && command.meshRequest->meshes.first().artMeshId == meshId;
            return accept;
        });
        QVERIFY(!controller.Submit(edit, &error));
        QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(!document.CanUndoEdit());
        backing[0] = meshId[0];
        accept = true;
        QVERIFY2(controller.Submit(edit, &error), qPrintable(error));
        QCOMPARE(calls, 2);
        QVERIFY(canonical);
        QVERIFY(document.UndoEdit());
        backing[0] = meshId[0];
        controller.SetMeshPublisher([&](const CModelDocument&, const CModelEdit&, bool, QString*) {
            document.SetLocked("artmesh", document.Drawables().last().id, true);
            return true;
        });
        const int vertices = document.Drawables().first().vertexCount;
        QVERIFY(!controller.Submit(edit, &error));
        QCOMPARE(document.Drawables().first().vertexCount, vertices);
        QVERIFY(!document.CanUndoEdit());
    }

    void denseTopologyPreservesAllParametersAndEveryKeyform()
    {
        WideMeshData::Fixture fixture;
        QString error;
        QVERIFY2(WideMeshData::make(65536, WideMeshData::Layout::Grid, fixture, &error, 1, 2), qPrintable(error));
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "/tmp/mesh-transaction-wide.moc3", &error));
        for (const auto& parameter : document.Parameters()) {
            const auto limits = document.ParameterMetadata(parameter.id);
            QVERIFY(document.SetParameterValue(parameter.id, limits.minimum * .75f + limits.maximum * .25f));
        }
        const auto original = document;
        const auto id = fixture.ids.first();
        auto before = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
            [&](const auto& item) { return item.id == id; });
        QVERIFY(before != document.Drawables().cend());
        QVERIFY(before->keyforms.size() >= 2);
        CMeshEditPlanner::Input input { id, before->keyforms.first().positions, fixture.indices, { } };
        CMeshEditPlanner::Result plan;
        CMeshEditPlanner::Diagnostic diagnostic;
        QVERIFY2(CMeshEditPlanner::Delete(
                     input, { 65535 }, CMeshEditPlanner::BoundaryPolicy::ReplaceWithChord, plan, &diagnostic),
            qPrintable(diagnostic.message));
        CMocMeshStructureBuilder::Request request;
        request.meshes = { plan.edit };
        CModelEdit command;
        QVERIFY2(document.MakeMeshStructureEdit(request, command, &error), qPrintable(error));
        auto legacy = document;
        auto strict = command;
        strict.meshVerification = { };
        QVERIFY(document.ExecuteEdit(command));
        QVERIFY(legacy.ExecuteEdit(strict));
        QCOMPARE(document.AuthoringStructureState(), legacy.AuthoringStructureState());
        QCOMPARE(document.ParameterValues(), original.ParameterValues());
        for (int form = 0; form < before->keyforms.size(); ++form)
            QCOMPARE(document.DrawableKeyformPositions(id, form),
                original.DrawableKeyformPositions(id, form).mid(0, 65535 * 2));
        CModelDrawableMaterial oldMaterial, newMaterial;
        QVERIFY(original.CaptureDrawableMaterial(id, oldMaterial));
        QVERIFY(document.CaptureDrawableMaterial(id, newMaterial));
        QCOMPARE(newMaterial.uvs, oldMaterial.uvs.mid(0, 65535 * 2));
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), original.AuthoringStructureState());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.AuthoringStructureState(), legacy.AuthoringStructureState());
    }

    void malformedSharedLargeRequestIsRejectedBeforeOwningBuffers()
    {
        CModelDocument document;
        QString error;
        QVERIFY(load(document, &error));
        const QVector<quint16> sharedIndices(3000000, 0);
        const QVector<int> sharedMapping(65536, -1);
        CMocMeshStructureBuilder::Request request;
        for (const auto& mesh : document.Drawables()) {
            CMocMeshStructureBuilder::MeshEdit edit;
            edit.artMeshId = mesh.id;
            edit.vertices = { { { 0 }, { 1 } } };
            edit.indices = sharedIndices;
            edit.oldToNew = sharedMapping;
            request.meshes.append(edit);
        }
        QVERIFY(request.meshes.size() > 10);
        CModelEdit unchanged;
        unchanged.name = "sentinel";
        QElapsedTimer timer;
        timer.start();
        QVERIFY(!document.MakeMeshStructureEdit(request, unchanged, &error));
        QVERIFY2(timer.elapsed() < 1000, "Invalid shared buffers must fail before deep-copying each target");
        QCOMPARE(unchanged.name, QString("sentinel"));
        QVERIFY(!document.CanUndoEdit());
    }

    void sharedWorkspacePngStaysSharedInTheSealedCommand()
    {
        CModelDocument document;
        QString error;
        QTemporaryDir directory;
        QVERIFY(load(document, &error));
        QVERIFY2(addWorkspaceAndArtwork(document, directory, &error), qPrintable(error));
        auto workspace = document.TextureWorkspace();
        const auto prototype = workspace.atlases.value(1);
        for (int i = 2; i <= 200; ++i) {
            auto atlas = prototype;
            atlas.id = QString("generated-%1").arg(i);
            workspace.atlases.insert(i, atlas);
        }
        QVERIFY(document.SetTextureWorkspace(workspace, &error));
        CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(insertion(document), edit, &error), qPrintable(error));
        const auto before = edit.before.toMap()["textureWorkspace"].value<CTextureWorkspace::State>();
        const auto after = edit.after.toMap()["textureWorkspace"].value<CTextureWorkspace::State>();
        const char* storage = before.atlases.value(1).png.constData();
        for (int i = 1; i <= 200; ++i) {
            QCOMPARE(quintptr(before.atlases.value(i).png.constData()), quintptr(storage));
            QCOMPARE(quintptr(after.atlases.value(i).png.constData()), quintptr(storage));
        }
        QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(quintptr(document.TextureSources().value(0).png.constData()), quintptr(storage));
    }

    void compositeFloatingFieldsRejectSignedZeroSubstitution()
    {
        CModelDrawableMaterial first { "mesh", 0, { 0.0f, 0.0f } }, second = first;
        second.uvs[0] = -0.0f;
        QVERIFY(first != second);
        CGeometryObjectPairs::Axis axis;
        axis.keys = { 0.0f };
        auto otherAxis = axis;
        otherAxis.keys[0] = -0.0f;
        QVERIFY(axis != otherAxis);
        CModelDocument document;
        QString error;
        QTemporaryDir directory;
        QVERIFY(load(document, &error));
        QVERIFY2(addWorkspaceAndArtwork(document, directory, &error), qPrintable(error));
        CModelEdit edit;
        QVERIFY(document.MakeMeshStructureEdit(insertion(document), edit, &error));
        auto state = edit.after.toMap();
        auto artwork = state["artwork"].value<CModelArtwork::State>();
        artwork.placements["placement"].familyToAtlas = QTransform(1, -0.0, 0, 1, 0, 0);
        QVERIFY(std::signbit(artwork.placements["placement"].familyToAtlas.m12()));
        state["artwork"] = QVariant::fromValue(artwork);
        edit.after = state;
        QVERIFY(!document.ExecuteEdit(edit));
    }

    void preparedCommitKeepsHistoryOnTheStrictPath()
    {
        CModelDocument document;
        QString error;
        QVERIFY2(load(document, &error), qPrintable(error));
        const auto before = document.AuthoringStructureState();
        CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(insertion(document), edit, &error), qPrintable(error));
        QVERIFY(!edit.meshVerification.IsEmpty());
        QVERIFY(document.ExecuteEdit(edit));
        const auto after = document.AuthoringStructureState();
        CModelEdit archived;
        QVERIFY(document.PeekUndoEdit(&archived));
        QVERIFY(archived.meshVerification.IsEmpty());
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.AuthoringStructureState(), after);
    }

    void glueWeightEditCommitsUndoRedoAndSession()
    {
        const QString path = QString(TEST_RESOURCES) + "/Rice/Rice.moc3";
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        CModelDocument loaded; QString error;
        QVERIFY2(loaded.LoadBytes(file.readAll(), path, &error), qPrintable(error));
        CModelDocument document = loaded;
        auto reader = CReader::FromBytes(document.SourceBytes()); CSectionTable table(reader); QVERIFY(table.IsValid());
        QVERIFY(!table.glue.isEmpty());
        const int row = table.glue.first()->glue_info_sources_begin_indices;
        const float original = table.glue_info[row]->weights;
        const float replacement = original == .25f ? .75f : .25f;
        CMocMeshStructureBuilder::Request request;
        request.glueWeights.append({0, 0, CMocMeshStructureBuilder::GlueRetarget::A, replacement});
        CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(request, edit, &error), qPrintable(error));
        const auto before = document.AuthoringStructureState();
        QVERIFY(document.ExecuteEdit(edit));
        const auto after = document.AuthoringStructureState();
        auto changedReader = CReader::FromBytes(document.WorkingBaseBytes()); CSectionTable changed(changedReader); QVERIFY(changed.IsValid());
        QCOMPARE(changed.glue_info[changed.glue.first()->glue_info_sources_begin_indices]->weights, replacement);
        QVERIFY(document.CanUndoEdit());
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.AuthoringStructureState(), after);
        CModelSession parsed;
        QVERIFY2(CModelSession::FromJson(savedSession(document).ToJson(), &parsed, &error), qPrintable(error));
        CModelDocument restored;
        QVERIFY2(parsed.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.AuthoringStructureState(), after);
        CModelEdit forged;
        auto forgedRequest = request;
        forgedRequest.glueWeights[0].weight = 0.5f;
        QVERIFY2(document.MakeMeshStructureEdit(forgedRequest, forged, &error), qPrintable(error));
        auto forgedState = forged.after.toMap();
        auto visibility = forgedState.value(QStringLiteral("visibility")).value<QMap<QString, bool>>();
        visibility[QStringLiteral("artmesh|") + document.Drawables().first().id] = false;
        forgedState.insert(QStringLiteral("visibility"), QVariant::fromValue(visibility));
        forged.after = forgedState;
        const auto stable = document.AuthoringStructureState();
        QVERIFY(!document.ExecuteEdit(forged));
        QCOMPARE(document.AuthoringStructureState(), stable);
    }

    void cloneArtMeshCommitsUndoRedoAndSession()
    {
        CModelDocument document;
        QString error;
        QVERIFY2(load(document, &error), qPrintable(error));
        const QString sourceId = document.Drawables().first().id;
        const QString cloneId = sourceId + QStringLiteral("__document_clone");
        CModelEdit edit;
        QVERIFY2(document.MakeCloneArtMeshEdit({sourceId, cloneId, -2, -2, -2}, edit, &error), qPrintable(error));
        const auto before = document.AuthoringStructureState();
        const int beforeDrawables = document.Drawables().size();
        QVERIFY(document.ExecuteEdit(edit));
        const auto after = document.AuthoringStructureState();
        QCOMPARE(document.Drawables().size(), beforeDrawables + 1);
        QVERIFY(std::any_of(document.Drawables().cbegin(), document.Drawables().cend(),
                            [&](const auto& mesh) { return mesh.id == cloneId; }));
        QVERIFY(document.CanUndoEdit());
        CModelSession parsed;
        QVERIFY2(CModelSession::FromJson(savedSession(document).ToJson(), &parsed, &error), qPrintable(error));
        CModelDocument restored;
        QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.AuthoringStructureState(), after);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.AuthoringStructureState(), after);
    }

    void cloneArtMeshComposesAndRejectsForgedState()
    {
        CModelDocument document;
        QString error;
        QVERIFY2(load(document, &error), qPrintable(error));
        const QString sourceId = document.Drawables().first().id;
        const int originalCount = document.Drawables().size();
        CModelEdit first;
        QVERIFY2(document.MakeCloneArtMeshEdit({sourceId, sourceId + QStringLiteral("__one"), -2, -2, -2}, first, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(first));
        CModelEdit second;
        QVERIFY2(document.MakeCloneArtMeshEdit({sourceId + QStringLiteral("__one"), sourceId + QStringLiteral("__two"), -2, -2, -2}, second, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(second));
        QCOMPARE(document.Drawables().size(), originalCount + 2);
        CModelEdit topology;
        QVERIFY2(document.MakeMeshStructureEdit(insertion(document), topology, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(topology));
        CModelSession parsed;
        QVERIFY2(CModelSession::FromJson(savedSession(document).ToJson(), &parsed, &error), qPrintable(error));
        CModelDocument restored;
        QVERIFY2(parsed.RestoreDocument(document, restored, &error), qPrintable(error));
        QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
        CModelEdit forged;
        QVERIFY2(document.MakeCloneArtMeshEdit({sourceId, sourceId + QStringLiteral("__three"), -2, -2, -2}, forged, &error), qPrintable(error));
        auto forgedState = forged.after.toMap();
        auto visibility = forgedState.value(QStringLiteral("visibility")).value<QMap<QString, bool>>();
        visibility[QStringLiteral("artmesh|") + sourceId] = false;
        forgedState.insert(QStringLiteral("visibility"), QVariant::fromValue(visibility));
        forged.after = forgedState;
        const auto before = document.AuthoringStructureState();
        QVERIFY(!document.ExecuteEdit(forged));
        QCOMPARE(document.AuthoringStructureState(), before);
    }
};

QTEST_GUILESS_MAIN(MeshTransactionTests)
#include "mesh_transaction_tests.moc"
