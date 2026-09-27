#ifndef CWARPGRIDRESAMPLER_H
#define CWARPGRIDRESAMPLER_H

#include <QVector>
#include <QString>
#include <array>
#include <memory>

class CExportControl;

namespace live2dstruct {
class CWarpGridResampler
{
public:
    static constexpr int MaximumDimension = 1000;
    static constexpr int MaximumCoordinates = 1000000;
    struct Influence
    {
        std::array<int, 4> indices { };
        std::array<double, 4> weights { };
        int count = 0;
    };
    class Plan
    {
    public:
        bool IsValid() const { return bool(m_data); }
        int SourceRows() const;
        int SourceColumns() const;
        int Rows() const;
        int Columns() const;
        bool Quad() const;
        bool IsUnchanged() const;
        bool ExactInteriorSubdivision() const;
        const QVector<int>& OldToNew() const;
        const QVector<Influence>& Influences() const;

    private:
        struct Data;
        std::shared_ptr<const Data> m_data;
        friend class CWarpGridResampler;
    };
    static bool Prepare(int sourceRows, int sourceColumns, bool quad, int rows, int columns, Plan& result,
        QString* error = nullptr, CExportControl* control = nullptr);
    static bool Resample(const Plan& plan, const QVector<float>& source, QVector<float>& result,
        QString* error = nullptr, CExportControl* control = nullptr);
};
}

#endif
