#include "ccoordinateframe.h"
#include "crotationcoortype.h"
#include "cwarpcoortype.h"
#include <QtMath>
#include <limits>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
bool validCoordinates(const std::vector<float>& coordinates)
{
    return coordinates.size() <= 2000000 && coordinates.size() % 2 == 0 &&
           std::all_of(coordinates.cbegin(), coordinates.cend(), [](float value) { return std::isfinite(value); });
}
}

namespace live2dstruct {

struct CCoordinateFrame::Link
{
    std::unique_ptr<CCoorTypeObject> transform;
    std::shared_ptr<const Link> parent;
    Link* pendingNext = nullptr;

    static void Destroy(Link* link) noexcept
    {
        // Queue the last shared parent release to avoid recursive chain destruction.
        thread_local Link* pending = nullptr;
        thread_local bool draining = false;
        link->pendingNext = pending;
        pending = link;
        if (draining) return;
        draining = true;
        while (pending) {
            Link* current = pending;
            pending = current->pendingNext;
            delete current;
        }
        draining = false;
    }
};

CCoordinateFrame::CCoordinateFrame(std::unique_ptr<CCoorTypeObject> transform,
                                 std::shared_ptr<const CCoordinateFrame> parent)
    : m_link(new Link{std::move(transform), parent ? parent->m_link : nullptr}, &Link::Destroy)
{
    const auto* rotation = dynamic_cast<const CRotationCoorType*>(m_link->transform.get());
    m_rotation = rotation != nullptr;
    const double scale = (parent ? double(parent->m_totalScale) : 1.0) *
                         (rotation ? double(rotation->GetCalculatedTotalScale()) : 1.0);
    m_totalScale = std::isfinite(scale) && std::abs(scale) <= std::numeric_limits<float>::max()
        ? float(scale) : std::numeric_limits<float>::quiet_NaN();
}

bool CCoordinateFrame::CreateRotation(float baseAngle, CRotationNodeKeyform& key,
                                      const std::shared_ptr<const CCoordinateFrame>& parent,
                                      std::shared_ptr<const CCoordinateFrame>& result, RotationBasis& basis)
{
    if (key.GetPosition().size() != 2 || !std::isfinite(baseAngle) || !std::isfinite(key.Getangles()) ||
        !std::isfinite(key.Getscales()) || !validCoordinates(key.GetPosition())) return false;
    std::vector<float> origin = key.GetPosition();
    double parentAngle = 0;
    if (parent) {
        if (!parent->MapToCanvas(key.GetPosition(), origin)) return false;
        const float length = parent->m_rotation ? -10.0f : -0.1f;
        float directionX = 0, directionY = 0, multiplier = 1;
        for (int attempt = 0; attempt < 10; ++attempt, multiplier *= 0.1f) {
            std::vector<float> endpoint{key.Getorigin_x(), key.Getorigin_y() + length * multiplier};
            std::vector<float> transformed;
            if (!parent->MapToCanvas(endpoint, transformed)) return false;
            directionX = transformed[0] - origin[0];
            directionY = transformed[1] - origin[1];
            if (directionX != 0 || directionY != 0) break;
            endpoint[1] = key.Getorigin_y() - length * multiplier;
            if (!parent->MapToCanvas(endpoint, transformed)) return false;
            directionX = origin[0] - transformed[0];
            directionY = origin[1] - transformed[1];
            if (directionX != 0 || directionY != 0) break;
        }
        if (!std::isfinite(directionX) || !std::isfinite(directionY)) return false;
        if (directionX == 0 && directionY == 0) return false;
        parentAngle = std::remainder(qRadiansToDegrees(std::atan2(double(directionY), double(directionX))) + 90.0, 360.0);
    }
    const double totalScale = double(key.Getscales()) * (parent ? parent->m_totalScale : 1.0f);
    if (!std::isfinite(totalScale) || std::abs(totalScale) > std::numeric_limits<float>::max()) return false;
    const double angle = double(key.Getangles()) + parentAngle + baseAngle;
    const double radians = qDegreesToRadians(std::remainder(angle, 360.0));
    RotationBasis prepared;
    prepared.origin = QPointF(origin[0], origin[1]);
    const double scaleSign = totalScale < 0 ? -1.0 : 1.0;
    prepared.xAxis = QPointF(std::cos(radians), std::sin(radians)) * (key.Getis_reflect_x() ? -scaleSign : scaleSign);
    prepared.yAxis = QPointF(-std::sin(radians), std::cos(radians)) * (key.Getis_reflect_y() ? -scaleSign : scaleSign);
    auto transform = std::make_unique<CRotationCoorType>(origin[0], origin[1], angle, float(totalScale),
                                                        key.Getis_reflect_x(), key.Getis_reflect_y());
    auto frame = std::make_shared<CCoordinateFrame>(std::move(transform));
    result = std::move(frame);
    basis = prepared;
    return true;
}

bool CCoordinateFrame::CreateWarp(int columns, int rows, const std::vector<float>& canvasPositions, bool quad,
                                  const std::shared_ptr<const CCoordinateFrame>& parent,
                                  std::shared_ptr<const CCoordinateFrame>& result)
{
    const float scale = parent ? parent->m_totalScale : 1.0f;
    if (!std::isfinite(scale)) return false;
    auto transform = std::make_unique<CWarpCoorType>(columns, rows, canvasPositions, quad);
    if (!transform->transformLocalToCanvas(nullptr, nullptr, 0, 0, 2)) return false;
    auto frame = std::make_shared<CCoordinateFrame>(std::move(transform));
    frame->m_totalScale = scale;
    result = std::move(frame);
    return true;
}

CCoordinateFrame::~CCoordinateFrame() = default;

bool CCoordinateFrame::MapToCanvas(const std::vector<float>& source, std::vector<float>& destination) const
{
    if (!validCoordinates(source)) return false;
    std::vector<float> result = source;
    for (const Link* link = m_link.get(); link; link = link->parent.get()) {
        if (link->transform &&
            (!link->transform->transformLocalToCanvas(result.data(), result.data(), static_cast<int>(result.size() / 2), 0, 2) ||
             !validCoordinates(result))) return false;
    }
    destination = std::move(result);
    return true;
}

bool CCoordinateFrame::MapFromCanvas(const std::vector<float>& source, std::vector<float>& destination) const
{
    if (!validCoordinates(source)) return false;
    std::vector<const Link*> chain;
    for (const Link* link = m_link.get(); link; link = link->parent.get()) chain.push_back(link);
    std::vector<float> result = source;
    for (auto entry = chain.crbegin(); entry != chain.crend(); ++entry) {
        if ((*entry)->transform &&
            (!(*entry)->transform->transformCanvasToLocal(result.data(), result.data(), static_cast<int>(result.size() / 2)) ||
             !validCoordinates(result))) return false;
    }
    destination = std::move(result);
    return true;
}

}
