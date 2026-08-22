# Model asset converter — specification

**Status:** file-view structs built and verified; the walk is not yet written.

## Why a converter exists

GoldenEye's model files store **32-bit VMA-relative offsets**, not pointers. At load,
`load_object_fill_header()` (`objecthandler_2.c:89`) reads the file and calls
`sub_GAME_7F075A90(objheader, 0x5000000, filedata)` (`model.c:5881`), which computes
`diff = addr - vma` and promotes every offset to a real pointer **in place**:

```c
/* model.c:5677 */
#define PROMOTE(var)  if (var) var = (void *)((u32)var + diff)
```

That works on the N64 because a pointer is 4 bytes and fits the file's slot. On arm64
the struct field is 8 bytes, so the loaded buffer can no longer be reinterpreted as
the struct at all, and `(u32)var + diff` truncates the base address as well.

**Decision: the asset format stays byte-identical; load becomes a conversion pass.**
The files are ROM-derived and cannot be repacked, and the engine passes these records
around as real pointers, so they cannot stay 32-bit in memory either.

## Half that is already done

`tools/gen_asset_fileview.py` generates `src/ge_asset_fileview.h` — a `<Name>_file`
mirror of `ModelNode` and all 19 `union ModelRoData` members, with every pointer
member replaced by a `u32` file offset.

It proves correctness rather than assuming it: each struct's true N64 size is derived
by binary-searching `sizeof` with `_Static_assert` under **armv7** (4-byte pointers),
then the arm64 `_file` view is asserted equal. **20 of 20 match** — e.g. `ModelNode`
is 24 bytes, exactly its documented `0x00`–`0x14` layout.

## What the walk must do

Two passes over the loaded buffer, replacing `modelPromoteNodeOffsetsToPointers`:

1. **Discover and allocate.** Walk the node tree through the `_file` views. For each
   node and each payload record, allocate the native-width struct and record
   `file_offset -> native pointer` in a map.
2. **Bind.** Re-walk and set every pointer field on the native structs by looking up
   the `u32` offset from the `_file` record in that map.

An offset of 0 means NULL — `PROMOTE` skips falsy values, so the converter must too.

## The complete field list — 18 opcodes, 34 sites

Extracted mechanically from `modelPromoteNodeOffsetsToPointers`. This is the contract;
a missing field is a silent corruption, so regenerate rather than hand-edit.

| Opcode | Pointer fields |
|---|---|
| *(every node)* | `Data`, `Parent`, `Next`, `Prev`, `Child` |
| `HEADER` | `FirstGroup` |
| `GROUP` | `ChildGroup` |
| `OP03` | `ChildGroup` |
| `DL` | `Vertices` |
| `DLCOLLISION` | `Vertices`, `CollisionVertices`, `PointUsage`, **`CollisionVertices[i].LinkedTo`** |
| `OP20` | `FirstGroup` |
| `OP05` | `Children`, `Vertices`, `Images`, **`Children[i].unk04`** |
| `OP07` | `unk00`, `unk04`, `Children`, `Vertices`, `Images`, **`Children[i].unk04`** |
| `OP06` | *(none — see note)* |
| `LOD` | `Affects` |
| `SWITCH` | `Controls` |
| `BSP` | `leftChild`, `rightChild` |
| `OP17` | `ChildGroup` |
| `OP11` | `unk0c[15]` |
| `GUNFIRE` | `Image` |
| `SHADOW` | `image`, `Header` |
| `DLPRIMARY` | `Vertices` |

### Cases needing more than a scalar rebind

- **`DLCOLLISION.CollisionVertices[i].LinkedTo`** (`model.c:5735`) — per-element walk
  over an array whose own base pointer is also converted. Convert the base first.
- **`OP05.Children[i].unk04`** (`model.c:5758`) and **`OP07.Children[i].unk04`**
  (`model.c:5777`) — same shape.
- **`OP11.unk0c[15]`** (`model.c:5824`) — *not* a loop despite appearances: a single
  fixed index into `unk0c`.

### Note on `OP06`

`OP06` has no PROMOTE sites, yet the layout audit flags
`ModelRoData_Op06Record.BaseAddr` as pointer-width sensitive. That field is therefore
populated at runtime rather than from the file — it still changes the struct's size,
but the converter must **not** try to resolve it as a file offset.

## Verification plan

The converter cannot be tested until the port builds and runs. Until then:

- `tools/gen_asset_fileview.py` keeps the `_file` views provably in sync with the N64
  layout — re-run it after any `bondtypes.h` change.
- Re-extract this field list from `modelPromoteNodeOffsetsToPointers` after any
  upstream decomp sync; a field added there and missed here corrupts silently.
