// Round-trip test for the PSD reader/writer (self-contained, deterministic).
//
// The contract under test is the one documented at the top of src/psd/psd.h
// and src/psd/psd_write.cpp:
//   * Document::layers is bottom-most first;
//   * Layer::rgba / Document::composite are STRAIGHT alpha (PSD files store
//     premultiplied pixels);
//   * names round-trip exactly as UTF-8 (Chinese + emoji included);
//   * a layer with blend_key == "group" is an OBS group placeholder; the file
//     stores it as a "<" / "</" section-divider pair and the reader rebuilds
//     the placeholder, so a re-write is byte-identical;
//   * the writer emits an RLE composite of the three colour planes built from
//     the top-level layers.
//
// Sections:
//   1. flat document: three layers with UTF-8 names, offsets (incl. negative),
//      opacity, visibility, blend key, all kinds of alpha -> write, read, compare;
//   2. composite: present, opaque, equal (RGB) to a reference flatten;
//   3. empty / fully transparent layers, memory round trip, rewrite stability;
//   4. group document: placeholders, names, content, composite skips groups,
//      rewrite stability;
//   5. document without layers, and the error paths.
#include "../src/psd/psd.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

// Deterministic RGBA pattern with every kind of alpha (0, 1, 127, 254, 255)
// and straight (non-premultiplied) colour, including colour inside fully
// transparent pixels (the one thing PSD cannot represent, checked separately).
static void fill(psd::Layer &L, int seed)
{
    L.rgba.assign(size_t(L.width()) * size_t(L.height()) * 4, 0);
    for (int y = 0; y < L.height(); ++y) {
        for (int x = 0; x < L.width(); ++x) {
            const size_t i = (size_t(y) * size_t(L.width()) + size_t(x)) * 4;
            L.rgba[i + 0] = uint8_t(x + seed * 41); // 0..63
            L.rgba[i + 1] = uint8_t(64 + y * 6 + seed * 7); // 64..123
            L.rgba[i + 2] = uint8_t(128 + ((x * y + seed * 13) & 0x3F)); // 128..191
            const int a = (x + y + seed) % 5;
            L.rgba[i + 3] = uint8_t(a == 0 ? 0 : (a == 1 ? 1 : (a == 2 ? 127 : (a == 3 ? 254 : 255))));
        }
    }
}

static psd::Layer make_layer(const std::string &name, int32_t l, int32_t t, int32_t w, int32_t h, int seed)
{
    psd::Layer L;
    L.name = name;
    L.left = l;
    L.top = t;
    L.right = l + w;
    L.bottom = t + h;
    fill(L, seed);
    return L;
}

static psd::Layer make_group(const std::string &name, bool visible)
{
    psd::Layer G;
    G.name = name;
    G.blend_key = "group";
    G.blend_mode = 'N';
    G.visible = visible;
    // Group placeholders carry no pixels; the reader returns opacity 100 for
    // them (Photoshop keeps the opacity on the "</" divider).
    G.opacity = 100.0f;
    return G;
}

static bool read_bytes(const char *path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path, "rb");
    if (f == nullptr)
        return false;
    out.clear();
    uint8_t buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// The documented integer rule (psd.h) applied independently of the codec.
static uint8_t premul8(int v, int a) { return uint8_t(a == 0 ? 0 : (v * a + 127) / 255); }

static uint8_t unpremul8(int p, int a)
{
    if (a == 0)
        return 0;
    const int v = (p * 255 + a / 2) / a;
    return uint8_t(v > 255 ? 255 : v);
}

// What the reader may return for a straight RGBA buffer after one round trip:
// straight -> premultiplied -> straight. Exact unless colour was stored in
// fully transparent pixels (a == 0), where the file can only keep zeros.
static std::vector<uint8_t> expected_roundtrip(const std::vector<uint8_t> &rgba)
{
    std::vector<uint8_t> out(rgba.size(), 0);
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const int a = rgba[i + 3];
        out[i + 0] = unpremul8(premul8(rgba[i + 0], a), a);
        out[i + 1] = unpremul8(premul8(rgba[i + 1], a), a);
        out[i + 2] = unpremul8(premul8(rgba[i + 2], a), a);
        out[i + 3] = uint8_t(a);
    }
    return out;
}

