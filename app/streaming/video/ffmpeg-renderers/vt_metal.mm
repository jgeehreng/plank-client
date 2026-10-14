// Nasty hack to avoid conflict between AVFoundation and
// libavutil both defining AVMediaType
#define AVMediaType AVMediaType_FFmpeg
#include "vt.h"
#undef AVMediaType

#include <SDL3/SDL_system.h>
#include <Limelight.h>
#include "streaming/session.h"
#include "streaming/streamutils.h"
#include "path.h"

#import <Cocoa/Cocoa.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <dispatch/dispatch.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

extern "C" {
    #include <libavutil/pixdesc.h>
    #include <libavutil/hwcontext.h>
}

#include "vt_colors.h"

#include <array>
#include <memory>
#include <vector>

struct Vertex
{
    vector_float4 position;
    vector_float2 texCoord;
};

#define MAX_VIDEO_PLANES 3

// A drawable may report presentation after the renderer has been destroyed.
// Callbacks retain only this state, never the renderer or its SDL windows.
struct MetalPresentationPacer
{
    SDL_Mutex* mutex = SDL_CreateMutex();
    SDL_Condition* condition = SDL_CreateCondition();
    int pending = 0;
    ~MetalPresentationPacer()
    {
        SDL_DestroyCondition(condition);
        SDL_DestroyMutex(mutex);
    }
};

struct MetalPresentationTarget
{
    PlankPresentationOutput output;
    SDL_MetalView view = nullptr;
    CAMetalLayer* layer = nil;
    id<CAMetalDrawable> drawable = nil;
    id<MTLBuffer> vertices = nil;
    bool visible = false;
    QSize frameSize;
    QSize drawableSize;
    QSize videoSize;
    vector_float2 horizontalCrop = {0, 1};
    bool reduceHorizontal = false;
    std::array<id<MTLTexture>, MAX_VIDEO_PLANES> reducedPlanes {};

    ~MetalPresentationTarget()
    {
        [drawable release];
        [vertices release];
        for (auto texture : reducedPlanes) [texture release];
        if (view) SDL_Metal_DestroyView(view);
    }
};

struct MetalFrameTextures
{
    std::array<CVMetalTextureRef, MAX_VIDEO_PLANES> cv {};
    ~MetalFrameTextures()
    {
        for (auto texture : cv) if (texture) CFRelease(texture);
    }
};

class VTMetalRenderer : public VTBaseRenderer
{
    friend class VTMetalRendererProbe;
public:
    VTMetalRenderer(bool hwAccel)
        : m_HwAccel(hwAccel),
          m_HwContext(nullptr),
          m_MetalLayer(nullptr),
          m_TextureCache(nullptr),
          m_CscParamsBuffer(nullptr),
          m_OverlayTextures{},
          m_OverlayLock(0),
          m_VideoPipelineState(nullptr),
          m_HorizontalReductionPipeline(nullptr),
          m_OverlayPipelineState(nullptr),
          m_ShaderLibrary(nullptr),
          m_CommandQueue(nullptr),
          m_SwMappingTextures{},
          m_LastColorSpace(-1),
          m_LastFullRange(false),
          m_Pacer(std::make_shared<MetalPresentationPacer>())
    {
    }

    virtual ~VTMetalRenderer() override
    { @autoreleasepool {
        // renderFrame waits for its command buffer, so frame textures and layers
        // have no outstanding GPU use here. Presentation callbacks own m_Pacer.
        m_Targets.clear();
        if (m_HwContext) av_buffer_unref(&m_HwContext);
        [m_CscParamsBuffer release];
        [m_VideoPipelineState release];
        [m_HorizontalReductionPipeline release];
        [m_OverlayPipelineState release];
        [m_ShaderLibrary release];
        [m_CommandQueue release];
        for (auto texture : m_OverlayTextures) [texture release];
        for (auto texture : m_SwMappingTextures) [texture release];
        if (m_TextureCache) CFRelease(m_TextureCache);
    }}

    void discardNextDrawable()
    { @autoreleasepool {
        for (auto& target : m_Targets) {
            [target->drawable release];
            target->drawable = nil;
        }
    }}

