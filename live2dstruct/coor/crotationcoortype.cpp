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
    if (!m_valid) return;
    const double radians = qDegreesToRadians(std::remainder(GetAngle_forRotation(), 360.0));
    m_cosine = std::cos(radians);
    m_sine = std::sin(radians);
    m_scaleX = static_cast<double>(Scale) * (isReflectX ? -1.0 : 1.0);
    m_scaleY = static_cast<double>(Scale) * (isReflectY ? -1.0 : 1.0);
}

CRotationCoorType::~CRotationCoorType() = default;

bool CRotationCoorType::IsIdentity() const
{
    return m_valid && OriginX == 0.0f && OriginY == 0.0f && m_cosine == 1.0 && m_sine == 0.0 &&
        m_scaleX == 1.0 && m_scaleY == 1.0;
}

bool CRotationCoorType::transformLocalToCanvas(float* src, float* des, int vet, int offset, int step)
{
    if (!m_valid) return false;
    return transformPoints(src, des, vet, offset, step,
        [&](double x, double y, double& outputX, double& outputY) {
            outputX = m_cosine * m_scaleX * x - m_sine * m_scaleY * y + OriginX;
            outputY = m_sine * m_scaleX * x + m_cosine * m_scaleY * y + OriginY;
        });
}

bool CRotationCoorType::transformCanvasToLocal(float* src, float* des, int vet)
{
    if (!m_valid || Scale == 0.0f) return false;
    return transformPoints(src, des, vet, 0, 2,
        [&](double x, double y, double& outputX, double& outputY) {
            const double dx = x - OriginX, dy = y - OriginY;
            outputX = (m_cosine * dx + m_sine * dy) / m_scaleX;
            outputY = (-m_sine * dx + m_cosine * dy) / m_scaleY;
        });
}

void CRotationCoorType::Test() {}

}
