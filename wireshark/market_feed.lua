-- Wireshark Lua Dissector for Low Latency Trading Exchange
-- Implements SRS Section 4.4 and protocol_design_and_plan.md Sections 4.1 - 4.6.
-- Usage in Wireshark:
--   wireshark -X lua_script:wireshark/market_feed.lua market_feed.pcap

-- ============================================================================
-- 1. TCP INGRESS & CONTROL PROTOCOL DISSECTOR (Port 8085)
-- ============================================================================
local exch_tcp_proto = Proto("exch_tcp", "Low Latency Exchange TCP Protocol")

local msg_type_names = {
    [0x01] = "LIST_FEED",
    [0x02] = "BID",
    [0x03] = "ASK",
    [0x04] = "CANCEL",
    [0x05] = "SUBSCRIBE",
    [0x06] = "UNSUBSCRIBE",
    [0x07] = "DISCONNECT",
    [0x81] = "LIST_FEED_RSP",
    [0x82] = "ORDER_ACK",
    [0x83] = "CANCEL_ACK",
    [0x84] = "SUBSCRIBE_ACK",
    [0x85] = "UNSUBSCRIBE_ACK",
    [0x86] = "ORDER_UPDATE",
    [0xFF] = "ERROR"
}

local order_status_names = {
    [0] = "RESTING",
    [1] = "FILLED",
    [2] = "PARTIAL_FILL",
    [3] = "CANCELLED"
}

local error_code_names = {
    [1] = "BAD_FORMAT",
    [2] = "UNKNOWN_SYMBOL",
    [3] = "QUEUE_FULL",
    [4] = "INVALID_PRICE_VOL",
    [5] = "ORDER_NOT_FOUND",
    [6] = "UNKNOWN_CLIENT",
    [7] = "CLIENT_ID_REQUIRED"
}

local f_tcp_len       = ProtoField.uint32("exch_tcp.length", "Frame Length", base.DEC)
local f_tcp_req_id    = ProtoField.uint32("exch_tcp.req_id", "Request ID", base.DEC)
local f_tcp_client_id = ProtoField.uint64("exch_tcp.client_id", "Client ID", base.DEC)
local f_tcp_cb_port   = ProtoField.uint16("exch_tcp.callback_port", "Callback Port", base.DEC)
local f_tcp_type      = ProtoField.uint8("exch_tcp.type", "Message Type", base.HEX, msg_type_names)
local f_tcp_ver       = ProtoField.uint8("exch_tcp.version", "Protocol Version", base.DEC)

-- Payload Fields
local f_order_sym_id  = ProtoField.uint16("exch_tcp.order.symbol_id", "Symbol ID", base.DEC)
local f_order_price   = ProtoField.uint64("exch_tcp.order.price", "Limit Price", base.DEC)
local f_order_vol     = ProtoField.uint64("exch_tcp.order.volume", "Order Volume", base.DEC)
local f_order_id      = ProtoField.uint64("exch_tcp.order.order_id", "Order ID", base.HEX)
local f_order_status  = ProtoField.uint8("exch_tcp.order.status", "Order Status", base.DEC, order_status_names)
local f_order_rem_vol = ProtoField.uint64("exch_tcp.order.remaining_vol", "Remaining Volume", base.DEC)
local f_err_code      = ProtoField.uint8("exch_tcp.error_code", "Error Code", base.DEC, error_code_names)
local f_sub_udp_port  = ProtoField.uint16("exch_tcp.sub.udp_port", "UDP Port", base.DEC)
local f_sub_grp_mask  = ProtoField.uint32("exch_tcp.sub.group_mask", "Group Mask", base.HEX)

exch_tcp_proto.fields = {
    f_tcp_len, f_tcp_req_id, f_tcp_client_id, f_tcp_cb_port, f_tcp_type, f_tcp_ver,
    f_order_sym_id, f_order_price, f_order_vol, f_order_id, f_order_status, f_order_rem_vol,
    f_err_code, f_sub_udp_port, f_sub_grp_mask
}

