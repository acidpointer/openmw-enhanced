#ifndef OPENMW_COMPONENTS_RENDERASSETS_PARTICLES_H
#define OPENMW_COMPONENTS_RENDERASSETS_PARTICLES_H

#include <components/render/backend.hpp>

#include <osg/Matrixd>
#include <osgParticle/ParticleSystem>

namespace RenderAssets
{
    struct ParticleOrder
    {
        std::vector<int> mIndices;
        std::vector<osg::Vec3> mCenters;
        double mLockWaitMs = 0;
    };
    // Reuses storage and topology; positions are emitted in view space so each
    // camera gets its own billboard orientation, including reflected views.
    std::uint32_t importParticles(const osgParticle::ParticleSystem& source, const osg::Matrixd& viewFromModel,
        std::vector<Render::Vertex>& vertices, std::vector<std::uint32_t>& indices, ParticleOrder& scratch);
}

#endif
