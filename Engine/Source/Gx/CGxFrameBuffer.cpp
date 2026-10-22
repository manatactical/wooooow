#include "Engine/Source/Gx/CGxFrameBuffer.h"

// STATIC: 0x00D09498/0x140BBB2F8
uint32_t CGxFrameBuffer::s_nextVersion;

// OFFSET: 0x00856350/0x140587000
CGxFrameBuffer::CGxFrameBuffer() {
    this->m_version = 0;
    this->m_targets[0].m_texture = NULL;
    this->m_targets[0].m_plane = 0;
    this->m_targets[1].m_texture = NULL;
    this->m_targets[1].m_plane = 0;
    this->m_targets[2].m_texture = NULL;
    this->m_targets[2].m_plane = 0;
    this->m_targets[3].m_texture = NULL;
    this->m_targets[3].m_plane = 0;
    this->m_targets[4].m_texture = NULL;
    this->m_targets[4].m_plane = 0;
    this->m_count = 0;
    this->m_bound = 0;
}

// OFFSET: 0x00856380
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

// OFFSET: 0x008563C0/0x140587080
int32_t CGxFrameBuffer::Finalize() {
    uint32_t version = CGxFrameBuffer::s_nextVersion;
    this->m_version = version;
    CGxFrameBuffer::s_nextVersion = version + 1;
    return 1;
}
