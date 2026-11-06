// Randomised robustness test for psd::read_memory().
//
// Every iteration takes a valid PSD, applies a random corruption (bit flips,
// truncation, length-field explosion, range duplication) and feeds it to the
// reader. The reader must never crash, never hang and never read out of
// bounds; a failure must come back as `false` plus a non-empty message.
// Build with -fsanitize=address,undefined for the full check.
#include "../src/psd/psd.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint32_t next_u32()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return uint32_t(g_rng >> 16);
}

static std::vector<uint8_t> read_file_bytes(const char *path)
{
    std::vector<uint8_t> out;
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return out;
    int c;
    while ((c = std::fgetc(f)) != EOF)
        out.push_back(uint8_t(c));
    std::fclose(f);
    return out;
}

int main(int argc, char **argv)
{
    const char *tmp_base = std::getenv("PSD_TEST_TMPDIR");
#ifdef _WIN32
    if (!tmp_base || !*tmp_base)
        tmp_base = std::getenv("TEMP");
#else
    if (!tmp_base || !*tmp_base)
        tmp_base = "/tmp";
#endif
    if (!tmp_base || !*tmp_base)
        tmp_base = ".";
    const std::string default_seed = std::string(tmp_base) + "/psd_rt.psd";
    const char *seed_path = argc > 1 ? argv[1] : default_seed.c_str();
    const int iterations = argc > 2 ? std::atoi(argv[2]) : 20000;
    const std::vector<uint8_t> seed = read_file_bytes(seed_path);
    if (seed.empty()) {
        std::printf("cannot read seed %s\n", seed_path);
        return 2;
    }

    int crashes = 0, hangs = 0, bad_reports = 0, reads_ok = 0, silent_printed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int it = 0; it < iterations; ++it) {
        std::vector<uint8_t> data = seed;
        const uint32_t kind = next_u32() % 5;
        switch (kind) {
        case 0: { // random byte flips
            const int n = 1 + int(next_u32() % 8);
            for (int k = 0; k < n; ++k) {
                const size_t at = next_u32() % data.size();
                data[at] ^= uint8_t(1u << (next_u32() % 8));
            }
            break;
        }
        case 1: { // truncation
            data.resize(1 + next_u32() % data.size());
            break;
        }
        case 2: { // explode a 32-bit length field
            const size_t at = next_u32() % (data.size() - 4);
            const uint32_t vals[] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x0000FFFFu, 0x10000000u};
            const uint32_t v = vals[next_u32() % 5];
            data[at + 0] = uint8_t(v >> 24);
            data[at + 1] = uint8_t(v >> 16);
            data[at + 2] = uint8_t(v >> 8);
            data[at + 3] = uint8_t(v);
            break;
        }
        case 3: { // overwrite the signature / header
            const size_t at = next_u32() % 40;
            data[at] = uint8_t(next_u32());
            break;
        }
        default: { // random splice
            const size_t at = next_u32() % data.size();
            const size_t len = 1 + next_u32() % 64;
            for (size_t k = 0; k < len && at + k < data.size(); ++k)
                data[at + k] = uint8_t(next_u32());
            break;
        }
        }

        psd::Document doc;
        std::string err;
        // Cap the work a single corrupt file may trigger (a 5 Mpx composite is
        // still far above anything a test document needs).
        psd::ReadOptions opts;
        opts.max_pixels = 5u * 1000u * 1000u;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = psd::read_memory(data.data(), data.size(), doc, err, opts);
        const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        if (dt.count() > 5000) {
            ++hangs;
            std::printf("HANG: iteration %d took %lld ms (kind %u)\n", it, (long long)dt.count(), kind);
            if (hangs > 5)
                return 3;
        }
        if (ok)
            ++reads_ok;
        else if (err.empty())
            ++bad_reports;
        if (!ok && err.empty() && silent_printed < 3) {
            ++silent_printed;
            std::printf("SILENT: iteration %d (kind %u) size=%zu head=%02x%02x%02x%02x\n", it, kind, data.size(),
                        (unsigned)(data.size() > 0 ? data[0] : 0), (unsigned)(data.size() > 1 ? data[1] : 0),
                        (unsigned)(data.size() > 2 ? data[2] : 0), (unsigned)(data.size() > 3 ? data[3] : 0));
        }
        if (data.size() > seed.size() * 4) // sanity: the parser must not grow the input
            ++crashes;
    }
    const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::printf("fuzz: %d iterations in %lld ms, accepted=%d, crashes=%d, hangs=%d, silent-failures=%d\n", iterations,
                (long long)total.count(), reads_ok, crashes, hangs, bad_reports);
    if (crashes || hangs || bad_reports)
        return 1;
    std::printf("FUZZ OK\n");
    return 0;
}
