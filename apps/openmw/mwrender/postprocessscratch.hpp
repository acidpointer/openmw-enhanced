#ifndef OPENMW_MWRENDER_POSTPROCESSSCRATCH_H
#define OPENMW_MWRENDER_POSTPROCESSSCRATCH_H

#include <array>
#include <tuple>

#include <osg/FrameBufferObject>
#include <osg/State>
#include <osg/Texture>
#include <osg/buffered_value>

namespace MWRender
{
    // Draws on one GL context are ordered. Cull metadata and scene inputs stay
    // double buffered, but these scratch images have no lifetime beyond a draw.
    class PostprocessScratch : public osg::Referenced
    {
    public:
        using Fbos = std::array<osg::ref_ptr<osg::FrameBufferObject>, 3>;

        void resizeGLObjectBuffers(unsigned int size)
        {
            if (size > mContexts.size())
                mContexts.resize(size);
        }

        Fbos& get(osg::State& state, const osg::Texture& texture)
        {
            auto& entry = mContexts[state.getContextID()];
            const auto descriptor = std::make_tuple(texture.getTextureTarget(), texture.getTextureWidth(),
                texture.getTextureHeight(), texture.getTextureDepth(), texture.getInternalFormat(),
                texture.getSourceFormat(), texture.getSourceType());
            if (entry.mDescriptor != descriptor)
            {
                entry.mFbos.fill(nullptr);
                entry.mDescriptor = descriptor;
            }
            return entry.mFbos;
        }

        unsigned int allocated(unsigned int context) const
        {
            unsigned int result = 0;
            for (const auto& fbo : mContexts[context].mFbos)
                if (fbo && fbo->getHandle(context))
                    ++result;
            return result;
        }

        void releaseGLObjects(osg::State* state) const
        {
            for (unsigned int i = 0; i < mContexts.size(); ++i)
                for (auto& fbo : mContexts[i].mFbos)
                    if (fbo)
                        fbo->releaseGLObjects(state);
        }

    private:
        using Descriptor = std::tuple<GLenum, int, int, int, GLint, GLenum, GLenum>;
        struct Entry
        {
            Descriptor mDescriptor{};
            Fbos mFbos;
        };
        mutable osg::buffered_object<Entry> mContexts;
    };
}

#endif
