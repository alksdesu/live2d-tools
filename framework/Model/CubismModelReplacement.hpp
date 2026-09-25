#ifndef CUBISMMODELREPLACEMENT_H
#define CUBISMMODELREPLACEMENT_H

#include "framework/CubismFramework.hpp"
#include <QString>
#include <functional>

namespace Live2D { namespace Cubism { namespace Framework {
class CubismMoc;
class CubismModel;
namespace Rendering { class CubismRenderer; }

class CubismModelReplacement
{
public:
    using RendererPreparation = std::function<bool(
        Rendering::CubismRenderer*, CubismModel*, const Rendering::CubismRenderer&, QString*)>;

    CubismModelReplacement() = default;
    ~CubismModelReplacement();
    CubismModelReplacement(const CubismModelReplacement&) = delete;
    CubismModelReplacement& operator=(const CubismModelReplacement&) = delete;

    bool Prepare(const csmByte* bytes, csmSizeInt size, const CubismMoc& sourceMoc, const CubismModel& source,
                 const Rendering::CubismRenderer& renderer, const RendererPreparation& prepareRenderer,
                 QString* error = nullptr);
    CubismModel* GetModel() const { return _model; }
    bool Commit(CubismMoc*& moc, CubismModel*& model, Rendering::CubismRenderer*& renderer) noexcept;

private:
    CubismMoc* _moc = nullptr;
    CubismModel* _model = nullptr;
    Rendering::CubismRenderer* _renderer = nullptr;
    const CubismMoc* _sourceMoc = nullptr;
    const CubismModel* _sourceModel = nullptr;
    const Rendering::CubismRenderer* _sourceRenderer = nullptr;
    bool _prepared = false;
};
}}}

#endif
