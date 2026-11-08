#ifndef OPENMW_MWRENDER_TRANSPARENTPASS_H
#define OPENMW_MWRENDER_TRANSPARENTPASS_H

#include <array>
#include <osg/FrameBufferObject>
#include <osg/StateSet>

#include <osgUtil/RenderBin>

namespace Shader
{
    class ShaderManager;
}

namespace MWRender
{
    class Water;

    class TransparentDepthBinCallback : public osgUtil::RenderBin::DrawCallback
    {
    public:
        TransparentDepthBinCallback(Shader::ShaderManager& shaderManager, bool postPass);

        void setWater(const Water* water) { mWater = water; }

        void drawImplementation(
            osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) override;

        std::array<osg::ref_ptr<osg::FrameBufferObject>, 2> mFbo;
        std::array<osg::ref_ptr<osg::FrameBufferObject>, 2> mMsaaFbo;
        std::array<osg::ref_ptr<osg::FrameBufferObject>, 2> mOpaqueFbo;

    private:
        osg::ref_ptr<osg::StateSet> mStateSet;
        bool mPostPass;
        const Water* mWater = nullptr;
    };

}

#endif
