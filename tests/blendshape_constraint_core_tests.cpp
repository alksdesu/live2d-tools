#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/cmocblendshapeconstraintbuilder.h"
#include <QtTest>
#include <QFile>

using namespace live2dstruct;

class BlendShapeConstraintCoreTests : public QObject
{
    Q_OBJECT
    CModelDocument m_document;
    int m_index = -1;
    CModelBlendShapeConstraintEntry m_entry;
private slots:
    void initTestCase()
    {
        const QString path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to an official BlendShape MOC");
        QString error;
        QVERIFY2(m_document.Load(path, &error), qPrintable(error));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        for (int index = 0; index < table.blend_shape_constraints.size(); ++index) {
            const auto* row = table.blend_shape_constraints[index];
            if (row && row->blend_shape_constraint_value_sources_counts >= 2) {
                CModelBlendShapeConstraintEntry entry;
                if (m_document.CaptureBlendShapeConstraint(index, entry, &error)) {
                    m_index = index; m_entry = std::move(entry); break;
                }
            }
        }
        if (m_index < 0) QSKIP("Official fixture has no editable BlendShape constraint curve");
    }

    void builderDocumentUndoRedoAndSession()
    {
        auto values = m_entry.values;
        values[0].weight = values[0].weight < 0.9f ? values[0].weight + 0.05f : values[0].weight - 0.05f;
        CModelEdit edit; QString error;
        QVERIFY2(m_document.MakeBlendShapeConstraintEdit(m_entry, values, edit, &error), qPrintable(error));
        QVERIFY(edit.field == CModelEdit::BlendShapeConstraints);
        QVERIFY2(m_document.ExecuteEdit(edit), qPrintable(error));
        QCOMPARE(m_document.BlendShapeConstraints().size(), 1);
        QCOMPARE(m_document.BlendShapeConstraints().first().values, values);
        QVERIFY2(m_document.UndoEdit(), qPrintable(error));
        QVERIFY(m_document.BlendShapeConstraints().isEmpty());
        QVERIFY2(m_document.RedoEdit(), qPrintable(error));
        QCOMPARE(m_document.BlendShapeConstraints().first().values, values);
        QByteArray saved;
        CModelSession session;
        session.version = 19; session.sourceMoc = m_document.SourcePath(); session.sourceDigest = m_document.SourceDigest();
        session.objectDisplayNames = m_document.DisplayNames();
        session.blendShapeConstraints = m_document.BlendShapeConstraints();
        saved = session.ToJson();
        QVERIFY(!saved.isEmpty());
        CModelSession parsed; QVERIFY2(CModelSession::FromJson(saved, &parsed, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(parsed.RestoreDocument(m_document, restored, &error), qPrintable(error));
        QCOMPARE(restored.BlendShapeConstraints(), m_document.BlendShapeConstraints());
        QByteArray preview; QVERIFY2(restored.BuildPreviewMoc(preview, &error), qPrintable(error));
        QVERIFY(!preview.isEmpty());
    }

    void rejectsInvalidCurveAtomically()
    {
        const auto before = m_document.BlendShapeConstraints();
        const auto revision = m_document.Revision();
        auto invalid = m_entry.values;
        invalid[0].weight = -0.1f;
        CModelEdit edit; QString error;
        QVERIFY(!m_document.MakeBlendShapeConstraintEdit(m_entry, invalid, edit, &error));
        QCOMPARE(m_document.BlendShapeConstraints(), before);
        QCOMPARE(m_document.Revision(), revision);
    }
};

QTEST_APPLESS_MAIN(BlendShapeConstraintCoreTests)
#include "blendshape_constraint_core_tests.moc"
