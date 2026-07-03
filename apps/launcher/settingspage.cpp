#include "settingspage.hpp"

#include <array>
#include <cmath>
#include <string>

#include <QCompleter>
#include <QDesktopServices>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QString>
#include <QTextStream>
#include <QVBoxLayout>
#include <QVariant>

#include <components/config/gamesettings.hpp>
#include <components/debug/debuglog.hpp>
#include <components/files/qtconversion.hpp>
#include <components/sceneutil/enhancedsettings.hpp>
#include <components/settings/values.hpp>

#include "utils/openalutil.hpp"

namespace
{
    void loadSettingBool(const Settings::SettingValue<bool>& value, QCheckBox& checkbox)
    {
        checkbox.setCheckState(value ? Qt::Checked : Qt::Unchecked);
    }

    void saveSettingBool(const QCheckBox& checkbox, Settings::SettingValue<bool>& value)
    {
        value.set(checkbox.checkState() == Qt::Checked);
    }

    void loadSettingInt(const Settings::SettingValue<int>& value, QComboBox& comboBox)
    {
        comboBox.setCurrentIndex(value);
    }

    void loadSettingInt(const Settings::SettingValue<DetourNavigator::CollisionShapeType>& value, QComboBox& comboBox)
    {
        comboBox.setCurrentIndex(static_cast<int>(value.get()));
    }

    void saveSettingInt(const QComboBox& comboBox, Settings::SettingValue<int>& value)
    {
        value.set(comboBox.currentIndex());
    }

    void saveSettingInt(const QComboBox& comboBox, Settings::SettingValue<DetourNavigator::CollisionShapeType>& value)
    {
        value.set(static_cast<DetourNavigator::CollisionShapeType>(comboBox.currentIndex()));
    }

    void loadSettingInt(const Settings::SettingValue<int>& value, QSpinBox& spinBox)
    {
        spinBox.setValue(value);
    }

    void saveSettingInt(const QSpinBox& spinBox, Settings::SettingValue<int>& value)
    {
        value.set(spinBox.value());
    }

    int toIndex(Settings::HrtfMode value)
    {
        switch (value)
        {
            case Settings::HrtfMode::Auto:
                return 0;
            case Settings::HrtfMode::Disable:
                return 1;
            case Settings::HrtfMode::Enable:
                return 2;
        }
        return 0;
    }

    enum FileTypeRoles
    {
        Role_ThisFile = Qt::ItemDataRole::UserRole,
        Role_IsMainUserConfigDirectory,
        Role_ConfigDirectory,
        Role_LauncherLog,
        Role_OpenMWCfg,
        Role_OpenMWLog,
        Role_OpenMWCSLog,
        Role_SettingsCfg,
        Role_OpenMWEnhancedCfg,
    };

    struct FileType
    {
        FileTypeRoles itemDataRole;
        const char* name;
        bool showInAllConfigDirectories;
    };

    const std::array configDirectoryFiles{
        FileType{ Role_LauncherLog, "launcher.log", false },
        FileType{ Role_OpenMWCfg, "openmw.cfg", true },
        FileType{ Role_OpenMWLog, "openmw.log", false },
        FileType{ Role_OpenMWCSLog, "openmw-cs.log", false },
        FileType{ Role_SettingsCfg, "settings.cfg", true },
        FileType{ Role_OpenMWEnhancedCfg, "openmw-enhanced.cfg", true },
    };

    QCheckBox* addCheckBox(QGridLayout& layout, QWidget* parent, const QString& text, const QString& tooltip, int row,
        int column)
    {
        auto* checkBox = new QCheckBox(text, parent);
        checkBox->setToolTip(tooltip);
        layout.addWidget(checkBox, row, column);
        return checkBox;
    }

    void addComboRow(QGridLayout& layout, QLabel*& label, QComboBox*& comboBox, QWidget* parent, const QString& text,
        const QString& tooltip, int row)
    {
        label = new QLabel(text, parent);
        label->setToolTip(tooltip);
        comboBox = new QComboBox(parent);
        comboBox->setToolTip(tooltip);
        layout.addWidget(label, row, 0);
        layout.addWidget(comboBox, row, 1);
    }

    void setComboValue(QComboBox& comboBox, const QString& value)
    {
        const int index = comboBox.findData(value);
        if (index >= 0)
            comboBox.setCurrentIndex(index);
    }

    QString comboValue(const QComboBox& comboBox)
    {
        const QVariant data = comboBox.currentData();
        return data.isValid() ? data.toString() : comboBox.currentText();
    }

    QString boolText(const QCheckBox& checkbox)
    {
        return checkbox.checkState() == Qt::Checked ? QStringLiteral("true") : QStringLiteral("false");
    }
}

Launcher::SettingsPage::SettingsPage(
    const Files::ConfigurationManager& configurationManager, Config::GameSettings& gameSettings, QWidget* parent)
    : QWidget(parent)
    , mCfgMgr(configurationManager)
    , mGameSettings(gameSettings)
{
    setObjectName("SettingsPage");
    setupUi(this);
    setupEnhancedTab();

    for (const std::string& name : Launcher::enumerateOpenALDevices())
    {
        audioDeviceSelectorComboBox->addItem(QString::fromStdString(name), QString::fromStdString(name));
    }
    for (const std::string& name : Launcher::enumerateOpenALDevicesHrtf())
    {
        hrtfProfileSelectorComboBox->addItem(QString::fromStdString(name), QString::fromStdString(name));
    }

    loadSettings();

    mCellNameCompleter.setModel(&mCellNameCompleterModel);
    startDefaultCharacterAtField->setCompleter(&mCellNameCompleter);

    connect(configsList, &QTreeWidget::itemActivated, this, &SettingsPage::slotOpenFile);

    auto actionOpenDir = new QAction(tr("Open Directory"), configsList);
    connect(actionOpenDir, &QAction::triggered, [this]() {
        QUrl configFolderUrl = configsList->currentItem()->data(0, Role_ConfigDirectory).toUrl();
        QDesktopServices::openUrl(configFolderUrl);
    });

    QList<QAction*> openFileActions;
    openFileActions.reserve(configDirectoryFiles.size());
    for (const auto& fileType : configDirectoryFiles)
    {
        QAction* action = new QAction(tr("Open %1").arg(fileType.name), configsList);
        connect(action, &QAction::triggered, [this, role = fileType.itemDataRole]() {
            QVariant fileUrl = configsList->currentItem()->data(0, role);
            if (fileUrl.isValid())
                QDesktopServices::openUrl(fileUrl.toUrl());
        });
        openFileActions.push_back(action);
    }

    connect(configsList, &QTreeWidget::customContextMenuRequested, [=, this](const QPoint& pos) {
        if (configsList->currentItem())
        {
            QMenu contextMenu;

            contextMenu.addAction(actionOpenDir);

            bool topLevel = !configsList->currentItem()->parent();

            for (qsizetype i = 0; i < openFileActions.size(); ++i)
            {
                if (configsList->currentItem()->data(0, Role_IsMainUserConfigDirectory).toBool()
                    || configDirectoryFiles[i].showInAllConfigDirectories)
                {
                    QVariant fileUrl = configsList->currentItem()->data(0, configDirectoryFiles[i].itemDataRole);
                    bool fileExists = fileUrl.isValid();
                    openFileActions[i]->setEnabled(fileExists);
                    openFileActions[i]->setVisible(topLevel || fileExists);
                    contextMenu.addAction(openFileActions[i]);
                }
            }

            contextMenu.exec(configsList->mapToGlobal(pos));
        }
    });
}

