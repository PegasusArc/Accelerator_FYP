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