    virtual void waitToRender() override
    { @autoreleasepool {
        SDL_LockMutex(m_Pacer->mutex);
        if (m_Pacer->pending > 2) {
            SDL_WaitConditionTimeout(m_Pacer->condition, m_Pacer->mutex, 100);
        }
        SDL_UnlockMutex(m_Pacer->mutex);
        for (auto& target : m_Targets) {
            if (!target->drawable) target->drawable = [[target->layer nextDrawable] retain];
            if (!target->drawable) {
                // Keep both outputs on the same decoded frame if an output is
                // temporarily hidden/unavailable. Do not retain the other one.
                discardNextDrawable();
                return;
            }
        }
    }}

    virtual void cleanupRenderContext() override
    {
        discardNextDrawable();
    }

    bool updateVideoRegionSizeForFrame(MetalPresentationTarget& target, AVFrame* frame)
    {
        int width = 0, height = 0;
        if (!SDL_GetWindowSizeInPixels(target.output.window, &width, &height) ||
                width <= 0 || height <= 0) return false;
        const QSize drawable(width, height);
        const QSize frameSize(frame->width, frame->height);
        if (target.vertices && target.frameSize == frameSize && target.drawableSize == drawable)
            return true;

        const bool multi = m_Targets.size() > 1;
        const auto slice = PlankPresentation::sliceForDrawable(frameSize,
            multi ? m_PresentationCanvas : drawable,
            multi ? target.output.canvasRect : QRect(QPoint(0, 0), drawable), drawable);
        target.visible = slice.visible;
        target.drawableSize = drawable;
        target.frameSize = frameSize;
        if (!slice.visible) return true; // Clear this output to black.

        const QRect& dst = slice.destinationRect;
        target.videoSize = dst.size();
        target.reduceHorizontal = slice.sourceRect.width() > 8.0 * dst.width();
        target.horizontalCrop = {float(slice.sourceRect.left() / frame->width),
                                 float(slice.sourceRect.right() / frame->width)};
        // Native-size/upscale and ordinary reductions allocate no intermediate.
        for (auto& texture : target.reducedPlanes) {
            [texture release];
            texture = nil;
        }
        const float x0 = -1.0f + 2.0f * dst.x() / width;
        const float x1 = -1.0f + 2.0f * (dst.x() + dst.width()) / width;
        const float y0 = 1.0f - 2.0f * dst.y() / height;
        const float y1 = 1.0f - 2.0f * (dst.y() + dst.height()) / height;
        const float u0 = target.reduceHorizontal ? 0.0f : target.horizontalCrop.x;
        const float u1 = target.reduceHorizontal ? 1.0f : target.horizontalCrop.y;
        const float v0 = slice.sourceRect.top() / frame->height;
        const float v1 = slice.sourceRect.bottom() / frame->height;
        const Vertex verts[] = {
            {{x0, y1, 0, 1}, {u0, v1}}, {{x0, y0, 0, 1}, {u0, v0}},
            {{x1, y1, 0, 1}, {u1, v1}}, {{x1, y0, 0, 1}, {u1, v0}},
        };
        [target.vertices release];
        target.vertices = [m_MetalLayer.device newBufferWithBytes:verts length:sizeof(verts)
            options:MTLCPUCacheModeWriteCombined | MTLResourceStorageModeManaged];
        return target.vertices != nil;
    }

