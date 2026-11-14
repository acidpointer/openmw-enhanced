#version 450

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;
layout(set = 1, binding = 0) uniform sampler2D blendMap;
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
    if (clipDistance < 0.0)
        discard;
    vec4 sampled = texture(diffuseMap, uv);
    vec4 result = sampled * color;
    if (materialKind > 1.5 && materialKind < 2.5)
    {
        // The water texture carries normals; reflected color has its own
        // camera target and is projected from the current clip coordinates.
        vec3 field = sampled.xyz * 2.0 - 1.0;
        float waveLight = 0.65 + 0.35 * abs(field.z) * inversesqrt(max(dot(field, field), 1e-6));
        vec3 waterColor = vec3(0.10, 0.22, 0.25) * waveLight;
        float waterAlpha = 0.72;
        if (materialKind > 2.125)
        {
            vec2 projectedUV = screenPosition.xy / max(abs(screenPosition.w), 1e-6) * 0.5 + 0.5;
            vec2 reflectedUV = clamp(projectedUV + field.xy * 0.015, vec2(0.001), vec2(0.999));
            vec3 reflected = texture(blendMap, reflectedUV).rgb;
            float eyeLength = max(length(viewPosition), 1e-6);
            vec3 normal = viewNormal * inversesqrt(max(dot(viewNormal, viewNormal), 1e-6));
            float facing = clamp(abs(dot(normal, -viewPosition / eyeLength)), 0.0, 1.0);
            float fresnel = 0.04 + 0.96 * pow(1.0 - facing, 5.0);
            waterColor = mix(waterColor, reflected, fresnel);
            waterAlpha = mix(0.6, 1.0, fresnel);
        }
        result = vec4(waterColor, waterAlpha) * color;
    }
    else if (materialKind > 0.5 && materialKind < 1.5)
        result.a = color.a * texture(blendMap, maskUV).a;
    else if (materialKind > 2.5 && materialKind < 3.5)
    {
        if(result.a<0.1)discard;
        vec2 screenUV=screenPosition.xy/screenPosition.w*0.5+0.5;
        float sceneDepth=texture(blendMap,screenUV).r;
        float occluded=gl_FragCoord.z>sceneDepth+1e-5?1.0:0.0;
        result=vec4((result.rg*2.0-1.0)*(1.0-occluded)*opacity*result.a,occluded,result.a);
    }
    else if (materialKind > 3.5)
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
    result.rgb += specularColor;
    bool accepted = true;
    switch (int(alphaPolicy.y))
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
    if (fog.range.y > fog.range.x && fog.range.y > 0.0)
    {
        float depth = 1.0 / gl_FragCoord.w;
        float factor = clamp((depth - fog.range.x) / (fog.range.y - fog.range.x), 0.0, 1.0);
        result.rgb = mix(result.rgb, fog.color.rgb, factor);
    }
    outColor = result;
    vec3 normal = viewNormal * inversesqrt(max(dot(viewNormal,viewNormal),1e-12));
    outNormals = vec4(materialKind > 3.5 ? vec3(0.5) : normal*0.5+0.5,result.a);
}
