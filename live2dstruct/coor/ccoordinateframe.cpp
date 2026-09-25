#include "ccoordinateframe.h"

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
