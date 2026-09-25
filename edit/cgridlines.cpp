#include "cgridlines.h"

#include <cmath>
#include <utility>

bool CGridLines::SetGrid(int columns, int rows, const std::vector<float>& positions)
{
    const qint64 count = (static_cast<qint64>(columns) + 1) * (static_cast<qint64>(rows) + 1);
    if (columns <= 0 || rows <= 0 || count > 1000000 ||
        static_cast<quint64>(count * 2) != positions.size()) {
        Clear();
        return false;
    }
    for (float value : positions) {
        if (!std::isfinite(value)) {
            Clear();
            return false;
        }
    }
    if (columns != m_columns || rows != m_rows) {
        const quint32 stride = static_cast<quint32>(columns) + 1;
        const qint64 edgeCount = static_cast<qint64>(columns) * (rows + 1) +
                                static_cast<qint64>(rows) * (columns + 1);
        std::vector<quint32> indices;
        indices.reserve(static_cast<std::size_t>(edgeCount * 2));
        for (int row = 0; row <= rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                const quint32 start = static_cast<quint32>(row) * stride + static_cast<quint32>(column);
                indices.push_back(start);
                indices.push_back(start + 1);
            }
        }
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column <= columns; ++column) {
                const quint32 start = static_cast<quint32>(row) * stride + static_cast<quint32>(column);
                indices.push_back(start);
                indices.push_back(start + stride);
            }
        }
        m_indices = std::move(indices);
        m_columns = columns;
        m_rows = rows;
    }
    m_positions = positions;
    return true;
}

void CGridLines::Clear()
{
    m_columns = 0;
    m_rows = 0;
    m_positions.clear();
    m_indices.clear();
}
