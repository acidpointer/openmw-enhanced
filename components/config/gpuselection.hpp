#ifndef OPENMW_COMPONENTS_CONFIG_GPUSELECTION_H
#define OPENMW_COMPONENTS_CONFIG_GPUSELECTION_H

#include <QProcessEnvironment>
#include <QString>
#include <QVector>

namespace Config
{
    struct RenderGpu
    {
        QString mId;
        QString mName;
        QString mDriver;
        bool mSelectable = true;
    };

    QVector<RenderGpu> enumerateRenderGpus(const QString& drmPath = QStringLiteral("/sys/class/drm"),
        const QString& nvidiaPath = QStringLiteral("/proc/driver/nvidia/gpus"));

    // Automatic keeps the inherited environment. Explicit selection changes
    // only GPU routing; AppImage library paths and unrelated variables survive.
    QProcessEnvironment renderGpuEnvironment(const RenderGpu& gpu, QProcessEnvironment environment);
}

#endif
