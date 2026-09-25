#ifndef CGRIDLINES_H
#define CGRIDLINES_H

#include <QtGlobal>
#include <vector>

class CGridLines
{
public:
    bool SetGrid(int columns, int rows, const std::vector<float>& positions);
    void Clear();
    const std::vector<float>& Positions() const { return m_positions; }
    const std::vector<quint32>& Indices() const { return m_indices; }

private:
    int m_columns = 0;
    int m_rows = 0;
    std::vector<float> m_positions;
    std::vector<quint32> m_indices;
};

#endif
