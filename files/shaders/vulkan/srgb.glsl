vec3 decodeSrgb(vec3 value)
{
    vec3 magnitude = abs(value);
    return sign(value) * mix(pow((magnitude + 0.055) / 1.055, vec3(2.4)), magnitude / 12.92,
        lessThanEqual(magnitude, vec3(0.04045)));
}

vec3 encodeSrgb(vec3 value)
{
    return mix(1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, value * 12.92,
        lessThanEqual(value, vec3(0.0031308)));
}
