#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>
#include <opus/opus.h>

// Lightweight streaming WebM (EBML SimpleBlock) + Ogg Opus demuxer & decoder
// Designed specifically for YouTube Music audio streams (itag=251/250/249, 48kHz stereo Opus)
// with tiny RAM footprint (< 128 KB) for Nintendo Switch sysmodules.
class WebmOpusStreamDecoder {
public:
    WebmOpusStreamDecoder() : m_decoder(nullptr), m_initialized(false) {}

    ~WebmOpusStreamDecoder() {
        reset();
    }

    bool init() {
        reset();
        int err = OPUS_OK;
        // Switch audren operates natively at 48,000 Hz stereo (2 channels)
        m_decoder = opus_decoder_create(48000, 2, &err);
        if (err != OPUS_OK || !m_decoder) {
            m_decoder = nullptr;
            return false;
        }
        m_initialized = true;
        return true;
    }

    void reset() {
        if (m_decoder) {
            opus_decoder_destroy(m_decoder);
            m_decoder = nullptr;
        }
        m_buffer.clear();
        m_initialized = false;
    }

    // Feed raw network bytes from curl and receive decoded 48kHz 16-bit stereo PCM samples
    void feed(const uint8_t* data, size_t len, std::vector<int16_t>& pcm_out) {
        if (!m_initialized && !init()) return;
        if (data && len > 0) {
            m_buffer.insert(m_buffer.end(), data, data + len);
        }
        processBuffer(pcm_out);
    }

private:
    OpusDecoder* m_decoder;
    bool m_initialized;
    std::vector<uint8_t> m_buffer;

    // Read an EBML Variable Size Integer (VINT)
    // If keep_marker is true (used for Element IDs), keeps the leading VINT marker bit.
    static bool readVint(const uint8_t* buf, size_t avail, uint64_t& val, size_t& bytes_read, bool keep_marker) {
        if (avail == 0) return false;
        uint8_t first = buf[0];
        if (first == 0) return false;

        size_t len = 1;
        uint8_t mask = 0x80;
        while (len <= 8 && !(first & mask)) {
            mask >>= 1;
            len++;
        }
        if (len > 8 || avail < len) return false;

        val = keep_marker ? first : (first & (mask - 1));
        for (size_t i = 1; i < len; i++) {
            val = (val << 8) | buf[i];
        }
        bytes_read = len;
        return true;
    }

    static bool isMasterElement(uint64_t id) {
        switch (id) {
            case 0x18538067: // Segment
            case 0x1F43B675: // Cluster
            case 0xA0:       // BlockGroup
                return true;
            default:
                return false;
        }
    }

    void decodeOpusFrame(const uint8_t* pkt, int pkt_len, std::vector<int16_t>& pcm_out) {
        if (!m_decoder || pkt_len <= 0 || pkt_len > 8192) return;
        int16_t frame_pcm[5760 * 2]; // Max Opus frame size (120ms @ 48kHz stereo)
        int samples = opus_decode(m_decoder, pkt, pkt_len, frame_pcm, 5760, 0);
        if (samples > 0) {
            pcm_out.insert(pcm_out.end(), frame_pcm, frame_pcm + (samples * 2));
        }
    }

    void parseSimpleBlock(const uint8_t* payload, size_t size, std::vector<int16_t>& pcm_out) {
        if (size < 4) return;
        uint64_t track_num = 0;
        size_t track_bytes = 0;
        if (!readVint(payload, size, track_num, track_bytes, false)) return;
        if (track_bytes + 3 > size) return;

        // Skip 2-byte timecode + 1-byte flags
        uint8_t flags = payload[track_bytes + 2];
        uint8_t lacing = (flags >> 1) & 0x03;
        const uint8_t* frame_data = payload + track_bytes + 3;
        size_t frame_avail = size - (track_bytes + 3);

        if (lacing == 0) {
            // Single Opus packet in SimpleBlock (standard for YouTube itag=251)
            decodeOpusFrame(frame_data, (int)frame_avail, pcm_out);
        } else if (lacing == 2) {
            // Fixed-size lacing
            if (frame_avail < 1) return;
            uint8_t num_frames = frame_data[0] + 1;
            frame_data++;
            frame_avail--;
            if (num_frames > 0 && frame_avail % num_frames == 0) {
                size_t fsize = frame_avail / num_frames;
                for (uint8_t i = 0; i < num_frames; i++) {
                    decodeOpusFrame(frame_data + i * fsize, (int)fsize, pcm_out);
                }
            }
        } else if (lacing == 1) {
            // Xiph lacing
            if (frame_avail < 1) return;
            uint8_t num_frames = frame_data[0] + 1;
            size_t pos = 1;
            std::vector<size_t> sizes;
            size_t total_laced = 0;
            for (uint8_t i = 0; i + 1 < num_frames && pos < frame_avail; i++) {
                size_t s = 0;
                while (pos < frame_avail) {
                    uint8_t b = frame_data[pos++];
                    s += b;
                    if (b < 255) break;
                }
                sizes.push_back(s);
                total_laced += s;
            }
            if (pos + total_laced <= frame_avail) {
                sizes.push_back(frame_avail - pos - total_laced);
                for (size_t s : sizes) {
                    decodeOpusFrame(frame_data + pos, (int)s, pcm_out);
                    pos += s;
                }
            }
        }
    }

    void processBuffer(std::vector<int16_t>& pcm_out) {
        size_t offset = 0;
        const size_t total = m_buffer.size();

        while (offset + 4 <= total) {
            const uint8_t* ptr = m_buffer.data() + offset;
            size_t avail = total - offset;

            uint64_t elem_id = 0;
            size_t id_len = 0;
            if (!readVint(ptr, avail, elem_id, id_len, true)) {
                if (avail < 8) break; // Wait for more network bytes
                offset++;
                continue;
            }

            if (avail < id_len + 1) break; // Need more bytes for size VINT
            uint64_t elem_size = 0;
            size_t size_len = 0;
            if (!readVint(ptr + id_len, avail - id_len, elem_size, size_len, false)) {
                if (avail < id_len + 8) break;
                offset++;
                continue;
            }

            size_t header_len = id_len + size_len;

            // Master elements (Segment, Cluster, BlockGroup) have unknown/huge sizes; step into their children
            if (isMasterElement(elem_id)) {
                offset += header_len;
                continue;
            }

            // Sanity check against corrupted element size (> 1 MB in an audio stream)
            if (elem_size > 1024 * 1024) {
                offset++;
                continue;
            }

            // Wait until the full element payload arrives from the network
            if (avail < header_len + elem_size) {
                break;
            }

            if (elem_id == 0xA3 || elem_id == 0xA1) { // SimpleBlock or Block
                parseSimpleBlock(ptr + header_len, (size_t)elem_size, pcm_out);
            }

            offset += header_len + (size_t)elem_size;
        }

        if (offset > 0) {
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + offset);
        }
    }
};
