#include "gpuselection.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace Config
{
    namespace
    {
        QString readText(const QString& path)
        {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
        }

        QString pciName(QString vendor, QString device)
        {
            vendor.remove(QStringLiteral("0x"));
            device.remove(QStringLiteral("0x"));
            for (const QString& path :
                { QStringLiteral("/usr/share/hwdata/pci.ids"), QStringLiteral("/usr/share/misc/pci.ids") })
            {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                    continue;
                QTextStream stream(&file);
                bool inVendor = false;
                while (!stream.atEnd())
                {
                    const QString line = stream.readLine();
                    if (line.isEmpty() || line.startsWith('#'))
                        continue;
                    if (!line.startsWith('\t'))
                    {
                        if (inVendor)
                            break;
                        inVendor = line.left(4).compare(vendor, Qt::CaseInsensitive) == 0;
                    }
                    else if (inVendor && !line.startsWith(QStringLiteral("\t\t"))
                        && line.mid(1, 4).compare(device, Qt::CaseInsensitive) == 0)
                        return line.mid(5).trimmed();
                }
            }
            return {};
        }
    }

    QVector<RenderGpu> enumerateRenderGpus(const QString& drmPath, const QString& nvidiaPath)
    {
        QVector<RenderGpu> result;
#ifdef Q_OS_LINUX
        const QRegularExpression pciAddress(QStringLiteral("^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]$"));
        QSet<QString> seen;
        const QDir drm(drmPath);
        const auto nodes
            = drm.entryList({ QStringLiteral("renderD*") }, QDir::Dirs | QDir::System | QDir::NoDotAndDotDot);
        for (const QString& node : nodes)
        {
            const QString path = drm.filePath(node + QStringLiteral("/device"));
            const QString id = QFileInfo(QFileInfo(path).canonicalFilePath()).fileName().toLower();
            if (!pciAddress.match(id).hasMatch() || seen.contains(id))
                continue;
            seen.insert(id);
            const QString driver
                = QFileInfo(QFileInfo(path + QStringLiteral("/driver")).canonicalFilePath()).fileName();
            const QString vendor = readText(path + QStringLiteral("/vendor"));
            QString name;
            if (driver == QStringLiteral("nvidia"))
            {
                const QString info = readText(nvidiaPath + '/' + id + QStringLiteral("/information"));
                for (const QString& line : info.split('\n'))
                    if (line.startsWith(QStringLiteral("Model:")))
                        name = line.mid(6).trimmed();
            }
            if (name.isEmpty())
                name = pciName(vendor, readText(path + QStringLiteral("/device")));
            if (name.isEmpty())
                name = QStringLiteral("GPU %1:%2").arg(vendor, readText(path + QStringLiteral("/device")));
            // PCI-based PRIME selection is available for Mesa drivers. Do not
            // offer an explicit device when we cannot route OpenGL to it.
            const bool selectable = driver == QStringLiteral("nvidia") || driver == QStringLiteral("i915")
                || driver == QStringLiteral("xe") || driver == QStringLiteral("amdgpu")
                || driver == QStringLiteral("radeon") || driver == QStringLiteral("nouveau");
            result.push_back({ id, name, driver, selectable });
        }
        int nvidiaCount = 0;
        for (const RenderGpu& gpu : result)
            nvidiaCount += gpu.mDriver == QStringLiteral("nvidia");
        // Generic NVIDIA offload selects the first GPU. Multiple proprietary
        // NVIDIA devices require provider mapping; never claim an exact choice.
        if (nvidiaCount > 1)
            for (RenderGpu& gpu : result)
                if (gpu.mDriver == QStringLiteral("nvidia"))
                    gpu.mSelectable = false;
#else
        Q_UNUSED(drmPath)
        Q_UNUSED(nvidiaPath)
#endif
        return result;
    }

    QProcessEnvironment renderGpuEnvironment(const RenderGpu& gpu, QProcessEnvironment environment)
    {
        if (gpu.mId.isEmpty() || !gpu.mSelectable)
            return environment;
        environment.insert(QStringLiteral("OPENMW_RENDER_GPU"), gpu.mId);
        environment.remove(QStringLiteral("DRI_PRIME"));
        environment.remove(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"));
        environment.remove(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD_PROVIDER"));
        environment.remove(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"));
        if (gpu.mDriver == QStringLiteral("nvidia"))
        {
            environment.insert(QStringLiteral("__NV_PRIME_RENDER_OFFLOAD"), QStringLiteral("1"));
            environment.insert(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("nvidia"));
        }
        else
        {
            environment.insert(QStringLiteral("__GLX_VENDOR_LIBRARY_NAME"), QStringLiteral("mesa"));
            QString address = gpu.mId;
            address.replace(':', '_');
            address.replace('.', '_');
            environment.insert(QStringLiteral("DRI_PRIME"), QStringLiteral("pci-") + address);
        }
        return environment;
    }
}
