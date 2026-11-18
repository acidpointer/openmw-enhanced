#version 450
#extension GL_GOOGLE_include_directive : require

#include "heightfog.glsl"

layout(constant_id = 0) const uint materialShader = 0u;
#ifdef NATIVE_PBR
const bool receivesShadows = materialShader == 11u || materialShader == 12u;
const uint surfaceShader = materialShader == 10u || materialShader == 12u || materialShader == 13u ? 5u : 0u;
#else
const bool receivesShadows = materialShader == 7u || materialShader == 8u;
const uint surfaceShader = materialShader == 7u ? 0u : materialShader == 8u ? 5u : materialShader;
#endif
layout(constant_id = 1) const bool writeNormals = true;

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;
layout(set = 1, binding = 0) uniform sampler2D blendMap;
#ifdef NATIVE_PBR
#include "sceneinstance.glsl"
#else
layout(set = 3, binding = 0) uniform sampler2D opaqueWaterDepth;
#endif
layout(push_constant) uniform SceneState
{
    vec4 color;
    vec4 range;
    vec4 worldUp;
    vec4 hemisphere;
    vec4 heightFog;
}
fog;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 2) in flat vec3 alphaPolicy;
layout(location = 3) in vec2 maskUV;
layout(location = 4) in flat float materialKind;
layout(location = 5) in vec3 specularColor;
layout(location = 6) in vec4 skyDiffuse;
layout(location = 7) in vec4 skyMoon;
layout(location = 8) in vec4 skyAtmosphere;
layout(location = 9) in flat float opacity;
layout(location = 10) in vec4 screenPosition;
layout(location = 11) in vec3 viewPosition;
layout(location = 12) in vec3 viewNormal;
layout(location = 13) in float clipDistance;
layout(location = 14) in vec3 sunDiffuseColor;
layout(location = 15) in vec3 sunSpecularColor;
#ifdef NATIVE_PBR
layout(location = 16) flat in uint pbrInstance;
#endif
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormals;

// Fixed sampler bindings avoid requiring descriptor indexing or dynamically
// indexed sampler arrays on low-end Vulkan 1.1 devices.
layout(std140, set = 2, binding = 2) uniform ShadowState
{
    mat4 matrices[8];
    mat4 regions[8];
    vec4 parameters;
    uvec4 flags;
} shadows;
layout(set = 2, binding = 3) uniform sampler2D shadowMap0;
layout(set = 2, binding = 4) uniform sampler2D shadowMap1;
layout(set = 2, binding = 5) uniform sampler2D shadowMap2;
layout(set = 2, binding = 6) uniform sampler2D shadowMap3;
layout(set = 2, binding = 7) uniform sampler2D shadowMap4;
layout(set = 2, binding = 8) uniform sampler2D shadowMap5;
layout(set = 2, binding = 9) uniform sampler2D shadowMap6;
layout(set = 2, binding = 10) uniform sampler2D shadowMap7;

float shadowCompare(sampler2D map, vec3 position)
{
    vec2 size = vec2(textureSize(map, 0));
    vec2 pixel = position.xy * size - .5;
    ivec2 base = ivec2(floor(pixel));
    ivec2 limit = ivec2(size) - 1;
    vec2 blend = fract(pixel);
    vec4 depths = vec4(texelFetch(map, clamp(base, ivec2(0), limit), 0).r,
        texelFetch(map, clamp(base + ivec2(1, 0), ivec2(0), limit), 0).r,
        texelFetch(map, clamp(base + ivec2(0, 1), ivec2(0), limit), 0).r,
        texelFetch(map, clamp(base + ivec2(1, 1), ivec2(0), limit), 0).r);
    vec4 lit = step(vec4(position.z), depths);
    return mix(mix(lit.x, lit.y, blend.x), mix(lit.z, lit.w, blend.x), blend.y);
}

float filteredShadow(sampler2D map, vec3 position)
{
    if (shadows.flags.y == 0u)
        return shadowCompare(map, position);
    const vec2 disc[8] = vec2[8](vec2(-.7071,.7071), vec2(0,-.875), vec2(.5303,.5303),
        vec2(-.625,0), vec2(.3536,-.3536), vec2(0,.375), vec2(-.1768,-.1768), vec2(.125,0));
    float angle = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(.06711056,.00583715)))) * 6.283185;
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    vec2 radius = 2.6 * mix(1.0, 3.0, clamp(position.z, 0.0, 1.0)) / vec2(textureSize(map, 0));
    float result = 0.0;
    for (int i = 0; i < 8; ++i)
        result += shadowCompare(map, position + vec3(rotation * disc[i] * radius, 0));
    return result * .125;
}