function exch_tcp_proto.dissector(buffer, pinfo, tree)
    local length = buffer:len()
    if length < 20 then return end

    local frame_len = buffer(0, 4):uint()
    local req_id    = buffer(4, 4):uint()
    local client_id = buffer(8, 8):uint64()
    local cb_port   = buffer(16, 2):uint()
    local msg_type  = buffer(18, 1):uint()
    local version   = buffer(19, 1):uint()

    local type_name = msg_type_names[msg_type] or string.format("0x%02X", msg_type)
    pinfo.cols.protocol = "EXCH-TCP"
    pinfo.cols.info = string.format("[%s] Req #%d | Client: %s | Len: %d", type_name, req_id, tostring(client_id), frame_len)

    local subtree = tree:add(exch_tcp_proto, buffer(0, frame_len), string.format("Exchange TCP: %s (Req #%d)", type_name, req_id))
    local hdr_tree = subtree:add("Frame Header (20 bytes)")
    hdr_tree:add(f_tcp_len, buffer(0, 4))
    hdr_tree:add(f_tcp_req_id, buffer(4, 4))
    hdr_tree:add(f_tcp_client_id, buffer(8, 8))
    hdr_tree:add(f_tcp_cb_port, buffer(16, 2))
    hdr_tree:add(f_tcp_type, buffer(18, 1))
    hdr_tree:add(f_tcp_ver, buffer(19, 1))

    local payload_len = frame_len - 20
    if payload_len > 0 and length >= frame_len then
        local p_tree = subtree:add("Payload (" .. payload_len .. " bytes)")
        if msg_type == 0x02 or msg_type == 0x03 then -- BID / ASK
            if payload_len >= 18 then
                p_tree:add(f_order_sym_id, buffer(20, 2))
                p_tree:add(f_order_price, buffer(22, 8))
                p_tree:add(f_order_vol, buffer(30, 8))
            end
        elseif msg_type == 0x04 then -- CANCEL
            if payload_len >= 8 then
                p_tree:add(f_order_id, buffer(20, 8))
            end
        elseif msg_type == 0x05 then -- SUBSCRIBE
            if payload_len >= 6 then
                p_tree:add(f_sub_udp_port, buffer(20, 2))
                p_tree:add(f_sub_grp_mask, buffer(22, 4))
            end
        elseif msg_type == 0x82 then -- ORDER_ACK
            if payload_len >= 17 then
                p_tree:add(f_order_id, buffer(20, 8))
                p_tree:add(f_order_status, buffer(28, 1))
                p_tree:add(f_order_rem_vol, buffer(29, 8))
            end
        elseif msg_type == 0x83 then -- CANCEL_ACK
            if payload_len >= 9 then
                p_tree:add(f_order_id, buffer(20, 8))
                p_tree:add(f_order_status, buffer(28, 1))
            end
        elseif msg_type == 0xFF then -- ERROR
            if payload_len >= 1 then
                p_tree:add(f_err_code, buffer(20, 1))
            end
        end
    end
end

-- ============================================================================
-- 2. UDP MARKET FEED DISSECTOR (124/128-byte datagrams)
-- ============================================================================
local marketfeed_proto = Proto("marketfeed", "Low Latency Market Feed Protocol")

local f_seq_num     = ProtoField.uint64("marketfeed.seq_num", "Sequence Number", base.DEC)
local f_timestamp   = ProtoField.uint64("marketfeed.timestamp", "Microsecond Timestamp", base.DEC)
local f_group_id    = ProtoField.uint32("marketfeed.group_id", "Ticker Group ID", base.DEC)
local f_ticker      = ProtoField.string("marketfeed.ticker", "Ticker Symbol")

local f_bid1_price  = ProtoField.uint64("marketfeed.bid1.price", "Bid L1 Price", base.DEC)
local f_bid1_vol    = ProtoField.uint64("marketfeed.bid1.vol", "Bid L1 Volume", base.DEC)
local f_bid2_price  = ProtoField.uint64("marketfeed.bid2.price", "Bid L2 Price", base.DEC)
local f_bid2_vol    = ProtoField.uint64("marketfeed.bid2.vol", "Bid L2 Volume", base.DEC)
local f_bid3_price  = ProtoField.uint64("marketfeed.bid3.price", "Bid L3 Price", base.DEC)
local f_bid3_vol    = ProtoField.uint64("marketfeed.bid3.vol", "Bid L3 Volume", base.DEC)

local f_ask1_price  = ProtoField.uint64("marketfeed.ask1.price", "Ask L1 Price", base.DEC)
local f_ask1_vol    = ProtoField.uint64("marketfeed.ask1.vol", "Ask L1 Volume", base.DEC)
local f_ask2_price  = ProtoField.uint64("marketfeed.ask2.price", "Ask L2 Price", base.DEC)
local f_ask2_vol    = ProtoField.uint64("marketfeed.ask2.vol", "Ask L2 Volume", base.DEC)
local f_ask3_price  = ProtoField.uint64("marketfeed.ask3.price", "Ask L3 Price", base.DEC)
local f_ask3_vol    = ProtoField.uint64("marketfeed.ask3.vol", "Ask L3 Volume", base.DEC)

marketfeed_proto.fields = {
    f_seq_num, f_timestamp, f_group_id, f_ticker,
    f_bid1_price, f_bid1_vol, f_bid2_price, f_bid2_vol, f_bid3_price, f_bid3_vol,
    f_ask1_price, f_ask1_vol, f_ask2_price, f_ask2_vol, f_ask3_price, f_ask3_vol
}

