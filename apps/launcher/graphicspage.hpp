#ifndef GRAPHICSPAGE_H
#define GRAPHICSPAGE_H

#include "ui_graphicspage.h"

#include <components/config/gpuselection.hpp>
#include <components/settings/windowmode.hpp>

namespace Files
{
    struct ConfigurationManager;
}

namespace Config
{
    class LauncherSettings;
}

class QComboBox;
class QLabel;

namespace Launcher
{
    class GraphicsSettings;

    class GraphicsPage : public QWidget, private Ui::GraphicsPage
    {
        Q_OBJECT

    public:
        GraphicsPage(Config::LauncherSettings& launcherSettings, const QString& logPath, QWidget* parent = nullptr);

        QProcessEnvironment gameEnvironment() const;

        void saveSettings();
        bool loadSettings();

    public slots:
        void screenChanged(int screen);

    private slots:
        void slotFullScreenChanged(int state);
        void slotStandardToggled(bool checked);
        void slotFramerateLimitToggled(bool checked);

    private:
        Config::LauncherSettings& mLauncherSettings;
        QVector<Config::RenderGpu> mGpus;
        QComboBox* mGpuSelector;
        QLabel* mLastRenderer;
        QString mLogPath;
        QVector<QStringList> mResolutionsPerScreen;

        void loadGpuSelection();

        static QStringList getAvailableResolutions(int screen);
        static QRect getMaximumResolution();

        bool setupSDL();
        void handleWindowModeChange(Settings::WindowMode state);
    };
}
#endif