float sampleCascade(uint index, vec3 position)
{
    if (index == 0u) return filteredShadow(shadowMap0, position);
    else if (index == 1u) return filteredShadow(shadowMap1, position);
    else if (index == 2u) return filteredShadow(shadowMap2, position);
    else if (index == 3u) return filteredShadow(shadowMap3, position);
    else if (index == 4u) return filteredShadow(shadowMap4, position);
    else if (index == 5u) return filteredShadow(shadowMap5, position);
    else if (index == 6u) return filteredShadow(shadowMap6, position);
    else if (index == 7u) return filteredShadow(shadowMap7, position);
    return 1.0;
}

float sunVisibility()
{
    float distance = -viewPosition.z;
    float fade = shadows.parameters.x > 0.0
        ? clamp((distance - shadows.parameters.y) / max(shadows.parameters.x - shadows.parameters.y, 1e-5), 0.0, 1.0)
        : 0.0;
    if (fade >= 1.0)
        return 1.0;
    vec4 position = vec4(viewPosition + (viewNormal * inversesqrt(max(dot(viewNormal, viewNormal), 1e-12))) * shadows.parameters.z, 1.0);
    float result = 1.0;
    for (uint i = 0u; i < shadows.flags.x; ++i)
    {
        vec4 projected = i == 0u ? skyDiffuse : i == 1u ? skyMoon
            : i == 2u ? skyAtmosphere : i == 3u ? screenPosition : shadows.matrices[i] * position;
        if (projected.w <= 0.0)
            continue;
        vec3 uv = projected.xyz / projected.w;
        if (all(greaterThan(uv.xy, vec2(0.0))) && all(lessThan(uv.xy, vec2(1.0))))
        {
            result = min(result, sampleCascade(i, uv));
            bool validRegion = true;
            if ((shadows.flags.z & (1u << i)) != 0u)
            {
                vec4 region = shadows.regions[i] * vec4(viewPosition, 1.0);
                vec3 valid = region.xyz / max(region.w, 1e-8);
                validRegion = all(lessThan(abs(valid.xy), vec2(1.0))) && valid.z < 1.0;
            }
            if (all(greaterThan(uv, vec3(.05,.05,0))) && all(lessThan(uv, vec3(.95,.95,1))) && validRegion)
                break;
        }
    }
    return mix(result, 1.0, fade);
}

#ifdef NATIVE_PBR
#include "pbr.glsl"
#endif

