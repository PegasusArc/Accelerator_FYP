// ─────────────────────────────────────────────────────────────────────────────
// price_probe.cpp — what price range does the design actually have to cope with?
//
// The client raised a good point: some assets trade at very high prices, and a
// penny stock that keeps reverse-splitting can end up in the thousands or
// higher. That matters here because lob_filter casts the price to fixed_t
// before centring it:
//
//     Bt[i][0] = (fixed_t)(((acc_t)(fixed_t)price - (acc_t)price_ref) * scale)
//
// fixed_t is ap_fixed<24,12>, so it saturates at +/-2048. Above that both the
// price and the reference clamp to the same value, their difference becomes
// zero, and the price half of the tick matrix carries no information at all.
//
// This scans a real ITCH file and reports the price range of every symbol, so
// the question can be answered from data rather than argument:
//
//     - which symbols in this file exceed the ceiling?
//     - how close does the busiest instrument get to it?
//
// Build:
//   g++ -std=c++17 -O2 price_probe.cpp -o price_probe
// Run:
//   ./price_probe ~/quant_accel/20190730.BX_ITCH_50 [max-MB-to-scan]
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// The ceiling that matters: ap_fixed<24,12> represents +/-2048.
// (Widening to <32,12> keeps the SAME range - only the resolution changes -
//  so this limit is unaffected by the precision work.)
static const double FIXED_T_CEILING = 2048.0;

// price_t is ap_fixed<32,16>, a second and much higher ceiling.
static const double PRICE_T_CEILING = 32768.0;

struct Info {
    double lo = 1e18, hi = -1e18;
    uint64_t adds = 0;
};

static uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <itch-file> [max-MB] [--select N]\n"
                     "  default      human-readable price report\n"
                     "  --select N   print a space-separated symbol list for the\n"
                     "               format sweep: the N busiest instruments plus\n"
                     "               every one that exceeds the fixed_t ceiling and\n"
                     "               has enough messages to replay. Nothing is\n"
                     "               learned from testing only well-behaved symbols,\n"
                     "               which is how the range constraint stayed hidden.\n";
        return 1;
    }
    const std::string path = argv[1];
    size_t max_bytes = 0;
    int select_n = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--select")) {
            select_n = (i + 1 < argc) ? atoi(argv[++i]) : 12;
        } else {
            max_bytes = (size_t)atol(argv[i]) * 1024 * 1024;
        }
    }
    const bool quiet = (select_n > 0);

    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << path << "\n"; return 2; }

    if (!quiet) std::cout << "Reading " << path << " ...\n";
    std::vector<uint8_t> buf{std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>()};
    size_t len = buf.size();
    if (max_bytes && max_bytes < len) len = max_bytes;
    if (!quiet) std::cout << "Scanning " << (len / (1024 * 1024)) << " MB\n\n";

    std::unordered_map<std::string, Info> m;
    size_t pos = 0;
    uint64_t msgs = 0;

    while (pos + 2 <= len) {
        uint16_t mlen = rd16(buf.data() + pos);
        pos += 2;
        if (pos + mlen > len) break;
        const uint8_t* msg = buf.data() + pos;

        // 'A' Add Order and 'F' Add Order with MPID both carry stock + price.
        if (msg[0] == 'A' || msg[0] == 'F') {
            char sym[9] = {0};
            std::memcpy(sym, msg + 24, 8);
            int e = 8;
            while (e > 0 && sym[e - 1] == ' ') sym[--e] = '\0';
            if (sym[0]) {
                double px = (double)rd32(msg + 32) / 10000.0;
                Info& i = m[sym];
                i.lo = std::min(i.lo, px);
                i.hi = std::max(i.hi, px);
                i.adds++;
            }
        }
        pos += mlen;
        msgs++;
    }

    if (!quiet) std::cout << "Decoded " << msgs << " messages, "
                          << m.size() << " symbols\n\n";

    std::vector<std::pair<std::string, Info>> v(m.begin(), m.end());

    // ── Selection mode: build a list that spans the price range ─────────────
    if (select_n > 0) {
        std::vector<std::pair<std::string, Info>> busy = v, dear;
        std::sort(busy.begin(), busy.end(),
                  [](auto& a, auto& b) { return a.second.adds > b.second.adds; });

        // Minimum liquidity. Add messages are only part of the update stream,
        // but a symbol with under ~1500 adds will not produce the few thousand
        // decided ticks the decomposition needs to converge. Thinner symbols
        // return huge errors for every format and would hijack the worst-case
        // ranking without saying anything about the format itself.
        const uint64_t MIN_ADDS = 1500;

        std::vector<std::string> out;
        for (auto& kv : busy) {
            if ((int)out.size() >= select_n) break;
            if (kv.second.adds >= MIN_ADDS) out.push_back(kv.first);
        }

        // Force in the high-priced instruments - they are the stress cases and
        // the reason the range constraint went unnoticed - but only those with
        // enough data to give a trustworthy answer.
        for (auto& kv : busy) {
            if (kv.second.hi >= FIXED_T_CEILING && kv.second.adds >= MIN_ADDS &&
                std::find(out.begin(), out.end(), kv.first) == out.end())
                out.push_back(kv.first);
        }
        // Then the priciest liquid symbol below the ceiling, so the sweep
        // always spans the price range even if nothing exceeds it.
        std::sort(v.begin(), v.end(),
                  [](auto& a, auto& b) { return a.second.hi > b.second.hi; });
        for (auto& kv : v) {
            if (kv.second.adds >= MIN_ADDS &&
                std::find(out.begin(), out.end(), kv.first) == out.end()) {
                out.push_back(kv.first);
                break;
            }
        }

        for (size_t i = 0; i < out.size(); i++)
            std::cout << (i ? " " : "") << out[i];
        std::cout << "\n";
        return 0;
    }

    // ── Anything above the fixed_t ceiling ──────────────────────────────────
    std::vector<std::pair<std::string, Info>> over;
    for (auto& kv : v) if (kv.second.hi >= FIXED_T_CEILING) over.push_back(kv);
    std::sort(over.begin(), over.end(),
              [](auto& a, auto& b) { return a.second.hi > b.second.hi; });

    std::cout << "=== Symbols above the fixed_t ceiling ("
              << FIXED_T_CEILING << ") ===\n";
    if (over.empty()) {
        std::cout << "  none in this file.\n"
                     "  So the current design handles every instrument here - but that is a\n"
                     "  property of this data, not a guarantee. See the note at the end.\n";
    } else {
        std::cout << "  " << over.size() << " symbol(s). These would saturate:\n\n";
        printf("  %-10s %12s %12s %10s\n", "SYMBOL", "LOW", "HIGH", "ADDS");
        for (size_t i = 0; i < over.size() && i < 30; i++)
            printf("  %-10s %12.2f %12.2f %10llu\n", over[i].first.c_str(),
                   over[i].second.lo, over[i].second.hi,
                   (unsigned long long)over[i].second.adds);
    }

    // ── The most expensive symbols, whatever they are ───────────────────────
    std::sort(v.begin(), v.end(),
              [](auto& a, auto& b) { return a.second.hi > b.second.hi; });
    std::cout << "\n=== Highest-priced symbols in the file ===\n";
    printf("  %-10s %12s %12s %10s %9s\n",
           "SYMBOL", "LOW", "HIGH", "ADDS", "% OF LIMIT");
    for (size_t i = 0; i < v.size() && i < 15; i++)
        printf("  %-10s %12.2f %12.2f %10llu %8.1f%%\n",
               v[i].first.c_str(), v[i].second.lo, v[i].second.hi,
               (unsigned long long)v[i].second.adds,
               100.0 * v[i].second.hi / FIXED_T_CEILING);

    // ── The busiest symbols, and how close they get ─────────────────────────
    std::sort(v.begin(), v.end(),
              [](auto& a, auto& b) { return a.second.adds > b.second.adds; });
    std::cout << "\n=== Busiest symbols (the ones actually worth replaying) ===\n";
    printf("  %-10s %12s %12s %10s %9s\n",
           "SYMBOL", "LOW", "HIGH", "ADDS", "% OF LIMIT");
    for (size_t i = 0; i < v.size() && i < 15; i++)
        printf("  %-10s %12.2f %12.2f %10llu %8.1f%%\n",
               v[i].first.c_str(), v[i].second.lo, v[i].second.hi,
               (unsigned long long)v[i].second.adds,
               100.0 * v[i].second.hi / FIXED_T_CEILING);

    std::cout << R"(
=== What this means ===

The price is cast to fixed_t before it is centred on the reference price, so the
absolute price has to fit in +/-2048 - not just the movement around it. Above
that, price and reference both clamp to the same number, their difference
becomes zero, and the price half of the tick matrix goes blank.

It would fail loudly rather than quietly: the reference model uses ordinary
double-precision arithmetic and does not saturate, so the distribution check
would diverge and the run would report FAILED.

Note that widening fixed_t from <24,12> to <32,12> does NOT help here. That
change buys resolution, not range - the ceiling stays at 2048 either way.

If instruments above 2048 need to be supported, the fix is to centre the price
BEFORE narrowing it: keep the subtraction in price_t (ap_fixed<32,16>, ceiling
32768) or wider, and only convert the small centred result to fixed_t. The
centred value stays small no matter how large the absolute price is, so this
costs almost nothing. It is a change inside lob_filter plus the price_ref
register, and it needs a rebuild.
)";
    return 0;
}
