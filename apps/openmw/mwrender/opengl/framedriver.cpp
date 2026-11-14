#include "framedriver.hpp"

#include <stdexcept>
#include <utility>

namespace MWRender
{
    OpenGLFrameDriver::OpenGLFrameDriver(osg::ref_ptr<osgViewer::Viewer> viewer)
        : mViewer(std::move(viewer))
    {
        if (!mViewer)
            throw std::invalid_argument("OpenGL frame driver requires a viewer");
    }

    bool OpenGLFrameDriver::isDone() const
    {
        return mViewer->done();
    }
    void OpenGLFrameDriver::advance(double simulationTime)
    {
        mViewer->advance(simulationTime);
    }
    unsigned int OpenGLFrameDriver::frameNumber() const
    {
        return mViewer->getFrameStamp()->getFrameNumber();
    }
    void OpenGLFrameDriver::processEvents()
    {
        mViewer->eventTraversal();
    }
    void OpenGLFrameDriver::updateScene()
    {
        mViewer->updateTraversal();
    }
    void OpenGLFrameDriver::renderFrame()
    {
        mViewer->renderingTraversals();
    }
    void OpenGLFrameDriver::stop()
    {
        mViewer->stopThreading();
    }
}