    int getFramePlaneCount(AVFrame* frame)
    {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            return CVPixelBufferGetPlaneCount((CVPixelBufferRef)frame->data[3]);
        }
        else {
            return av_pix_fmt_count_planes((AVPixelFormat)frame->format);
        }
    }


    bool updateColorSpaceForFrame(AVFrame* frame)
    {
        int colorspace = getFrameColorspace(frame);
        bool fullRange = isFrameFullRange(frame);
        if (colorspace != m_LastColorSpace || fullRange != m_LastFullRange) {
            discardNextDrawable();
            AVPixelFormat storage = (AVPixelFormat)frame->format;
            if (frame->hw_frames_ctx)
                storage = ((AVHWFramesContext*)frame->hw_frames_ctx->data)->sw_format;
            const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(storage);
            if (!descriptor || (descriptor->comp[0].depth != 8 && descriptor->comp[0].depth != 10))
                return false;
            const int depth = descriptor->comp[0].depth;
            const bool highBits = descriptor->comp[0].shift > 0;
            PlankVTMatrix matrix = PlankVTMatrix::Bt709;
            CFStringRef colorSpaceName = frame->color_trc == AVCOL_TRC_IEC61966_2_1 ?
                    kCGColorSpaceSRGB : kCGColorSpaceITUR_709;
            if (colorspace == COLORSPACE_IDENTITY_GBR) {
                matrix = PlankVTMatrix::IdentityGbr;
                colorSpaceName = kCGColorSpaceSRGB;
            } else if (colorspace == COLORSPACE_REC_601) {
                matrix = PlankVTMatrix::Bt601;
                colorSpaceName = kCGColorSpaceSRGB;
            } else if (colorspace == COLORSPACE_REC_2020) {
                matrix = PlankVTMatrix::Bt2020;
                colorSpaceName = kCGColorSpaceITUR_2020;
            }
            const auto paramBuffer = plankVTColorParams(matrix, fullRange, depth, highBits);
            CGColorSpaceRef newColorSpace = CGColorSpaceCreateWithName(colorSpaceName);
            for (auto& target : m_Targets) {
                target->layer.colorspace = newColorSpace;
                target->layer.pixelFormat = depth == 10 ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGRA8Unorm;
            }
            CGColorSpaceRelease(newColorSpace);

            // Create the new colorspace parameter buffer for our fragment shader
            [m_CscParamsBuffer release];
            auto bufferOptions = MTLCPUCacheModeWriteCombined | MTLResourceStorageModeManaged;
            m_CscParamsBuffer = [m_MetalLayer.device newBufferWithBytes:(void*)&paramBuffer length:sizeof(paramBuffer) options:bufferOptions];
            if (!m_CscParamsBuffer) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to create CSC parameters buffer");
                return false;
            }

            int planes = getFramePlaneCount(frame);
            SDL_assert(planes == 2 || planes == 3);

            MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor new] autorelease];
            pipelineDesc.vertexFunction = [[m_ShaderLibrary newFunctionWithName:@"vs_draw"] autorelease];
            pipelineDesc.fragmentFunction = [[m_ShaderLibrary newFunctionWithName:planes == 2 ? @"ps_draw_biplanar" : @"ps_draw_triplanar"] autorelease];
            pipelineDesc.colorAttachments[0].pixelFormat = m_MetalLayer.pixelFormat;
            [m_VideoPipelineState release];
            m_VideoPipelineState = [m_MetalLayer.device newRenderPipelineStateWithDescriptor:pipelineDesc error:nullptr];
            if (!m_VideoPipelineState) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to create video pipeline state");
                return false;
            }

            pipelineDesc = [[MTLRenderPipelineDescriptor new] autorelease];
            pipelineDesc.vertexFunction = [[m_ShaderLibrary newFunctionWithName:@"vs_draw"] autorelease];
            pipelineDesc.fragmentFunction = [[m_ShaderLibrary newFunctionWithName:@"ps_draw_rgb"] autorelease];
            pipelineDesc.colorAttachments[0].pixelFormat = m_MetalLayer.pixelFormat;
            pipelineDesc.colorAttachments[0].blendingEnabled = YES;
            pipelineDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
            pipelineDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
            pipelineDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pipelineDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
            pipelineDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pipelineDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            [m_OverlayPipelineState release];
            m_OverlayPipelineState = [m_MetalLayer.device newRenderPipelineStateWithDescriptor:pipelineDesc error:nullptr];
            if (!m_OverlayPipelineState) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to create overlay pipeline state");
                return false;
            }

            m_LastColorSpace = colorspace;
            m_LastFullRange = fullRange;
        }

        return true;
    }

    id<MTLTexture> mapPlaneForSoftwareFrame(AVFrame* frame, int planeIndex)
    {
        const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
        if (!formatDesc) {
            // This shouldn't be possible but handle it anyway
            SDL_assert(formatDesc);
            return nil;
        }

        SDL_assert(planeIndex < MAX_VIDEO_PLANES);

        NSUInteger planeWidth = planeIndex ? AV_CEIL_RSHIFT(frame->width, formatDesc->log2_chroma_w) : frame->width;
        NSUInteger planeHeight = planeIndex ? AV_CEIL_RSHIFT(frame->height, formatDesc->log2_chroma_h) : frame->height;

        // Recreate the texture if the plane size changes
        if (m_SwMappingTextures[planeIndex] && (m_SwMappingTextures[planeIndex].width != planeWidth ||
                                                m_SwMappingTextures[planeIndex].height != planeHeight)) {
            [m_SwMappingTextures[planeIndex] release];
            m_SwMappingTextures[planeIndex] = nil;
        }

        if (!m_SwMappingTextures[planeIndex]) {
            MTLPixelFormat metalFormat;

            switch (formatDesc->comp[planeIndex].step) {
            case 1:
                metalFormat = MTLPixelFormatR8Unorm;
                break;
            case 2:
                metalFormat = MTLPixelFormatR16Unorm;
                break;
            default:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Unhandled plane step: %d (plane: %d)",
                             formatDesc->comp[planeIndex].step,
                             planeIndex);
                SDL_assert(false);
                return nil;
            }

            auto texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalFormat
                                                                              width:planeWidth
                                                                             height:planeHeight
                                                                          mipmapped:NO];
            texDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
            texDesc.storageMode = MTLStorageModeManaged;
            texDesc.usage = MTLTextureUsageShaderRead;

            m_SwMappingTextures[planeIndex] = [m_MetalLayer.device newTextureWithDescriptor:texDesc];
            if (!m_SwMappingTextures[planeIndex]) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to allocate software frame texture");
                return nil;
            }
        }

        [m_SwMappingTextures[planeIndex] replaceRegion:MTLRegionMake2D(0, 0, planeWidth, planeHeight)
                                           mipmapLevel:0
                                             withBytes:frame->data[planeIndex]
                                           bytesPerRow:frame->linesize[planeIndex]];

        return m_SwMappingTextures[planeIndex];
    }

    // Caller frees frame after we return
    virtual void renderFrame(AVFrame* frame) override
    { @autoreleasepool {
        // Handle changes to the frame's colorspace from last time we rendered
        if (!updateColorSpaceForFrame(frame)) {
            // Trigger the main thread to recreate the decoder
            SDL_Event event;
            event.type = SDL_EVENT_RENDER_DEVICE_RESET;
            SDL_PushEvent(&event);
            return;
        }

        for (auto& target : m_Targets) {
            if (!updateVideoRegionSizeForFrame(*target, frame)) {
                SDL_Event event = {};
                event.type = SDL_EVENT_RENDER_DEVICE_RESET;
                SDL_PushEvent(&event);
                return;
            }
            if (!target->drawable) return;
        }

        MetalFrameTextures textures;
        size_t planes = getFramePlaneCount(frame);
        SDL_assert(planes <= MAX_VIDEO_PLANES);

        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            CVPixelBufferRef pixBuf = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);

            // Create Metal textures for the planes of the CVPixelBuffer
            for (size_t i = 0; i < planes; i++) {
                MTLPixelFormat fmt;

                switch (CVPixelBufferGetPixelFormatType(pixBuf)) {
                case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
                case kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange:
                case kCVPixelFormatType_422YpCbCr8BiPlanarFullRange:
                case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
                case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
                case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
                    fmt = (i == 0) ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm;
                    break;

                case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
                case kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange:
                case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
                case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
                case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
                case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
                    fmt = (i == 0) ? MTLPixelFormatR16Unorm : MTLPixelFormatRG16Unorm;
                    break;

                default:
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Unknown pixel format: %x",
                                 CVPixelBufferGetPixelFormatType(pixBuf));
                    return;
                }

                CVReturn err = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, m_TextureCache, pixBuf, nullptr, fmt,
                                                                         CVPixelBufferGetWidthOfPlane(pixBuf, i),
                                                                         CVPixelBufferGetHeightOfPlane(pixBuf, i),
                                                                         i,
                                                                         &textures.cv[i]);
                if (err != kCVReturnSuccess) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "CVMetalTextureCacheCreateTextureFromImage() failed: %d",
                                 err);
                    return;
                }
            }
        }

        std::array<id<MTLTexture>, MAX_VIDEO_PLANES> planesToDraw {};
        for (size_t i = 0; i < planes; ++i) {
            planesToDraw[i] = frame->format == AV_PIX_FMT_VIDEOTOOLBOX ?
                CVMetalTextureGetTexture(textures.cv[i]) : mapPlaneForSoftwareFrame(frame, i);
            if (!planesToDraw[i]) return;
        }
        // Allocate before encoding any output or incrementing presentation
        // counters, so an allocation failure cannot strand a pending callback.
        for (auto& target : m_Targets) {
            if (!target->visible || !target->reduceHorizontal) continue;
            for (size_t i = 0; i < planes; ++i) {
                auto& reduced = target->reducedPlanes[i];
                if (reduced && reduced.width == NSUInteger(target->videoSize.width()) &&
                        reduced.height == planesToDraw[i].height) continue;
                [reduced release];
                // Half floats retain all 10-bit code levels and are filterable
                // on every supported Apple GPU; RG32Float filtering is optional.
                auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
                    width:target->videoSize.width() height:planesToDraw[i].height mipmapped:NO];
                descriptor.storageMode = MTLStorageModePrivate;
                descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                reduced = [m_MetalLayer.device newTextureWithDescriptor:descriptor];
                if (!reduced) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Metal video reduction allocation failed");
                    discardNextDrawable();
                    return;
                }
            }
        }
        auto commandBuffer = [m_CommandQueue commandBuffer];
        for (auto& target : m_Targets) {
            auto targetPlanes = planesToDraw;
            if (target->visible && target->reduceHorizontal) {
                for (size_t i = 0; i < planes; ++i) {
                    auto reduced = target->reducedPlanes[i];
                    auto encoder = [commandBuffer computeCommandEncoder];
                    [encoder setComputePipelineState:m_HorizontalReductionPipeline];
                    [encoder setTexture:planesToDraw[i] atIndex:0];
                    [encoder setTexture:reduced atIndex:1];
                    [encoder setBytes:&target->horizontalCrop length:sizeof(target->horizontalCrop) atIndex:0];
                    const NSUInteger width = m_HorizontalReductionPipeline.threadExecutionWidth;
                    [encoder dispatchThreads:MTLSizeMake(reduced.width, reduced.height, 1)
                        threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
                    [encoder endEncoding];
                    targetPlanes[i] = reduced;
                }
            }
            auto renderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
            renderPassDescriptor.colorAttachments[0].texture = target->drawable.texture;
            renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
            renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
            auto renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
            if (target->visible) {
                [renderEncoder setRenderPipelineState:m_VideoPipelineState];
                for (size_t i = 0; i < planes; ++i)
                    [renderEncoder setFragmentTexture:targetPlanes[i] atIndex:i];
                [renderEncoder setFragmentBuffer:m_CscParamsBuffer offset:0 atIndex:0];
                [renderEncoder setVertexBuffer:target->vertices offset:0 atIndex:0];
                [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }

            // The toolbar and statistics belong to the primary output, matching
            // their input hit-testing and the Linux multi-output renderers.
            const int drawableWidth = target->drawableSize.width();
            const int drawableHeight = target->drawableSize.height();
            if (target->output.primary) {
                // Now draw any overlays that are enabled
                for (int i = 0; i < Overlay::OverlayMax; i++) {
                    id<MTLTexture> overlayTexture = nullptr;

                    // Try to acquire a reference on the overlay texture
                    SDL_LockSpinlock(&m_OverlayLock);
                    overlayTexture = [m_OverlayTextures[i] retain];
                    SDL_UnlockSpinlock(&m_OverlayLock);

                    if (overlayTexture) {
                        SDL_FRect renderRect = {};
                        if (i == Overlay::OverlayStatusUpdate) {
                            // Bottom Left
                            renderRect.x = 0;
                            renderRect.y = 0;
                        }
                        else if (i == Overlay::OverlayDebug) {
                            // Top left
                            renderRect.x = 0;
                            renderRect.y = drawableHeight - overlayTexture.height;
                        }
                        else if (i == Overlay::OverlayToolbar) {
                            const float position = Session::get()->getOverlayManager().getOverlayHorizontalPosition(Overlay::OverlayToolbar);
                            renderRect.x = std::max(0, drawableWidth - (int)overlayTexture.width) * position;
                            renderRect.y = drawableHeight - overlayTexture.height;
                        }

                        renderRect.w = overlayTexture.width;
                        renderRect.h = overlayTexture.height;

                        // Convert screen space to normalized device coordinates
                        StreamUtils::screenSpaceToNormalizedDeviceCoords(&renderRect, drawableWidth, drawableHeight);

                        Vertex verts[] =
                        {
                            { { renderRect.x, renderRect.y, 0.0f, 1.0f }, { 0.0f, 1.0f } },
                            { { renderRect.x, renderRect.y+renderRect.h, 0.0f, 1.0f }, { 0.0f, 0} },
                            { { renderRect.x+renderRect.w, renderRect.y, 0.0f, 1.0f }, { 1.0f, 1.0f} },
                            { { renderRect.x+renderRect.w, renderRect.y+renderRect.h, 0.0f, 1.0f }, { 1.0f, 0} },
                        };

                        [renderEncoder setRenderPipelineState:m_OverlayPipelineState];
                        [renderEncoder setFragmentTexture:overlayTexture atIndex:0];
                        [renderEncoder setVertexBytes:verts length:sizeof(verts) atIndex:0];
                        [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:SDL_arraysize(verts)];

                        [overlayTexture release];
                    }
                }

            }
            [renderEncoder endEncoding];

            // Only one output paces a frame; the secondary output must not double
            // the in-flight count. A late callback cannot touch a deleted renderer.
            if (target->output.primary && target->layer.displaySyncEnabled) {
                auto pacer = m_Pacer;
                SDL_LockMutex(pacer->mutex);
                pacer->pending++;
                SDL_UnlockMutex(pacer->mutex);
                [target->drawable addPresentedHandler:^(id<MTLDrawable>) {
                    SDL_LockMutex(pacer->mutex);
                    pacer->pending--;
                    SDL_SignalCondition(pacer->condition);
                    SDL_UnlockMutex(pacer->mutex);
                }];
            }
            [commandBuffer presentDrawable:target->drawable];
        }
        [commandBuffer commit];
        // One submission, with frame textures retained until both passes finish.
        [commandBuffer waitUntilCompleted];
        if (commandBuffer.status == MTLCommandBufferStatusError) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Metal presentation command failed: %s",
                commandBuffer.error.localizedDescription.UTF8String);
            SDL_Event event = {};
            event.type = SDL_EVENT_RENDER_DEVICE_RESET;
            SDL_PushEvent(&event);
        }
        discardNextDrawable();
    }}

    id<MTLDevice> getMetalDevice() {
        if (qgetenv("VT_FORCE_METAL") == "0") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Avoiding Metal renderer due to VT_FORCE_METAL=0 override.");
            return nullptr;
        }

        NSArray<id<MTLDevice>> *devices = [MTLCopyAllDevices() autorelease];
        if (devices.count == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "No Metal device found!");
            return nullptr;
        }

        for (id<MTLDevice> device in devices) {
            if (device.isLowPower || device.hasUnifiedMemory) {
                return device;
            }
        }

        if (!m_HwAccel) {
            // Metal software decoding is always available
            return [MTLCreateSystemDefaultDevice() autorelease];
        }
        else if (qgetenv("VT_FORCE_METAL") == "1") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Using Metal renderer due to VT_FORCE_METAL=1 override.");
            return [MTLCreateSystemDefaultDevice() autorelease];
        }
        else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Avoiding Metal renderer due to use of dGPU/eGPU. Use VT_FORCE_METAL=1 to override.");
        }

        return nullptr;
    }

    virtual bool initialize(PDECODER_PARAMETERS params) override
    { @autoreleasepool {
        int err;
        if (!m_Pacer->mutex || !m_Pacer->condition) return false;

        id<MTLDevice> device = getMetalDevice();
        if (!device) {
            return false;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Selected Metal device: %s",
                    device.name.UTF8String);

        if (m_HwAccel && !checkDecoderCapabilities(device, params)) {
            return false;
        }

        err = av_hwdevice_ctx_create(&m_HwContext,
                                     AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                     nullptr,
                                     nullptr,
                                     0);
        if (err < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "av_hwdevice_ctx_create() failed for VT decoder: %d",
                        err);
            return false;
        }

        QVector<PlankPresentationOutput> outputs;
        if (params->presentationLayout && params->presentationLayout->isMultiOutput()) {
            m_PresentationCanvas = params->presentationLayout->canvasSize;
            outputs = params->presentationLayout->outputs;
            if (!m_PresentationCanvas.isValid() || outputs.size() != 2) return false;
        } else {
            int width = 0, height = 0;
            if (!SDL_GetWindowSizeInPixels(params->window, &width, &height)) return false;
            m_PresentationCanvas = QSize(width, height);
            outputs.append({params->window, QRect(QPoint(0, 0), m_PresentationCanvas), true});
        }
        for (const auto& output : outputs) {
            auto target = std::make_unique<MetalPresentationTarget>();
            target->output = output;
            target->view = SDL_Metal_CreateView(output.window);
            if (!target->view) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to create Metal view for presentation output: %s", SDL_GetError());
                return false;
            }
            target->layer = (CAMetalLayer*)SDL_Metal_GetLayer(target->view);
            if (!target->layer) return false;
            target->layer.device = device;
            target->layer.wantsExtendedDynamicRangeContent = !!(params->videoFormat & VIDEO_FORMAT_MASK_10BIT);
            target->layer.maximumDrawableCount = 3;
            target->layer.displaySyncEnabled = params->enableVsync;
            if (output.primary) m_MetalLayer = target->layer;
            m_Targets.push_back(std::move(target));
        }
        if (!m_MetalLayer) return false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Metal presentation initialized: outputs=%zu canvas=%dx%d",
            m_Targets.size(), m_PresentationCanvas.width(), m_PresentationCanvas.height());

        // Create the Metal texture cache for our CVPixelBuffers
        CFStringRef keys[1] = { kCVMetalTextureUsage };
        NSUInteger values[1] = { MTLTextureUsageShaderRead };
        auto cacheAttributes = CFDictionaryCreate(kCFAllocatorDefault, (const void**)keys, (const void**)values, 1, nullptr, nullptr);
        err = CVMetalTextureCacheCreate(kCFAllocatorDefault, cacheAttributes, m_MetalLayer.device, nullptr, &m_TextureCache);
        CFRelease(cacheAttributes);

        if (err != kCVReturnSuccess) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "CVMetalTextureCacheCreate() failed: %d",
                         err);
            return false;
        }

        // Compile our shaders
        QString shaderSource = QString::fromUtf8(Path::readDataFile("vt_renderer.metal"));
        NSError* shaderError = nil;
        m_ShaderLibrary = [m_MetalLayer.device newLibraryWithSource:shaderSource.toNSString() options:nullptr error:&shaderError];
        if (!m_ShaderLibrary) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to compile shaders: %s", shaderError.localizedDescription.UTF8String);
            return false;
        }

        auto reductionFunction = [m_ShaderLibrary newFunctionWithName:@"cs_reduce_horizontal"];
        m_HorizontalReductionPipeline = [device newComputePipelineStateWithFunction:reductionFunction error:&shaderError];
        [reductionFunction release];
        if (!m_HorizontalReductionPipeline) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to compile video reduction: %s",
                shaderError.localizedDescription.UTF8String);
            return false;
        }

        // Create a command queue for submission
        m_CommandQueue = [m_MetalLayer.device newCommandQueue];
        return true;
    }}

    virtual void notifyOverlayUpdated(Overlay::OverlayType type) override
    { @autoreleasepool {
        SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
        bool overlayEnabled = Session::get()->getOverlayManager().isOverlayEnabled(type);
        updateOverlayTexture(type, newSurface, overlayEnabled);
    }}

    // Consumes newSurface. The UI thread prepares each replacement while the
    // render thread continues using the last complete texture.
    void updateOverlayTexture(Overlay::OverlayType type, SDL_Surface* newSurface,
                              bool overlayEnabled)
    { @autoreleasepool {
        if (newSurface == nullptr && overlayEnabled) {
            // The overlay is enabled and there is no new surface. Leave the old texture alone.
            return;
        }

        // Only an explicit hide may publish an empty texture slot.
        if (!overlayEnabled) {
            SDL_LockSpinlock(&m_OverlayLock);
            auto oldTexture = m_OverlayTextures[type];
            m_OverlayTextures[type] = nullptr;
            SDL_UnlockSpinlock(&m_OverlayLock);
            [oldTexture release];
            SDL_DestroySurface(newSurface);
            return;
        }

        // Create a texture to hold our pixel data
        SDL_assert(!SDL_MUSTLOCK(newSurface));
        SDL_assert(newSurface->format == SDL_PIXELFORMAT_ARGB8888);
        auto texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                          width:newSurface->w
                                                                         height:newSurface->h
                                                                      mipmapped:NO];
        texDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
        texDesc.storageMode = MTLStorageModeManaged;
        texDesc.usage = MTLTextureUsageShaderRead;
        auto newTexture = [m_MetalLayer.device newTextureWithDescriptor:texDesc];
        if (newTexture == nil) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to allocate Metal overlay texture; retaining the previous image");
            SDL_DestroySurface(newSurface);
            return;
        }

        // Load the pixel data into the new texture
        [newTexture replaceRegion:MTLRegionMake2D(0, 0, newSurface->w, newSurface->h)
                      mipmapLevel:0
                        withBytes:newSurface->pixels
                      bytesPerRow:newSurface->pitch];

        // The surface is no longer required
        SDL_DestroySurface(newSurface);
        newSurface = nullptr;

        SDL_LockSpinlock(&m_OverlayLock);
        auto oldTexture = m_OverlayTextures[type];
        m_OverlayTextures[type] = newTexture;
        SDL_UnlockSpinlock(&m_OverlayLock);
        // Readers retain under the same lock; submitted Metal commands retain
        // their resources. Never block the render thread during allocation/upload.
        [oldTexture release];
    }}

    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override
    {
        if (m_HwAccel) {
            context->hw_device_ctx = av_buffer_ref(m_HwContext);
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using Metal renderer with %s decoding",
                    m_HwAccel ? "hardware" : "software");

        return true;
    }

    virtual bool needsTestFrame() override
    {
        // We used to trust VT to tell us whether decode will work, but
        // there are cases where it can lie because the hardware technically
        // can decode the format but VT is unserviceable for some other reason.
        // Decoding the test frame will tell us for sure whether it will work.
        return true;
    }

    int getDecoderColorspace() override
    {
        return COLORSPACE_REC_709;
    }

    int getDecoderCapabilities() override
    {
        return CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
               CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
    }

    int getRendererAttributes() override
    {
        // Metal supports HDR output
        return RENDERER_ATTRIBUTE_HDR_SUPPORT;
    }

    bool isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) override
    {
        if (m_HwAccel) {
            return pixelFormat == AV_PIX_FMT_VIDEOTOOLBOX;
        }
        else {
            if (pixelFormat == AV_PIX_FMT_VIDEOTOOLBOX) {
                // VideoToolbox frames are always supported
                return true;
            }
            else {
                // Otherwise it's supported if we can map it
                const int expectedPixelDepth = (videoFormat & VIDEO_FORMAT_MASK_10BIT) ? 10 : 8;
                const int expectedLog2ChromaW = (videoFormat & VIDEO_FORMAT_MASK_YUV444) ? 0 : 1;
                const int expectedLog2ChromaH = (videoFormat & (VIDEO_FORMAT_MASK_YUV444 | VIDEO_FORMAT_MASK_YUV422)) ? 0 : 1;

                const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get(pixelFormat);
                if (!formatDesc) {
                    // This shouldn't be possible but handle it anyway
                    SDL_assert(formatDesc);
                    return false;
                }

                int planes = av_pix_fmt_count_planes(pixelFormat);
                return (planes == 2 || planes == 3) &&
                       formatDesc->comp[0].depth == expectedPixelDepth &&
                       formatDesc->log2_chroma_w == expectedLog2ChromaW &&
                       formatDesc->log2_chroma_h == expectedLog2ChromaH;
            }
        }
    }

    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override
    {
        auto unhandledStateFlags = info->stateChangeFlags;

        // We can always handle size changes
        unhandledStateFlags &= ~WINDOW_STATE_CHANGE_SIZE;

        // We can handle monitor changes
        unhandledStateFlags &= ~WINDOW_STATE_CHANGE_DISPLAY;

        // If nothing is left, we handled everything
        return unhandledStateFlags == 0;
    }

private:
    bool m_HwAccel;
    AVBufferRef* m_HwContext;
    CAMetalLayer* m_MetalLayer;
    CVMetalTextureCacheRef m_TextureCache;
    id<MTLBuffer> m_CscParamsBuffer;
    id<MTLTexture> m_OverlayTextures[Overlay::OverlayMax];
    SDL_SpinLock m_OverlayLock;
    id<MTLRenderPipelineState> m_VideoPipelineState;
    id<MTLComputePipelineState> m_HorizontalReductionPipeline;
    id<MTLRenderPipelineState> m_OverlayPipelineState;
    id<MTLLibrary> m_ShaderLibrary;
    id<MTLCommandQueue> m_CommandQueue;
    id<MTLTexture> m_SwMappingTextures[MAX_VIDEO_PLANES];
    int m_LastColorSpace;
    bool m_LastFullRange;
    QSize m_PresentationCanvas;
    std::vector<std::unique_ptr<MetalPresentationTarget>> m_Targets;
    std::shared_ptr<MetalPresentationPacer> m_Pacer;
};

IFFmpegRenderer* VTMetalRendererFactory::createRenderer(bool hwAccel) {
    return new VTMetalRenderer(hwAccel);
}
