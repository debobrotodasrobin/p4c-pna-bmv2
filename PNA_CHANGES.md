# PNA-in-bmv2: Changes over upstream

This branch adds full PNA (Programmable Network Architecture) feature support to the bmv2 PNA backend (`p4c-bm2-pna`), enabling the `pna_nic` target to support the complete PNA specification.

**Companion repo**: [behavioral-model-pna-nic](https://github.com/debobrotodasrobin/behavioral-model-pna-nic) (bmv2 target-side changes)

## Summary

| Metric | Value |
|---|---|
| Files changed | 12 |
| Lines added | +435 |
| Lines removed | -32 |
| Net | +403 |
| Impact on v1model | **Zero** (224 programs byte-identical) |
| Impact on PSA | **Zero** (28 programs, 0 semantic diffs) |
| PNA programs newly compiling | 4 |

## Features enabled

### 1. Value-returning extern functions (`is_host_port`, `is_net_port`)

**Problem**: PNA extern functions that return a value (e.g. `bool is_host_port(PortId_t)`) had no lowering path in the bmv2 backend.

**Fix**: `action.cpp` recognizes value-returning extern functions and prepends the destination as argument 0. `extern.cpp` emits that destination as a writable reference (detected by arity).

**Files**: `action.cpp`, `extern.cpp`

### 2. Meter colour argument (`Meter.execute(index, color)`)

**Problem**: `action.cpp:78` pushed only `arguments->at(0)`, silently discarding `color`. Colour-aware metering ran colour-blind with no diagnostic.

**Fix**: Forward ALL original arguments. `ExternConverter_Meter` detects 2 vs 3 args and emits `_Meter_execute` vs `_Meter_execute_color`.

**Files**: `action.cpp`, `portable.cpp`

### 3. DirectMeter (`pna_direct_meter`)

**Problem**: bmv2 implements direct meters purely from JSON (`meter_arrays` with `is_direct:true` + `binding:<table>` + `result_target:<field>`). None of that JSON was emitted for PNA because `directMeterMap` was populated only from v1model's `meters` property.

**Fix**: `control.h` reads `pna_direct_meter`/`psa_direct_meter` table properties, populates the map, emits the table's `direct_meters` field. `portable.cpp` handles the `[dest, (pre_color)]` shape.

**Files**: `control.h`, `action.cpp`, `portable.cpp`

### 4. DirectCounter (`pna_direct_counter`)

**Problem**: p4c never recognized the `pna_direct_counter` table property, so `with_counters` was always `false`.

**Fix**: Recognize `pna_direct_counter`/`psa_direct_counter` properties and emit `with_counters: true`. bmv2 auto-increments `EntryMeta::counter` on every table hit — the flag only gates the query API.

**Files**: `control.h`

### 5. ActionProfile (`pna_implementation`)

**Problem**: `ActionProfileTraits<Arch::PNA>` inherited PSA's `propertyName()` of `"implementation"`, but PNA validation only accepts `"pna_implementation"`. Tables passed validation but were never bound to the profile.

**Fix**: Give `ActionProfileTraits<Arch::PNA>` and `ActionSelectorTraits<Arch::PNA>` their own specialisations returning `"pna_implementation"`.

**Files**: `helpers.h`

### 6. ActionSelector (`indirect_ws`)

**Problem**: `SharedActionSelectorCheck` hard-coded `"implementation"` when looking up the table property, so for PNA it never recorded the selector input. The selector was dropped as "never referenced by a table".

**Fix**: Pass the property name as a constructor parameter from `control.h` (avoids `-Wtemplate-body` cascade that occurs when calling traits inside the template class body).

**Files**: `sharedActionSelectorCheck.h`, `control.h`

### 7. SelectByDirection

**Problem**: `SelectByDirection<T>(is_network_port, from_net_value, from_host_value)` is a `@pure` extern in `bmv2/pna.p4`. The bmv2 backend explicitly rejects it (xfail list).

**Fix**: A `SelectByDirectionToTernary` pass rewrites the extern call to `IR::Mux(cond, ifNet, ifHost)` before `RemoveComplexExpressions` runs. The bmv2 runtime already supports ternary (`op == "?"`).

**Files**: `pnaNic.cpp`, `pnaNic.h`

### 8. Complex table keys (scalars metadata bug)

**Problem**: Any table key computed in a code_block (binary arithmetic, ternary, etc.) read 0 at lookup time in pna_nic. The `scalars` header was emitted with `"metadata": false`. Since it's never extracted by the parser, `is_valid()` returns false, and `MatchKeyBuilder::operator()` appends zeros instead of the actual field value.

**Fix**: Changed `add_header` → `add_metadata` in `portable.cpp:117`. One-word fix.

**Files**: `portable.cpp`

### 9. InternetChecksum / Hash field_list

**Problem**: `InternetChecksum` and `Hash` externs need `field_list` arguments. The PNA backend lacked the `mkFieldListValue` helper.

**Fix**: Added `mkFieldListValue` and the `ExternConverter_InternetChecksum`/`ExternConverter_Hash` implementations in `pnaNic.cpp`.

**Files**: `pnaNic.cpp`, `pnaNic.h`

### 10. p4c segfault fix (convertCondition/yieldsBoolean)

**Problem**: PNA programs with non-boolean select expressions segfaulted `p4c-bm2-pna`.

**Fix**: Added `convertCondition()` and `yieldsBoolean()` to handle non-boolean conditions in select expressions safely.

**Files**: `expression.cpp`, `expression.h`

### 11. Bound `inout` action parameters (DASH blocker)

**Problem**: DASH-style actions that take the headers/metadata struct as an
`inout` parameter and are bound at the table (`set_tunnel_mapping(hdr, meta)`)
are rewritten by the midend into copy-in/copy-out through synthesized
control-level temporaries (`ethernet_t h_0_u0_ethernet;`). The portable PNA
path dropped header-/struct-typed variable declarations, so the JSON
referenced headers that were never registered and bmv2 rejected the config
("Invalid reference to object of type 'header'"). DASH's PNA build had 52
such dangling references.

**Fix**: `InspectPnaProgram::preorder(Declaration_Variable)` now registers
`Type_Header` declarations as header instances and `Type_Struct` declarations
as metadata instances, mirroring v1model's `HeaderConverter`.

**Files**: `pnaProgramStructure.cpp`

### 12. `extern void recirculate()` declaration

**Problem**: `recirculate` was not declared in `bmv2/pna.p4` (only the `recirculated` metadata field existed).

**Fix**: Added `extern void recirculate();` to the PNA header.

**Files**: `p4include/bmv2/pna.p4`

### 13. IdleTimeout externs lowering (`add_entry` + `pna_idle_timeout` property)

**Fix**:
- `control.h` recognises the `pna_idle_timeout` table property and emits
  `support_timeout: true` plus the mode string (`AUTO_DELETE` /
  `NOTIFY_CONTROL`) into the bmv2 JSON.
- `pnaNic.{h,cpp}` add an `ExternConverter` for `add_entry`, which wraps its
  struct action-parameters argument into a `parameters_vector` for the bmv2
  primitive. `restart_expire_timer`, `set_entry_expire_time` and
  `allocate_flow_id` lower through the generic extern-function path.

**Files**: `backends/bmv2/common/control.h`, `backends/bmv2/pna_nic/pnaNic.{h,cpp}`

### 14. Action conditionals (drop Predication from the PNA pipeline)

**Problem**: The PNA midend ran `Predication`, which rewrites action
conditionals into predicated (mux) assignments and hard-errors on any
method-call statement inside an `if`. This blocks the IdleTimeout externs that
DASH conntrack calls under a TCP-flag guard
(`Conditional execution in actions unsupported on this target`).

**Fix**: Removed `Predication` from `pna_nic/midend.cpp`. bmv2 supports
conditional execution in actions natively — the shared `ActionConverter` lowers
`IR::IfStatement` to conditional jumps (this is how v1model, which never runs
Predication, handles it). The pass is a no-op for programs without action
conditionals; for those that have them the output changes only from predicated
muxes to conditional jumps (semantically equivalent, runtime-verified).

**Files**: `backends/bmv2/pna_nic/midend.cpp`

## DASH (sonic-net/DASH) compatibility

The DASH pipeline (`dash-pipeline/bmv2`, PNA/DPDK variant) now compiles for
pna_nic and runs:

```bash
p4c-bm2-pna -I/usr/local/share/p4c/p4include/bmv2 -o dash.json \
    -DTARGET_DPDK_PNA -DDISABLE_128BIT_ARITHMETIC dash_pipeline.p4
```

Verified: 116 tables, 176 actions, 84 headers load cleanly on pna_nic; a test
packet traverses the parser (extracts `u0_ethernet`, `u0_ipv4`), 34 tables,
and the deparser.

**`-DPNA_CONNTRACK` now compiles and loads**: with the IdleTimeout externs
(feature 13) and action conditionals (feature 14), the conntrack-enabled DASH
pipeline builds and loads on pna_nic:

```bash
p4c-bm2-pna -I/usr/local/share/p4c/p4include/bmv2 -I. -o dash_ct.json \
    -DTARGET_DPDK_PNA -DDISABLE_128BIT_ARITHMETIC -DPNA_CONNTRACK dash_pipeline.p4
```

DASH's own PNA variant already excludes counters, meters and asserts
(`dash_arch_specific.p4`).

## Regression testing

| Test | Result |
|---|---|
| v1model A/B (224 programs) | Byte-identical |
| PSA A/B (28 programs) | 0 semantic diffs |
| PNA A/B (35 programs) | 0 semantic diffs |
| PNA newly compiling | 4 programs |
| PNA A/B after dropping Predication | identical compilable set; 15/17 byte-identical, 2 conditional-representation (runtime-verified) |
| Feature suites (13, incl. ft_ageing) | All pass |
| Mininet NIC+fabric demo | 6/6 host pairs |
| DASH conntrack (`-DPNA_CONNTRACK`) | compiles + loads |

## Build

```bash
cd p4c
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) p4c-bm2-pna p4c-bm2-psa p4c-bm2-ss
sudo cp backends/bmv2/p4c-bm2-{ss,pna,psa} /usr/local/bin/
sudo cp ../p4include/bmv2/pna.p4 /usr/local/share/p4c/p4include/bmv2/pna.p4
```
