#version 450

layout(constant_id = 0) const uint materialShader = 0u;
layout(constant_id = 1) const bool writeNormals = true;

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;
layout(set = 1, binding = 0) uniform sampler2D blendMap;
layout(set = 3, binding = 0) uniform sampler2D opaqueWaterDepth;
layout(push_constant) uniform FogState
{
    vec4 color;
    vec4 range;
}
fog;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 2) in flat vec2 alphaPolicy;
layout(location = 3) in vec2 maskUV;
layout(location = 4) in flat float materialKind;
layout(location = 5) in vec3 specularColor;
layout(location = 6) in flat vec4 skyDiffuse;
layout(location = 7) in flat vec4 skyMoon;
layout(location = 8) in flat vec4 skyAtmosphere;
layout(location = 9) in flat float opacity;
layout(location = 10) in vec4 screenPosition;
layout(location = 11) in vec3 viewPosition;
layout(location = 12) in vec3 viewNormal;
layout(location = 13) in float clipDistance;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormals;

void main()
{
    // Solid surfaces have no alpha test or clipping. Specializing away every
    // discard lets the driver reject hidden fragments before running this
    // shader, including the expensive texture and normal attachment work.
    if (materialShader != 4u && materialShader != 5u && clipDistance < 0.0)
        discard;
    bool waterSurface = materialShader == 1u;
    vec4 sampled = texture(diffuseMap, waterSurface ? uv * 3.75 + skyMoon.z * vec2(-.015, -.005) : uv);
    vec4 result = sampled * color;
    vec3 surfaceNormal = vec3(0.0, 0.0, 1.0);
    if (materialShader == 1u || (writeNormals && materialShader != 2u && materialShader != 4u))
        surfaceNormal = viewNormal * inversesqrt(max(dot(viewNormal, viewNormal), 1e-12));
    if (materialShader == 1u)
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
        if (bottom < 1.0 && abs(denominator) > 1e-8)
        {
            float bottomDistance = abs((skyAtmosphere.y - bottom * skyAtmosphere.w) / denominator);
            // Projection depth measures camera Z. Convert the separation to
            // optical distance along this pixel's viewing ray.
            thickness = max(bottomDistance - abs(viewPosition.z), 0.0) / max(abs(eyeDirection.z), 1e-6);
        }
        // Beer-Lambert absorption starts at the shore, rather than leaving the
        // first 2500 world units completely transparent. The e-folding
        // visibility distance is 1250 world units: deep water obscures the
        // bottom while the zero-thickness shoreline stays clear.
        const float absorptionDistance = 1250.0;
        float transmission = bottom < 1.0 ? exp2(-thickness * (1.44269504089 / absorptionDistance)) : 0.0;
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
    else if ((materialShader == 0u || materialShader == 5u) && materialKind > 0.5 && materialKind < 1.5)
        result.a = color.a * texture(blendMap, maskUV).a;
    else if (materialShader == 3u)
    {
        if (result.a < 0.1)
            discard;
        vec2 screenUV = screenPosition.xy / screenPosition.w * 0.5 + 0.5;
        float sceneDepth = texture(blendMap, screenUV).r;
        float occluded = gl_FragCoord.z > sceneDepth + 1e-5 ? 1.0 : 0.0;
        result = vec4((result.rg * 2.0 - 1.0) * (1.0 - occluded) * opacity * result.a, occluded, result.a);
    }
    else if (materialShader == 2u)
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
    if (materialShader != 4u)
        result.rgb += specularColor;
    bool accepted = true;
    switch (materialShader == 4u || materialShader == 5u ? 0 : int(alphaPolicy.y))
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
    if (materialShader != 4u && fog.range.y > fog.range.x && fog.range.y > 0.0)
    {
        float depth = 1.0 / gl_FragCoord.w;
        float factor = clamp((depth - fog.range.x) / (fog.range.y - fog.range.x), 0.0, 1.0);
        result.rgb = mix(result.rgb, fog.color.rgb, factor);
    }
    outColor = result;
    if (writeNormals)
        outNormals = vec4(materialShader == 2u ? vec3(0.5) : surfaceNormal * 0.5 + 0.5, result.a);
}
