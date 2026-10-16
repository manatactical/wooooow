// Not in the original binary. Ported from 15595 (Cataclysm): CGxFrameBuffer, plus the CGxDevice
// FrameBuffer* wrappers. 12340 reaches the same device state through GxRenderTargetSet and
// DeviceSetRenderTarget; these just name the whole attachment set so a pass can save and restore it.

#include <cstdint>
#include "Engine/Source/Gx/CGxFrameBuffer.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Storm/H/storm.h"

uint32_t CGxFrameBuffer::s_nextVersion;

CGxFrameBuffer::CGxFrameBuffer() {
    this->m_version = 0;

    for (uint32_t i = 0; i < GxBuffers_Last; i++) {
        this->m_targets[i].m_texture = NULL;
        this->m_targets[i].m_plane = 0;
    }

    this->m_count = 0;
    this->m_bound = 0;
}

int32_t CGxFrameBuffer::Attach(EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    this->m_targets[buffer].m_texture = texture;
    this->m_targets[buffer].m_plane = plane;
    int32_t count = buffer + 1;

    if (this->m_version) {
        this->m_version = 0;
        this->m_count = count;
        return count;
    }

    if (count < this->m_count) {
        count = this->m_count;
    }

    this->m_count = count;
    return count;
}

int32_t CGxFrameBuffer::Finalize() {
    uint32_t version = CGxFrameBuffer::s_nextVersion;
    this->m_version = version;
    CGxFrameBuffer::s_nextVersion = version + 1;
    return 1;
}

CGxFrameBuffer* CGxDevice::FrameBufferCreate() {
    return new (SMemAlloc(sizeof(CGxFrameBuffer), __FILE__, __LINE__, 0)) CGxFrameBuffer();
}

void CGxDevice::FrameBufferDelete(CGxFrameBuffer*& frameBuffer) {
    if (frameBuffer) {
        SMemFree(frameBuffer, "delete", -1, 0);
    }

    frameBuffer = NULL;
}

int32_t CGxDevice::FrameBufferAttach(CGxFrameBuffer* frameBuffer, EGxBuffer buffer, CGxTex* texture, uint32_t plane) {
    return frameBuffer->Attach(buffer, texture, plane);
}

int32_t CGxDevice::FrameBufferFinalize(CGxFrameBuffer* frameBuffer) {
    CGxTex* depth = frameBuffer->m_targets[GxBuffers_Depth].m_texture;
    CGxTex* color = frameBuffer->m_targets[GxBuffers_Color].m_texture;

    // 15595 rejects a target set whose depth is smaller than its colour; keep that test, but 12340 may
    // legitimately attach a colour target with no depth (the reflection passes only need colour).
    if (!color) {
        return frameBuffer->Finalize();
    }

    if (!depth || (!depth->m_width && !depth->m_height) || (depth->m_width >= color->m_width && depth->m_height >= color->m_height)) {
        return frameBuffer->Finalize();
    }

    return 0;
}

CGxFrameBuffer* CGxDevice::FrameBufferGet() {
    return this->m_frameBuffer;
}

void CGxDevice::FrameBufferSet(CGxFrameBuffer* frameBuffer) {
    CGxFrameBuffer* current = this->m_frameBuffer;

    if (current == frameBuffer && (!frameBuffer || this->m_frameBufferVersion == frameBuffer->m_version)) {
        return;
    }

    int32_t oldCount = current ? current->m_count : 0;
    int32_t count = frameBuffer ? frameBuffer->m_count : 0;
    int32_t i;

    for (i = 0; i < count; i++) {
        this->DeviceSetRenderTarget(static_cast<EGxBuffer>(i), frameBuffer->m_targets[i].m_texture, frameBuffer->m_targets[i].m_plane);
    }

    for (i = count; i < oldCount; i++) {
        this->DeviceSetRenderTarget(static_cast<EGxBuffer>(i), NULL, 0);
    }

    if (current) {
        current->m_bound = 0;
    }

    this->m_frameBuffer = frameBuffer;

    if (frameBuffer) {
        frameBuffer->m_bound = 1;
        this->m_frameBufferVersion = frameBuffer->m_version;
    } else {
        this->m_frameBufferVersion = 0;
    }
}

CGxFrameBuffer* CGxDevice::BackBuffer() {
    return this->m_backBuffer;
}

CGxTex* CGxDevice::BackBufferColorTex() {
    return this->m_backBufferColorTex;
}

CGxTex* CGxDevice::BackBufferDepthTex() {
    return this->m_backBufferDepthTex;
}