static void compare_layers(const std::vector<psd::Layer> &a, const std::vector<psd::Layer> &b, const char *what)
{
    CHECK(a.size() == b.size(), "%s: layer count %zu != %zu", what, a.size(), b.size());
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t k = 0; k < n; ++k) {
        const psd::Layer &x = a[k];
        const psd::Layer &y = b[k];
        if (x.blend_key == "group") {
            // Groups are structural: name, visibility and (for non-empty groups)
            // the union bounds of their contents are carried on the divider pair.
            CHECK(x.name == y.name, "%s: [%zu] group name differs", what, k);
            CHECK(y.blend_key == "group" && y.rgba.empty(), "%s: [%zu] group placeholder carries pixels", what, k);
            const bool x_empty_bounds = x.left == 0 && x.top == 0 && x.right == 0 && x.bottom == 0;
            if (!x_empty_bounds)
                CHECK(x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom,
                      "%s: [%zu] group bounds differ", what, k);
            continue;
        }
        CHECK(x.name == y.name, "%s: [%zu] name '%s' != '%s'", what, k, x.name.c_str(), y.name.c_str());
        CHECK(x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom,
              "%s: [%zu] bounds (%d,%d,%d,%d) != (%d,%d,%d,%d)", what, k, x.left, x.top, x.right, x.bottom, y.left,
              y.top, y.right, y.bottom);
        CHECK(x.opacity == y.opacity, "%s: [%zu] opacity %f != %f", what, k, double(x.opacity), double(y.opacity));
        CHECK(x.visible == y.visible, "%s: [%zu] visibility differs", what, k);
        CHECK(x.clipped == y.clipped, "%s: [%zu] clipped differs", what, k);
        CHECK(x.blend_key == y.blend_key, "%s: [%zu] blend key '%s' != '%s'", what, k, x.blend_key.c_str(),
              y.blend_key.c_str());
        CHECK(x.blend_mode == y.blend_mode, "%s: [%zu] blend mode differs", what, k);
        if (x.blend_key == "group") {
            CHECK(y.empty() && y.rgba.empty(), "%s: [%zu] group placeholder carries pixels", what, k);
            continue;
        }
        const std::vector<uint8_t> want = expected_roundtrip(x.rgba);
        CHECK(want.size() == y.rgba.size(), "%s: [%zu] pixel buffer size %zu != %zu", what, k, want.size(),
              y.rgba.size());
        size_t diff = 0;
        const size_t m = want.size() < y.rgba.size() ? want.size() : y.rgba.size();
        for (size_t i = 0; i < m; ++i)
            diff += (want[i] != y.rgba[i]);
        CHECK(diff == 0, "%s: [%zu] %zu of %zu pixel bytes differ", what, k, diff, want.size());
    }
}

