#ifndef OPENMW_COMPONENTS_SCENEUTIL_ENHANCEDSETTINGS_H
#define OPENMW_COMPONENTS_SCENEUTIL_ENHANCEDSETTINGS_H

#include <string>
#include <string_view>

namespace Files
{
    class ConfigurationManager;
}

namespace SceneUtil::Enhanced
{
    void ensureUserSettingsFile(const Files::ConfigurationManager& cfgMgr);
    void loadSettings(const Files::ConfigurationManager& cfgMgr);

    bool settingBool(std::string_view category, std::string_view setting, bool defaultValue = false);
    int settingInt(std::string_view category, std::string_view setting, int defaultValue = 0);
    double settingDouble(std::string_view category, std::string_view setting, double defaultValue = 0.0);
    std::string settingString(std::string_view category, std::string_view setting, std::string_view defaultValue = {});

    bool occlusionCulling();
    bool occlusionCullingTerrain();
    bool occlusionCullingStatics();
    bool occlusionCullingStaticOccluders();
    bool occlusionCullingSmallObjects();
    int occlusionBufferWidth();
    int occlusionBufferHeight();
    int occlusionTerrainLod();
    int occlusionTerrainRadius();
    float occlusionOccluderMinRadius();
    float occlusionOccluderMaxRadius();
    float occlusionOccluderShrinkFactor();
    int occlusionOccluderMeshResolution();
    int occlusionOccluderMaxMeshResolution();
    float occlusionOccluderInsideThreshold();
    float occlusionOccluderMaxDistance();
    bool occlusionDebugOverlay();
    bool occlusionDebugMessages();
    bool occlusionCullingInteriors();
    int occlusionMaxTriangles();
    double occlusionStaticRasterTimeBudgetMs();
    double occlusionMinOccluderScreenRatio();
    double occlusionMinOccludeeScreenRatio();
    bool occlusionAdaptiveStatics();
    double occlusionAdaptiveMinBenefitRatio();
    int occlusionAdaptiveCooldownFrames();
    bool occlusionWaterStaticOccluders();
    std::string occlusionWaterCameras();
}

#endif
