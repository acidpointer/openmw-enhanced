#ifndef OPENMW_MWRENDER_OPENGL_FRAMEDRIVER_H
#define OPENMW_MWRENDER_OPENGL_FRAMEDRIVER_H

#include <components/render/backend.hpp>

#include <osg/ref_ptr>
#include <osgViewer/Viewer>

namespace MWRender
{
    class OpenGLFrameDriver final : public Render::FrameDriver
    {
    public:
        explicit OpenGLFrameDriver(osg::ref_ptr<osgViewer::Viewer> viewer);

        Render::Api api() const override { return Render::Api::OpenGL; }
        bool isDone() const override;
        void advance(double simulationTime) override;
        unsigned int frameNumber() const override;
        void processEvents() override;
        void updateScene() override;
        void renderFrame() override;
        void stop() override;

    private:
        osg::ref_ptr<osgViewer::Viewer> mViewer;
    };
}

#endif
