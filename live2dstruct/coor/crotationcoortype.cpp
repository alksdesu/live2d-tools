#include "crotationcoortype.h"

#include <QtMath>
#include <cmath>
#include <limits>

namespace {
template <typename Transform>
bool transformPoints(float* source, float* destination, int count, int offset, int stride,
                     const Transform& transform)
{
    if (count < 0 || count > 1000000 || offset < 0 || stride < 2) return false;
    if (count == 0) return true;
    const qint64 last = static_cast<qint64>(offset) + static_cast<qint64>(count - 1) * stride + 1;
    if (!source || !destination || last > std::numeric_limits<int>::max()) return false;
    std::vector<float> result(static_cast<size_t>(count) * 2);
    for (int vertex = 0; vertex < count; ++vertex) {
        const size_t index = static_cast<size_t>(offset) + static_cast<size_t>(vertex) * stride;
        const double x = source[index], y = source[index + 1];
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        double outputX, outputY;
        transform(x, y, outputX, outputY);
        const double limit = std::numeric_limits<float>::max();
        if (!std::isfinite(outputX) || !std::isfinite(outputY) ||
            std::abs(outputX) > limit || std::abs(outputY) > limit) return false;
        result[static_cast<size_t>(vertex) * 2] = static_cast<float>(outputX);
        result[static_cast<size_t>(vertex) * 2 + 1] = static_cast<float>(outputY);
    }
    for (int vertex = 0; vertex < count; ++vertex) {
        const size_t index = static_cast<size_t>(offset) + static_cast<size_t>(vertex) * stride;
        destination[index] = result[static_cast<size_t>(vertex) * 2];
        destination[index + 1] = result[static_cast<size_t>(vertex) * 2 + 1];
    }
    return true;
}
}

namespace live2dstruct {

float CRotationCoorType::GetOriginX() const { return OriginX; }
float CRotationCoorType::GetOriginY() const { return OriginY; }
double CRotationCoorType::GetAngle_forRotation() const { return static_cast<double>(angle) + base_angles; }
bool CRotationCoorType::GetisReflectX() const { return isReflectX; }
bool CRotationCoorType::GetisReflectY() const { return isReflectY; }
float CRotationCoorType::GetCalculatedTotalScale() const { return Scale; }

CRotationCoorType::CRotationCoorType(float base_angles, CRotationNodeKeyform& keyform)
    : Scale(keyform.Getscales()), isReflectX(keyform.Getis_reflect_x()),
      isReflectY(keyform.Getis_reflect_y()), angle(keyform.Getangles()), base_angles(base_angles)
{
    if (keyform.GetPosition().size() != 2) return;
    OriginX = keyform.Getorigin_x();
    OriginY = keyform.Getorigin_y();
    m_valid = std::isfinite(base_angles) && std::isfinite(angle) && std::isfinite(Scale) &&
              std::isfinite(OriginX) && std::isfinite(OriginY);
}

CRotationCoorType::~CRotationCoorType() = default;

bool CRotationCoorType::transformLocalToCanvas(float* src, float* des, int vet, int offset, int step)
{
    if (!m_valid) return false;
    const double radians = qDegreesToRadians(std::remainder(GetAngle_forRotation(), 360.0));
    const double cosine = std::cos(radians), sine = std::sin(radians);
    const double scaleX = static_cast<double>(Scale) * (isReflectX ? -1.0 : 1.0);
    const double scaleY = static_cast<double>(Scale) * (isReflectY ? -1.0 : 1.0);
    return transformPoints(src, des, vet, offset, step,
        [&](double x, double y, double& outputX, double& outputY) {
            outputX = cosine * scaleX * x - sine * scaleY * y + OriginX;
            outputY = sine * scaleX * x + cosine * scaleY * y + OriginY;
        });
}

bool CRotationCoorType::transformCanvasToLocal(float* src, float* des, int vet)
{
    if (!m_valid || Scale == 0.0f) return false;
    const double radians = qDegreesToRadians(std::remainder(GetAngle_forRotation(), 360.0));
    const double cosine = std::cos(radians), sine = std::sin(radians);
    const double scaleX = static_cast<double>(Scale) * (isReflectX ? -1.0 : 1.0);
    const double scaleY = static_cast<double>(Scale) * (isReflectY ? -1.0 : 1.0);
    return transformPoints(src, des, vet, 0, 2,
        [&](double x, double y, double& outputX, double& outputY) {
            const double dx = x - OriginX, dy = y - OriginY;
            outputX = (cosine * dx + sine * dy) / scaleX;
            outputY = (-sine * dx + cosine * dy) / scaleY;
        });
}

void CRotationCoorType::Test() {}

}
