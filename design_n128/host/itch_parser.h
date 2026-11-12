#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
#include <utility>
#include <functional>   // FIX (Offer 2): LOBCallback is std::function but the
                       // Offer 1 header never included <functional>; it only
                       // compiled because <unordered_map> pulled it in
                       // transitively. Breaks on libstdc++ 13 / clang 16.

// ─── ITCH 5.0 message type codes ─────────────────────────────────────────────
#define ITCH_SYSTEM_EVENT      'S'
#define ITCH_ADD_ORDER         'A'
#define ITCH_ADD_ORDER_MPID    'F'
#define ITCH_ORDER_EXECUTED    'E'
#define ITCH_ORDER_EXEC_PRICE  'C'
#define ITCH_ORDER_CANCEL      'X'
#define ITCH_ORDER_DELETE      'D'
#define ITCH_ORDER_REPLACE     'U'
#define ITCH_TRADE             'P'

// ─── LOB update structure (host representation) ──────────────────────────────
// RENAMED in Offer 2: this was `LOBUpdate`, which collides with the ap_fixed
// LOBUpdate in qf_types.h the moment a testbench includes both the parser and
// the HLS headers — which is exactly what end-to-end validation requires.
// Host side is plain double/uint; the FPGA side is fixed_t. They are different
// types and now have different names.
struct LOBUpdate_host {
    uint8_t  msg_type;    // MSG_ADD, MSG_DELETE, MSG_EXECUTE, MSG_REPLACE
    uint8_t  side;        // 0=bid, 1=ask
    double   price;
    uint32_t qty;
    uint64_t ref_num;

    // Which instrument this update belongs to, for context interleaving.
    // Always 0 in single-symbol mode, so existing callers are unaffected.
    uint8_t  asset = 0;
};

// ─── Order tracking ───────────────────────────────────────────────────────────
struct OrderRecord {
    double   price;
    uint32_t qty;
    uint8_t  side;        // 'B' or 'S'
    bool     valid;
};

// ─── LOB update output callback type ─────────────────────────────────────────
using LOBCallback = std::function<void(const LOBUpdate_host&)>;

// ─── ITCH Parser class ────────────────────────────────────────────────────────
class ITCHParser {
public:
    ITCHParser();

    // Parse a raw ITCH binary buffer, call cb for each LOB update
    void parse(const uint8_t* buf, size_t len, LOBCallback cb);

    // Parse from file (for testing / replay)
    void parse_file(const std::string& path, LOBCallback cb);

    // Reset state (clear order map)
    void reset();

    // ── Symbol filtering (REQUIRED for real NASDAQ files) ───────────────────
    // A production ITCH file contains every stock on the venue — thousands of
    // them — interleaved in one stream. Without a filter, orders from every
    // symbol land in the same 10-level book, mixing a $190 stock with a $3 one,
    // and the tick matrix is meaningless. Synthetic files from gen_itch.py hold
    // a single symbol, so this is only needed for real data.
    //
    // Non-Add messages carry only stock_locate, not the ticker, so the locate is
    // learned from the first Add message whose symbol matches, then used to
    // filter everything.
    void set_symbol(const std::string& sym);      // e.g. "AAPL"
    bool symbol_resolved() const { return locate_valid_; }
    uint16_t resolved_locate() const { return locate_filter_; }
    uint64_t messages_skipped() const { return skipped_other_symbol_; }

    // ── Multi-symbol filtering, for context interleaving ────────────────────
    // set_symbol() above follows one instrument and is unchanged; every
    // existing caller behaves exactly as before. set_symbols() follows several
    // at once and tags each update with the index of the instrument it belongs
    // to, so the caller can interleave them and later separate the results.
    //
    // The two modes are mutually exclusive: calling one clears the other.
    //
    // Resolution works as in the single-symbol case. Only Add messages carry a
    // ticker, so each instrument's stock locate is learned from its first Add
    // and the locate is used to filter everything thereafter. An instrument
    // whose first Add has not yet been seen is simply skipped, which means the
    // opening moments of a stream may be uneven across instruments.
    void set_symbols(const std::vector<std::string>& syms);

    // How many of the requested instruments have been resolved so far.
    size_t symbols_resolved() const { return locate_to_asset_.size(); }

    // The first price observed for a given instrument. Each instrument needs
    // its own price_ref, since the centring in lob_filter is relative to it and
    // a reference belonging to a different instrument would centre on the wrong
    // level entirely.
    double first_price_of(size_t asset) const {
        return asset < first_price_.size() ? first_price_[asset] : 0.0;
    }
    bool has_first_price(size_t asset) const {
        return asset < have_first_.size() && have_first_[asset];
    }
    const std::vector<std::string>& symbols() const { return symbol_list_; }

    // Scan a file and return {symbol, add-message count}, most active first.
    // Use this to choose a liquid symbol before replaying.
    static std::vector<std::pair<std::string, uint64_t>>
        scan_symbols(const std::string& path, size_t max_bytes = 0);

    // Offer 2: first observed price. Used to seed lob_filter's price_ref
    // register so the SVD is fed a CENTRED price column instead of one whose
    // ~150.0 DC level swamps the 0.05-scale dynamics it is meant to model.
    double first_price = 0.0;
    bool   have_first  = false;

    // Stats
    uint64_t orders_added    = 0;
    uint64_t orders_deleted  = 0;
    uint64_t orders_executed = 0;
    uint64_t orders_replaced = 0;

private:
    std::unordered_map<uint64_t, OrderRecord> order_map_;

    std::string symbol_filter_;          // empty = accept everything
    uint16_t    locate_filter_  = 0;
    bool        locate_valid_   = false;
    uint64_t    skipped_other_symbol_ = 0;

    // Multi-symbol mode. symbol_list_ holds the requested tickers in the order
    // given, and that order defines each instrument's index. locate_to_asset_
    // maps a resolved stock locate to that index.
    //
    // Note that order_map_ is NOT split per instrument and does not need to be:
    // ITCH order reference numbers are unique across the entire feed, so a
    // single map remains correct however many instruments are followed.
    std::vector<std::string>              symbol_list_;
    std::unordered_map<uint16_t, uint8_t> locate_to_asset_;
    std::vector<double>                   first_price_;
    std::vector<bool>                     have_first_;
    uint8_t                               current_asset_ = 0;

    // Per-message parsers
    void handle_add_order    (const uint8_t* msg, LOBCallback cb);
    void handle_order_exec   (const uint8_t* msg, LOBCallback cb);
    void handle_order_cancel (const uint8_t* msg, LOBCallback cb);
    void handle_order_delete (const uint8_t* msg, LOBCallback cb);
    void handle_order_replace(const uint8_t* msg, LOBCallback cb);

    // Endian helpers (ITCH is big-endian)
    static uint16_t read_u16(const uint8_t* p);
    static uint32_t read_u32(const uint8_t* p);
    static uint64_t read_u64(const uint8_t* p);
    static double   read_price(const uint8_t* p); // 4-byte raw / 10000
};
