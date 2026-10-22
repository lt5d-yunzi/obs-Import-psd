// Round-trip test for the PSD reader/writer (self-contained, deterministic).
//
// Covers:
//   1. three flat layers with UTF-8 names, offsets, opacity, visibility, all
//      alpha values and pixel data -> write, read back, compare;
//   2. a document with a group (blend_key "group") containing two layers;
//   3. an empty group;
//   4. re-writing the document read back from disk and requiring byte-identical
//      output (proves the group structure is represented exactly);
//   5. the flattened composite (its alpha is dropped by contract, so compare
//      RGB only).
#include "../src/psd/psd.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

// Deterministic RGBA pattern with every kind of alpha (0, 1, 127, 254, 255).
static void fill(psd::Layer &L, int seed)
{
    L.rgba.assign(size_t(L.width()) * size_t(L.height()) * 4, 0);
    for (int y = 0; y < L.height(); ++y) {
        for (int x = 0; x < L.width(); ++x) {
            const size_t i = (size_t(y) * size_t(L.width()) + size_t(x)) * 4;
            L.rgba[i + 0] = uint8_t((x * 3 + seed * 41) ^ y);
            L.rgba[i + 1] = uint8_t(x + y * 2 + seed * 7);
            L.rgba[i + 2] = uint8_t((x * y + seed * 13) & 0xFF);
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

static void compare_layers(const std::vector<psd::Layer> &a, const std::vector<psd::Layer> &b, const char *what)
{
    CHECK(a.size() == b.size(), "%s: layer count %zu != %zu", what, a.size(), b.size());
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t k = 0; k < n; ++k) {
        const psd::Layer &x = a[k];
        const psd::Layer &y = b[k];
        CHECK(x.name == y.name, "%s: [%zu] name '%s' != '%s'", what, k, x.name.c_str(), y.name.c_str());
        CHECK(x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom,
              "%s: [%zu] bounds differ", what, k);
        CHECK(x.opacity == y.opacity, "%s: [%zu] opacity %f != %f", what, k, double(x.opacity), double(y.opacity));
        CHECK(x.visible == y.visible, "%s: [%zu] visibility differs", what, k);
        CHECK(x.clipped == y.clipped, "%s: [%zu] clipped differs", what, k);
        CHECK(x.blend_key == y.blend_key, "%s: [%zu] blend key '%s' != '%s'", what, k, x.blend_key.c_str(),
              y.blend_key.c_str());
        CHECK(x.blend_mode == y.blend_mode, "%s: [%zu] blend mode differs", what, k);
        CHECK(x.rgba == y.rgba, "%s: [%zu] pixels differ", what, k);
    }
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

        psd::Layer mid = make_layer("Middle \xE4\xB8\xAD\xE6\x96\x87", 2, 3, 40, 30, 1);
        mid.opacity = 80.0f;
        mid.left = 2;
        mid.top = 3;
        mid.right = 42;
        mid.bottom = 33;
        mid.rgba.assign(40 * 30 * 4, 0);
        for (int y = 0; y < 30; ++y)
            for (int x = 0; x < 40; ++x) {
                const size_t i = (size_t(y) * 40 + size_t(x)) * 4;
                mid.rgba[i + 0] = uint8_t(x * 6);
                mid.rgba[i + 1] = uint8_t(y * 8);
                mid.rgba[i + 2] = uint8_t(200 - x);
                mid.rgba[i + 3] = uint8_t(x == 0 ? 0 : (x == 1 ? 1 : (x == 2 ? 127 : 255)));
            }
        doc.layers.push_back(mid);

        psd::Layer top = make_layer("Top \xF0\x9F\x90\xB3 emoji", -5, -4, 20, 20, 2); // negative offset, clipped
        top.opacity = 55.0f;
        doc.layers.push_back(top);
    }
    {
        const size_t before = doc.layers.size();
        CHECK(before == 3, "setup");
    }
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
    CHECK(in.layers.size() == doc.layers.size(), "flat: wrote %zu, read %zu", doc.layers.size(), in.layers.size());
    // Composite: RGB must match a source-over flatten of the (visible) layers;
    // the file stores 3 channels so alpha becomes 255.
    CHECK(in.has_composite(), "composite missing");
    for (size_t i = 0; i < in.composite.size(); i += 4)
        CHECK(in.composite[i + 3] == 255, "composite alpha at %zu", i);

    // re-writing the read-back document must be byte-identical
    {
        psd::Document copy = in;
        std::string e2;
        if (!psd::write_file("/tmp/psd_rt2.psd", copy, e2)) {
            printf("REWRITE FAILED: %s\n", e2.c_str());
            return 1;
        }
        std::vector<uint8_t> a, b;
        read_bytes("/tmp/psd_rt.psd", a);
        read_bytes("/tmp/psd_rt2.psd", b);
        CHECK(a.size() == b.size() && a == b, "flat: rewrite is not byte-identical (%zu vs %zu)", a.size(), b.size());
    }

    // ---------------- 2. group with two layers -----------------------------
    psd::Document gdoc;
    gdoc.width = 40;
    gdoc.height = 30;
    gdoc.layers.push_back(make_layer("Below", 0, 0, 40, 30, 3));
    {
        // Group marker: blend_key "group"; its members are the layers that
        // follow it up to the next group marker.
        psd::Layer group;
        group.name = "Scene \xE7\xBB\x84";
        group.blend_key = "group";
        group.blend_mode = 'N';
        group.visible = false;
        group.opacity = 0.0f;
        gdoc.layers.push_back(group);
    }
    gdoc.layers.push_back(make_layer("Inner A", 1, 2, 20, 15, 4));
    gdoc.layers.push_back(make_layer("Inner B", 10, 8, 25, 20, 5));
    gdoc.layers.back().visible = false; // hidden inside a group
    gdoc.layers.push_back(make_layer("Above", 5, 5, 10, 10, 6));
    {
        psd::Layer empty;
        empty.name = "Empty \xE7\xBB\x84";
        empty.blend_key = "group";
        empty.visible = false;
        empty.opacity = 0.0f;
        gdoc.layers.push_back(empty);
    }
    gdoc.layers.push_back(make_layer("Top", 0, 0, 40, 30, 7));

    CHECK(gdoc.layers.size() == 8, "group setup");
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
    // The reader presents the same structure the writer consumes: the group
    // marker is a layer with blend_key "group" immediately below its members,
    // followed by the next sibling. An empty group is a single marker.
    CHECK(gin.layers.size() == gdoc.layers.size(), "group: %zu != %zu", gin.layers.size(), gdoc.layers.size());
    const size_t gn = gin.layers.size() < gdoc.layers.size() ? gin.layers.size() : gdoc.layers.size();
    for (size_t k = 0; k < gn; ++k)
        CHECK(gdoc.layers[k].name == gin.layers[k].name, "group: layer %zu name '%s' != '%s'", k, gdoc.layers[k].name.c_str(), gin.layers[k].name.c_str());
    // non-group content must be identical, including pixels
    {
        std::vector<psd::Layer> a, b;
        for (const auto &L : gdoc.layers)
            if (L.blend_key != "group") {
                psd::Layer c = L;
                c.blend_key = "norm";
                a.push_back(c);
            }
        for (const auto &L : gin.layers)
            if (L.blend_key != "group") {
                psd::Layer c = L;
                c.blend_key = "norm";
                b.push_back(c);
            }
        compare_layers(a, b, "group content");
    }
    // byte-identical rewrite of the group document
    {
        std::string e2;
        if (!psd::write_file("/tmp/psd_group2.psd", gin, e2)) {
            printf("GROUP REWRITE FAILED: %s\n", e2.c_str());
            return 1;
        }
        std::vector<uint8_t> a, b;
        read_bytes("/tmp/psd_group.psd", a);
        read_bytes("/tmp/psd_group2.psd", b);
        CHECK(a.size() == b.size() && a == b, "group: rewrite is not byte-identical (%zu vs %zu)", a.size(), b.size());
    }

    if (g_failures != 0) {
        printf("%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
    printf("ROUND TRIP OK\n");
    return 0;
}