void Launcher::SettingsPage::setupEnhancedTab()
{
    auto* enhancedPage = new QWidget(AdvancedTabWidget);
    auto* pageLayout = new QVBoxLayout(enhancedPage);

    auto* performanceGroup = new QGroupBox(tr("Performance diagnostics"), enhancedPage);
    auto* performanceLayout = new QGridLayout(performanceGroup);
    mEnhancedGpuProfile = addCheckBox(*performanceLayout, performanceGroup, tr("GPU profile"),
        tr("Enable GPU timer queries for enhanced performance diagnostics."), 0, 0);
    mEnhancedSceneProfile = addCheckBox(*performanceLayout, performanceGroup, tr("Scene profile"),
        tr("Record scene-level GPU timing. Requires GPU profile."), 0, 1);
    mEnhancedCameraProfile = addCheckBox(*performanceLayout, performanceGroup, tr("Camera profile"),
        tr("Record per-camera GPU timing. Requires scene profile."), 1, 0);
    mEnhancedRenderbinProfile = addCheckBox(*performanceLayout, performanceGroup, tr("Render bin profile"),
        tr("Record render-bin GPU timing. Intended for diagnostics, not normal play."), 1, 1);
    mEnhancedDrawableProfile = addCheckBox(*performanceLayout, performanceGroup, tr("Drawable profile"),
        tr("Record drawable-level GPU timing. This can be expensive."), 2, 0);

    auto* csvLabel = new QLabel(tr("GPU profile CSV"), performanceGroup);
    csvLabel->setToolTip(tr("Optional path for GPU profile CSV output. Leave empty to disable CSV output."));
    mEnhancedGpuProfileCsv = new QLineEdit(performanceGroup);
    mEnhancedGpuProfileCsv->setToolTip(csvLabel->toolTip());
    performanceLayout->addWidget(csvLabel, 3, 0);
    performanceLayout->addWidget(mEnhancedGpuProfileCsv, 3, 1);
    pageLayout->addWidget(performanceGroup);

    auto* rendererGroup = new QGroupBox(tr("Renderer diagnostics"), enhancedPage);
    auto* rendererLayout = new QGridLayout(rendererGroup);
    mEnhancedDisableActors = addCheckBox(*rendererLayout, rendererGroup, tr("Disable actors"),
        tr("Do not render actor scene categories. Diagnostic switch only."), 0, 0);
    mEnhancedDisableObjects = addCheckBox(*rendererLayout, rendererGroup, tr("Disable objects"),
        tr("Do not render object/static scene categories. Diagnostic switch only."), 0, 1);

    QLabel* transparentDepthModeLabel = nullptr;
    addComboRow(*rendererLayout, transparentDepthModeLabel, mEnhancedTransparentDepthMode, rendererGroup,
        tr("Transparent depth mode"), tr("Controls how transparent geometry participates in enhanced depth passes."), 1);
    mEnhancedTransparentDepthMode->addItem(tr("Legacy"), QStringLiteral("legacy"));
    mEnhancedTransparentDepthMode->addItem(tr("Alpha test only"), QStringLiteral("alpha-test-only"));
    mEnhancedTransparentDepthMode->addItem(tr("Off"), QStringLiteral("off"));
    mEnhancedTransparentDepthMode->addItem(tr("Profile only"), QStringLiteral("profile-only"));
    pageLayout->addWidget(rendererGroup);

    auto* waterGroup = new QGroupBox(tr("Water"), enhancedPage);
    auto* waterLayout = new QGridLayout(waterGroup);
    mEnhancedWaterSurface = addCheckBox(*waterLayout, waterGroup, tr("Surface"), tr("Render water surface."), 0, 0);
    mEnhancedWaterReflection
        = addCheckBox(*waterLayout, waterGroup, tr("Reflection"), tr("Render water reflections."), 0, 1);
    mEnhancedWaterRefraction
        = addCheckBox(*waterLayout, waterGroup, tr("Refraction"), tr("Render water refractions."), 1, 0);

    QLabel* waterOcclusionLabel = nullptr;
    addComboRow(*waterLayout, waterOcclusionLabel, mEnhancedWaterOcclusionCameras, waterGroup, tr("Occlusion cameras"),
        tr("Select which cameras participate in enhanced water occlusion handling."), 2);
    mEnhancedWaterOcclusionCameras->addItem(tr("Main"), QStringLiteral("main"));
    mEnhancedWaterOcclusionCameras->addItem(tr("Water"), QStringLiteral("water"));
    mEnhancedWaterOcclusionCameras->addItem(tr("All"), QStringLiteral("all"));
    pageLayout->addWidget(waterGroup);

    auto* shadowsGroup = new QGroupBox(tr("Shadows"), enhancedPage);
    auto* enhancedShadowsLayout = new QGridLayout(shadowsGroup);
    mEnhancedScreenSpaceShadows = addCheckBox(*enhancedShadowsLayout, shadowsGroup, tr("Screen-space shadows"),
        tr("Auto-enable the OpenMW Enhanced screen-space shadow post-processing shader."), 0, 0);
    mEnhancedScreenSpaceShadowsForcePostprocess
        = addCheckBox(*enhancedShadowsLayout, shadowsGroup, tr("Force post-processing"),
            tr("Keep post-processing active when screen-space shadows are enabled, even if the normal OpenMW "
               "post-processing switch is off."),
            0, 1);
    pageLayout->addWidget(shadowsGroup);

    pageLayout->addStretch(1);
    AdvancedTabWidget->addTab(enhancedPage, tr("Enhanced"));
}

