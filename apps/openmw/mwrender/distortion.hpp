#ifndef OPENMW_MWRENDER_DISTORTION_H
#define OPENMW_MWRENDER_DISTORTION_H

#include <array>
#include <optional>

#include <osg/buffered_value>
#include <osgUtil/RenderBin>

namespace osg
{
    class FrameBufferObject;
}

namespace MWRender
{
    class DistortionCallback : public osgUtil::RenderBin::DrawCallback
    {
    public:
        void drawImplementation(
            osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) override;

        void setFBO(const osg::ref_ptr<osg::FrameBufferObject>& fbo, size_t frameId) { mFBO[frameId] = fbo; }
        void setOriginalFBO(const osg::ref_ptr<osg::FrameBufferObject>& fbo, size_t frameId)
        {
            mOriginalFBO[frameId] = fbo;
        }

        bool hasDistortion(unsigned int contextId, unsigned int frameNumber) const
        {
            const auto& content = mContent[contextId];
            // Only skip the resolve after the scene draw has positively identified an empty bin.
            return content.mFrameNumber != frameNumber || content.mHasDistortion;
        }

    private:
        struct Content
        {
            std::optional<unsigned int> mFrameNumber;
            bool mHasDistortion = true;
            std::array<osg::observer_ptr<osg::FrameBufferObject>, 2> mClearedFbos;
        };

        osg::buffered_object<Content> mContent;
        std::array<osg::observer_ptr<osg::FrameBufferObject>, 2> mFBO;
        std::array<osg::observer_ptr<osg::FrameBufferObject>, 2> mOriginalFBO;
    };
}

#endif