// Independent reference flatten of the top-level (non-group) visible layers,
// straight alpha, source-over, colour rounded to nearest.
static std::vector<uint8_t> reference_flatten(const psd::Document &doc)
{
    std::vector<uint8_t> out(size_t(doc.width) * size_t(doc.height) * 4, 0);
    for (const psd::Layer &L : doc.layers) {
        if (L.blend_key == "group")
            continue; // groups are structure only, their content is included
        if (!L.visible || L.empty() || !(L.opacity > 0.0f))
            continue;
        const double op = double(L.opacity) / 100.0;
        for (int32_t y = 0; y < L.height(); ++y) {
            const int32_t dy = L.top + y;
            if (dy < 0 || dy >= doc.height)
                continue;
            for (int32_t x = 0; x < L.width(); ++x) {
                const int32_t dx = L.left + x;
                if (dx < 0 || dx >= doc.width)
                    continue;
                const uint8_t *s = &L.rgba[(size_t(y) * size_t(L.width()) + size_t(x)) * 4];
                const double sa = double(s[3]) / 255.0 * op;
                uint8_t *d = &out[(size_t(dy) * size_t(doc.width) + size_t(dx)) * 4];
                const double da = double(d[3]) / 255.0;
                const double oa = sa + da * (1.0 - sa);
                if (oa <= 0.0)
                    continue;
                for (int c = 0; c < 3; ++c) {
                    // Opaque output: weighted sum with the same exact integer
                    // arithmetic as the writer (see psd_write.cpp flatten).
                    const double a = double(s[3]);
                    const double S = std::floor((double(s[c]) * a * 2.0 + 255.0) / 510.0 + 0.5);
                    const double num = S * 255.0 + double(d[c]) * (255.0 - a);
                    const int r = int(std::floor(num / 255.0 + 0.5));
                    d[c] = uint8_t(r > 255 ? 255 : r);
                }
                d[3] = uint8_t(int(oa * 255.0 + 0.5));
            }
        }
    }
    return out;
}

// Compares the composite against the reference on RGB (the file has no alpha
// plane, so the composite is opaque) with a +-1 rounding tolerance.
static void compare_composite_rgb(const psd::Document &in, const std::vector<uint8_t> &ref, const char *what)
{
    CHECK(in.has_composite(), "%s: composite missing (%dx%d, %zu bytes)", what, in.width, in.height,
          in.composite.size());
    if (!in.has_composite())
        return;
    size_t bad = 0, maxd = 0;
    for (size_t i = 0; i + 3 < in.composite.size() && i + 3 < ref.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const int a = int(in.composite[i + size_t(c)]);
            const int b = int(ref[i + size_t(c)]);
            const int d = a > b ? a - b : b - a;
            if (d > 0) {
                ++bad;
                if (size_t(d) > maxd)
                    maxd = size_t(d);
            }
        }
    }
    // The writer composites in exact integer arithmetic, the reference in
    // double precision; one step of rounding up to 3 grey levels is allowed so
    // the check stays an independent cross-check instead of a mirror.
    CHECK(maxd <= 3, "%s: composite differs from the reference flatten by up to %zu", what, maxd);
    for (size_t i = 3; i < in.composite.size(); i += 4)
        if (in.composite[i] != 255) {
            CHECK(false, "%s: composite alpha byte at %zu is not 255", what, i);
            break;
        }
}

static void rewrite_identical(const char *path1, const char *path2, const char *what)
{
    std::vector<uint8_t> a, b;
    const bool ok1 = read_bytes(path1, a);
    const bool ok2 = read_bytes(path2, b);
    CHECK(ok1 && ok2, "%s: cannot read back %s/%s", what, path1, path2);
    const bool same = ok1 && ok2 && a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
    CHECK(same, "%s: rewrite is not byte-identical (%zu vs %zu bytes)", what, a.size(), b.size());
}