void Launcher::SettingsPage::loadEnhancedSettings()
{
    SceneUtil::Enhanced::loadSettings(mCfgMgr);

    mEnhancedGpuProfile->setCheckState(
        SceneUtil::Enhanced::settingBool("Performance", "gpu profile") ? Qt::Checked : Qt::Unchecked);
    mEnhancedSceneProfile->setCheckState(
        SceneUtil::Enhanced::settingBool("Performance", "scene profile") ? Qt::Checked : Qt::Unchecked);
    mEnhancedCameraProfile->setCheckState(
        SceneUtil::Enhanced::settingBool("Performance", "camera profile") ? Qt::Checked : Qt::Unchecked);
    mEnhancedRenderbinProfile->setCheckState(
        SceneUtil::Enhanced::settingBool("Performance", "renderbin profile") ? Qt::Checked : Qt::Unchecked);
    mEnhancedDrawableProfile->setCheckState(
        SceneUtil::Enhanced::settingBool("Performance", "drawable profile") ? Qt::Checked : Qt::Unchecked);
    mEnhancedGpuProfileCsv->setText(
        QString::fromStdString(SceneUtil::Enhanced::settingString("Performance", "gpu profile csv")));

    mEnhancedDisableActors->setCheckState(
        SceneUtil::Enhanced::settingBool("Renderer", "disable actors") ? Qt::Checked : Qt::Unchecked);
    mEnhancedDisableObjects->setCheckState(
        SceneUtil::Enhanced::settingBool("Renderer", "disable objects") ? Qt::Checked : Qt::Unchecked);
    setComboValue(*mEnhancedTransparentDepthMode,
        QString::fromStdString(SceneUtil::Enhanced::settingString("Renderer", "transparent depth mode", "legacy")));

    mEnhancedWaterSurface->setCheckState(
        SceneUtil::Enhanced::settingBool("Water", "surface", true) ? Qt::Checked : Qt::Unchecked);
    mEnhancedWaterReflection->setCheckState(
        SceneUtil::Enhanced::settingBool("Water", "reflection", true) ? Qt::Checked : Qt::Unchecked);
    mEnhancedWaterRefraction->setCheckState(
        SceneUtil::Enhanced::settingBool("Water", "refraction", true) ? Qt::Checked : Qt::Unchecked);
    setComboValue(*mEnhancedWaterOcclusionCameras,
        QString::fromStdString(SceneUtil::Enhanced::settingString("Water", "occlusion cameras", "main")));

    mEnhancedScreenSpaceShadows->setCheckState(
        SceneUtil::Enhanced::settingBool("Shadows", "screen space shadows", true) ? Qt::Checked : Qt::Unchecked);
    mEnhancedScreenSpaceShadowsForcePostprocess->setCheckState(
        SceneUtil::Enhanced::settingBool("Shadows", "screen space shadows force postprocess", true) ? Qt::Checked
                                                                                                    : Qt::Unchecked);
}

