#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "live2dstruct/cmocruntimebuilder.h"
#include "live2dstruct/cruntimepackage.h"
#include "deformer_order_fixture.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace live2dstruct;
namespace Fixture = DeformerOrderFixture;
namespace {
CModelEdit parentEdit(const CModelDocument& document)
{
    return {QStringLiteral("Rotation29"), QStringLiteral("rotation"), CModelEdit::DeformerParent,
            document.DeformerParentIndex(QStringLiteral("Rotation29")), 20, -1, false};
}

CModelSession capture(const CModelDocument& document)
{
    CModelSession session;
    session.sourceMoc = document.SourcePath();
    session.sourceDigest = document.SourceDigest();
    session.deformerParentIndices = document.DeformerParentOverrides();
    session.structure = document.ParameterStructure();
    session.meshStructure = document.HasMeshStructure();
    session.version = session.meshStructure ? 5 : session.structure.columns.isEmpty() ? 1 : 2;
    return session;
}

bool scalarInput(const CModelDocument& document, QByteArray& result, QString* error)
{
    QByteArray authoring;
    if (!document.BuildPreviewMoc(authoring, error)) return false;
    CMocRuntimeBuilder::Overrides overrides;
    overrides.visibility = document.VisibilityState();
    overrides.drawableOpacity = document.DrawableOpacityOverrides();
    overrides.drawableDrawOrder = document.DrawableDrawOrderOverrides();
    overrides.drawableIndices = document.DrawableIndexOverrides();
    return CMocRuntimeBuilder::Build(authoring, overrides, result, error);
}
}

class DeformerOrderDocumentTests : public QObject
{
    Q_OBJECT
    CModelDocument m_source;

