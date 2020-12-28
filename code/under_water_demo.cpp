
#include "under_water_demo.h"
#include "tiled_deferred.cpp"

//
// NOTE: Asset Storage System
//

inline u32 SceneMeshAdd(render_scene* Scene, vk_image Color, vk_image Normal, VkBuffer VertexBuffer, VkBuffer IndexBuffer, u32 NumIndices)
{
    Assert(Scene->NumRenderMeshes < Scene->MaxNumRenderMeshes);
    
    u32 MeshId = Scene->NumRenderMeshes++;
    render_mesh* Mesh = Scene->RenderMeshes + MeshId;
    Mesh->Color = Color;
    Mesh->Normal = Normal;
    Mesh->VertexBuffer = VertexBuffer;
    Mesh->IndexBuffer = IndexBuffer;
    Mesh->NumIndices = NumIndices;
    Mesh->MaterialDescriptor = VkDescriptorSetAllocate(RenderState->Device, RenderState->DescriptorPool, Scene->MaterialDescLayout);
    VkDescriptorImageWrite(&RenderState->DescriptorManager, Mesh->MaterialDescriptor, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           Color.View, DemoState->PointSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageWrite(&RenderState->DescriptorManager, Mesh->MaterialDescriptor, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           Normal.View, DemoState->PointSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    return MeshId;
}

inline u32 SceneMeshAdd(render_scene* Scene, vk_image Color, vk_image Normal, procedural_mesh Mesh)
{
    u32 Result = SceneMeshAdd(Scene, Color, Normal, Mesh.Vertices, Mesh.Indices, Mesh.NumIndices);
    return Result;
}

inline void SceneOpaqueInstanceAdd(render_scene* Scene, u32 MeshId, m4 WTransform, v4 Color, float SpecularPower, float RimBound,
                                   float RimThreshold)
{
    Assert(Scene->NumOpaqueInstances < Scene->MaxNumOpaqueInstances);

    instance_entry* Instance = Scene->OpaqueInstances + Scene->NumOpaqueInstances++;
    Instance->MeshId = MeshId;
    Instance->GpuData.WTransform = WTransform;
    Instance->GpuData.WVPTransform = CameraGetVP(&Scene->Camera)*Instance->GpuData.WTransform;
    Instance->GpuData.Color = Color;
    Instance->GpuData.SpecularPower = SpecularPower;
    Instance->GpuData.RimBound = RimBound;
    Instance->GpuData.RimThreshold = RimThreshold;
}

inline void ScenePointLightAdd(render_scene* Scene, v3 Pos, v3 Color, f32 MaxDistance)
{
    Assert(Scene->NumPointLights < Scene->MaxNumPointLights);

    // TODO: Specify strength or a sphere so that we can visualize nicely too?
    point_light* PointLight = Scene->PointLights + Scene->NumPointLights++;
    PointLight->Pos = Pos;
    PointLight->Color = Color;
    PointLight->MaxDistance = MaxDistance;
}

inline void SceneDirectionalLightSet(render_scene* Scene, v3 LightDir, v3 Color, v3 AmbientColor, v3 BoundsMin, v3 BoundsMax)
{
    Scene->DirectionalLight.Dir = LightDir;
    Scene->DirectionalLight.Color = Color;
    Scene->DirectionalLight.AmbientColor = AmbientColor;
}

//
// NOTE: Demo Code
//

inline void DemoAllocGlobals(linear_arena* Arena)
{
    // IMPORTANT: These are always the top of the program memory
    DemoState = PushStruct(Arena, demo_state);
    RenderState = PushStruct(Arena, render_state);
}

DEMO_INIT(Init)
{
    // NOTE: Init Memory
    {
        linear_arena Arena = LinearArenaCreate(ProgramMemory, ProgramMemorySize);
        DemoAllocGlobals(&Arena);
        *DemoState = {};
        *RenderState = {};
        DemoState->Arena = Arena;
        DemoState->TempArena = LinearSubArena(&DemoState->Arena, MegaBytes(10));
    }

    // NOTE: Init Vulkan
    {
        {
            const char* DeviceExtensions[] =
            {
                "VK_EXT_shader_viewport_index_layer",
            };
            
            render_init_params InitParams = {};
            InitParams.ValidationEnabled = true;
            InitParams.WindowWidth = WindowWidth;
            InitParams.WindowHeight = WindowHeight;
            InitParams.StagingBufferSize = MegaBytes(400);
            InitParams.DeviceExtensionCount = ArrayCount(DeviceExtensions);
            InitParams.DeviceExtensions = DeviceExtensions;
            VkInit(VulkanLib, hInstance, WindowHandle, &DemoState->Arena, &DemoState->TempArena, InitParams);
        }
        
        // NOTE: Init descriptor pool
        {
            VkDescriptorPoolSize Pools[5] = {};
            Pools[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            Pools[0].descriptorCount = 1000;
            Pools[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            Pools[1].descriptorCount = 1000;
            Pools[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            Pools[2].descriptorCount = 1000;
            Pools[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            Pools[3].descriptorCount = 1000;
            Pools[4].type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            Pools[4].descriptorCount = 1000;
            
            VkDescriptorPoolCreateInfo CreateInfo = {};
            CreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            CreateInfo.maxSets = 1000;
            CreateInfo.poolSizeCount = ArrayCount(Pools);
            CreateInfo.pPoolSizes = Pools;
            VkCheckResult(vkCreateDescriptorPool(RenderState->Device, &CreateInfo, 0, &RenderState->DescriptorPool));
        }
    }
    
    // NOTE: Create samplers
    DemoState->PointSampler = VkSamplerCreate(RenderState->Device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f);
    DemoState->LinearSampler = VkSamplerCreate(RenderState->Device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f);
    DemoState->AnisoSampler = VkSamplerMipMapCreate(RenderState->Device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 16.0f,
                                                    VK_SAMPLER_MIPMAP_MODE_LINEAR, 0, 0, 5);    
        
    // NOTE: Init render target entries
    DemoState->SwapChainEntry = RenderTargetSwapChainEntryCreate(RenderState->WindowWidth, RenderState->WindowHeight,
                                                                 RenderState->SwapChainFormat);

    // NOTE: Copy To Swap RT
    {
        {
            vk_descriptor_layout_builder Builder = VkDescriptorLayoutBegin(&DemoState->CopyToSwapDescLayout);
            VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
            VkDescriptorLayoutEnd(RenderState->Device, &Builder);
        }

        render_target_builder Builder = RenderTargetBuilderBegin(&DemoState->Arena, &DemoState->TempArena, RenderState->WindowWidth,
                                                                 RenderState->WindowHeight);
        RenderTargetAddTarget(&Builder, &DemoState->SwapChainEntry, VkClearColorCreate(0, 0, 0, 1));
                            
        vk_render_pass_builder RpBuilder = VkRenderPassBuilderBegin(&DemoState->TempArena);

        u32 ColorId = VkRenderPassAttachmentAdd(&RpBuilder, RenderState->SwapChainFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_UNDEFINED,
                                                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        VkRenderPassSubPassBegin(&RpBuilder, VK_PIPELINE_BIND_POINT_GRAPHICS);
        VkRenderPassColorRefAdd(&RpBuilder, ColorId, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderPassSubPassEnd(&RpBuilder);

        DemoState->CopyToSwapTarget = RenderTargetBuilderEnd(&Builder, VkRenderPassBuilderEnd(&RpBuilder, RenderState->Device));
    }

    // NOTE: Init scene system
    {
        render_scene* Scene = &DemoState->Scene;

        Scene->Camera = CameraFpsCreate(V3(0, 0, -5), V3(0, 0, 1), f32(RenderState->WindowWidth / RenderState->WindowHeight),
                                        0.01f, 1000.0f, 90.0f, 1.0f, 0.005f);

        Scene->SceneBuffer = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            sizeof(scene_globals));

        Scene->MaxNumRenderMeshes = 1000;
        Scene->RenderMeshes = PushArray(&DemoState->Arena, render_mesh, Scene->MaxNumRenderMeshes);

        Scene->MaxNumOpaqueInstances = 1000;
        Scene->OpaqueInstances = PushArray(&DemoState->Arena, instance_entry, Scene->MaxNumOpaqueInstances);
        Scene->OpaqueInstanceBuffer = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     sizeof(gpu_instance_entry)*Scene->MaxNumOpaqueInstances);
        
        Scene->MaxNumPointLights = 1000;
        Scene->PointLights = PushArray(&DemoState->Arena, point_light, Scene->MaxNumPointLights);
        Scene->PointLightBuffer = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                 sizeof(point_light)*Scene->MaxNumPointLights);
        Scene->PointLightTransforms = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     sizeof(m4)*Scene->MaxNumPointLights);

        Scene->DirectionalLightGpu = VkBufferCreate(RenderState->Device, &RenderState->GpuArena,
                                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                    sizeof(directional_light));
        
        // NOTE: Create general descriptor set layouts
        {
            {
                vk_descriptor_layout_builder Builder = VkDescriptorLayoutBegin(&Scene->MaterialDescLayout);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
                VkDescriptorLayoutEnd(RenderState->Device, &Builder);
            }

            {
                vk_descriptor_layout_builder Builder = VkDescriptorLayoutBegin(&Scene->SceneDescLayout);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutAdd(&Builder, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
                VkDescriptorLayoutEnd(RenderState->Device, &Builder);
            }
        }

        // NOTE: Populate descriptors
        Scene->SceneDescriptor = VkDescriptorSetAllocate(RenderState->Device, RenderState->DescriptorPool, Scene->SceneDescLayout);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, Scene->SceneBuffer);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Scene->OpaqueInstanceBuffer);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Scene->PointLightBuffer);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Scene->PointLightTransforms);
        VkDescriptorBufferWrite(&RenderState->DescriptorManager, Scene->SceneDescriptor, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, Scene->DirectionalLightGpu);
    }

    // NOTE: Create render data
    DemoState->SwapChainFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    DemoState->CopyToSwapDesc = VkDescriptorSetAllocate(RenderState->Device, RenderState->DescriptorPool, DemoState->CopyToSwapDescLayout);
    {
        renderer_create_info CreateInfo = {};
        CreateInfo.Width = RenderState->WindowWidth; //710;
        CreateInfo.Height = RenderState->WindowHeight; //400;
        CreateInfo.ColorFormat = DemoState->SwapChainFormat;
        CreateInfo.MaterialDescLayout = DemoState->Scene.MaterialDescLayout;
        CreateInfo.SceneDescLayout = DemoState->Scene.SceneDescLayout;
        CreateInfo.Scene = &DemoState->Scene;
        TiledDeferredCreate(CreateInfo, &DemoState->CopyToSwapDesc, &DemoState->TiledDeferredState);
    }

    // NOTE: Copy To Swap FullScreen Pass
    DemoState->CopyToSwapPass = FullScreenPassCreate("shader_copy_to_swap_frag.spv", "main", &DemoState->CopyToSwapTarget, 1,
                                                     &DemoState->CopyToSwapDescLayout, 1, &DemoState->CopyToSwapDesc);
    
    // NOTE: Upload assets
    vk_commands Commands = RenderState->Commands;
    VkCommandsBegin(RenderState->Device, Commands);
    {
        render_scene* Scene = &DemoState->Scene;
        
        // NOTE: Push textures
        vk_image WhiteTexture = {};
        {
            u32 Texels[] =
            {
                0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 
                0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
                0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 
                0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
                0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 
                0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
                0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 
                0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF, 0xFF000000, 0xFFFFFFFF,
            };

            u32 Dim = 8;
            u32 ImageSize = Dim*Dim*sizeof(u32);
            WhiteTexture = VkImageCreate(RenderState->Device, &RenderState->GpuArena, Dim, Dim, VK_FORMAT_R8G8B8A8_UNORM,
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

            // TODO: Better barrier here pls
            u8* GpuMemory = VkTransferPushWriteImage(&RenderState->TransferManager, WhiteTexture.Image, Dim, Dim, ImageSize,
                                                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                     BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
                                                     BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));

            Copy(Texels, GpuMemory, ImageSize);
        }
                        
        // NOTE: Push meshes
        DemoState->Quad = SceneMeshAdd(Scene, WhiteTexture, WhiteTexture, AssetsPushQuad());
        DemoState->Cube = SceneMeshAdd(Scene, WhiteTexture, WhiteTexture, AssetsPushCube());
        DemoState->Sphere = SceneMeshAdd(Scene, WhiteTexture, WhiteTexture, AssetsPushSphere(64, 64));
        TiledDeferredAddMeshes(&DemoState->TiledDeferredState, Scene->RenderMeshes + DemoState->Quad);
        
        VkDescriptorManagerFlush(RenderState->Device, &RenderState->DescriptorManager);
        VkTransferManagerFlush(&RenderState->TransferManager, RenderState->Device, RenderState->Commands.Buffer, &RenderState->BarrierManager);
    }
    
    VkCommandsSubmit(RenderState->GraphicsQueue, Commands);
}

DEMO_DESTROY(Destroy)
{
}

DEMO_SWAPCHAIN_CHANGE(SwapChainChange)
{
    VkCheckResult(vkDeviceWaitIdle(RenderState->Device));
    VkSwapChainReCreate(&DemoState->TempArena, WindowWidth, WindowHeight, RenderState->PresentMode);

    DemoState->SwapChainEntry.Width = RenderState->WindowWidth;
    DemoState->SwapChainEntry.Height = RenderState->WindowHeight;

    DemoState->Scene.Camera.AspectRatio = f32(RenderState->WindowWidth / RenderState->WindowHeight);
    
    TiledDeferredSwapChainChange(&DemoState->TiledDeferredState, RenderState->WindowWidth, RenderState->WindowHeight,
                                 DemoState->SwapChainFormat, &DemoState->Scene, &DemoState->CopyToSwapDesc);
}

DEMO_CODE_RELOAD(CodeReload)
{
    linear_arena Arena = LinearArenaCreate(ProgramMemory, ProgramMemorySize);
    // IMPORTANT: We are relying on the memory being the same here since we have the same base ptr with the VirtualAlloc so we just need
    // to patch our global pointers here
    DemoAllocGlobals(&Arena);

    VkGetGlobalFunctionPointers(VulkanLib);
    VkGetInstanceFunctionPointers();
    VkGetDeviceFunctionPointers();
}

DEMO_MAIN_LOOP(MainLoop)
{
    u32 ImageIndex;
    VkCheckResult(vkAcquireNextImageKHR(RenderState->Device, RenderState->SwapChain, UINT64_MAX, RenderState->ImageAvailableSemaphore,
                                        VK_NULL_HANDLE, &ImageIndex));
    DemoState->SwapChainEntry.View = RenderState->SwapChainViews[ImageIndex];

    vk_commands Commands = RenderState->Commands;
    VkCommandsBegin(RenderState->Device, Commands);

    // NOTE: Update pipelines
    VkPipelineUpdateShaders(RenderState->Device, &RenderState->CpuArena, &RenderState->PipelineManager);

    RenderTargetUpdateEntries(&DemoState->TempArena, &DemoState->CopyToSwapTarget);
    
    // NOTE: Upload scene data
    {
        render_scene* Scene = &DemoState->Scene;
        Scene->NumOpaqueInstances = 0;
        Scene->NumPointLights = 0;
        CameraUpdate(&Scene->Camera, CurrInput, PrevInput);
        
        // NOTE: Populate scene
        {
            // NOTE: Add point lights
            /*
            ScenePointLightAdd(Scene, V3(0.0f, 0.0f, -1.0f), V3(1.0f, 0.0f, 0.0f), 1);
            ScenePointLightAdd(Scene, V3(-1.0f, 0.0f, 0.0f), V3(1.0f, 1.0f, 0.0f), 1);
            ScenePointLightAdd(Scene, V3(0.0f, 1.0f, 1.0f), V3(1.0f, 0.0f, 1.0f), 1);
            ScenePointLightAdd(Scene, V3(0.0f, -1.0f, 1.0f), V3(0.0f, 1.0f, 1.0f), 1);
            ScenePointLightAdd(Scene, V3(-1.0f, 0.0f, -1.0f), V3(0.0f, 0.0f, 1.0f), 1);
            */
            
            local_global f32 T = 0.0f;
            T += 0.001f;
            if (T > 2.0f * Pi32)
            {
                T = 0.0f;
            }

            //v3 LightDir = Normalize(V3(0.5f*Sin(T), 0.5f*Cos(T), 0.0f));
            v3 LightDir = Normalize(V3(0.4f, -1.0f, 0.0f));
            
            SceneDirectionalLightSet(Scene, LightDir, V3(1.0f, 1.0f, 1.0f), V3(0.4),
                                     V3(-5.0f, -5.0f, -10.0f), V3(5.0f, 5.0f, 10.0f));
            
            // NOTE: Add Instances
            {
#if 0
                i32 NumX = 1;
                i32 NumY = 1;
                i32 NumZ = 1;
                for (i32 Z = -NumZ; Z <= NumZ; ++Z)
                {
                    for (i32 Y = -NumY; Y <= NumY; ++Y)
                    {
                        for (i32 X = -NumX; X <= NumX; ++X)
                        {
                            m4 Transform = M4Pos(V3(X, Y, Z)) * M4Scale(V3(0.25f));
                            SceneOpaqueInstanceAdd(Scene, DemoState->Sphere, Transform);
                        }
                    }
                }
#endif
                
                SceneOpaqueInstanceAdd(Scene, DemoState->Sphere, M4Pos(V3(0.0f, 0.0f, 0.0f)) * M4Scale(V3(1.0f)), V4(0.3f, 0.3f, 0.9f, 1.0f),
                                       32, 0.716, 0.1);

                SceneOpaqueInstanceAdd(Scene, DemoState->Cube, M4Pos(V3(0.0f, -3.0f, 0.0f)) * M4Scale(V3(10.0f, 1.0f, 10.0f)), V4(0.2f, 0.2f, 0.7f, 1.0f),
                                       32, 0.99, 1);
                SceneOpaqueInstanceAdd(Scene, DemoState->Cube, M4Pos(V3(-3.0f, 0.0f, 0.0f)) * M4Scale(V3(1.0f, 10.0f, 10.0f)), V4(0.2f, 0.2f, 0.7f, 1.0f),
                                       32, 0.99, 1);
            }
        }        

        // NOTE: Push opaque instances
        if (Scene->NumOpaqueInstances > 0)
        {
            gpu_instance_entry* GpuData = VkTransferPushWriteArray(&RenderState->TransferManager, Scene->OpaqueInstanceBuffer, gpu_instance_entry, Scene->NumOpaqueInstances,
                                                                   BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                   BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));

            for (u32 InstanceId = 0; InstanceId < Scene->NumOpaqueInstances; ++InstanceId)
            {
                GpuData[InstanceId] = Scene->OpaqueInstances[InstanceId].GpuData;
            }
        }
        
        // NOTE: Push Point Lights
        if (Scene->NumPointLights > 0)
        {
            point_light* PointLights = VkTransferPushWriteArray(&RenderState->TransferManager, Scene->PointLightBuffer, point_light, Scene->NumPointLights,
                                                                BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
            m4* Transforms = VkTransferPushWriteArray(&RenderState->TransferManager, Scene->PointLightTransforms, m4, Scene->NumPointLights,
                                                      BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                      BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));

            for (u32 LightId = 0; LightId < Scene->NumPointLights; ++LightId)
            {
                point_light* CurrLight = Scene->PointLights + LightId;
                PointLights[LightId] = *CurrLight;
                // NOTE: Convert to view space
                v4 Test = CameraGetV(&Scene->Camera) * V4(CurrLight->Pos, 1.0f);
                PointLights[LightId].Pos = (CameraGetV(&Scene->Camera) * V4(CurrLight->Pos, 1.0f)).xyz;
                Transforms[LightId] = CameraGetVP(&Scene->Camera) * M4Pos(CurrLight->Pos) * M4Scale(V3(CurrLight->MaxDistance));
            }
        }

        // NOTE: Push Directional Lights
        {
            directional_light* GpuData = VkTransferPushWriteStruct(&RenderState->TransferManager, Scene->DirectionalLightGpu, directional_light,
                                                                   BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                   BarrierMask(VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
            Copy(&Scene->DirectionalLight, GpuData, sizeof(directional_light));
        }

        // NOTE: Push Scene Globals
        {
            scene_globals* Data = VkTransferPushWriteStruct(&RenderState->TransferManager, Scene->SceneBuffer, scene_globals,
                                                            BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                            BarrierMask(VK_ACCESS_UNIFORM_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
            *Data = {};
            Data->CameraPos = Scene->Camera.Pos;
            Data->NumPointLights = Scene->NumPointLights;
        }

        // NOTE: Push Caustics Globals
        {
            local_global f32 T = 0.0f;
            
            gpu_caustics_input_buffer* Data = VkTransferPushWriteStruct(&RenderState->TransferManager, DemoState->TiledDeferredState.CausticsInputBuffer, gpu_caustics_input_buffer,
                                                                        BarrierMask(VkAccessFlagBits(0), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
                                                                        BarrierMask(VK_ACCESS_UNIFORM_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT));
            *Data = {};
            Data->Time = T;

            T += FrameTime;
        }
        
        VkTransferManagerFlush(&RenderState->TransferManager, RenderState->Device, RenderState->Commands.Buffer, &RenderState->BarrierManager);
    }

    // NOTE: Render Scene
    TiledDeferredRender(Commands, &DemoState->TiledDeferredState, &DemoState->Scene);
    FullScreenPassRender(Commands, &DemoState->CopyToSwapPass);
    
    VkCheckResult(vkEndCommandBuffer(Commands.Buffer));
                    
    // NOTE: Render to our window surface
    // NOTE: Tell queue where we render to surface to wait
    VkPipelineStageFlags WaitDstMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo SubmitInfo = {};
    SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    SubmitInfo.waitSemaphoreCount = 1;
    SubmitInfo.pWaitSemaphores = &RenderState->ImageAvailableSemaphore;
    SubmitInfo.pWaitDstStageMask = &WaitDstMask;
    SubmitInfo.commandBufferCount = 1;
    SubmitInfo.pCommandBuffers = &Commands.Buffer;
    SubmitInfo.signalSemaphoreCount = 1;
    SubmitInfo.pSignalSemaphores = &RenderState->FinishedRenderingSemaphore;
    VkCheckResult(vkQueueSubmit(RenderState->GraphicsQueue, 1, &SubmitInfo, Commands.Fence));
    
    VkPresentInfoKHR PresentInfo = {};
    PresentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    PresentInfo.waitSemaphoreCount = 1;
    PresentInfo.pWaitSemaphores = &RenderState->FinishedRenderingSemaphore;
    PresentInfo.swapchainCount = 1;
    PresentInfo.pSwapchains = &RenderState->SwapChain;
    PresentInfo.pImageIndices = &ImageIndex;
    VkResult Result = vkQueuePresentKHR(RenderState->PresentQueue, &PresentInfo);

    switch (Result)
    {
        case VK_SUCCESS:
        {
        } break;

        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_SUBOPTIMAL_KHR:
        {
            // NOTE: Window size changed
            InvalidCodePath;
        } break;

        default:
        {
            InvalidCodePath;
        } break;
    }
}
