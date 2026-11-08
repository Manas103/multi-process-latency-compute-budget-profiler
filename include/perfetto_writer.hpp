#pragma once
// Minimal, hand-rolled protobuf writer for the small subset of Perfetto's
// trace.proto this profiler needs: a TrackDescriptor per process/stage and
// TYPE_SLICE_BEGIN/TYPE_SLICE_END TrackEvent packets. Field numbers below
// (TracePacket.timestamp=8, .trusted_packet_sequence_id=10,
// .track_event=11, .track_descriptor=60; TrackEvent.type=9,
// .track_uuid=11, .name=23; TrackDescriptor.uuid=1, .name=2;
// TrackEvent.TYPE_SLICE_BEGIN=1, TYPE_SLICE_END=2) are the real field
// numbers from Perfetto's own protos, not invented ones, so the output
// file is byte-for-byte a valid (if partial) Perfetto trace, openable by
// any real Perfetto trace_processor; tools/decode_trace.py is this
// project's own from-scratch decoder, used as the independent check that
// what was written parses back to what was meant (see README
// "Validation").
#include <cstdint>
#include <fstream>
#include <string>

namespace shmwprof {

inline void put_varint(std::string& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}
inline void put_tag(std::string& out, int field, int wiretype) { put_varint(out, (uint64_t)((field << 3) | wiretype)); }
inline void put_varint_field(std::string& out, int field, uint64_t v) {
    put_tag(out, field, 0);
    put_varint(out, v);
}
inline void put_submsg_field(std::string& out, int field, const std::string& sub) {
    put_tag(out, field, 2);
    put_varint(out, sub.size());
    out += sub;
}

class PerfettoWriter {
public:
    explicit PerfettoWriter(const std::string& path) : out_(path, std::ios::binary) {}

    void add_track(uint64_t uuid, const std::string& name) {
        std::string td;
        put_varint_field(td, 1, uuid);
        put_tag(td, 2, 2);
        put_varint(td, name.size());
        td += name;
        write_packet(0, [&](std::string& pkt) { put_submsg_field(pkt, 60, td); });
    }

    void add_slice_begin(uint64_t track_uuid, uint64_t ts_ns, const std::string& name) {
        add_event(track_uuid, ts_ns, /*TYPE_SLICE_BEGIN=*/1, name);
    }
    void add_slice_end(uint64_t track_uuid, uint64_t ts_ns) {
        add_event(track_uuid, ts_ns, /*TYPE_SLICE_END=*/2, "");
    }

private:
    void add_event(uint64_t track_uuid, uint64_t ts_ns, int type, const std::string& name) {
        std::string te;
        put_varint_field(te, 9, (uint64_t)type);
        put_varint_field(te, 11, track_uuid);
        if (!name.empty()) {
            put_tag(te, 23, 2);
            put_varint(te, name.size());
            te += name;
        }
        write_packet(ts_ns, [&](std::string& pkt) { put_submsg_field(pkt, 11, te); });
    }

    template <typename F>
    void write_packet(uint64_t ts_ns, F add_data_field) {
        std::string pkt;
        put_varint_field(pkt, 8, ts_ns);
        put_varint_field(pkt, 10, 1); // trusted_packet_sequence_id, fixed: one writer, one sequence
        add_data_field(pkt);
        std::string packet_field;
        put_submsg_field(packet_field, 1, pkt);
        out_.write(packet_field.data(), (std::streamsize)packet_field.size());
    }

    std::ofstream out_;
};

} // namespace shmwprof
