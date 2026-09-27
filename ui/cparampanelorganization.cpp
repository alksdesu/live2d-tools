#include "cparampanel.h"
#include "cparameterorganizationdialog.h"
#include "live2dstruct/cmodeldocument.h"
#include <QCheckBox>
#include <QLabel>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QToolButton>

void CParamPanel::SetOrganizationEditHandler(MetadataEditHandler handler)
{
    m_organizationEditHandler = std::move(handler);
}

void CParamPanel::SetRelatedParameterIds(const QSet<QString>& ids, const QString& detail)
{
    m_relatedParameterIds = ids;
    m_relatedDetail = detail;
    FilterParameters();
}

void CParamPanel::RefreshOrganization()
{
    if (!m_document) return;
    ReconcileParameters();
    emit OrganizationChanged();
}

void CParamPanel::EditOrganization()
{
    if (!m_document || !m_document->IsLoaded() || m_metadataDialogOpen || m_submittingEdit) return;
    const QPointer<CParamPanel> alive(this);
    if (!FinishUserValueGesture() || !alive) return;
    auto* document = m_document;
    const auto revision = document->Revision();
    const auto source = document->SourceDigest();
    m_metadataDialogOpen = true;
    const auto reset = qScopeGuard([alive] { if (alive) alive->m_metadataDialogOpen = false; });
    QPointer<CParameterOrganizationDialog> dialog = new CParameterOrganizationDialog(*document, m_cachedCid.get(), this);
    const auto cleanup = qScopeGuard([dialog] { if (dialog) delete dialog.data(); });
    dialog->SetSubmissionHandler([this, alive, document, revision, source](const auto& state, QString* error) {
        if (!alive || m_document != document || document->Revision() != revision || document->SourceDigest() != source) {
            if (error) *error = QStringLiteral("参数或文档已改变，请重新打开组织设置");
            return false;
        }
        live2dstruct::CModelEdit edit;
        if (!document->MakeParameterOrganizationEdit(state, edit, error)) return false;
        if (edit.before == edit.after) return true;
        const auto handler = m_organizationEditHandler;
        m_submittingEdit = true;
        const auto release = qScopeGuard([alive] { if (alive) alive->m_submittingEdit = false; });
        if (!(handler ? handler(edit, error) : document->ExecuteEdit(edit))) return false;
        return bool(alive);
    });
    if (dialog->exec() != QDialog::Accepted || !alive || !dialog) return;
    RefreshOrganization();
    if (alive) EmitHistoryState();
}