function marketfeed_proto.dissector(buffer, pinfo, tree)
    local length = buffer:len()
    if length < 124 then return end

    pinfo.cols.protocol = "MKT-FEED"

    local seq_num   = buffer(0, 8):uint64()
    local timestamp = buffer(8, 8):uint64()
    local group_id  = buffer(16, 4):uint()
    local ticker    = buffer(20, 8):string():gsub("%z+$", "")

    pinfo.cols.info = string.format("Ticker: %s | Seq: %s | Group: %d | Len: %d", ticker, tostring(seq_num), group_id, length)

    local subtree = tree:add(marketfeed_proto, buffer(0, math.min(length, 128)), string.format("Market Feed: %s (Seq #%s)", ticker, tostring(seq_num)))

    local hdr_tree = subtree:add("Header Fields")
    hdr_tree:add(f_seq_num, buffer(0, 8))
    hdr_tree:add(f_timestamp, buffer(8, 8))
    hdr_tree:add(f_group_id, buffer(16, 4))
    hdr_tree:add(f_ticker, buffer(20, 8), ticker)

    local bids_tree = subtree:add("Top-3 Bid Levels (BBO)")
    local b1_tree = bids_tree:add(string.format("Level 1: Price %d | Vol %d", buffer(32, 8):uint64():tonumber(), buffer(40, 8):uint64():tonumber()))
    b1_tree:add(f_bid1_price, buffer(32, 8))
    b1_tree:add(f_bid1_vol, buffer(40, 8))

    local b2_tree = bids_tree:add(string.format("Level 2: Price %d | Vol %d", buffer(48, 8):uint64():tonumber(), buffer(56, 8):uint64():tonumber()))
    b2_tree:add(f_bid2_price, buffer(48, 8))
    b2_tree:add(f_bid2_vol, buffer(56, 8))

    local b3_tree = bids_tree:add(string.format("Level 3: Price %d | Vol %d", buffer(64, 8):uint64():tonumber(), buffer(72, 8):uint64():tonumber()))
    b3_tree:add(f_bid3_price, buffer(64, 8))
    b3_tree:add(f_bid3_vol, buffer(72, 8))

    local asks_tree = subtree:add("Top-3 Ask Levels (BBO)")
    local a1_tree = asks_tree:add(string.format("Level 1: Price %d | Vol %d", buffer(80, 8):uint64():tonumber(), buffer(88, 8):uint64():tonumber()))
    a1_tree:add(f_ask1_price, buffer(80, 8))
    a1_tree:add(f_ask1_vol, buffer(88, 8))

    local a2_tree = asks_tree:add(string.format("Level 2: Price %d | Vol %d", buffer(96, 8):uint64():tonumber(), buffer(104, 8):uint64():tonumber()))
    a2_tree:add(f_ask2_price, buffer(96, 8))
    a2_tree:add(f_ask2_vol, buffer(104, 8))

    local a3_tree = asks_tree:add(string.format("Level 3: Price %d | Vol %d", buffer(112, 8):uint64():tonumber(), buffer(120, 8):uint64():tonumber()))
    a3_tree:add(f_ask3_price, buffer(112, 8))
    a3_tree:add(f_ask3_vol, buffer(120, 8))
end

-- Heuristic dissectors so any port carrying 128-byte market feed or 20-byte TCP frame is automatically decoded:
local function heur_dissect_exch_tcp(buffer, pinfo, tree)
    if buffer:len() >= 20 then
        local len = buffer(0, 4):uint()
        local ver = buffer(19, 1):uint()
        local mtype = buffer(18, 1):uint()
        if ver == 1 and (len == 20 or len == 38 or len == 28 or len == 26 or len == 29 or (mtype >= 1 and mtype <= 7) or (mtype >= 0x81 and mtype <= 0x86) or mtype == 0xFF) then
            exch_tcp_proto.dissector(buffer, pinfo, tree)
            return true
        end
    end
    return false
end
exch_tcp_proto:register_heuristic("tcp", heur_dissect_exch_tcp)

local function heur_dissect_marketfeed(buffer, pinfo, tree)
    if buffer:len() == 128 then
        marketfeed_proto.dissector(buffer, pinfo, tree)
        return true
    end
    return false
end
marketfeed_proto:register_heuristic("udp", heur_dissect_marketfeed)

-- Register known default ports
local tcp_port_table = DissectorTable.get("tcp.port")
tcp_port_table:add(8085, exch_tcp_proto)
tcp_port_table:add(8095, exch_tcp_proto)
tcp_port_table:add(8091, exch_tcp_proto)
tcp_port_table:add(8092, exch_tcp_proto)

local udp_port_table = DissectorTable.get("udp.port")
udp_port_table:add(54321, marketfeed_proto)
udp_port_table:add(54322, marketfeed_proto)
udp_port_table:add(54323, marketfeed_proto)
udp_port_table:add(9876, marketfeed_proto)
udp_port_table:add(9901, marketfeed_proto)
udp_port_table:add(9902, marketfeed_proto)
udp_port_table:add(9903, marketfeed_proto)

