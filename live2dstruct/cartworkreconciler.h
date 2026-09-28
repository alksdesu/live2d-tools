#ifndef CARTWORKRECONCILER_H
#define CARTWORKRECONCILER_H

#include "cmodelatlasedit.h"
#include "cmocmeshstructurebuilder.h"

namespace live2dstruct {
class CArtworkReconciler
{
public:
    using Triangles = QMap<QString, QVector<quint16>>;
    struct Rebase {
        QTransform pixelDelta;
        int targetSlot = -1;
        int gutter = 1;
        QRect sourcePatch;
    };
    static bool SourcePatch(const CModelArtwork::State& artwork, const QString& meshId, int gutter,
                            QRect& result, QString* error = nullptr);
    static bool ValidateLinks(const CModelArtwork::State& artwork, const CTextureWorkspace::State& workspace,
                              const CModelMaterialState& materials, const Triangles& triangles, QString* error = nullptr);
    static bool ManualAppearance(const CTextureAssets::Catalog& sources, const CModelAtlasState& before,
                                 const CTextureWorkspace::State& workspace, const CModelMaterialState& materials,
                                 const Triangles& triangles, CModelArtwork::State& result, QString* error = nullptr);
    static bool RemapMesh(const CModelArtwork::State& before, const CMocMeshStructureBuilder::MeshEdit& edit,
                          const CModelDrawableMaterial& material, const QVector<quint16>& triangles,
                          CModelArtwork::State& result, QString* error = nullptr);
    static bool RebasePlacements(const CModelArtwork::State& baseline, const CModelAtlasState& before,
                                 const CTextureWorkspace::State& workspace, const CModelMaterialState& materials,
                                 const Triangles& triangles, const QMap<QString, Rebase>& rebases,
                                 CModelArtwork::State& result, QString* error = nullptr);
};
}
#endif
