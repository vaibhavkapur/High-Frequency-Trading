# Data Contracts and Binary Event Interface

**Schema version:** `fpga-ob.v1`  
**Scope:** Synthetic, bounded L2 price-level order-book accelerator (offline simulation/research prototype).

---

## 1. Units, Coordinate System, and Bounded Parameters

All numeric quantities are **unsigned integers** with deterministic fixed-width representations:

| Parameter | Default Value | Width / Type | Meaning |
|---|---|---|---|
| `PRICE_BASE` | `10000` | `uint32_t` | Minimum representable price tick (e.g., `$100.00` in integer ticks of `$0.01`). |
| `NUM_LEVELS` | `64` | Power of 2 (`8..256`) | Number of addressable price ticks per side: `[PRICE_BASE, PRICE_BASE + NUM_LEVELS - 1]`. |
| `MAX_ACTIVE_LEVELS` | `32` | `<= NUM_LEVELS` | Hard hardware capacity bound on simultaneously active (non-zero quantity) price levels per side. |
| `QTY_W` | `32` | `uint32_t` | Aggregate quantity at a price level (shares/contracts, unsigned). Max value `2^32 - 1`. |
| `ORD_CNT_W` | `16` | `uint16_t` | Aggregate resting order count at a price level. Max value `65535`. |
| `SEQ_W` | `32` | `uint32_t` | Monotonic caller-supplied event identifier passed through unchanged to outputs. |
| `TS_W` | `32` | `uint32_t` | Synthetic ingress timestamp in nanoseconds modulo `2^32` (informational pass-through; ordering is strictly by ingress cycle/lane, not timestamp). |

### Address Mapping

A price `P` is in-bounds if and only if:

```text
PRICE_BASE <= P < PRICE_BASE + NUM_LEVELS
level_index = P - PRICE_BASE   // 0 .. NUM_LEVELS - 1
```

---

## 2. Binary Input Event Schema (`EventPacket`, 160 bits / 20 bytes)

Packed binary layout shared by SystemVerilog (`order_book_pkg::event_pkt_t`), C++20 (`fpga_ob::EventPacket`), and Python (`struct.Struct('<IIIIBBBB')`):

| Byte Offset | Width (bits) | Field | Encoding |
|---|---|---|---|
| `0..3` | 32 | `seq_no` | `uint32_t` stable output identifier / sequence number (little-endian on host wire) |
| `4..7` | 32 | `timestamp_ns` | `uint32_t` synthetic ingress timestamp (ns, mod `2^32`) |
| `8..11` | 32 | `price` | `uint32_t` integer tick price |
| `12..15` | 32 | `qty` | `uint32_t` unsigned quantity (shares/lots; must be `> 0` for ADD/CANCEL/EXECUTE) |
| `16` | 8 | `opcode` | `uint8_t` (see Section 2.1) |
| `17` | 8 | `side` | `uint8_t` (`0 = BID`, `1 = ASK`; values `>= 2` are invalid) |
| `18` | 8 | `flags` | `uint8_t` reserved (must be `0`) |
| `19` | 8 | `schema_ver` | `uint8_t` must be `1` (`SCHEMA_VERSION = 1`) |

**Total wire frame size:** `160` bits (`20` bytes) on disk / host-to-RTL stream, packed into a `160-bit` SystemVerilog `packed struct` (`event_pkt_t`) with MSB-to-LSB deterministic bit order:

```text
[159:128] seq_no       (32 bits)
[127:96]  timestamp_ns (32 bits)
[95:64]   price        (32 bits)
[63:32]   qty          (32 bits)
[31:24]   opcode       (8 bits)
[23:16]   side         (8 bits)
[15:8]    flags        (8 bits)
[7:0]     schema_ver   (8 bits)
```

### 2.1 Opcodes (`opcode_e`)

