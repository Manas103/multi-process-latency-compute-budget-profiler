// Independent, from-scratch protobuf decoder (not linked to
// perfetto_writer.hpp's encoder) used as the reference oracle for what
// PerfettoWriter actually wrote: parses the top-level repeated
// TracePacket (field 1) structure and counts packets and their
// track_descriptor (field 60) / track_event (field 11) sub-fields,
// without trusting any of PerfettoWriter's own code.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "perfetto_writer.hpp"

static int g_checks = 0;
#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } \
} while (0)

namespace {

uint64_t read_varint(const std::string& s, size_t& pos) {
    uint64_t v = 0;
    int shift = 0;
    while (pos < s.size()) {
        uint8_t b = (uint8_t)s[pos++];
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return v;
}

struct Field {
    int number;
    int wiretype;
    uint64_t varint_value;
    std::string bytes_value;
};

std::vector<Field> parse_fields(const std::string& s) {
    std::vector<Field> out;
    size_t pos = 0;
    while (pos < s.size()) {
        uint64_t tag = read_varint(s, pos);
        Field f;
        f.number = (int)(tag >> 3);
        f.wiretype = (int)(tag & 7);
        if (f.wiretype == 0) {
            f.varint_value = read_varint(s, pos);
        } else if (f.wiretype == 2) {
            uint64_t len = read_varint(s, pos);
            f.bytes_value = s.substr(pos, len);
            pos += len;
        } else {
            break; // not used by this subset
        }
        out.push_back(f);
    }
    return out;
}

} // namespace

int main() {
    const char* path = "/tmp/test_perfetto_output.trace";
    {
        shmwprof::PerfettoWriter w(path);
        w.add_track(1000, "stage0");
        w.add_slice_begin(1000, 111, "tick0");
        w.add_slice_end(1000, 222);
    }

    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    CHECK(!content.empty());

    auto top = parse_fields(content);
    CHECK(top.size() == 3); // one TracePacket per add_track/add_slice_begin/add_slice_end call
    for (auto& pkt_field : top) CHECK(pkt_field.number == 1 && pkt_field.wiretype == 2);

    auto pkt0 = parse_fields(top[0].bytes_value);
    bool found_track_descriptor = false;
    for (auto& f : pkt0) if (f.number == 60) found_track_descriptor = true;
    CHECK(found_track_descriptor);

    auto td = [&]() -> std::string {
        for (auto& f : pkt0) if (f.number == 60) return f.bytes_value;
        return "";
    }();
    auto td_fields = parse_fields(td);
    bool uuid_ok = false, name_ok = false;
    for (auto& f : td_fields) {
        if (f.number == 1 && f.wiretype == 0 && f.varint_value == 1000) uuid_ok = true;
        if (f.number == 2 && f.wiretype == 2 && f.bytes_value == "stage0") name_ok = true;
    }
    CHECK(uuid_ok);
    CHECK(name_ok);

    auto pkt1 = parse_fields(top[1].bytes_value);
    bool has_track_event = false;
    int64_t ts = -1;
    for (auto& f : pkt1) {
        if (f.number == 8) ts = (int64_t)f.varint_value;
        if (f.number == 11) has_track_event = true;
    }
    CHECK(has_track_event);
    CHECK(ts == 111);

    std::printf("%d checks passed\n", g_checks);
    return 0;
}
