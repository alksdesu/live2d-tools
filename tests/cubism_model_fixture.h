#ifndef CUBISM_MODEL_FIXTURE_H
#define CUBISM_MODEL_FIXTURE_H

#include "framework/CubismFramework.hpp"
#include <QSet>
#include <QString>
#include <array>
#include <initializer_list>

namespace CubismModelFixture {
namespace Core = Live2D::Cubism::Core;

// Core getter doubles exercise the production CubismModel without the proprietary Core runtime.
struct GetterState
{
    GetterState() = default;
    GetterState(const GetterState&) = delete;
    GetterState& operator=(const GetterState&) = delete;

    int parameterCount = 2, partCount = 2, drawableCount = 2, offscreenCount = 0;
    std::array<const char*, 4> parameterIds{{"ParamA", "ParamB", "ParamC", "ParamD"}};
    std::array<const char*, 2> partIds{{"PartA", "PartB"}};
    std::array<const char*, 3> drawableIds{{"MeshA", "MeshB", "MeshC"}};
    std::array<float, 4> minimum{{-1, -2, -1, -1}}, maximum{{1, 2, 1, 1}}, defaults{{0, 0.25f, 0.4f, 0.6f}}, values{{0, 0.25f, 0.4f, 0.6f}};
    std::array<Core::csmParameterType, 4> types{{Core::csmParameterType_Normal, Core::csmParameterType_BlendShape, Core::csmParameterType_Normal, Core::csmParameterType_Normal}};
    std::array<int, 4> keyCounts{{3, 2, 0, 0}};
    std::array<std::array<float, 3>, 4> keys{{{{-1, 0, 1}}, {{-2, 2, 0}}, {}, {}}};
    std::array<const float*, 4> keyPointers{{keys[0].data(), keys[1].data(), keys[2].data(), keys[3].data()}};
    std::array<int, 2> partParents{{-1, 0}};
    std::array<int, 3> drawableParents{{0, 1, -1}};
    std::array<float, 2> partOpacities{{1, 0.75f}};
    std::array<float, 3> drawableOpacities{{0.9f, 0.8f, 1}};
    std::array<Core::csmFlags, 3> flags{{Core::csmIsDoubleSided, Core::csmBlendMultiplicative | Core::csmIsInvertedMask, 0}};
    std::array<Core::csmFlags, 3> dynamicFlags{{Core::csmIsVisible, Core::csmIsVisible, Core::csmIsVisible}};
    std::array<int, 3> textures{{0, 1, 2}}, drawOrders{{0, 1, 2}};
    std::array<int, 5> renderOrders{{0, 1, 2, 3, 4}};
    std::array<int, 3> blendModes{{0, 2, 0}};
    std::array<int, 2> partOffscreens{{-1, -1}}, offscreenOwners{{0, 1}}, offscreenBlendModes{{0, 2}}, offscreenMaskCounts{{0, 1}};
    std::array<float, 2> offscreenOpacities{{1, .5f}};
    std::array<Core::csmFlags, 2> offscreenFlags{{0, Core::csmIsInvertedMask}};
    std::array<std::array<int, 1>, 2> offscreenMasks{{{{0}}, {{1}}}};
    std::array<const int*, 2> offscreenMaskPointers{{offscreenMasks[0].data(), offscreenMasks[1].data()}};
    std::array<Core::csmVector4, 2> offscreenMultiply{{{1, 1, 1, 1}, {.8f, .7f, .6f, 1}}};
    std::array<Core::csmVector4, 2> offscreenScreen{{{0, 0, 0, 0}, {.1f, .2f, .3f, 0}}};
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

inline void SetParameterLayout(GetterState& target, std::initializer_list<const char*> ids)
{
    GetterState defaults;
    target.parameterCount = int(ids.size());
    int index = 0;
    for (const auto* id : ids) {
        int source = -1;
        for (int i = 0; i < 4; ++i) if (QString::fromUtf8(defaults.parameterIds[i]) == QString::fromUtf8(id)) source = i;
        Q_ASSERT(source >= 0 && index < 4);
        target.parameterIds[index] = id;
        target.minimum[index] = defaults.minimum[source];
        target.maximum[index] = defaults.maximum[source];
        target.defaults[index] = defaults.defaults[source];
        target.values[index] = defaults.values[source];
        target.types[index] = defaults.types[source];
        target.keyCounts[index] = defaults.keyCounts[source];
        target.keys[index] = defaults.keys[source];
        target.keyPointers[index] = target.keys[index].data();
        ++index;
    }
}

struct ModelHandle
{
    GetterState* state;
};

Core::csmModel* AttachModel(void* address, GetterState* state);
GetterState& State(const Core::csmModel* model);
}

#endif