    void verifyAuthoring(const CModelDocument& document)
    {
        QString error;
        QByteArray bytes;
        QVERIFY2(document.BuildPreviewMoc(bytes, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QCOMPARE(table.deformers.size(), document.Deformers().size());
        for (int index = 0; index < table.deformers.size(); ++index) {
            QCOMPARE(table.deformers[index]->ids, m_source.Deformers()[index].id);
            QCOMPARE(table.deformers[index]->parent_deformer_indices, document.DeformerParentIndex(table.deformers[index]->ids));
        }
        QCOMPARE(table.deformers[19]->ids, QStringLiteral("Rotation29"));
        QCOMPARE(table.deformers[20]->ids, QStringLiteral("Rotation43"));
        QCOMPARE(document.SourceBytes(), m_source.SourceBytes());
        QCOMPARE(document.SourceDigest(), m_source.SourceDigest());
    }

    void verifyRuntime(const CModelDocument& document)
    {
        const auto revision = document.Revision();
        const auto storage = document.AuthoringStructureState();
        QByteArray before, runtime, repeat;
        QString error;
        QVERIFY2(scalarInput(document, before, &error), qPrintable(error));
        QVERIFY2(document.BuildRuntimeMoc(runtime, &error), qPrintable(error));
        QVERIFY2(document.BuildRuntimeMoc(repeat, &error), qPrintable(error));
        QCOMPARE(repeat, runtime);
        QCOMPARE(Fixture::semanticDifference(before, runtime), QString());
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.AuthoringStructureState(), storage);
    }

private slots:
    void initTestCase()
    {
        QString error;
        QVERIFY2(m_source.Load(QString(TEST_RESOURCES) + "/Rice/Rice.moc3", &error), qPrintable(error));
        QCOMPARE(m_source.Deformers()[19].id, QStringLiteral("Rotation29"));
        QCOMPARE(m_source.Deformers()[20].id, QStringLiteral("Rotation43"));
    }

    void runtimeOrderingDoesNotChangeAuthoringIdentity()
    {
        auto document = m_source;
        QVERIFY(document.ExecuteEdit(parentEdit(document)));
        QCOMPARE(document.DeformerParentIndex(QStringLiteral("Rotation29")), 20);
        verifyAuthoring(document);
        verifyRuntime(document);
        QVERIFY(document.CanUndoEdit());
        QVERIFY(!document.CanRedoEdit());
    }

    void historyAndSessionKeepAuthoringParentIndices()
    {
        auto document = m_source;
        const int before = document.DeformerParentIndex(QStringLiteral("Rotation29"));
        QVERIFY(document.ExecuteEdit(parentEdit(document)));
        QString error;
        QByteArray edited;
        QVERIFY2(document.BuildRuntimeMoc(edited, &error), qPrintable(error));
        CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(capture(document).ToJson(), &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.deformerParentIndices.value(QStringLiteral("Rotation29")), 20);
        CModelDocument restored;
        QVERIFY2(decoded.RestoreDocument(m_source, restored, &error), qPrintable(error));
        verifyAuthoring(restored);
        verifyRuntime(restored);
        QByteArray reopened;
        QVERIFY(restored.BuildRuntimeMoc(reopened, &error));
        QCOMPARE(reopened, edited);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DeformerParentIndex(QStringLiteral("Rotation29")), before);
        verifyAuthoring(document);
        verifyRuntime(document);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DeformerParentIndex(QStringLiteral("Rotation29")), 20);
        QVERIFY(document.BuildRuntimeMoc(reopened, &error));
        QCOMPARE(reopened, edited);
    }

    void structuralEditsNeverBakeRuntimeRowNumbersIntoAuthoringData()
    {
        auto document = m_source;
        QVERIFY(document.ExecuteEdit(parentEdit(document)));
        QString error;
        CModelEdit create;
        CMocParameterStructureBuilder::CreateRequest parameter;
        parameter.parameter.id = QStringLiteral("ParamOrderCheck");
        parameter.bindings.append({{CMocParameterStructureBuilder::Owner::ArtMesh, QStringLiteral("ArtMesh38")}, {-1, 0, 1}});
        QVERIFY2(document.MakeCreateParameterEdit(parameter, QStringLiteral("Order check"), create, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(create));
        verifyAuthoring(document);
        verifyRuntime(document);

        CMocMeshStructureBuilder::MeshEdit mesh;
        mesh.artMeshId = QStringLiteral("ArtMesh38");
        for (const auto& drawable : document.Drawables()) if (drawable.id == mesh.artMeshId) {
            for (int vertex = 0; vertex < drawable.vertexCount; ++vertex) {
                mesh.vertices.append({{vertex}, {1}});
                mesh.oldToNew.append(vertex);
            }
            mesh.indices = document.DrawableIndices(mesh.artMeshId);
        }
        QVERIFY(mesh.vertices.size() >= 2);
        mesh.vertices.append({{0, 1}, {0.5, 0.5}});
        CMocMeshStructureBuilder::Request request;
        request.meshes.append(mesh);
        CModelEdit geometry;
        QVERIFY2(document.MakeMeshStructureEdit(request, geometry, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(geometry));
        verifyAuthoring(document);
        verifyRuntime(document);

        CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(capture(document).ToJson(), &decoded, &error), qPrintable(error));
        QVERIFY(decoded.meshStructure);
        CModelDocument restored;
        QVERIFY2(decoded.RestoreDocument(m_source, restored, &error), qPrintable(error));
        verifyAuthoring(restored);
        verifyRuntime(restored);
        for (int count = 0; count < 3; ++count) {
            QVERIFY(document.UndoEdit());
            verifyAuthoring(document);
            verifyRuntime(document);
        }
        QVERIFY(!document.CanUndoEdit());
        for (int count = 0; count < 3; ++count) {
            QVERIFY(document.RedoEdit());
            verifyAuthoring(document);
            verifyRuntime(document);
        }
    }

    void packageNormalizesAuthoringInputAndPreservesSourceDigest()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QJsonObject json{{"Version", 3}, {"FileReferences", QJsonObject{{"Moc", m_source.SourcePath()}}}};
        const auto settings = QJsonDocument(json).toJson();
        const auto path = directory.filePath(QStringLiteral("model.model3.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(settings), settings.size());
        file.close();
        auto document = m_source;
        QVERIFY(document.ExecuteEdit(parentEdit(document)));
        CRuntimePackage::Request request;
        request.modelJsonPath = path;
        request.modelSettingsDigest = QCryptographicHash::hash(settings, QCryptographicHash::Sha256);
        request.sourceMocDigest = m_source.SourceDigest();
        QString error;
        QVERIFY(document.BuildPreviewMoc(request.mocBytes, &error));
        const auto source = request.mocBytes;
        CRuntimePackage::Prepared prepared;
        QVERIFY2(CRuntimePackage::Prepare(request, prepared, nullptr, &error), qPrintable(error));
        const auto mocName = prepared.model["FileReferences"].toObject()["Moc"].toString();
        QByteArray packaged;
        for (const auto& entry : prepared.entries) if (entry.relativePath == mocName) packaged = entry.bytes;
        QVERIFY(!packaged.isEmpty());
        QVERIFY(packaged != source);
        QCOMPARE(Fixture::semanticDifference(source, packaged), QString());
        QCOMPARE(request.mocBytes, source);
        QCOMPARE(request.sourceMocDigest, m_source.SourceDigest());
        QCOMPARE(Fixture::read(m_source.SourcePath()), m_source.SourceBytes());
    }
};

QTEST_GUILESS_MAIN(DeformerOrderDocumentTests)
#include "deformer_order_document_tests.moc"
