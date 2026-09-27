#include "mainwindow.h"
#include "cparampanel.h"
#include "live2dstruct/cmodelsavefingerprint.h"
#include <exception>

void MainWindow::RefreshRelatedParameters()
{
    if (!m_paramPanel || !m_document.IsLoaded()) return;
    QString error;
    live2dstruct::CParameterSelectionQuery::Result result;
    QVector<live2dstruct::CParameterSelectionQuery::Target> targets;
    for (int index = 0; index < m_selectedObjectIds.size() && index < m_selectedObjectTypes.size(); ++index)
        targets.append({m_selectedObjectTypes[index], m_selectedObjectIds[index]});
    if (!m_parameterSelectionQuery.Build(m_document, &error) || !m_parameterSelectionQuery.Query(targets, result, &error)) {
        m_paramPanel->SetRelatedParameterIds({}, error); return;
    }
    m_paramPanel->SetRelatedParameterIds(QSet<QString>(result.all.cbegin(), result.all.cend()), result.diagnostics.join(QStringLiteral("；")));
}

bool MainWindow::HasSessionChanges() const
{
    if (m_textureEditor.IsDirty()) return true;
    if (!m_document.IsLoaded()) return m_sessionDirty;
    if (m_savedAuthoringFingerprint.isEmpty()) return true;
    if (!m_dirtyComparisonReady || m_dirtyComparedRevision != m_document.Revision() ||
        m_dirtyComparedSource != m_document.SourceDigest() || m_dirtyComparedPath != m_document.SourcePath()) {
        try {
            QByteArray fingerprint;
            m_documentDirty = !live2dstruct::CModelSaveFingerprint::Capture(m_document, fingerprint) ||
                              fingerprint != m_savedAuthoringFingerprint;
            m_dirtyComparedRevision = m_document.Revision();
            m_dirtyComparedSource = m_document.SourceDigest();
            m_dirtyComparedPath = m_document.SourcePath();
            m_dirtyComparisonReady = true;
        } catch (const std::exception&) {
            return true;
        }
    }
    if (m_documentDirty) return true;
    try {
        QJsonObject resources;
        return !live2dstruct::CParameterResourceEdits::ToJson(m_parameterResources, resources) ||
               resources != m_savedParameterResources;
    } catch (const std::exception&) { return true; }
}

void MainWindow::MarkDocumentSaved()
{
    m_savedAuthoringFingerprint.clear();
    if (m_document.IsLoaded()) live2dstruct::CModelSaveFingerprint::Capture(m_document, m_savedAuthoringFingerprint);
    m_savedParameterResources = {};
    try {
        if (!live2dstruct::CParameterResourceEdits::ToJson(m_parameterResources, m_savedParameterResources))
            m_savedAuthoringFingerprint.clear();
    } catch (const std::exception&) { m_savedAuthoringFingerprint.clear(); }
    m_sessionDirty = false;
    m_dirtyComparisonReady = false;
}