int main()
{
    std::string err;

    // ---------------- 1. flat document -------------------------------------
    psd::Document doc;
    doc.width = 64;
    doc.height = 48;
    {
        psd::Layer bg = make_layer("Background", 0, 0, 64, 48, 0);
        bg.opacity = 100.0f;
        doc.layers.push_back(bg);

        psd::Layer mid = make_layer("Middle \xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x90\xB3", 2, 3, 40, 30, 1);
        mid.opacity = 80.0f;
        doc.layers.push_back(mid);

        // Negative offset: sticks out of the document on the left and top.
        psd::Layer top = make_layer("Top", -5, -4, 20, 20, 2);
        top.opacity = 55.0f;
        top.clipped = true;
        top.blend_mode = 'M';
        top.blend_key = "mult";
        doc.layers.push_back(top);
    }
    CHECK(doc.layers.size() == 3, "setup");
    if (!psd::write_file("/tmp/psd_rt.psd", doc, err)) {
        printf("WRITE FAILED: %s\n", err.c_str());
        return 1;
    }
    printf("wrote /tmp/psd_rt.psd\n");

    psd::Document in;
    if (!psd::read_file("/tmp/psd_rt.psd", in, err)) {
        printf("READ FAILED: %s\n", err.c_str());
        return 1;
    }
    printf("read back: %dx%d layers=%zu composite=%zu\n", in.width, in.height, in.layers.size(), in.composite.size());
    CHECK(in.width == doc.width && in.height == doc.height, "document size");
    compare_layers(doc.layers, in.layers, "flat");
    compare_composite_rgb(in, reference_flatten(doc), "flat");

    // write_memory() must produce exactly what write_file() wrote.
    {
        std::vector<uint8_t> mem;
        std::string e2;
        if (!psd::write_memory(mem, doc, e2)) {
            printf("WRITE_MEMORY FAILED: %s\n", e2.c_str());
            return 1;
        }
        psd::Document back;
        if (!psd::read_memory(mem.data(), mem.size(), back, e2)) {
            printf("READ_MEMORY FAILED: %s\n", e2.c_str());
            return 1;
        }
        std::vector<uint8_t> file;
        CHECK(read_bytes("/tmp/psd_rt.psd", file) && file.size() == mem.size() &&
                  std::memcmp(file.data(), mem.data(), file.size()) == 0,
              "write_memory differs from write_file");
        compare_layers(doc.layers, back.layers, "memory");
        compare_composite_rgb(back, reference_flatten(doc), "memory");
    }

    // Re-writing the read-back document must be byte-identical (the layer and
    // group representation is preserved exactly).
    {
        std::string e2;
        if (!psd::write_file("/tmp/psd_rt2.psd", in, e2)) {
            printf("REWRITE FAILED: %s\n", e2.c_str());
            return 1;
        }
        rewrite_identical("/tmp/psd_rt.psd", "/tmp/psd_rt2.psd", "flat");
    }

    // ---------------- 2. empty & fully transparent layers ------------------
    {
        psd::Document d;
        d.width = 8;
        d.height = 6;
        psd::Layer e;
        e.name = "Empty"; // no bounds, no pixels
        d.layers.push_back(e);
        psd::Layer t = make_layer("Clear", 1, 1, 4, 4, 3);
        for (size_t i = 0; i < t.rgba.size(); i += 4)
            t.rgba[i + 3] = 0; // fully transparent
        d.layers.push_back(t);
        std::string e2;
        if (!psd::write_file("/tmp/psd_empty.psd", d, e2)) {
            printf("EMPTY WRITE FAILED: %s\n", e2.c_str());
            return 1;
        }
        psd::Document r;
        if (!psd::read_file("/tmp/psd_empty.psd", r, e2)) {
            printf("EMPTY READ FAILED: %s\n", e2.c_str());
            return 1;
        }
        compare_layers(d.layers, r.layers, "empty");
        compare_composite_rgb(r, reference_flatten(d), "empty");
        if (!psd::write_file("/tmp/psd_empty2.psd", r, e2)) {
            printf("EMPTY REWRITE FAILED: %s\n", e2.c_str());
            return 1;
        }
        rewrite_identical("/tmp/psd_empty.psd", "/tmp/psd_empty2.psd", "empty");
    }

    // ---------------- 3. group document ------------------------------------
    psd::Document gdoc;
    gdoc.width = 40;
    gdoc.height = 30;
    gdoc.layers.push_back(make_layer("Below", 0, 0, 40, 30, 7));
    gdoc.layers.push_back(make_group("Scene \xE7\xBB\x84 \xF0\x9F\x8E\xAC", true));
    gdoc.layers.push_back(make_layer("Inner A", 1, 2, 20, 15, 8));
    gdoc.layers.push_back(make_layer("Inner B", 10, 8, 25, 20, 9));
    gdoc.layers.back().visible = false; // hidden inside a group
    gdoc.layers.push_back(make_layer("Above", 5, 5, 10, 10, 10));
    gdoc.layers.push_back(make_group("Empty \xE7\xBB\x84", false)); // empty group
    gdoc.layers.push_back(make_layer("Top", 0, 0, 40, 30, 11));

    CHECK(gdoc.layers.size() == 7, "group setup");
    if (!psd::write_file("/tmp/psd_group.psd", gdoc, err)) {
        printf("GROUP WRITE FAILED: %s\n", err.c_str());
        return 1;
    }
    psd::Document gin;
    if (!psd::read_file("/tmp/psd_group.psd", gin, err)) {
        printf("GROUP READ FAILED: %s\n", err.c_str());
        return 1;
    }
    printf("group read back: layers=%zu\n", gin.layers.size());
    CHECK(gin.layers.size() == gdoc.layers.size(), "group: %zu != %zu layers", gin.layers.size(),
          gdoc.layers.size());
    compare_layers(gdoc.layers, gin.layers, "group");
    for (const psd::Layer &L : gin.layers)
        if (L.name == "Inner A" || L.name == "Inner B")
            CHECK(!L.group_name.empty(), "group: '%s' has no group annotation", L.name.c_str());
    // The composite is built from the top-level layers only.
    compare_composite_rgb(gin, reference_flatten(gdoc), "group");
    {
        std::string e2;
        if (!psd::write_file("/tmp/psd_group2.psd", gin, e2)) {
            printf("GROUP REWRITE FAILED: %s\n", e2.c_str());
            return 1;
        }
        rewrite_identical("/tmp/psd_group.psd", "/tmp/psd_group2.psd", "group");
    }

    // ---------------- 4. document without layers ---------------------------
    {
        psd::Document d;
        d.width = 5;
        d.height = 4;
        std::string e2;
        if (!psd::write_file("/tmp/psd_nolayers.psd", d, e2)) {
            printf("NOLAYER WRITE FAILED: %s\n", e2.c_str());
            return 1;
        }
        psd::Document r;
        if (!psd::read_file("/tmp/psd_nolayers.psd", r, e2)) {
            printf("NOLAYER READ FAILED: %s\n", e2.c_str());
            return 1;
        }
        CHECK(r.layers.empty(), "no-layer document: %zu layers", r.layers.size());
        CHECK(r.has_composite(), "no-layer document: composite missing");
    }

    // ---------------- 5. error paths ---------------------------------------
    {
        psd::Document bad;
        bad.width = 0;
        bad.height = 10;
        std::string e2;
        CHECK(!psd::write_file("/tmp/psd_bad.psd", bad, e2), "zero-width document accepted");
        bad.width = 4;
        bad.height = 4;
        psd::Layer L;
        L.left = 0;
        L.top = 0;
        L.right = 4;
        L.bottom = 4; // claims pixels ...
        bad.layers.push_back(L); // ... but carries none
        std::remove("/tmp/psd_bad.psd");
        CHECK(!psd::write_file("/tmp/psd_bad.psd", bad, e2), "layer without pixels accepted");
        FILE *f = std::fopen("/tmp/psd_bad.psd", "rb");
        CHECK(f == nullptr, "failed write created a file");
        if (f != nullptr)
            std::fclose(f);
        psd::Document r;
        CHECK(!psd::read_file("/tmp/psd_does_not_exist.psd", r, e2), "missing file accepted");
        const uint8_t junk[4] = {'N', 'O', 'P', 'E'};
        CHECK(!psd::read_memory(junk, sizeof junk, r, e2), "junk buffer accepted");
        CHECK(!psd::read_memory(nullptr, 0, r, e2), "empty buffer accepted");
    }

    if (g_failures != 0) {
        printf("%d OF %d CHECK(S) FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("ROUND TRIP OK (%d checks)\n", g_checks);
    return 0;
}
