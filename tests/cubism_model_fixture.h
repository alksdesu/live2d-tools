#ifndef CUBISM_MODEL_FIXTURE_H
#define CUBISM_MODEL_FIXTURE_H

#include "framework/CubismFramework.hpp"
#include <QSet>
#include <QString>
#include <array>

namespace CubismModelFixture {
namespace Core = Live2D::Cubism::Core;

// Core getter doubles exercise the production CubismModel without the proprietary Core runtime.
struct GetterState
{
    GetterState() = default;
    GetterState(const GetterState&) = delete;
    GetterState& operator=(const GetterState&) = delete;

    int parameterCount = 2, partCount = 2, drawableCount = 2;
    std::array<const char*, 2> parameterIds{{"ParamA", "ParamB"}};
    std::array<const char*, 2> partIds{{"PartA", "PartB"}};
    std::array<const char*, 3> drawableIds{{"MeshA", "MeshB", "MeshC"}};
    std::array<float, 2> minimum{{-1, -2}}, maximum{{1, 2}}, defaults{{0, 0.25f}}, values{{0, 0.25f}};
    std::array<Core::csmParameterType, 2> types{{Core::csmParameterType_Normal, Core::csmParameterType_BlendShape}};
    std::array<int, 2> keyCounts{{3, 2}};
    std::array<std::array<float, 3>, 2> keys{{{{-1, 0, 1}}, {{-2, 2, 0}}}};
    std::array<const float*, 2> keyPointers{{keys[0].data(), keys[1].data()}};
    std::array<int, 2> partParents{{-1, 0}};
    std::array<int, 3> drawableParents{{0, 1, -1}};
    std::array<float, 2> partOpacities{{1, 0.75f}};
    std::array<float, 3> drawableOpacities{{0.9f, 0.8f, 1}};
    std::array<Core::csmFlags, 3> flags{{Core::csmIsDoubleSided, Core::csmBlendMultiplicative | Core::csmIsInvertedMask, 0}};
    std::array<Core::csmFlags, 3> dynamicFlags{{Core::csmIsVisible, Core::csmIsVisible, Core::csmIsVisible}};
    std::array<int, 3> textures{{0, 1, 2}}, drawOrders{{0, 1, 2}}, renderOrders{{0, 1, 2}};
    std::array<int, 3> vertexCounts{{4, 4, 4}}, indexCounts{{6, 6, 6}}, maskCounts{{0, 1, 0}};
    std::array<std::array<unsigned short, 6>, 3> indices{{{{0, 1, 2, 0, 2, 3}}, {{0, 1, 2, 0, 2, 3}}, {{0, 1, 2, 0, 2, 3}}}};
    std::array<const unsigned short*, 3> indexPointers{{indices[0].data(), indices[1].data(), indices[2].data()}};
    std::array<std::array<int, 2>, 3> masks{{{{0, 0}}, {{0, 0}}, {{0, 1}}}};
    std::array<const int*, 3> maskPointers{{masks[0].data(), masks[1].data(), masks[2].data()}};
    std::array<std::array<Core::csmVector2, 4>, 3> positions{{
        {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}},
        {{{-2, -2}, {2, -2}, {2, 2}, {-2, 2}}},
        {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}}
    }};
    std::array<std::array<Core::csmVector2, 4>, 3> uvs{{
        {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
        {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
        {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}}
    }};
    std::array<const Core::csmVector2*, 3> positionPointers{{positions[0].data(), positions[1].data(), positions[2].data()}};
    std::array<const Core::csmVector2*, 3> uvPointers{{uvs[0].data(), uvs[1].data(), uvs[2].data()}};
    std::array<Core::csmVector4, 3> multiply{{{1, 1, 1, 1}, {0.8f, 0.7f, 0.6f, 1}, {1, 1, 1, 1}}};
    std::array<Core::csmVector4, 3> screen{{{0, 0, 0, 1}, {0.1f, 0.2f, 0.3f, 1}, {0, 0, 0, 1}}};
    Core::csmVector2 size{1000, 800}, origin{430, 510};
    float ppu = 200;
    QSet<QString> nullGetters;
    int updates = 0;
};

struct ModelHandle
{
    GetterState* state;
};

Core::csmModel* AttachModel(void* address, GetterState* state);
GetterState& State(const Core::csmModel* model);
}

#endif