void Launcher::SettingsPage::saveEnhancedSettings() const
{
    const auto settingsPath = mCfgMgr.getUserConfigPath() / "openmw-enhanced.cfg";
    QDir().mkpath(QFileInfo(Files::pathToQString(settingsPath)).absolutePath());

    QFile file(Files::pathToQString(settingsPath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        Log(Debug::Error) << "Could not write OpenMW Enhanced settings: " << settingsPath;
        return;
    }

    QTextStream stream(&file);
    stream << "# OpenMW Enhanced fork-specific settings.\n";
    stream << "# This file is managed by the launcher Enhanced settings tab.\n\n";

    stream << "[Performance]\n";
    stream << "gpu profile = " << boolText(*mEnhancedGpuProfile) << '\n';
    stream << "scene profile = " << boolText(*mEnhancedSceneProfile) << '\n';
    stream << "camera profile = " << boolText(*mEnhancedCameraProfile) << '\n';
    stream << "renderbin profile = " << boolText(*mEnhancedRenderbinProfile) << '\n';
    stream << "drawable profile = " << boolText(*mEnhancedDrawableProfile) << '\n';
    stream << "gpu profile csv = " << mEnhancedGpuProfileCsv->text() << "\n\n";

    stream << "[Renderer]\n";
    stream << "disable actors = " << boolText(*mEnhancedDisableActors) << '\n';
    stream << "disable objects = " << boolText(*mEnhancedDisableObjects) << '\n';
    stream << "transparent depth mode = " << comboValue(*mEnhancedTransparentDepthMode) << "\n\n";

    stream << "[Water]\n";
    stream << "surface = " << boolText(*mEnhancedWaterSurface) << '\n';
    stream << "reflection = " << boolText(*mEnhancedWaterReflection) << '\n';
    stream << "refraction = " << boolText(*mEnhancedWaterRefraction) << '\n';
    stream << "occlusion cameras = " << comboValue(*mEnhancedWaterOcclusionCameras) << "\n\n";

    stream << "[Shadows]\n";
    stream << "screen space shadows = " << boolText(*mEnhancedScreenSpaceShadows) << '\n';
    stream << "screen space shadows force postprocess = " << boolText(*mEnhancedScreenSpaceShadowsForcePostprocess)
           << '\n';

    file.close();
    SceneUtil::Enhanced::loadSettings(mCfgMgr);
}

void Launcher::SettingsPage::loadCellsForAutocomplete(QStringList cellNames)
{
    // Update the list of suggestions for the "Start default character at" field
    mCellNameCompleterModel.setStringList(cellNames);
    mCellNameCompleter.setCompletionMode(QCompleter::PopupCompletion);
    mCellNameCompleter.setCaseSensitivity(Qt::CaseSensitivity::CaseInsensitive);
}

void Launcher::SettingsPage::on_skipMenuCheckBox_stateChanged(int state)
{
    startDefaultCharacterAtLabel->setEnabled(state == Qt::Checked);
    startDefaultCharacterAtField->setEnabled(state == Qt::Checked);
}

void Launcher::SettingsPage::on_runScriptAfterStartupBrowseButton_clicked()
{
    QString scriptFile = QFileDialog::getOpenFileName(
        this, QObject::tr("Select script file"), QDir::currentPath(), QString(tr("Text file (*.txt)")));

    if (scriptFile.isEmpty())
        return;

    QFileInfo info(scriptFile);

    if (!info.exists() || !info.isReadable())
        return;

    const QString path(QDir::toNativeSeparators(info.absoluteFilePath()));
    runScriptAfterStartupField->setText(path);
}

namespace
{
    constexpr double cellSizeInUnits = 8192;

    double convertToCells(double unitRadius)
    {
        return unitRadius / cellSizeInUnits;
    }

    int convertToUnits(double cellGridRadius)
    {
        return static_cast<int>(cellSizeInUnits * cellGridRadius);
    }
}

bool Launcher::SettingsPage::loadSettings()
{
    // Game mechanics
    {
        loadSettingBool(Settings::game().mCanLootDuringDeathAnimation, *canLootDuringDeathAnimationCheckBox);
        loadSettingBool(Settings::game().mFollowersAttackOnSight, *followersAttackOnSightCheckBox);
        loadSettingBool(Settings::game().mRebalanceSoulGemValues, *rebalanceSoulGemValuesCheckBox);
        loadSettingBool(Settings::game().mEnchantedWeaponsAreMagical, *enchantedWeaponsMagicalCheckBox);
        loadSettingBool(
            Settings::game().mBarterDispositionChangeIsPermanent, *permanentBarterDispositionChangeCheckBox);
        loadSettingBool(Settings::game().mClassicReflectedAbsorbSpellsBehavior, *classicReflectedAbsorbSpellsCheckBox);
        loadSettingBool(Settings::game().mClassicCalmSpellsBehavior, *classicCalmSpellsCheckBox);
        loadSettingBool(
            Settings::game().mOnlyAppropriateAmmunitionBypassesResistance, *requireAppropriateAmmunitionCheckBox);
        loadSettingBool(Settings::game().mUncappedDamageFatigue, *uncappedDamageFatigueCheckBox);
        loadSettingBool(Settings::game().mNormaliseRaceSpeed, *normaliseRaceSpeedCheckBox);
        loadSettingBool(Settings::game().mSwimUpwardCorrection, *swimUpwardCorrectionCheckBox);
        loadSettingBool(Settings::game().mNPCsAvoidCollisions, *avoidCollisionsCheckBox);
        loadSettingInt(Settings::game().mStrengthInfluencesHandToHand, *unarmedFactorsStrengthComboBox);
        loadSettingBool(Settings::game().mAlwaysAllowStealingFromKnockedOutActors, *stealingFromKnockedOutCheckBox);
        loadSettingBool(Settings::navigator().mEnable, *enableNavigatorCheckBox);
        loadSettingInt(Settings::physics().mAsyncNumThreads, *physicsThreadsSpinBox);
        loadSettingBool(
            Settings::game().mAllowActorsToFollowOverWaterSurface, *allowNPCToFollowOverWaterSurfaceCheckBox);
        loadSettingInt(Settings::game().mActorCollisionShapeType, *actorCollisonShapeTypeComboBox);
    }

    // Visuals
    {
        loadSettingBool(Settings::shaders().mAutoUseObjectNormalMaps, *autoUseObjectNormalMapsCheckBox);
        loadSettingBool(Settings::shaders().mAutoUseObjectSpecularMaps, *autoUseObjectSpecularMapsCheckBox);
        loadSettingBool(Settings::shaders().mAutoUseTerrainNormalMaps, *autoUseTerrainNormalMapsCheckBox);
        loadSettingBool(Settings::shaders().mAutoUseTerrainSpecularMaps, *autoUseTerrainSpecularMapsCheckBox);
        loadSettingBool(Settings::shaders().mApplyLightingToEnvironmentMaps, *bumpMapLocalLightingCheckBox);
        loadSettingBool(Settings::shaders().mSoftParticles, *softParticlesCheckBox);
        loadSettingBool(Settings::shaders().mAntialiasAlphaTest, *antialiasAlphaTestCheckBox);
        if (Settings::shaders().mAntialiasAlphaTest == 0)
            antialiasAlphaTestCheckBox->setCheckState(Qt::Unchecked);
        loadSettingBool(Settings::shaders().mAdjustCoverageForAlphaTest, *adjustCoverageForAlphaTestCheckBox);
        loadSettingBool(Settings::shaders().mWeatherParticleOcclusion, *weatherParticleOcclusionCheckBox);
        loadSettingBool(Settings::game().mUseMagicItemAnimations, *magicItemAnimationsCheckBox);
        connect(animSourcesCheckBox, &QCheckBox::toggled, this, &SettingsPage::slotAnimSourcesToggled);
        loadSettingBool(Settings::game().mUseAdditionalAnimSources, *animSourcesCheckBox);
        if (animSourcesCheckBox->checkState() != Qt::Unchecked)
        {
            loadSettingBool(Settings::game().mWeaponSheathing, *weaponSheathingCheckBox);
            loadSettingBool(Settings::game().mShieldSheathing, *shieldSheathingCheckBox);
        }
        loadSettingBool(Settings::game().mSmoothAnimTransitions, *smoothAnimTransitionsCheckBox);
        loadSettingBool(Settings::game().mTurnToMovementDirection, *turnToMovementDirectionCheckBox);
        loadSettingBool(Settings::game().mSmoothMovement, *smoothMovementCheckBox);
        loadSettingBool(Settings::game().mPlayerMovementIgnoresAnimation, *playerMovementIgnoresAnimationCheckBox);

        connect(distantLandCheckBox, &QCheckBox::toggled, this, &SettingsPage::slotDistantLandToggled);
        bool distantLandEnabled = Settings::terrain().mDistantTerrain && Settings::terrain().mObjectPaging;
        distantLandCheckBox->setCheckState(distantLandEnabled ? Qt::Checked : Qt::Unchecked);
        slotDistantLandToggled(distantLandEnabled);

        loadSettingBool(Settings::terrain().mObjectPagingActiveGrid, *activeGridObjectPagingCheckBox);
        viewingDistanceComboBox->setValue(convertToCells(Settings::camera().mViewingDistance));
        objectPagingMinSizeComboBox->setValue(Settings::terrain().mObjectPagingMinSize);

        loadSettingBool(Settings::game().mDayNightSwitches, *nightDaySwitchesCheckBox);

        connect(postprocessEnabledCheckBox, &QCheckBox::toggled, this, &SettingsPage::slotPostProcessToggled);
        loadSettingBool(Settings::postProcessing().mEnabled, *postprocessEnabledCheckBox);
        loadSettingBool(Settings::postProcessing().mTransparentPostpass, *postprocessTransparentPostpassCheckBox);
        postprocessHDRTimeComboBox->setValue(Settings::postProcessing().mAutoExposureSpeed);

        connect(skyBlendingCheckBox, &QCheckBox::toggled, this, &SettingsPage::slotSkyBlendingToggled);
        loadSettingBool(Settings::fog().mRadialFog, *radialFogCheckBox);
        loadSettingBool(Settings::fog().mExponentialFog, *exponentialFogCheckBox);
        loadSettingBool(Settings::fog().mSkyBlending, *skyBlendingCheckBox);
        skyBlendingStartComboBox->setValue(Settings::fog().mSkyBlendingStart);

        loadSettingBool(Settings::shadows().mActorShadows, *actorShadowsCheckBox);
        loadSettingBool(Settings::shadows().mPlayerShadows, *playerShadowsCheckBox);
        loadSettingBool(Settings::shadows().mTerrainShadows, *terrainShadowsCheckBox);
        loadSettingBool(Settings::shadows().mObjectShadows, *objectShadowsCheckBox);
        loadSettingBool(Settings::shadows().mEnableIndoorShadows, *indoorShadowsCheckBox);

        const auto& boundMethod = Settings::shadows().mComputeSceneBounds.get();
        if (boundMethod == "bounds")
            shadowComputeSceneBoundsComboBox->setCurrentIndex(0);
        else if (boundMethod == "primitives")
            shadowComputeSceneBoundsComboBox->setCurrentIndex(1);
        else
            shadowComputeSceneBoundsComboBox->setCurrentIndex(2);

        const int shadowDistLimit = Settings::shadows().mMaximumShadowMapDistance;
        if (shadowDistLimit > 0)
        {
            shadowDistanceCheckBox->setCheckState(Qt::Checked);
            shadowDistanceSpinBox->setValue(shadowDistLimit);
            shadowDistanceSpinBox->setEnabled(true);
            fadeStartSpinBox->setEnabled(true);
        }

        const float shadowFadeStart = Settings::shadows().mShadowFadeStart;
        if (shadowFadeStart != 0)
            fadeStartSpinBox->setValue(shadowFadeStart);

        const int shadowRes = Settings::shadows().mShadowMapResolution;
        int shadowResIndex = shadowResolutionComboBox->findText(QString::number(shadowRes));
        if (shadowResIndex != -1)
            shadowResolutionComboBox->setCurrentIndex(shadowResIndex);
        else
        {
            shadowResolutionComboBox->addItem(QString::number(shadowRes));
            shadowResolutionComboBox->setCurrentIndex(shadowResolutionComboBox->count() - 1);
        }

        connect(shadowDistanceCheckBox, &QCheckBox::toggled, this, &SettingsPage::slotShadowDistLimitToggled);
    }

    // Audio
    {
        const std::string& selectedAudioDevice = Settings::sound().mDevice;
        if (selectedAudioDevice.empty() == false)
        {
            int audioDeviceIndex = audioDeviceSelectorComboBox->findData(QString::fromStdString(selectedAudioDevice));
            if (audioDeviceIndex != -1)
            {
                audioDeviceSelectorComboBox->setCurrentIndex(audioDeviceIndex);
            }
        }
        enableHRTFComboBox->setCurrentIndex(toIndex(Settings::sound().mHrtfEnable));
        const std::string& selectedHRTFProfile = Settings::sound().mHrtf;
        if (selectedHRTFProfile.empty() == false)
        {
            int hrtfProfileIndex = hrtfProfileSelectorComboBox->findData(QString::fromStdString(selectedHRTFProfile));
            if (hrtfProfileIndex != -1)
            {
                hrtfProfileSelectorComboBox->setCurrentIndex(hrtfProfileIndex);
            }
        }
        loadSettingBool(Settings::sound().mCameraListener, *cameraListenerCheckBox);
        dopplerSpinBox->setValue(Settings::sound().mDopplerFactor);
    }

    // Interface Changes
    {
        loadSettingBool(Settings::game().mShowEffectDuration, *showEffectDurationCheckBox);
        loadSettingBool(Settings::game().mShowEnchantChance, *showEnchantChanceCheckBox);
        loadSettingBool(Settings::game().mShowMeleeInfo, *showMeleeInfoCheckBox);
        loadSettingBool(Settings::game().mShowProjectileDamage, *showProjectileDamageCheckBox);
        loadSettingBool(Settings::gui().mColorTopicEnable, *changeDialogTopicsCheckBox);
        showOwnedComboBox->setCurrentIndex(Settings::game().mShowOwned);
        loadSettingBool(Settings::gui().mStretchMenuBackground, *stretchBackgroundCheckBox);
        connect(controllerMenusCheckBox, &QCheckBox::toggled, this, &SettingsPage::slotControllerMenusToggled);
        loadSettingBool(Settings::gui().mControllerMenus, *controllerMenusCheckBox);
        loadSettingBool(Settings::gui().mControllerTooltips, *controllerMenuTooltipsCheckBox);
        loadSettingBool(Settings::map().mAllowZooming, *useZoomOnMapCheckBox);
        loadSettingBool(Settings::game().mGraphicHerbalism, *graphicHerbalismCheckBox);
        scalingSpinBox->setValue(Settings::gui().mScalingFactor);
        fontSizeSpinBox->setValue(Settings::gui().mFontSize);
    }

    // Bug fixes
    {
        loadSettingBool(Settings::game().mPreventMerchantEquipping, *preventMerchantEquippingCheckBox);
        loadSettingBool(
            Settings::game().mTrainersTrainingSkillsBasedOnBaseSkill, *trainersTrainingSkillsBasedOnBaseSkillCheckBox);
    }

    // Miscellaneous
    {
        // Saves
        loadSettingInt(Settings::saves().mMaxQuicksaves, *maximumQuicksavesComboBox);

        // Other Settings
        QString screenshotFormatString = QString::fromStdString(Settings::general().mScreenshotFormat).toUpper();
        if (screenshotFormatComboBox->findText(screenshotFormatString) == -1)
            screenshotFormatComboBox->addItem(screenshotFormatString);
        screenshotFormatComboBox->setCurrentIndex(screenshotFormatComboBox->findText(screenshotFormatString));

        loadSettingBool(Settings::general().mNotifyOnSavedScreenshot, *notifyOnSavedScreenshotCheckBox);

        populateLoadedConfigs();
    }

    // Testing
    {
        loadSettingBool(Settings::input().mGrabCursor, *grabCursorCheckBox);

        bool skipMenu = mGameSettings.value("skip-menu").value.toInt() == 1;
        if (skipMenu)
        {
            skipMenuCheckBox->setCheckState(Qt::Checked);
        }
        startDefaultCharacterAtLabel->setEnabled(skipMenu);
        startDefaultCharacterAtField->setEnabled(skipMenu);

        startDefaultCharacterAtField->setText(mGameSettings.value("start").value);
        runScriptAfterStartupField->setText(mGameSettings.value("script-run").value);
    }

    loadEnhancedSettings();
    return true;
}

void Launcher::SettingsPage::populateLoadedConfigs()
{
    configsList->clear();

    for (const auto& path : mCfgMgr.getActiveConfigPaths())
    {
        QString configPath = QDir(Files::pathToQString(path)).absolutePath();
        QString toolTipText;

        bool isMainUserConfig = path == mCfgMgr.getUserConfigPath();

        if (path == mCfgMgr.getLocalPath())
        {
            if (isMainUserConfig)
                toolTipText = tr(
                    "Local config directory used because it contains an openmw.cfg.\n"
                    "Logs and settings changed through the launcher and in-game will be saved here.");
            else
                toolTipText = tr("Local config directory used because it contains an openmw.cfg.");
        }
        else if (path == mCfgMgr.getGlobalPath())
        {
            if (isMainUserConfig)
                toolTipText = tr(
                    "Global config directory used because local directory did not contain an openmw.cfg.\n"
                    "Logs and settings changed through the launcher and in-game will be saved here.\n"
                    "This is typically a symptom of a broken OpenMW installation or bad package.");
            else
                toolTipText = tr("Global config directory used because local directory did not contain an openmw.cfg.");
        }
        else
        {
            Config::SettingValue configSetting;
            for (const auto& v : mGameSettings.values(QString("config")))
            {
                if (Files::pathFromQString(v.value) == path)
                {
                    configSetting = v;
                    break;
                }
            }

            if (!configSetting.value.isEmpty())
            {
                const QFileInfo configPathInfo = QFileInfo(configSetting.context + "/openmw.cfg");
                if (isMainUserConfig)
                    toolTipText = tr(
                        "User config directory used because %1 contains the line config=%2.\n"
                        "Logs and settings changed through the launcher and in-game will be saved here.")
                                      .arg(configPathInfo.absoluteFilePath(), configSetting.originalRepresentation);
                else
                    toolTipText = tr("User config directory used because %1 contains the line config=%2.")
                                      .arg(configPathInfo.absoluteFilePath(), configSetting.originalRepresentation);
            }
            else if (isMainUserConfig)
                toolTipText = tr("Logs and settings changed through the launcher and in-game will be saved here.");
        }

        QTreeWidgetItem* configItem = new QTreeWidgetItem(configsList);
        configItem->setText(0, configPath);
        configItem->setToolTip(0, toolTipText);
        configItem->setExpanded(true);

        QUrl directoryUrl = QUrl::fromLocalFile(configPath);
        configItem->setData(0, Role_ThisFile, directoryUrl);
        configItem->setData(0, Role_IsMainUserConfigDirectory, isMainUserConfig);
        configItem->setData(0, Role_ConfigDirectory, directoryUrl);

        for (const auto& fileType : configDirectoryFiles)
        {
            if ((isMainUserConfig || fileType.showInAllConfigDirectories)
                && std::filesystem::exists(path / fileType.name))
            {
                QTreeWidgetItem* fileItem = new QTreeWidgetItem(configItem);
                fileItem->setText(0, fileType.name);

                QUrl url = QUrl::fromLocalFile(Files::pathToQString(path / fileType.name));

                fileItem->setData(0, Role_ThisFile, url);
                fileItem->setData(0, fileType.itemDataRole, url);
                fileItem->setData(0, Role_IsMainUserConfigDirectory, isMainUserConfig);
                fileItem->setData(0, Role_ConfigDirectory, directoryUrl);

                configItem->setData(0, fileType.itemDataRole, url);
            }
        }
    }
}

void Launcher::SettingsPage::saveSettings()
{
    // Game mechanics
    {
        saveSettingBool(*canLootDuringDeathAnimationCheckBox, Settings::game().mCanLootDuringDeathAnimation);
        saveSettingBool(*followersAttackOnSightCheckBox, Settings::game().mFollowersAttackOnSight);
        saveSettingBool(*rebalanceSoulGemValuesCheckBox, Settings::game().mRebalanceSoulGemValues);
        saveSettingBool(*enchantedWeaponsMagicalCheckBox, Settings::game().mEnchantedWeaponsAreMagical);
        saveSettingBool(
            *permanentBarterDispositionChangeCheckBox, Settings::game().mBarterDispositionChangeIsPermanent);
        saveSettingBool(*classicReflectedAbsorbSpellsCheckBox, Settings::game().mClassicReflectedAbsorbSpellsBehavior);
        saveSettingBool(*classicCalmSpellsCheckBox, Settings::game().mClassicCalmSpellsBehavior);
        saveSettingBool(
            *requireAppropriateAmmunitionCheckBox, Settings::game().mOnlyAppropriateAmmunitionBypassesResistance);
        saveSettingBool(*uncappedDamageFatigueCheckBox, Settings::game().mUncappedDamageFatigue);
        saveSettingBool(*normaliseRaceSpeedCheckBox, Settings::game().mNormaliseRaceSpeed);
        saveSettingBool(*swimUpwardCorrectionCheckBox, Settings::game().mSwimUpwardCorrection);
        saveSettingBool(*avoidCollisionsCheckBox, Settings::game().mNPCsAvoidCollisions);
        saveSettingInt(*unarmedFactorsStrengthComboBox, Settings::game().mStrengthInfluencesHandToHand);
        saveSettingBool(*stealingFromKnockedOutCheckBox, Settings::game().mAlwaysAllowStealingFromKnockedOutActors);
        saveSettingBool(*enableNavigatorCheckBox, Settings::navigator().mEnable);
        saveSettingInt(*physicsThreadsSpinBox, Settings::physics().mAsyncNumThreads);
        saveSettingBool(
            *allowNPCToFollowOverWaterSurfaceCheckBox, Settings::game().mAllowActorsToFollowOverWaterSurface);
        saveSettingInt(*actorCollisonShapeTypeComboBox, Settings::game().mActorCollisionShapeType);
    }

    // Visuals
    {
        saveSettingBool(*autoUseObjectNormalMapsCheckBox, Settings::shaders().mAutoUseObjectNormalMaps);
        saveSettingBool(*autoUseObjectSpecularMapsCheckBox, Settings::shaders().mAutoUseObjectSpecularMaps);
        saveSettingBool(*autoUseTerrainNormalMapsCheckBox, Settings::shaders().mAutoUseTerrainNormalMaps);
        saveSettingBool(*autoUseTerrainSpecularMapsCheckBox, Settings::shaders().mAutoUseTerrainSpecularMaps);
        saveSettingBool(*bumpMapLocalLightingCheckBox, Settings::shaders().mApplyLightingToEnvironmentMaps);
        saveSettingBool(*radialFogCheckBox, Settings::fog().mRadialFog);
        saveSettingBool(*softParticlesCheckBox, Settings::shaders().mSoftParticles);
        saveSettingBool(*antialiasAlphaTestCheckBox, Settings::shaders().mAntialiasAlphaTest);
        saveSettingBool(*adjustCoverageForAlphaTestCheckBox, Settings::shaders().mAdjustCoverageForAlphaTest);
        saveSettingBool(*weatherParticleOcclusionCheckBox, Settings::shaders().mWeatherParticleOcclusion);
        saveSettingBool(*magicItemAnimationsCheckBox, Settings::game().mUseMagicItemAnimations);
        saveSettingBool(*animSourcesCheckBox, Settings::game().mUseAdditionalAnimSources);
        saveSettingBool(*weaponSheathingCheckBox, Settings::game().mWeaponSheathing);
        saveSettingBool(*shieldSheathingCheckBox, Settings::game().mShieldSheathing);
        saveSettingBool(*turnToMovementDirectionCheckBox, Settings::game().mTurnToMovementDirection);
        saveSettingBool(*smoothAnimTransitionsCheckBox, Settings::game().mSmoothAnimTransitions);
        saveSettingBool(*smoothMovementCheckBox, Settings::game().mSmoothMovement);
        saveSettingBool(*playerMovementIgnoresAnimationCheckBox, Settings::game().mPlayerMovementIgnoresAnimation);

        const bool wantDistantLand = distantLandCheckBox->checkState() == Qt::Checked;
        if (wantDistantLand != (Settings::terrain().mDistantTerrain && Settings::terrain().mObjectPaging))
        {
            Settings::terrain().mDistantTerrain.set(wantDistantLand);
            Settings::terrain().mObjectPaging.set(wantDistantLand);
        }

        saveSettingBool(*activeGridObjectPagingCheckBox, Settings::terrain().mObjectPagingActiveGrid);
        Settings::camera().mViewingDistance.set(convertToUnits(viewingDistanceComboBox->value()));
        Settings::terrain().mObjectPagingMinSize.set(objectPagingMinSizeComboBox->value());
        saveSettingBool(*nightDaySwitchesCheckBox, Settings::game().mDayNightSwitches);
        saveSettingBool(*postprocessEnabledCheckBox, Settings::postProcessing().mEnabled);
        saveSettingBool(*postprocessTransparentPostpassCheckBox, Settings::postProcessing().mTransparentPostpass);
        Settings::postProcessing().mAutoExposureSpeed.set(postprocessHDRTimeComboBox->value());
        saveSettingBool(*radialFogCheckBox, Settings::fog().mRadialFog);
        saveSettingBool(*exponentialFogCheckBox, Settings::fog().mExponentialFog);
        saveSettingBool(*skyBlendingCheckBox, Settings::fog().mSkyBlending);
        Settings::fog().mSkyBlendingStart.set(skyBlendingStartComboBox->value());

        const int cShadowDist
            = shadowDistanceCheckBox->checkState() != Qt::Unchecked ? shadowDistanceSpinBox->value() : 0;
        Settings::shadows().mMaximumShadowMapDistance.set(cShadowDist);
        const float cFadeStart = fadeStartSpinBox->value();
        if (cShadowDist > 0)
            Settings::shadows().mShadowFadeStart.set(cFadeStart);

        const bool cActorShadows = actorShadowsCheckBox->checkState() != Qt::Unchecked;
        const bool cObjectShadows = objectShadowsCheckBox->checkState() != Qt::Unchecked;
        const bool cTerrainShadows = terrainShadowsCheckBox->checkState() != Qt::Unchecked;
        const bool cPlayerShadows = playerShadowsCheckBox->checkState() != Qt::Unchecked;
        if (cActorShadows || cObjectShadows || cTerrainShadows || cPlayerShadows)
        {
            Settings::shadows().mEnableShadows.set(true);
            Settings::shadows().mActorShadows.set(cActorShadows);
            Settings::shadows().mPlayerShadows.set(cPlayerShadows);
            Settings::shadows().mObjectShadows.set(cObjectShadows);
            Settings::shadows().mTerrainShadows.set(cTerrainShadows);
        }
        else
        {
            Settings::shadows().mEnableShadows.set(false);
            Settings::shadows().mActorShadows.set(false);
            Settings::shadows().mPlayerShadows.set(false);
            Settings::shadows().mObjectShadows.set(false);
            Settings::shadows().mTerrainShadows.set(false);
        }

        Settings::shadows().mEnableIndoorShadows.set(indoorShadowsCheckBox->checkState() != Qt::Unchecked);
        Settings::shadows().mShadowMapResolution.set(shadowResolutionComboBox->currentText().toInt());

        auto index = shadowComputeSceneBoundsComboBox->currentIndex();
        if (index == 0)
            Settings::shadows().mComputeSceneBounds.set("bounds");
        else if (index == 1)
            Settings::shadows().mComputeSceneBounds.set("primitives");
        else
            Settings::shadows().mComputeSceneBounds.set("none");
    }

    // Audio
    {
        if (audioDeviceSelectorComboBox->currentIndex() != 0)
            Settings::sound().mDevice.set(audioDeviceSelectorComboBox->currentText().toStdString());
        else
            Settings::sound().mDevice.set({});

        static constexpr std::array<Settings::HrtfMode, 3> hrtfModes{
            Settings::HrtfMode::Auto,
            Settings::HrtfMode::Disable,
            Settings::HrtfMode::Enable,
        };
        Settings::sound().mHrtfEnable.set(hrtfModes[enableHRTFComboBox->currentIndex()]);

        if (hrtfProfileSelectorComboBox->currentIndex() != 0)
            Settings::sound().mHrtf.set(hrtfProfileSelectorComboBox->currentText().toStdString());
        else
            Settings::sound().mHrtf.set({});

        const bool cCameraListener = cameraListenerCheckBox->checkState() != Qt::Unchecked;
        Settings::sound().mCameraListener.set(cCameraListener);

        Settings::sound().mDopplerFactor.set(dopplerSpinBox->value());
    }

    // Interface Changes
    {
        saveSettingBool(*showEffectDurationCheckBox, Settings::game().mShowEffectDuration);
        saveSettingBool(*showEnchantChanceCheckBox, Settings::game().mShowEnchantChance);
        saveSettingBool(*showMeleeInfoCheckBox, Settings::game().mShowMeleeInfo);
        saveSettingBool(*showProjectileDamageCheckBox, Settings::game().mShowProjectileDamage);
        saveSettingBool(*changeDialogTopicsCheckBox, Settings::gui().mColorTopicEnable);
        saveSettingInt(*showOwnedComboBox, Settings::game().mShowOwned);
        saveSettingBool(*stretchBackgroundCheckBox, Settings::gui().mStretchMenuBackground);
        saveSettingBool(*controllerMenusCheckBox, Settings::gui().mControllerMenus);
        saveSettingBool(*controllerMenuTooltipsCheckBox, Settings::gui().mControllerTooltips);
        saveSettingBool(*useZoomOnMapCheckBox, Settings::map().mAllowZooming);
        saveSettingBool(*graphicHerbalismCheckBox, Settings::game().mGraphicHerbalism);
        Settings::gui().mScalingFactor.set(scalingSpinBox->value());
        Settings::gui().mFontSize.set(fontSizeSpinBox->value());
    }

    // Bug fixes
    {
        saveSettingBool(*preventMerchantEquippingCheckBox, Settings::game().mPreventMerchantEquipping);
        saveSettingBool(
            *trainersTrainingSkillsBasedOnBaseSkillCheckBox, Settings::game().mTrainersTrainingSkillsBasedOnBaseSkill);
    }

    // Miscellaneous
    {
        // Saves Settings
        saveSettingInt(*maximumQuicksavesComboBox, Settings::saves().mMaxQuicksaves);

        // Other Settings
        Settings::general().mScreenshotFormat.set(screenshotFormatComboBox->currentText().toLower().toStdString());
        saveSettingBool(*notifyOnSavedScreenshotCheckBox, Settings::general().mNotifyOnSavedScreenshot);
    }

    // Testing
    {
        saveSettingBool(*grabCursorCheckBox, Settings::input().mGrabCursor);

        int skipMenu = skipMenuCheckBox->checkState() == Qt::Checked;
        if (skipMenu != mGameSettings.value("skip-menu").value.toInt())
            mGameSettings.setValue("skip-menu", { QString::number(skipMenu) });

        QString startCell = startDefaultCharacterAtField->text();
        if (startCell != mGameSettings.value("start").value)
        {
            mGameSettings.setValue("start", { startCell });
        }
        QString scriptRun = runScriptAfterStartupField->text();
        if (scriptRun != mGameSettings.value("script-run").value)
            mGameSettings.setValue("script-run", { scriptRun });
    }

    saveEnhancedSettings();
}

void Launcher::SettingsPage::slotLoadedCellsChanged(QStringList cellNames)
{
    loadCellsForAutocomplete(std::move(cellNames));
}

void Launcher::SettingsPage::slotAnimSourcesToggled(bool checked)
{
    weaponSheathingCheckBox->setEnabled(checked);
    shieldSheathingCheckBox->setEnabled(checked);
    if (!checked)
    {
        weaponSheathingCheckBox->setCheckState(Qt::Unchecked);
        shieldSheathingCheckBox->setCheckState(Qt::Unchecked);
    }
}

void Launcher::SettingsPage::slotControllerMenusToggled(bool checked)
{
    controllerMenuTooltipsCheckBox->setEnabled(checked);
}

void Launcher::SettingsPage::slotPostProcessToggled(bool checked)
{
    postprocessTransparentPostpassCheckBox->setEnabled(checked);
    postprocessHDRTimeComboBox->setEnabled(checked);
    postprocessHDRTimeLabel->setEnabled(checked);
}

void Launcher::SettingsPage::slotSkyBlendingToggled(bool checked)
{
    skyBlendingStartComboBox->setEnabled(checked);
    skyBlendingStartLabel->setEnabled(checked);
}

void Launcher::SettingsPage::slotShadowDistLimitToggled(bool checked)
{
    shadowDistanceSpinBox->setEnabled(checked);
    fadeStartSpinBox->setEnabled(checked);
}

void Launcher::SettingsPage::slotDistantLandToggled(bool checked)
{
    activeGridObjectPagingCheckBox->setEnabled(checked);
    objectPagingMinSizeComboBox->setEnabled(checked);
}

void Launcher::SettingsPage::slotOpenFile(QTreeWidgetItem* item)
{
    QUrl configFolderUrl = item->data(0, Role_ThisFile).toUrl();
    QDesktopServices::openUrl(configFolderUrl);
}
