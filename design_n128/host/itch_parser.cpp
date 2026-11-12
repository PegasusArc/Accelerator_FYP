#include "itch_parser.h"
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Endian helpers (ITCH is big-endian / network byte order)
//
// These assemble the value byte-by-byte from the most-significant byte down,
// which is correct on any host regardless of its native endianness. This
// deliberately avoids ntohs/ntohl: those live in a platform library (winsock
// on Windows, libc on Linux) that the Vitis HLS C-simulation linker does not
// pull in by default, and the GUI has no reliable field to add -lws2_32. Doing
// the swap in plain arithmetic makes the parser build identically under HLS
// C-sim, native g++ on Windows or Linux, and v++ — with no linker flags.
// ─────────────────────────────────────────────────────────────────────────────
uint16_t ITCHParser::read_u16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
uint32_t ITCHParser::read_u32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) | ((uint32_t)p[3]);
}
uint64_t ITCHParser::read_u64(const uint8_t* p) {
    return ((uint64_t)read_u32(p) << 32) | (uint64_t)read_u32(p + 4);
}
// ITCH prices: 4-byte integer, implicit 4 decimal places (divide by 10000)
double ITCHParser::read_price(const uint8_t* p) {
    return (double)read_u32(p) / 10000.0;
}

// ─────────────────────────────────────────────────────────────────────────────
ITCHParser::ITCHParser() {}