void main()
{
#ifdef NATIVE_PBR
    if (materialShader == 13u)
    {
        outColor = bakePbrTerrain();
        return;
    }
    // Gradients must be evaluated before clip/alpha rejection. The geometric
    // frame and all maps share this derivative footprint.
    mat2 primary = mat2(0.0), secondary = mat2(0.0);
    vec3 dx = vec3(0.0), dy = vec3(0.0);
    uint flags = uint(alphaPolicy.z);
    if ((flags & 480u) != 0u)
    {
        primary = mat2(dFdx(uv), dFdy(uv));
    }
    if ((flags >> 19u) != 0u)
        secondary = mat2(dFdx(maskUV), dFdy(maskUV));
    if ((flags & 32u) != 0u)
    {
        dx = dFdx(viewPosition);
        dy = dFdy(viewPosition);
    }
    vec4 sampled = texture(diffuseMap, uv);
    float maskAlpha = materialKind > 0.5 && materialKind < 1.5 ? texture(blendMap, maskUV).a : sampled.a;
    if (surfaceShader != 5u && clipDistance < 0.0)
        discard;
    vec4 result = sampled * color;
    result.a = color.a * maskAlpha;
    vec3 surfaceNormal = pbrUnit(viewNormal);
    surfaceNormal = gl_FrontFacing ? surfaceNormal : -surfaceNormal;
#else
    bool reversedDepth = (uint(alphaPolicy.z) & 8u) != 0u;
    // Solid surfaces have no alpha test or clipping. Specializing away every
    // discard lets the driver reject hidden fragments before running this
    // shader, including the expensive texture and normal attachment work.
    if (surfaceShader != 4u && surfaceShader != 5u && clipDistance < 0.0)
        discard;
    bool waterSurface = surfaceShader == 1u;
    vec4 sampled = texture(diffuseMap, waterSurface ? uv * 3.75 + skyMoon.z * vec2(-.015, -.005) : uv);
    vec4 result = sampled * color;
    vec3 surfaceNormal = vec3(0.0, 0.0, 1.0);
    if (surfaceShader == 1u || (writeNormals && surfaceShader != 2u && surfaceShader != 4u))
        surfaceNormal = viewNormal * inversesqrt(max(dot(viewNormal, viewNormal), 1e-12));
    if (surfaceShader == 1u)
    {
        // The water texture carries normals; reflected color has its own
        // camera target and is projected from the current clip coordinates.
        float time = skyMoon.z;
        vec3 a = sampled.xyz * 2.0 - 1.0;
        vec3 b = texture(diffuseMap, uv * 18.75 + time * vec2(-.04, -.03) + a.xy * .05).xyz * 2.0 - 1.0;
        vec3 c = texture(diffuseMap, uv * 75.0 + time * vec2(-.02, .1) + b.xy * .05).xyz * 2.0 - 1.0;
        vec3 field = normalize(vec3(-(a.xy + b.xy + c.xy) * .5, max(a.z + b.z + c.z, .01)));
        vec3 dx = dFdx(viewPosition), dy = dFdy(viewPosition);
        vec2 tx = dFdx(uv), ty = dFdy(uv);
        float orientation = tx.x * ty.y - tx.y * ty.x < 0.0 ? -1.0 : 1.0;
        vec3 tangent = dx * ty.y - dy * tx.y;
        vec3 bitangent = dy * tx.x - dx * ty.x;
        tangent *= orientation * inversesqrt(max(dot(tangent, tangent), 1e-20));
        bitangent *= orientation * inversesqrt(max(dot(bitangent, bitangent), 1e-20));
        surfaceNormal = normalize(tangent * field.x + bitangent * field.y + surfaceNormal * field.z);
        vec3 eyeDirection = -viewPosition * inversesqrt(max(dot(viewPosition, viewPosition), 1e-12));
        // Read the opaque depth snapshot at the exact framebuffer pixel. This
        // also handles negative-height window viewports without an extra Y flip.
        ivec2 depthPixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), textureSize(opaqueWaterDepth, 0) - 1);
        float bottom = texelFetch(opaqueWaterDepth, depthPixel, 0).r;
        float denominator = bottom * skyAtmosphere.z - skyAtmosphere.x;
        float thickness = 5000.0;
        bool hasBottom = reversedDepth ? bottom > 0.0 : bottom < 1.0;
        if (hasBottom && abs(denominator) > 1e-8)
        {
            float bottomDistance = abs((skyAtmosphere.y - bottom * skyAtmosphere.w) / denominator);
            // Projection depth measures camera Z. Convert the separation to
            // optical distance along this pixel's viewing ray.
            thickness = max(bottomDistance - abs(viewPosition.z), 0.0) / max(abs(eyeDirection.z), 1e-6);
        }
        // The vertex stage calibrates log2 extinction to Water_World_Alpha.
        // Default .75 opacity transmits .25 at 128 units and .0625 at 256,
        // rather than leaving an actor-height column about 90% transparent.
        // Zero optical thickness remains clear, without a hard shoreline edge.
        float transmission = hasBottom ? exp2(-thickness * opacity) : 0.0;
        if (dot(viewNormal, -viewPosition) < 0.0)
            transmission = 1.0; // Underwater fog is applied by the scene path.
        vec3 reflected = fog.color.rgb;
        if (materialKind > 2.125)
        {
            vec2 projectedUV = screenPosition.xy / max(abs(screenPosition.w), 1e-6) * 0.5 + 0.5;
            // Reflection rows follow the same bottom-up convention as the
            // scene target. Do not introduce an extra image-space Y flip.
            vec2 reflectedUV = clamp(projectedUV + field.xy * .10 * clamp(thickness / 300.0, 0.0, 1.0),
                vec2(.001), vec2(.999));
            reflected = texture(blendMap, reflectedUV).rgb;
        }
        float facing = clamp(abs(dot(surfaceNormal, eyeDirection)), 0.0, 1.0);
        float fresnel = .02037 + .97963 * pow(1.0 - facing, 5.0);
        vec3 bodyColor = vec3(.090195, .115685, .12745) * length(skyDiffuse.rgb);
        float bodyWeight = (1.0 - fresnel) * (1.0 - transmission);
        float alpha = fresnel + bodyWeight;
        // Straight-alpha blending reproduces reflection + body absorption +
        // transmitted framebuffer color, without copying the scene color.
        result = vec4((reflected * fresnel + bodyColor * bodyWeight) / max(alpha, 1e-6), alpha);
        result.rgb *= color.rgb;
    }
    else if ((surfaceShader == 0u || surfaceShader == 5u) && materialKind > 0.5 && materialKind < 1.5)
        result.a = color.a * texture(blendMap, maskUV).a;
    else if (surfaceShader == 3u)
    {
        if (result.a < 0.1)
            discard;
        vec2 screenUV = screenPosition.xy / screenPosition.w * 0.5 + 0.5;
        float sceneDepth = texture(blendMap, screenUV).r;
        float bias = reversedDepth ? 2e-6 * max(abs(sceneDepth), abs(gl_FragCoord.z)) : 1e-5;
        float occluded = (reversedDepth ? gl_FragCoord.z < sceneDepth - bias
            : gl_FragCoord.z > sceneDepth + 1e-5) ? 1.0 : 0.0;
        result = vec4((result.rg * 2.0 - 1.0) * (1.0 - occluded) * opacity * result.a, occluded, result.a);
    }
    else if (surfaceShader == 2u)
    {
        int pass = int(materialKind) - 4;
        if (pass == 0)
            result = vec4(skyDiffuse.rgb, skyDiffuse.a * color.a);
        else if (pass == 1)
            result = vec4(sampled.rgb, sampled.a * color.a * opacity);
        else if (pass == 2)
        {
            result = vec4(clamp(sampled.rgb * skyDiffuse.rgb, 0.0, 1.0), sampled.a * color.a * opacity);
            result.rgb = mix(fog.color.rgb, result.rgb, color.a);
        }
        else if (pass == 3)
        {
            vec4 mask = texture(blendMap, uv);
            result.a = mask.a * skyAtmosphere.a;
            result.rgb
                = mask.rgb * skyAtmosphere.rgb * result.a + sampled.rgb * skyMoon.rgb * sampled.a * skyAtmosphere.a;
        }
        else if (pass == 4)
            result = vec4(sampled.rgb, sampled.a * opacity);
        else if (pass == 6)
            result = skyDiffuse;
    }
    if (surfaceShader != 4u)
        result.rgb += specularColor;
