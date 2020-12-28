/*

  NOTE: References
  
    - https://learnopengl.com/Lighting/Basic-Lighting
    - https://learnopengl.com/Advanced-Lighting/Advanced-Lighting
    - https://roystan.net/articles/toon-shader.html
  
 */

vec3 ToonBlinnPhongLighting(vec3 CameraView,
                            vec3 SurfaceColor, vec3 SurfaceNormal, float SurfaceSpecularPower, float RimParam, float RimThreshold,
                            vec3 LightDir, vec3 LightColor)
{
    // IMPORTANT: We assume LightDir is pointing from the surface to the light
    vec3 Result = vec3(0);
    float LightIntensity = 0.0f;
    
    // NOTE: Diffuse Light
    float NDotL = dot(-LightDir, SurfaceNormal);
    {
        //float DiffuseIntensity = dot(-LightDir, SurfaceNormal) > 0 ? 1 : 0;
        float DiffuseIntensity = smoothstep(0, 0.01, NDotL);
        LightIntensity += DiffuseIntensity;
    }

#if 0
    // NOTE: Specular Light
    {
        vec3 HalfwayDir = normalize(-LightDir + CameraView);
        float SpecularIntensity = pow(max(0, dot(SurfaceNormal, HalfwayDir)), SurfaceSpecularPower*SurfaceSpecularPower);
        SpecularIntensity = smoothstep(0.005, 0.01, SpecularIntensity);
        LightIntensity += SpecularIntensity;
    }
#endif
    
    // NOTE: Rim Light
    {
        float RimIntensity = (1 - dot(CameraView, SurfaceNormal)) * pow(NDotL, RimThreshold);
        RimIntensity = smoothstep(RimParam - 0.01, RimParam + 0.01, RimIntensity);
        LightIntensity += RimIntensity;
    }
    
    // NOTE: Light can only reflect the colors in the surface
    Result = LightIntensity * SurfaceColor * LightColor;
    
    return Result;
}