void ITCHParser::reset() {
    order_map_.clear();
    first_price = 0.0;
    have_first  = false;
    std::fill(first_price_.begin(), first_price_.end(), 0.0);
    std::fill(have_first_.begin(), have_first_.end(), false);
    orders_added = orders_deleted = orders_executed = orders_replaced = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// ITCH 5.0 Add Order (type 'A')
// Offset 0:  msg_type (1)
// Offset 1:  stock_locate (2)
// Offset 3:  tracking_num (2)
// Offset 5:  timestamp (6)
// Offset 11: order_ref (8)
// Offset 19: side ('B'/'S') (1)
// Offset 20: shares (4)
// Offset 24: stock (8)
// Offset 32: price (4)
// ─────────────────────────────────────────────────────────────────────────────
void ITCHParser::handle_add_order(const uint8_t* msg, LOBCallback cb) {
    uint64_t ref   = read_u64(msg + 11);
    uint8_t  side  = msg[19];
    uint32_t qty   = read_u32(msg + 20);
    double   price = read_price(msg + 32);

    // The single-symbol fields are kept for existing callers. In multi-symbol
    // mode each instrument needs its own reference, since lob_filter centres
    // prices against it and one instrument's reference applied to another would
    // centre on entirely the wrong level.
    if (!have_first) { first_price = price; have_first = true; }
    if (!symbol_list_.empty() && current_asset_ < first_price_.size()
        && !have_first_[current_asset_]) {
        first_price_[current_asset_] = price;
        have_first_[current_asset_]  = true;
    }

    order_map_[ref] = { price, qty, side, true };

    LOBUpdate_host upd;
    upd.msg_type = 0x01; // MSG_ADD
    upd.side     = (side == 'B') ? 0 : 1;
    upd.price    = price;
    upd.qty      = qty;
    upd.ref_num  = ref;
    upd.asset    = current_asset_;
    cb(upd);
    orders_added++;
}

// ─────────────────────────────────────────────────────────────────────────────
// ITCH 5.0 Order Executed (type 'E')
// Offset 11: order_ref (8)
// Offset 19: executed_shares (4)
// ─────────────────────────────────────────────────────────────────────────────
void ITCHParser::handle_order_exec(const uint8_t* msg, LOBCallback cb) {
    uint64_t ref  = read_u64(msg + 11);
    uint32_t exec = read_u32(msg + 19);

    auto it = order_map_.find(ref);
    if (it == order_map_.end()) return;

    LOBUpdate_host upd;
    upd.msg_type = 0x03; // MSG_EXECUTE
    upd.side     = (it->second.side == 'B') ? 0 : 1;
    upd.price    = it->second.price;
    upd.qty      = exec;
    upd.ref_num  = ref;
    upd.asset    = current_asset_;
    cb(upd);

    if (it->second.qty <= exec) order_map_.erase(it);
    else                        it->second.qty -= exec;
    orders_executed++;
}

// ─────────────────────────────────────────────────────────────────────────────
// ITCH 5.0 Order Cancel (type 'X')
// Offset 11: order_ref (8)
// Offset 19: cancelled_shares (4)
// ─────────────────────────────────────────────────────────────────────────────
void ITCHParser::handle_order_cancel(const uint8_t* msg, LOBCallback cb) {
    uint64_t ref    = read_u64(msg + 11);
    uint32_t cancel = read_u32(msg + 19);

    auto it = order_map_.find(ref);
    if (it == order_map_.end()) return;

    LOBUpdate_host upd;
    upd.msg_type = 0x03;
    upd.side     = (it->second.side == 'B') ? 0 : 1;
    upd.price    = it->second.price;
    upd.qty      = cancel;
    upd.ref_num  = ref;
    upd.asset    = current_asset_;
    cb(upd);

    if (it->second.qty <= cancel) order_map_.erase(it);
    else                          it->second.qty -= cancel;
}

// ─────────────────────────────────────────────────────────────────────────────
// ITCH 5.0 Order Delete (type 'D')
// Offset 11: order_ref (8)
// ─────────────────────────────────────────────────────────────────────────────
void ITCHParser::handle_order_delete(const uint8_t* msg, LOBCallback cb) {
    uint64_t ref = read_u64(msg + 11);

    auto it = order_map_.find(ref);
    if (it == order_map_.end()) return;

    LOBUpdate_host upd;
    upd.msg_type = 0x02; // MSG_DELETE
    upd.side     = (it->second.side == 'B') ? 0 : 1;
    upd.price    = it->second.price;
    upd.qty      = it->second.qty;
    upd.ref_num  = ref;
    upd.asset    = current_asset_;
    cb(upd);

    order_map_.erase(it);
    orders_deleted++;
}

// ─────────────────────────────────────────────────────────────────────────────
// ITCH 5.0 Order Replace (type 'U')
// Offset 11: orig_ref (8)
// Offset 19: new_ref (8)
// Offset 27: new_shares (4)
// Offset 31: new_price (4)
// ─────────────────────────────────────────────────────────────────────────────
void ITCHParser::handle_order_replace(const uint8_t* msg, LOBCallback cb) {
    uint64_t orig_ref = read_u64(msg + 11);
    uint64_t new_ref  = read_u64(msg + 19);
    uint32_t new_qty  = read_u32(msg + 27);
    double   new_px   = read_price(msg + 31);

    auto it = order_map_.find(orig_ref);
    if (it == order_map_.end()) return;

    uint8_t side = it->second.side;
    order_map_.erase(it);
    order_map_[new_ref] = { new_px, new_qty, side, true };

    LOBUpdate_host upd;
    upd.msg_type = 0x04; // MSG_REPLACE
    upd.side     = (side == 'B') ? 0 : 1;
    upd.price    = new_px;
    upd.qty      = new_qty;
    upd.ref_num  = orig_ref; // original ref so FPGA can cancel it
    upd.asset    = current_asset_;
    cb(upd);
    orders_replaced++;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main parse loop
// ─────────────────────────────────────────────────────────────────────────────
void ITCHParser::parse(const uint8_t* buf, size_t len, LOBCallback cb) {
    size_t pos = 0;
    while (pos + 2 <= len) {
        uint16_t msg_len = read_u16(buf + pos);
        pos += 2;
        if (pos + msg_len > len) break;

        const uint8_t* msg = buf + pos;
        uint8_t type = msg[0];

        // ── Multi-symbol filter ─────────────────────────────────────────────
        // Same mechanism as the single-symbol case below, applied to a set. An
        // instrument's locate is learned from its first Add message; until then
        // that instrument contributes nothing, so the very start of a stream may
        // be uneven across instruments. Once resolved, every subsequent message
        // for that locate is tagged with the instrument's index.
        if (!symbol_list_.empty()) {
            uint16_t locate = read_u16(msg + 1);

            if (type == ITCH_ADD_ORDER || type == ITCH_ADD_ORDER_MPID) {
                if (locate_to_asset_.find(locate) == locate_to_asset_.end()) {
                    char sym[9] = {0};
                    std::memcpy(sym, msg + 24, 8);
                    int e = 8;
                    while (e > 0 && sym[e - 1] == ' ') sym[--e] = '\0';
                    for (size_t a = 0; a < symbol_list_.size(); a++) {
                        if (symbol_list_[a] == sym) {
                            locate_to_asset_[locate] = (uint8_t)a;
                            break;
                        }
                    }
                }
            }

            auto la = locate_to_asset_.find(locate);
            if (la == locate_to_asset_.end()) {
                skipped_other_symbol_++;
                pos += msg_len;
                continue;
            }
            current_asset_ = la->second;
        }
        // ── Single-symbol filter ────────────────────────────────────────────
        // stock_locate sits at offset 1 in every order message. Non-Add
        // messages do not carry the ticker, so the locate is learned from the
        // first Add whose symbol (offset 24, 8 bytes, space padded) matches the
        // requested one, then used to filter the rest of the stream.
        else if (!symbol_filter_.empty()) {
            uint16_t locate = read_u16(msg + 1);

            if (!locate_valid_) {
                if (type == ITCH_ADD_ORDER || type == ITCH_ADD_ORDER_MPID) {
                    char sym[9] = {0};
                    std::memcpy(sym, msg + 24, 8);
                    int e = 8;
                    while (e > 0 && sym[e - 1] == ' ') sym[--e] = '\0';
                    if (symbol_filter_ == sym) {
                        locate_filter_ = locate;
                        locate_valid_  = true;
                    }
                }
                if (!locate_valid_) {          // still hunting for the symbol
                    skipped_other_symbol_++;
                    pos += msg_len;
                    continue;
                }
            }

            if (locate != locate_filter_) {
                skipped_other_symbol_++;
                pos += msg_len;
                continue;
            }
        }

        switch (type) {
            case ITCH_ADD_ORDER:
            case ITCH_ADD_ORDER_MPID:
                handle_add_order(msg, cb);    break;
            case ITCH_ORDER_EXECUTED:
            case ITCH_ORDER_EXEC_PRICE:
                handle_order_exec(msg, cb);   break;
            case ITCH_ORDER_CANCEL:
                handle_order_cancel(msg, cb); break;
            case ITCH_ORDER_DELETE:
                handle_order_delete(msg, cb); break;
            case ITCH_ORDER_REPLACE:
                handle_order_replace(msg, cb);break;
            default:
                break; // ignore other message types
        }
        pos += msg_len;
    }
}

void ITCHParser::parse_file(const std::string& path, LOBCallback cb) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("Cannot open ITCH file: " + path);
    // FIX (Offer 2): the Offer 1 version used parentheses here, which C++
    // parses as a FUNCTION DECLARATION (the "most vexing parse"), not a vector.
    // buf.data()/buf.size() then fail to compile. parse_file() was therefore
    // never actually built — braces fix it.
    std::vector<uint8_t> buf{
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>()
    };
    parse(buf.data(), buf.size(), cb);
}


// ─────────────────────────────────────────────────────────────────────────────
// Symbol filtering
// ─────────────────────────────────────────────────────────────────────────────
void ITCHParser::set_symbol(const std::string& sym) {
    symbol_filter_ = sym;
    locate_filter_ = 0;
    locate_valid_  = false;
    skipped_other_symbol_ = 0;
}

// Follow several instruments at once. The order of the list defines each
// instrument's index, which is what the caller uses to interleave the updates
// and to separate the decisions afterwards.
void ITCHParser::set_symbols(const std::vector<std::string>& syms) {
    symbol_list_ = syms;
    locate_to_asset_.clear();
    first_price_.assign(syms.size(), 0.0);
    have_first_.assign(syms.size(), false);
    current_asset_ = 0;
    skipped_other_symbol_ = 0;

    // The two filtering modes are mutually exclusive; entering this one leaves
    // the single-symbol filter inactive.
    symbol_filter_.clear();
    locate_valid_ = false;
}

// Scan a file and count Add messages per ticker. Used to pick a liquid symbol
// before replaying: a full trading day carries thousands of them and most are
// far too thin to produce a meaningful 10-level book.
std::vector<std::pair<std::string, uint64_t>>
ITCHParser::scan_symbols(const std::string& path, size_t max_bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);

    std::vector<uint8_t> buf{std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>()};
    size_t len = buf.size();
    if (max_bytes > 0 && max_bytes < len) len = max_bytes;

    std::unordered_map<std::string, uint64_t> counts;
    size_t pos = 0;
    while (pos + 2 <= len) {
        uint16_t msg_len = read_u16(buf.data() + pos);
        pos += 2;
        if (pos + msg_len > len) break;
        const uint8_t* msg = buf.data() + pos;
        if (msg[0] == ITCH_ADD_ORDER || msg[0] == ITCH_ADD_ORDER_MPID) {
            char sym[9] = {0};
            std::memcpy(sym, msg + 24, 8);
            int e = 8;
            while (e > 0 && sym[e - 1] == ' ') sym[--e] = '\0';
            if (sym[0]) counts[sym]++;
        }
        pos += msg_len;
    }

    std::vector<std::pair<std::string, uint64_t>> out(counts.begin(), counts.end());
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}
