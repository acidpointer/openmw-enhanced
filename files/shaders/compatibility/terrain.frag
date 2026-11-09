#version 120
#pragma import_defines(OMW_WRITE_NORMALS)

#if @useGPUShader4
    #extension GL_EXT_gpu_shader4: require
#endif

varying vec2 uv;

uniform sampler2D diffuseMap;
#if @terrainLayerBatch
uniform sampler2D diffuseMap2;
uniform sampler2D blendMap2;
#endif

#if @normalMap
uniform sampler2D normalMap;
#endif

#if @blendMap
uniform sampler2D blendMap;
#endif

varying float euclideanDepth;
varying float linearDepth;

#define PER_PIXEL_LIGHTING (@normalMap || @specularMap || @forcePPL)

#if !PER_PIXEL_LIGHTING
centroid varying vec3 shadedLighting;
centroid varying vec3 shadedSpecular;
centroid varying vec3 passLighting;
centroid varying vec3 passSpecular;
#else
#include "lib/light/clamp.glsl"
#endif
varying vec3 passViewPos;
varying vec3 passNormal;

uniform vec2 screenRes;
uniform float near;
uniform float far;

uniform mat4 texMat0;
uniform mat4 texMat1;

#include "lib/core/fragment.h.glsl"

#include "lib/material/vertexcolors.glsl"
#include "shadows_fragment.glsl"

#include "lib/material/parallax.glsl"
#include "fog.glsl"
#include "compatibility/normals.glsl"

centroid varying vec4 passColor;

void shadeTerrainLayer(sampler2D layerDiffuse, sampler2D layerBlend)
{
    Material material = getMaterial();

    vec2 adjustedUV = (texMat0 * vec4(uv, 0.0, 1.0)).xy;

#if @parallax
    float height = texture2D(normalMap, adjustedUV).a;
    adjustedUV += getParallaxOffset(transpose(normalToViewMatrix) * normalize(-passViewPos), height);
#endif
    vec4 diffuseTex = texture2D(layerDiffuse, adjustedUV);
    gl_FragData[0] = vec4(diffuseTex.xyz, 1.0);

    vec4 diffuseColor = getDiffuseColor(material, passColor);
    gl_FragData[0].a *= diffuseColor.a;

#if @blendMap
    vec2 blendMapUV = (texMat1 * vec4(uv, 0.0, 1.0)).xy;
    gl_FragData[0].a *= texture2D(layerBlend, blendMapUV).a;
#endif

#if @normalMap
    vec4 normalTex = texture2D(normalMap, adjustedUV);
    vec3 normal = normalTex.xyz * 2.0 - 1.0;
#if @reconstructNormalZ
    normal.z = sqrt(1.0 - dot(normal.xy, normal.xy));
#endif
    vec3 viewNormal = normalToView(normal);
#else
    vec3 viewNormal = normalize(gl_NormalMatrix * passNormal);
#endif

    float shadowing = unshadowedLightRatio(linearDepth);
    vec3 lighting, specular;
#if !PER_PIXEL_LIGHTING
    lighting = mix(shadedLighting, passLighting, shadowing);
    specular = mix(shadedSpecular, passSpecular, shadowing);
#else
#if @specularMap
    float shininess = 128.0; // TODO: make configurable
    vec3 specularColor = vec3(diffuseTex.a);
#else
    float shininess = material.shininess;
    vec3 specularColor = getSpecularColor(material, passColor).xyz;
#endif
    vec3 diffuseLight, ambientLight, specularLight;
    doLighting(gl_FragCoord.xy, passViewPos, viewNormal, shininess, shadowing, diffuseLight, ambientLight, specularLight);
    lighting = diffuseColor.xyz * diffuseLight + getAmbientColor(material, passColor).xyz * ambientLight + getEmissionColor(material, passColor).xyz;
    specular = specularColor * specularLight;
    clampLighting(lighting);
#endif

    gl_FragData[0].xyz = gl_FragData[0].xyz * lighting + specular;

    gl_FragData[0] = applyFogAtDist(gl_FragData[0], passViewPos, euclideanDepth, linearDepth, near, far);

#if !@disableNormals && defined(OMW_WRITE_NORMALS) && OMW_WRITE_NORMALS
    // Terrain normals are now additively blended the same way as terrain color is, thus using color's alpha.
    gl_FragData[1] = vec4(viewNormal * 0.5 + 0.5, gl_FragData[0].a);
#endif

    applyShadowDebugOverlay();
}

void main()
{
#if @blendMap
    shadeTerrainLayer(diffuseMap, blendMap);
#else
    shadeTerrainLayer(diffuseMap, diffuseMap);
#endif
#if @terrainLayerBatch
    vec4 first = gl_FragData[0];
    shadeTerrainLayer(diffuseMap2, blendMap2);
    vec4 second = gl_FragData[0];
    // Match the source-alpha products of the legacy normalized color target.
    // Batching changes per-layer framebuffer rounding and remains opt-in.
    first = clamp(first, 0.0, 1.0);
    second = clamp(second, 0.0, 1.0);
    gl_FragData[0] = vec4(first.rgb * first.a + second.rgb * second.a,
        first.a * first.a + second.a * second.a);
#if !@disableNormals && defined(OMW_WRITE_NORMALS) && OMW_WRITE_NORMALS
    // Eligible layers share the same geometric normal (no normal/parallax maps).
    gl_FragData[1] = vec4(gl_FragData[1].rgb * (first.a + second.a), 1.0);
#endif
#endif
}
