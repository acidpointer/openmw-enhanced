#ifndef SETTINGSPAGE_H
#define SETTINGSPAGE_H

#include <QCompleter>
#include <QStringListModel>

#include <components/files/configurationmanager.hpp>

#include "ui_settingspage.h"

namespace Config
{
    class GameSettings;
}

namespace Launcher
{
    class SettingsPage;
}

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace Launcher
{
    class SettingsPage : public QWidget, private Ui::SettingsPage
    {
        Q_OBJECT

    public:
        explicit SettingsPage(const Files::ConfigurationManager& configurationManager,
            Config::GameSettings& gameSettings, QWidget* parent = nullptr);

        bool loadSettings();
        void saveSettings();

    public slots:
        void slotLoadedCellsChanged(QStringList cellNames);

    private slots:
        void on_skipMenuCheckBox_stateChanged(int state);
        void on_runScriptAfterStartupBrowseButton_clicked();
        void slotAnimSourcesToggled(bool checked);
        void slotPostProcessToggled(bool checked);
        void slotSkyBlendingToggled(bool checked);
        void slotShadowDistLimitToggled(bool checked);
        void slotDistantLandToggled(bool checked);
        void slotControllerMenusToggled(bool checked);
        void slotOpenFile(QTreeWidgetItem* item);

    private:
        void setupEnhancedTab();
        void loadEnhancedSettings();
        void saveEnhancedSettings() const;
        void populateLoadedConfigs();

        const Files::ConfigurationManager& mCfgMgr;

        Config::GameSettings& mGameSettings;
        QCompleter mCellNameCompleter;
        QStringListModel mCellNameCompleterModel;

        QCheckBox* mEnhancedGpuProfile = nullptr;
        QCheckBox* mEnhancedSceneProfile = nullptr;
        QCheckBox* mEnhancedCameraProfile = nullptr;
        QCheckBox* mEnhancedRenderbinProfile = nullptr;
        QCheckBox* mEnhancedDrawableProfile = nullptr;
        QLineEdit* mEnhancedGpuProfileCsv = nullptr;

        QCheckBox* mEnhancedDisableActors = nullptr;
        QCheckBox* mEnhancedDisableObjects = nullptr;
        QComboBox* mEnhancedTransparentDepthMode = nullptr;

        QCheckBox* mEnhancedOcclusionCulling = nullptr;
        QCheckBox* mEnhancedOcclusionCullingTerrain = nullptr;
        QCheckBox* mEnhancedOcclusionCullingStatics = nullptr;
        QCheckBox* mEnhancedOcclusionCullingInteriors = nullptr;
        QCheckBox* mEnhancedOcclusionDebugOverlay = nullptr;
        QCheckBox* mEnhancedOcclusionDebugMessages = nullptr;
        QSpinBox* mEnhancedOcclusionBufferWidth = nullptr;
        QSpinBox* mEnhancedOcclusionBufferHeight = nullptr;
        QSpinBox* mEnhancedOcclusionTerrainLod = nullptr;
        QSpinBox* mEnhancedOcclusionTerrainRadius = nullptr;
        QDoubleSpinBox* mEnhancedOcclusionOccluderMinRadius = nullptr;
        QDoubleSpinBox* mEnhancedOcclusionOccluderMaxRadius = nullptr;
        QDoubleSpinBox* mEnhancedOcclusionOccluderShrinkFactor = nullptr;
        QSpinBox* mEnhancedOcclusionOccluderMeshResolution = nullptr;
        QSpinBox* mEnhancedOcclusionOccluderMaxMeshResolution = nullptr;
        QDoubleSpinBox* mEnhancedOcclusionOccluderInsideThreshold = nullptr;
        QDoubleSpinBox* mEnhancedOcclusionOccluderMaxDistance = nullptr;
        QSpinBox* mEnhancedOcclusionMaxTriangles = nullptr;
        QComboBox* mEnhancedOcclusionWaterCameras = nullptr;

        QCheckBox* mEnhancedWaterSurface = nullptr;
        QCheckBox* mEnhancedWaterReflection = nullptr;
        QCheckBox* mEnhancedWaterRefraction = nullptr;

        QCheckBox* mEnhancedScreenSpaceShadows = nullptr;
        QCheckBox* mEnhancedScreenSpaceShadowsForcePostprocess = nullptr;

        /**
         * Load the cells associated with the given content files for use in autocomplete
         * @param filePaths the file paths of the content files to be examined
         */
        void loadCellsForAutocomplete(QStringList filePaths);
    };
}
#endif
