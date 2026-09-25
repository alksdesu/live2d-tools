#include "LAppView.hpp"

#include <QOpenGLContext>
#include <exception>

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool shown(const CDrawObject* object)
{
    if (const auto* warp = dynamic_cast<const CDrawWarp*>(object)) return warp->IsShown();
    if (const auto* rotation = dynamic_cast<const CDrawRotation*>(object)) return rotation->IsShown();
    return false;
}
}

bool LAppView::PreparedDeformerOverlays::Prepare(const CTreeManage::Snapshot& snapshot,
                                                const PreparedDeformerOverlays& previous, QString* error)
{
    if (error) error->clear();
    auto* context = QOpenGLContext::currentContext();
    if (!snapshot.tree || !snapshot.HasValidEvaluation() || !context || !context->isValid())
        return fail(error, QStringLiteral("变形器线框需要有效的树和 OpenGL 上下文"));
    if ((previous.m_context && previous.m_context != context) || (m_context && m_context != context))
        return fail(error, QStringLiteral("变形器线框属于其他 OpenGL 上下文"));
    try {
        PreparedDeformerOverlays candidate;
        candidate.m_context = context;
        candidate.m_snapshotDigest = snapshot.MocDigest();
        candidate.m_treeIdentity = snapshot.tree.get();
        candidate.m_poseRevision = snapshot.PoseRevision();
        candidate.m_visibleNames = previous.m_visibleNames;
        candidate.m_warpColors = previous.m_warpColors;
        for (const auto& entry : previous.m_objects)
            if (const auto* warp = dynamic_cast<const CDrawWarp*>(entry.second.get()))
                candidate.m_warpColors.insert_or_assign(entry.first, warp->Color());
        for (auto* node : snapshot.Deformers()) {
            if (!node || snapshot.unsupportedDeformers.contains(node->getName())) continue;
            const auto* old = previous.Find(node->getName());
            std::unique_ptr<CDrawObject> object;
            if (auto* warp = dynamic_cast<CWarpNode*>(node)) {
                auto draw = std::make_unique<CDrawWarp>();
                const auto color = candidate.m_warpColors.find(node->getName());
                if (color != candidate.m_warpColors.end()) draw->SetColor(color->second);
                if (!draw->Init(warp) || !draw->PrepareOpenGl())
                    return fail(error, QStringLiteral("无法准备 Warp 线框：%1").arg(node->getName()));
                object = std::move(draw);
            } else if (auto* rotation = dynamic_cast<CRotationNode*>(node)) {
                auto draw = std::make_unique<CDrawRotation>();
                if (!draw->Init(rotation) || !draw->PrepareOpenGl())
                    return fail(error, QStringLiteral("无法准备 Rotation 线框：%1").arg(node->getName()));
                object = std::move(draw);
            } else return fail(error, QStringLiteral("未知变形器线框类型：%1").arg(node->getName()));
            object->SetDraw(shown(old) || candidate.m_visibleNames.contains(node->getName()));
            candidate.m_supportedNames.insert(node->getName());
            if (!candidate.m_objects.emplace(node->getName(), std::move(object)).second)
                return fail(error, QStringLiteral("变形器线框 ID 重复：%1").arg(node->getName()));
        }
        candidate.m_frameReady = true;
        Swap(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法准备变形器线框，原预览保持不变"));
    }
}

bool LAppView::PreparedDeformerOverlays::Update(const CTreeManage::Snapshot& snapshot, QString* error)
{
    if (error) error->clear();
    auto* context = QOpenGLContext::currentContext();
    if (!snapshot.tree || !snapshot.HasValidEvaluation() || !context || !context->isValid() ||
        (m_context && m_context != context)) {
        Invalidate();
        return fail(error, QStringLiteral("变形器线框的姿态或 OpenGL 上下文无效"));
    }
    if (m_frameReady && m_treeIdentity == snapshot.tree.get() && m_snapshotDigest == snapshot.MocDigest() &&
        m_poseRevision == snapshot.PoseRevision()) return true;
    Invalidate();
    try {
        QSet<QString> supportedNames;
        for (const auto& entry : m_objects) {
            if (shown(entry.second.get())) m_visibleNames.insert(entry.first);
            if (const auto* warp = dynamic_cast<const CDrawWarp*>(entry.second.get()))
                m_warpColors.insert_or_assign(entry.first, warp->Color());
            entry.second->SetDraw(false);
        }
        m_context = context;
        for (auto* node : snapshot.Deformers()) {
            if (!node || snapshot.unsupportedDeformers.contains(node->getName())) continue;
            auto* object = Find(node->getName());
            if (auto* warp = dynamic_cast<CWarpNode*>(node)) {
                auto* draw = dynamic_cast<CDrawWarp*>(object);
                if (!draw) {
                    auto candidate = std::make_unique<CDrawWarp>();
                    const auto color = m_warpColors.find(node->getName());
                    if (color != m_warpColors.end()) candidate->SetColor(color->second);
                    draw = candidate.get();
                    m_objects.insert_or_assign(node->getName(), std::move(candidate));
                }
                if (!draw->Init(warp) || !draw->PrepareOpenGl())
                    return fail(error, QStringLiteral("无法更新 Warp 线框：%1").arg(node->getName()));
                object = draw;
            } else if (auto* rotation = dynamic_cast<CRotationNode*>(node)) {
                auto* draw = dynamic_cast<CDrawRotation*>(object);
                if (!draw) {
                    auto candidate = std::make_unique<CDrawRotation>();
                    draw = candidate.get();
                    m_objects.insert_or_assign(node->getName(), std::move(candidate));
                }
                if (!draw->Init(rotation) || !draw->PrepareOpenGl())
                    return fail(error, QStringLiteral("无法更新 Rotation 线框：%1").arg(node->getName()));
                object = draw;
            } else return fail(error, QStringLiteral("未知变形器线框类型：%1").arg(node->getName()));
            object->SetDraw(m_visibleNames.contains(node->getName()));
            supportedNames.insert(node->getName());
        }
        m_supportedNames.swap(supportedNames);
        m_snapshotDigest = snapshot.MocDigest();
        m_treeIdentity = snapshot.tree.get();
        m_poseRevision = snapshot.PoseRevision();
        m_frameReady = true;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法更新变形器线框"));
    }
}

bool LAppView::PreparedDeformerOverlays::Synchronize(const QByteArray& mocDigest,
                                                    const QMap<QString, float>& values, QString* error)
{
    const auto& snapshot = CTreeManage::CurrentSnapshot();
    if (mocDigest.isEmpty() || mocDigest != snapshot.MocDigest()) {
        Invalidate();
        return fail(error, QStringLiteral("变形器线框与运行时模型不匹配"));
    }
    bool changed = false;
    if (!CTreeManage::SyncCurrentPose(values, changed, error)) {
        Invalidate();
        return false;
    }
    return Update(snapshot, error);
}

void LAppView::PreparedDeformerOverlays::Swap(PreparedDeformerOverlays& other) noexcept
{
    m_objects.swap(other.m_objects);
    m_warpColors.swap(other.m_warpColors);
    m_visibleNames.swap(other.m_visibleNames);
    m_supportedNames.swap(other.m_supportedNames);
    m_context.swap(other.m_context);
    m_snapshotDigest.swap(other.m_snapshotDigest);
    std::swap(m_treeIdentity, other.m_treeIdentity);
    std::swap(m_poseRevision, other.m_poseRevision);
    std::swap(m_frameReady, other.m_frameReady);
}

void LAppView::PreparedDeformerOverlays::Draw(const QMatrix4x4& projection)
{
    if (m_frameReady) for (const auto& entry : m_objects)
        if (m_supportedNames.contains(entry.first)) entry.second->Draw(projection);
}

void LAppView::PreparedDeformerOverlays::Show(const QStringList& names)
{
    m_visibleNames = QSet<QString>(names.cbegin(), names.cend());
    for (const auto& entry : m_objects)
        entry.second->SetDraw(m_visibleNames.contains(entry.first) && m_supportedNames.contains(entry.first));
}

CDrawObject* LAppView::PreparedDeformerOverlays::Find(const QString& name) const noexcept
{
    const auto found = m_objects.find(name);
    return found == m_objects.end() ? nullptr : found->second.get();
}

bool LAppView::PrepareDeformerOverlays(const CTreeManage::Snapshot& snapshot,
                                      PreparedDeformerOverlays& result, QString* error) const
{
    return result.Prepare(snapshot, m_deformerOverlays, error);
}

void LAppView::SwapDeformerOverlays(PreparedDeformerOverlays& candidate) noexcept
{
    m_deformerOverlays.Swap(candidate);
}

void LAppView::AddWrapKey(CWarpNode* node)
{
    if (!node) return;
    if (auto* warp = dynamic_cast<CDrawWarp*>(m_deformerOverlays.Find(node->getName()))) warp->Init(node);
    else {
        auto candidate = std::make_unique<CDrawWarp>();
        candidate->Init(node);
        const auto color = m_deformerOverlays.m_warpColors.find(node->getName());
        if (color != m_deformerOverlays.m_warpColors.end()) candidate->SetColor(color->second);
        candidate->SetDraw(m_deformerOverlays.m_visibleNames.contains(node->getName()));
        m_deformerOverlays.m_objects.insert_or_assign(node->getName(), std::move(candidate));
    }
}

void LAppView::AddRotationKey(CRotationNode* node)
{
    if (!node) return;
    if (auto* rotation = dynamic_cast<CDrawRotation*>(m_deformerOverlays.Find(node->getName()))) rotation->Init(node);
    else {
        auto candidate = std::make_unique<CDrawRotation>();
        candidate->Init(node);
        candidate->SetDraw(m_deformerOverlays.m_visibleNames.contains(node->getName()));
        m_deformerOverlays.m_objects.insert_or_assign(node->getName(), std::move(candidate));
    }
}

void LAppView::InitWrapAndRotation()
{
    auto* tree = CTreeManage::GetInstance();
    QString error;
    if (!tree || !CTreeManage::RefreshCurrentSnapshot(&error)) {
        m_deformerOverlays.Invalidate();
        return;
    }
    m_deformerOverlays.Update(CTreeManage::CurrentSnapshot(), &error);
}

void LAppView::ShowWarp(const QString& name)
{
    m_deformerOverlays.Show({name});
}

void LAppView::ShowWarps(const QStringList& names)
{
    m_deformerOverlays.Show(names);
}

void LAppView::HideAllWarps()
{
    m_deformerOverlays.Show({});
}