| Value | Name | Semantics |
|---|---|---|
| `0x00` | `OP_NOP` | No book mutation. Returns current BBO snapshot with `STATUS_OK`. `price`, `qty`, and `side` are ignored; `schema_ver` and `flags` are still validated. |
| `0x01` | `OP_ADD` | Adds `qty` to `(side, price)` and increments `order_count` by `1`. Activates the level if previously inactive. |
| `0x02` | `OP_CANCEL` | Subtracts `qty` from `(side, price)`. If remaining `qty == 0`, clears `order_count = 0` and deactivates the level; otherwise sets `order_count = max(1, order_count - 1)`. |
| `0x03` | `OP_EXECUTE` | Subtracts executed `qty` from passive liquidity at `(side, price)`. If remaining `qty == 0`, clears `order_count = 0` and deactivates the level; otherwise leaves `order_count` unchanged (partial fill). |
| `0x04..0xFF` | Reserved / Unsupported | Rejected with `ERR_UNSUPPORTED_OPCODE`. No book mutation. Examples: `0x04` (Modify), `0x05` (Mass Cancel), `0xFF` (Unknown). |

### 2.2 Sides (`side_e`)

| Value | Name |
|---|---|
| `0x00` | `SIDE_BID` |
| `0x01` | `SIDE_ASK` |
| `0x02..0xFF` | Invalid -> rejected with `ERR_INVALID_SIDE` |

---

## 3. Validation Priority, Overflow, and Unsupported Cases

Every non-idle event is validated deterministically in fixed precedence order. If any check fails, **no book state is modified** (atomic reject), the corresponding diagnostic counter increments, and the output packet emits the post-cycle unchanged BBO along with the exact `status` code.

| Priority | Condition | Status Code | Value |
|---|---|---|---|
| 1 | `schema_ver != 1` | `ERR_UNSUPPORTED_SCHEMA` | `0x01` |
| 2 | `flags != 0` | `ERR_UNSUPPORTED_FLAGS` | `0x02` |
| 3 | `opcode` not in `{NOP, ADD, CANCEL, EXECUTE}` | `ERR_UNSUPPORTED_OPCODE` | `0x03` |
| 4 | `opcode == OP_NOP` | `STATUS_OK` (short-circuit) | `0x00` |
| 5 | `side` not in `{SIDE_BID, SIDE_ASK}` | `ERR_INVALID_SIDE` | `0x04` |
| 6 | `price < PRICE_BASE` or `price >= PRICE_BASE + NUM_LEVELS` | `ERR_PRICE_OOB` | `0x05` |
| 7 | `qty == 0` | `ERR_ZERO_QTY` | `0x06` |
| 8 | `OP_ADD` on inactive level when `active_level_count == MAX_ACTIVE_LEVELS` | `ERR_FULL_CAPACITY` | `0x07` |
| 9 | `OP_ADD` where `uint64(level.qty) + qty > 0xFFFF_FFFF` | `ERR_OVERFLOW_QTY` | `0x08` |
| 10 | `OP_ADD` where `level.order_count == 0xFFFF` | `ERR_OVERFLOW_ORDERS` | `0x09` |
| 11 | `OP_CANCEL` on inactive level or `qty > level.qty` | `ERR_INVALID_CANCEL` | `0x0A` |
| 12 | `OP_EXECUTE` on inactive level or `qty > level.qty` | `ERR_INVALID_EXECUTE` | `0x0B` |
| — | All checks pass | `STATUS_OK` | `0x00` |

### Simultaneous Dual-Lane Semantics

When both `in_valid_0` and `in_valid_1` are asserted on the same rising clock edge:
- Lane 0 is validated and applied first, producing intermediate book state `S'`.
- Lane 1 is validated and applied against `S'` in the **same clock cycle**, producing committed state `S''`.
- If Lane 0 fails validation, `S' = S` and Lane 1 still executes against `S` independently.
- If both target the same side and same price level (e.g., `ADD` on Lane 0 + `CANCEL`/`EXECUTE`/`ADD` on Lane 1), Lane 1 observes Lane 0's updated quantity, order count, and active-level count atomically.
- The output packet reports `seq_no` (Lane 0), `seq_no_1` (Lane 1), `lane1_valid`, `status_0`, `status_1`, and the resulting BBO snapshot of `S''`.