#endif
    bool accepted = true;
    switch (surfaceShader == 4u || surfaceShader == 5u ? 0 : int(alphaPolicy.y))
    {
        case 1:
            accepted = false;
            break;
        case 2:
            accepted = result.a < alphaPolicy.x;
            break;
        case 3:
            accepted = result.a <= alphaPolicy.x;
            break;
        case 4:
            accepted = result.a == alphaPolicy.x;
            break;
        case 5:
            accepted = result.a != alphaPolicy.x;
            break;
        case 6:
            accepted = result.a >= alphaPolicy.x;
            break;
        case 7:
            accepted = result.a > alphaPolicy.x;
            break;
    }
    if (!accepted)
        discard;
#ifdef NATIVE_PBR
    result.rgb = shadePbr(sampled, surfaceNormal, primary, secondary, dx, dy);
#else
    if (surfaceShader == 6u)
    {
        if ((uint(alphaPolicy.z) & 16u) != 0u && result.a <= .5)
            discard;
        outColor = vec4(gl_FragCoord.z);
        return;
    }
    if (receivesShadows && shadows.flags.x > 0u)
        result.rgb = max(result.rgb - (sampled.rgb * sunDiffuseColor + sunSpecularColor) * (1.0 - sunVisibility()), vec3(0.0));
#endif
    if (surfaceShader != 4u && fog.range.y > fog.range.x && fog.range.y > 0.0)
    {
        float depth = 1.0 / gl_FragCoord.w;
        float factor = clamp((depth - fog.range.x) / (fog.range.y - fog.range.x), 0.0, 1.0);
        if (fog.heightFog.x > 0.0)
        {
            float startHeight = fog.worldUp.w * fog.heightFog.y;
            float endHeight = startHeight + dot(viewPosition, fog.worldUp.xyz) * fog.heightFog.y;
            factor = nativeHeightFogAmount(factor, startHeight, endHeight,
                fog.heightFog.z * fog.heightFog.y, fog.heightFog.w > 0.0, fog.heightFog.x);
        }
        // Additive emitters attenuate without injecting fog light. Terrain
        // retains weighted fog; premultiplied surfaces use alpha coverage.
        float coverage = fog.range.w > 0.0 ? result.a : 1.0;
        result.rgb = result.rgb * (1.0 - factor) + fog.color.rgb * (factor * fog.range.z * coverage);
    }
    outColor = result;
    if (writeNormals)
        outNormals = vec4(surfaceShader == 2u ? vec3(0.5) : surfaceNormal * 0.5 + 0.5, result.a);
}