---

## 4. Output Contract (`BboOutputPacket`)

Every committed cycle emits one deterministic `BboOutputPacket` containing:

| Field | Type | Meaning |
|---|---|---|
| `schema_ver` | `uint8_t` | Always `1` |
| `lane1_valid` | `bool` | `true` if a second simultaneous event was processed in this cycle |
| `status_0` | `status_e` (`uint8_t`) | Validation/execution status for Lane 0 |
| `status_1` | `status_e` (`uint8_t`) | Validation/execution status for Lane 1 (`STATUS_OK` when `lane1_valid == false`) |
| `seq_no_0` | `uint32_t` | Pass-through `seq_no` from Lane 0 |
| `seq_no_1` | `uint32_t` | Pass-through `seq_no` from Lane 1 (`0` when `lane1_valid == false`) |
| `timestamp_ns` | `uint32_t` | Pass-through `timestamp_ns` from Lane 0 (or Lane 1 if dual-lane latest) |
| `best_bid_valid` | `bool` | `true` iff at least one bid level has `qty > 0` |
| `best_bid_price` | `uint32_t` | Highest active bid price (`0` when `!best_bid_valid`) |
| `best_bid_qty` | `uint32_t` | Aggregate quantity at `best_bid_price` (`0` when `!best_bid_valid`) |
| `best_bid_orders` | `uint16_t` | Aggregate order count at `best_bid_price` (`0` when `!best_bid_valid`) |
| `best_ask_valid` | `bool` | `true` iff at least one ask level has `qty > 0` |
| `best_ask_price` | `uint32_t` | Lowest active ask price (`0` when `!best_ask_valid`) |
| `best_ask_qty` | `uint32_t` | Aggregate quantity at `best_ask_price` (`0` when `!best_ask_valid`) |
| `best_ask_orders` | `uint16_t` | Aggregate order count at `best_ask_price` (`0` when `!best_ask_valid`) |
| `spread` | `int32_t` | `int32_t(best_ask_price) - int32_t(best_bid_price)` when both valid; `0` otherwise |
| `crossed` | `bool` | `true` iff `best_bid_valid && best_ask_valid && best_bid_price >= best_ask_price` |
| `active_bid_levels` | `uint16_t` | Count of active bid price levels (`0..MAX_ACTIVE_LEVELS`) |
| `active_ask_levels` | `uint16_t` | Count of active ask price levels (`0..MAX_ACTIVE_LEVELS`) |

### Matching Scope Disclaimer

This engine maintains an **L2 aggregated price-level book** driven by an external market-data feed (`ADD`, `CANCEL`, `EXECUTE`). It does **not** perform automatic order matching when `best_bid_price >= best_ask_price`; instead, it preserves feed fidelity and raises `crossed = true` for downstream risk or strategy consumers.

---

## 5. Reference Fixtures

Versioned JSON fixtures live in `tests/fixtures/`:
- `deterministic_basic.json` and `deterministic_basic.expected.json`: 12-event valid sequence covering adds, price-level improvement, partial cancel, full cancel, partial execute, full execute, NOP snapshot, and crossed-book signaling.
- `invalid_and_boundary.json`: Boundary and error fixture covering unsupported schema version, reserved flags, unsupported opcode (`0x04`), invalid side (`2`), out-of-bounds low/high prices, zero quantity, `UINT32_MAX` quantity overflow, invalid cancel on empty level, over-cancel (`qty > level.qty`), invalid execute, and `MAX_ACTIVE_LEVELS` full-capacity rejection.
