# Allocator ownership across module boundaries

Status, 2026-11-02:

- **Part 1 (the cause)** is derived from symbol resolution. It is unverified on
  a Windows host except where Part 3 cites the Windows side's measurements.
  Facts W1, W2 and W4-W8 are measured on Windows (Part 3); W3's backtrace
  is pending a debugger.
- **Part 2 (the model)** is the owner's ownership-boundary model. It replaces an
  earlier draft that proposed a process-wide replacement; that draft was
  withdrawn.
- **Part 4** records what is decided and what is still open.
- **Implemented** (branch `claude/allocator-ownership-impl`): the global
  allocator is removed (2.5 A), named calls only (2.4 option 4), static
  libc++ enforced on MinGW (2.3), the tagging check mode and the must-trap
  death tests (2.6).

## Sources

- **libc++ 22.1.8 headers** (`include/c++/v1`), the same release as MSYS2
  CLANG64 22.1.8. Read on the M4 under `/opt/homebrew/opt/llvm@22`.
- **mimalloc v3.5.3** as vendored in `third_party/mimalloc`.
- **FreeType and HarfBuzz** at GUI.Forms' pins: FreeType
  f4205da14867c5387cd6a329b90ee10a6df6eeff, HarfBuzz
  c7a7457b7385f33178e8cf87615ca077a810bbe7 (`third_party/fetch_text_stack.sh`).
- **miniaudio and SheenBidi** as GUI.Forms vendors them.
- **The Windows side's record:** the private projects repository,
  `tracks/gui_forms/hot_paths.md`, "2026-11-02: Windows allocator boundaries".
  Its measurements used LLVM 22.1.8 CLANG64 on Windows 11 x86-64, built with
  -O1 -g.

Nothing in Part 1 rests on a repro outcome. Where a measurement agrees with it,
the measurement is cited as confirmation.

## Part 1. The cause (Windows: derived, then confirmed by the measurements in Part 3)

### 1.1 What binds where

**(a) In the executable, operator new and delete bind to stx_auxlib's replacements.**
On MinGW a consumer of libc++ compiles with all visibility annotations empty:
`__config` 197-201, `defined(__MINGW32__) && !defined(_LIBCPP_BUILDING_LIBRARY)`
sets `_LIBCPP_OVERRIDABLE_FUNC_VIS` and `_LIBCPP_EXPORTED_FROM_ABI` to nothing.
So `<new>` (`__new/global_new_delete.h` 30-65) declares every operator new and
delete without `dllimport`, and the program's calls are ordinary external
references. The linker resolves each in command-line order. The repro command
lists `libstx_auxlib_global_allocator.a` before the driver's implicit `-lc++`, and
`global_allocator.cpp` defines all 20 forms. The archive member is the first
definition, so the exe's references bind to it (mimalloc). **[W1]** confirms that
the exe imports no `_Znwy`/`_Zdl*` from libc++.dll.

**(b) Inside libc++.dll, operator new and delete bind to libc++'s own definitions.**
When the library was built, `__config` 210 made `_LIBCPP_OVERRIDABLE_FUNC_VIS`
`__declspec(dllexport)`. The DLL defines and exports them, and its own calls
were bound when the DLL was linked. PE/COFF has no symbol interposition: an
image's references are fixed at its link time to itself, or to (DLL name,
symbol) pairs in its import table, and the loader fills those from the named
DLL only. libc++.dll does not import from the executable. So no definition in
the exe can reach code inside libc++.dll. Its operator delete calls the C
library `free` (UCRT, `api-ms-win-crt-heap-l1-1-0.dll` → ucrtbase, the process
heap). **[W2]** gives the DLL's export and import tables.

**(c) So any block that is allocated on one side of the boundary and freed on the other is a mismatched pair.**
The C++ standard's rule that a program's replacement applies to the whole
program ([replacement.functions]) holds on ELF, through interposition, and on
Mach-O, through weak-definition coalescing (part 2). It does not hold on PE
when the C++ runtime is a DLL.

### 1.2 std::thread's crossing pair (libc++ 22 `__thread/thread.h`)

| Step | Code | Image that runs it | Allocator |
|---|---|---|---|
| 1 | `thread::thread(F&&, Args&&...)`, `_LIBCPP_HIDE_FROM_ABI` (line 209): `_TSPtr __tsp(new __thread_struct)` (215) | exe (header template, instantiated in the caller) | **exe new → mimalloc** |
| 2 | `__thread_struct::__thread_struct()`, `_LIBCPP_EXPORTED_FROM_ABI` class (58-66), allocates `__thread_struct_imp` | libc++.dll | DLL new → UCRT; freed by the DLL's `~__thread_struct`: matched |
| 3 | `new _Gp(std::move(__tsp), ...)` (217), deleted by `unique_ptr<_Fp>` in `__thread_proxy` (165-171, `_LIBCPP_HIDE_FROM_ABI`) | exe and exe | mimalloc and mimalloc: matched |
| 4 | `__thread_proxy`: `__thread_local_data().set_pointer(std::get<0>(*__p).release())` (168) passes ownership of block 1 to the thread-specific slot. `__thread_local_data()` is exported (56) and defined in the DLL; `set_pointer` (118-121) calls the exported `__libcpp_tls_set` (`__thread/support/windows.h` 127) | exe → libc++.dll | ownership moves into the DLL |
| 5 | Thread exit: the slot's destructor `__thread_specific_ptr<_Tp>::__at_thread_exit` runs `delete static_cast<pointer>(__p)` (98-100) | **libc++.dll** | **DLL delete → UCRT free of a mimalloc block** |

Why step 5 runs in the DLL: `__at_thread_exit` is registered in the
`__thread_specific_ptr()` constructor (103-105) through the exported
`__libcpp_tls_create` (`windows.h` 123; FlsAlloc in libc++'s
`support/win32/thread_win32.cpp`). The class comment (76-80) says only
`__thread_local_data()` may construct it, and it is a friend for that reason.
`__thread_local_data()` is defined in the DLL (`thread.cpp`). So the only
instantiation whose address is registered is the DLL's. The `delete` in it
calls the DLL's sized `operator delete(void*, size_t)`, bound by 1.1(b).

Result: every `std::thread` that starts has its 8-byte `__thread_struct`
allocated by mimalloc and freed with UCRT `free`, on the exiting thread. UCRT
passes it to `HeapFree` on the process heap. The address belongs to no heap
segment, and the heap's termination-on-corruption policy raises 0xC0000374
**[W3]**. This is deterministic: one wrong free per started thread, with no
race and no use-after-free. That matches the report ("any std::thread", "at
thread start or join"; the exiting thread faults while the joiner waits). The
math call in the repro has nothing to do with it. stx_auxlib's own `threads.cpp`
(line 258) and `thread_pool.cpp` (211) construct `std::thread`, so every
stx_auxlib thread hits it too.

The hypothesis in the handoff is **confirmed** in direction exe-new →
DLL-delete. The reverse direction does not occur for `std::thread`.

### 1.3 The defect is not thread-specific (derived; [W5] confirms)

`basic_string<char>` is an extern template whose out-of-line members are
compiled in the DLL (`__string/extern_template_lists.h` 31-81): `__init`,
`__grow_by`, `reserve`, `push_back`, `append`, `assign`, `~basic_string` and
others. Their allocations use DLL new (UCRT). `~basic_string` is declared
`inline` (`string` 1210), and its callee `__reset_internal_buffer` is
`_LIBCPP_HIDE_FROM_ABI`. Under optimisation, clang may emit and inline an
`available_externally` copy in the exe. That copy frees through the exe's
operator delete, which is `mi_free`. At -O0 the DLL's copy runs instead. So
whether a long string is freed by the right allocator depends on inlining
decisions. That is the reverse mismatch: `mi_free` of a UCRT block. In
mimalloc's ordinary build this is undefined behaviour: `mi_ptr_page_is_valid_ex`
(free.c 192-248) uses the unchecked or aligned page lookup (`_mi_ptr_page`,
internal.h 830-841) and reads metadata at an address mimalloc never mapped. In
the secure build (`MI_SECURE`) the checked lookup returns null, and the block is
silently leaked. So the threads crash is the first deterministic symptom of a
general boundary defect, not the whole defect.

### 1.4 Ruled out

- **mimalloc's Windows override or redirect.** `MI_MALLOC_OVERRIDE` is not
  defined (CMakeLists 139-143), so `alloc-override.c` (line 13 guard) compiles
  nothing. `_mi_is_redirected()` is the generic `false` (prim/prim.c 65). No
  `mimalloc-redirect.dll` is involved, and UCRT's malloc and free are untouched.
- **mimalloc's thread hooks.** `MI_WIN_INIT_USE_CRT_TLS` is the default
  (prim/windows/prim.c 785-791). On `__MINGW32__` it places `mi_tls_attach` and
  `mi_tls_detach` in `.CRT$XLB` and `.CRT$XLY` (902-907). They manage only
  mimalloc's own per-thread heap. The faulting free never reaches mimalloc; it
  goes to UCRT.
- **The faulting free is not a use-after-free.** Block 1 is live until step 5.
  The fault is the wrong allocator, not a stale pointer.

### 1.5 Whose fault

It is a mismatched free, not a use-after-free. libc++ behaves as specified for
a DLL runtime. The crossing exists because we replaced operator new in one
module, the executable, of a program whose C++ runtime is another module. That
was our design's precondition (`memory.hpp`, "One allocator per process"), and
our CMake admitted the configuration: `stx_auxlib_global_allocator` is offered
on WIN32 with no check of how the C++ runtime is linked, and stx_auxlib's
standalone MinGW build links libc++ dynamically.

The Windows side measured the same thing in general (hot_paths.md,
"Experiments"). With a libc++ DLL, *any* per-module replacement crosses:
std::thread and extern-template std::string trap, and std::async and
ostringstream cross in each direction. With static libc++ every case balances
(3002 of 3002 blocks for strings). The answer is Part 2's model, not a better
replacement.

## Part 2. The ownership-boundary model (owner, 2026-11-02)

### 2.1 Principle

- GUI.Forms' memory never crosses into foreign freeing, and we never free
  foreign memory.
- Foreign modules keep their own allocators; our allocator serves only our
  memory.
- We do not replace global operator new and delete, or malloc, for a process.
  That would force our allocator on code that is not ours, including the
  foreign code compiled into our modules.
- Whole-process redirection is off the table: mimalloc-redirect, interposition,
  malloc-zone takeover, Detours and import patching.
- Where a foreign interface takes ownership of a block, the block comes from the
  allocator that interface's contract names.

A boundary is safe by construction when, for every block that crosses it, the
allocating side and the freeing side are the same allocator. That holds when:

- (a) the foreign side allocates and frees through a hook we give it, so both
  calls are ours; or
- (b) ownership never crosses: we copy in or out, the foreign side keeps what it
  allocated and frees it through its own release call, or it only borrows; or
- (c) a block crosses with ownership, and both its allocation and its release
  are the allocator the foreign contract names.

### 2.2 Inventory of foreign boundaries

GUI.Forms was audited at main e22ea9c9 (the Windows side, hot_paths.md "Boundary
audit") and re-checked here. stx_auxlib was audited at 308c265.

| Boundary | Class | Allocates | Releases | Hook available (declaration) |
|---|---|---|---|---|
| FreeType (Skia's, `skia_executor`, `glyph_run_raster`, `harfbuzz_font_engine`, `paint_probe`, `prepared_raster`, `text_mask_fonts`) | (b) | FreeType, through the default `FT_Memory` (C library malloc) that `FT_Init_FreeType` installs | `FT_Done_Face`, `FT_Done_FreeType`. `FT_New_Memory_Face` borrows our bytes and never frees them | (a) if wanted: `FT_New_Library(FT_Memory, FT_Library*)` and `FT_Add_Default_Modules` (`ftmodapi.h` 590, 719), with `struct FT_MemoryRec_ { void* user; FT_Alloc_Func alloc; FT_Free_Func free; FT_Realloc_Func realloc; }` (`ftsystem.h` 169-175) |
| HarfBuzz (`hb_buffer_create`, `hb_font_create`, `hb_ft_font_create_referenced`) | (b) | HarfBuzz, through `hb_malloc_impl` and the others, which default to malloc (`src/hb.hh` 555-558) | `hb_buffer_destroy`, `hb_font_destroy`. We pass no `hb_blob_create` destroy callback, so HarfBuzz never frees our memory | (a) at compile time: `HB_CUSTOM_MALLOC` with `hb_malloc_impl`, `hb_calloc_impl`, `hb_realloc_impl` and `hb_free_impl` (`hb.hh` 542-553). For blobs: `hb_blob_create(data, length, mode, user_data, hb_destroy_func_t destroy)` (`hb-blob.h` 87-91; `hb_destroy_func_t` in `hb-common.h` 397) hands a release back to us, so the destroy callback must be our release |
| SheenBidi | (b) | `SBAlgorithmCreate` and `SBAlgorithmCreateParagraph` (malloc, `Source/Object.c` 48) | `SBParagraphRelease`, `SBAlgorithmRelease` (free, `Object.c` 143, 155). The input sequence is borrowed | none |
| libunibreak | (b) | none: it writes into caller storage | n/a | n/a |
| md4c | (b) | internal malloc/realloc | internal free. Callbacks receive borrowed text | none |
| miniaudio (`src/audio/audio.cpp`) | (b) | miniaudio, through its default callbacks | `ma_*_uninit`. `pUserData` borrows our state | (a) if wanted: `ma_allocation_callbacks { onMalloc, onRealloc, onFree, pUserData }` (`miniaudio.h` 4375-4378), carried in each config's `allocationCallbacks` |
| stx_skia | ours | `sk_malloc`, `sk_realloc`, `sk_free` (`SkMemory_malloc.cpp`, C library), and Skia's own new/delete pairs | same module | (a) by edit: it is our port, so `SkMemory_malloc.cpp` is the one place to route |
| stx_zlib, stx_png, stx_conv, stx_rtf2md | ours | standard containers only (no malloc or new expressions) | same | (a) through the container decision in 2.4 |
| stx_vorbis stb-compatible decode | (c), inside our modules | realloc'd sample buffer | caller, with `std::free`, by contract | flagged: if this is ever exported, the contract names the C library allocator |
| Win32: `SHBrowseForFolderW` | (c) | shell (COM task allocator) | `CoTaskMemFree` (`windows_host.cpp` 302) | n/a |
| Win32 clipboard | (c) | `GlobalAlloc(GMEM_MOVEABLE)` | Windows after `SetClipboardData` succeeds; `GlobalFree` on our failure path | n/a |
| UI Automation | (c) | `SysAllocString`/`SysAllocStringLen`, `SafeArrayCreateVector` | the UIA client (`SysFreeString`, `SafeArrayDestroy`); `SafeArrayDestroy` on our failure paths | n/a |
| GDI and shell handles | (c) | GDI, shell | `DeleteObject`, `DeleteDC`, `ReleaseDC`, `DestroyIcon` | n/a |
| Sealed font pages | ours | `VirtualAlloc` | `VirtualFree` | n/a |
| stx_auxlib Windows: process handles, `BCryptGenRandom`, environment, command-line quoting | (b)/(c) | caller buffers; kernel handles | `CloseHandle`. `BCryptGenRandom` fills our buffer. `CommandLineToArgvW` is only cited in comments as the parse we quote for | n/a |
| macOS CoreFoundation and CoreGraphics (`src/host/macos`) | (c) | CF/CG create and copy functions | `CFRelease`, `CGImageRelease`, `CGDataProviderRelease`, `CGContextRelease`, `CGColorSpaceRelease` (Create/Copy rule). The data providers either copy our bytes into a `CFData` (`appkit_clipboard_image.mm` 74, `coregraphics_raster.cpp` 344 and 421), or borrow them with no release callback (`macos_host.mm` 3027, over the window raster). The borrow is (b): CoreGraphics never frees our pixels. The raster must outlive the image, which is a lifetime check for that site, not an allocator one | n/a |
| X11 (`src/host/linux`) | (c) | Xlib (`XGetWindowProperty`, `XQueryTree`) | `XFree`, `XFreeGC`, `XFreePixmap`, `XFreeColormap` | n/a |
| X11 `XCreateImage` pixel buffer (`x11_tray_icons.cpp` 172-178) | (c) | **we** `std::malloc` the buffer, because `XDestroyImage` frees it with `free()` | Xlib (`XDestroyImage`); `std::free` on our failure path | n/a: the contract names the C library allocator, so this block must never come from ours |
| AT-SPI / GLib (`linux_accessibility.cpp`) | (c) | GLib | `g_free` | n/a |
| stx_auxlib POSIX: `realpath` | (b) | none: it writes into our `PATH_MAX` buffer (`desktop_posix.cpp` 121) | n/a | n/a |
| stx_auxlib POSIX: `getenv`, `environ` | (b) | the C library | never freed by us (borrowed) | n/a |
| stx_auxlib POSIX: `posix_spawn` attributes and file actions | (b) | the C library | `posix_spawnattr_destroy`, `posix_spawn_file_actions_destroy` | n/a |
| libc++ | see 2.3 | | | |

**Result:**

- **No boundary frees memory we allocated, and we free no foreign memory.** Only
  the X11 image buffer and the Win32 and UIA hand-offs pass ownership across
  with a block, and each uses the allocator its contract names. That makes them
  class (c), safe by construction. The other boundaries are (b).
- **None is (a) today.** Hooks exist for FreeType, HarfBuzz (allocation and blob
  destroy) and miniaudio, so they *can* take our allocator if the owner wants
  their memory counted as ours. They are not needed for safety.
- **Still to verify:** the borrowed window raster behind the `CGDataProvider`
  at `macos_host.mm` 3027 must outlive the `CGImage` built on it.

### 2.3 libc++: static in every module we build

A libc++ DLL keeps its own operator new and delete (1.1(b)), and its
extern-template and thread code splits allocation and release across the module
boundary (1.2, 1.3). So every module we build links libc++, libc++abi and
libunwind statically.

- **GUI.Forms already does this:** `add_link_options(-static)` (hot_paths.md,
  "Boundary audit").
- **stx_auxlib's standalone MinGW build does not.** It is the repro's setup.
  CMake change: on `MINGW`, stx_auxlib adds the static C++ runtime link options
  to its executables and stops configuration with FATAL_ERROR if a shared C++
  runtime is requested (`CMAKE_CXX_FLAGS` or `CMAKE_EXE_LINKER_FLAGS` naming the
  shared runtime, or `BUILD_SHARED_LIBS` for stx_auxlib itself).
- **Our own DLLs** (gui_forms_threading, gui_forms_c_api and the other C APIs)
  each hold their own static runtime copy. A block passed between them is safe
  only while every copy allocates from the same C library heap (UCRT). That
  holds today because operator new is not replaced in any of them, and it is the
  reason no module may install a replacement.

### 2.4 Open question for the owner: how our own containers and new-expressions reach stx_auxlib's allocator

Without a global replacement, `std::vector`, `std::string`, `std::make_unique`
and plain `new` in GUI.Forms use the toolchain's operator new. That is the C
library heap, shared safely by all our modules. Reaching stx_auxlib's allocator
needs one of the following.

1. **Allocator-aware containers with a stateless stx_auxlib allocator**, for
   example `stx_auxlib::memory::Allocator<T>` over `memory::allocate` and
   `release`, with aliases such as `stx_auxlib::Vector<T>`.
   - Cost: the type differs from `std::vector<T>` and `std::string`, so
     interfaces that take standard types copy at the edge, and the aliases
     spread through headers.
   - No runtime cost and no per-container state.
   - Ownership is decided at compile time: only our release frees our blocks,
     even across our own DLLs, if `memory::release` is exported from one module.
2. **`std::pmr`**: `polymorphic_allocator` over stx_auxlib `memory_resource`s
   (per-subsystem arenas).
   - Cost: a virtual call per allocation, a resource pointer in every container,
     and allocator non-propagation on copy-assign, which copies between
     resources.
   - Runtime dispatch is admitted only where the variation is real, which is
     arenas (house style §7).
   - libc++'s own default resource stays new/delete.
3. **Class-level `operator new` and `operator delete`** on our retained object
   families (controls, nodes).
   - Cheap, inherited, and paired by construction: a virtual destructor selects
     the class's operator delete.
   - Covers objects, not their containers' storage.
4. **Named allocation calls only**: `memory::allocate` into explicit owners
   (heap arrays, arenas, pools) for substantial or runtime-sized buffers. Other
   containers stay on the toolchain's new.
   - Cheapest, and already the house style's "heap arrays for explicit buffer
     ownership".
   - The allocator then sees only the buffers that matter for determinism and
     inspection, not every node.

**Recommendation:** 4 now. 1 for the containers on measured hot or
determinism-critical paths. 3 where a retained family is numerous. 2 only where
a subsystem needs runtime-chosen arenas. All four keep the principle, because
only our code allocates from our allocator and only our code releases to it.
The owner decides how far "every GUI.Forms allocation" should reach.

### 2.5 What `stx_auxlib_global_allocator` becomes

Today it is a static library defining every replaceable operator new and delete
over the backend. It is linked by every stx_auxlib test executable.

- **Option A, remove it** (recommended). A whole-program replacement routes
  foreign code compiled into the program (Skia, HarfBuzz, FreeType through C++
  wrappers) through our allocator. That contradicts "foreign modules keep their
  own allocators". On Windows it crosses whenever any module, ours included,
  has its own runtime copy. Then:
  - the tests link only `stx_auxlib`;
  - `memory_tests` exercises `memory::allocate` and `release` directly;
  - `memory.hpp`'s "One allocator per process" paragraph is replaced by the
    principle in 2.1.
- **Option B, keep it as an opt-in** for a whole program that chooses it:
  single-module, static runtime, never a library and never GUI.Forms. CMake
  would refuse it for shared libraries and for MinGW with a shared runtime. This
  keeps a feature that the model says no GUI.Forms consumer uses.

### 2.6 Verification by invariant

- **Tagging check mode** (debug and sanitizer builds; implemented as below; designed in the
  report to the coordinator, to be approved before implementation). Every block
  from `memory::allocate` carries a header tag. Our `release` of an untagged
  block aborts with a message, and a double release finds the tag cleared and
  aborts. A foreign free of our block traps deterministically, because the
  address handed out is not an allocation start for any foreign allocator.
  Implemented as `STX_AUXLIB_ALLOCATION_TAGS` (`AUTO`: Debug and sanitizer
  builds): a 16-byte header below the returned address holds the offset to
  the backend block and the address XOR a per-process secret whose low four
  bits are set. `release` checks the tag, clears it and frees the backend
  block; under AddressSanitizer it first asks whether the header is
  poisoned (a foreign red zone or a quarantined block), so the failure is
  ours rather than the sanitizer's. Release builds carry no header.
- **Must-trap death tests**, run as child processes by a parent test that
  checks the abnormal termination:
  1. `std::free` of a block from `memory::allocate` must trap.
  2. `memory::release` of a `std::malloc` block must abort with our message.
- **Per-boundary invariants:**
  - (a): a hooked library's allocations are counted by our allocator, and its
    releases match them, with a balance of zero after its `Done` or `destroy`
    call.
  - (b): our allocator's live count is unchanged across the foreign call
    sequence. Nothing of ours was retained or freed by it.
  - (c): the release call named in 2.2 is the only release for that block. This
    is checked by review per call site; the X11 image buffer and the UIA
    `SafeArray`/`BSTR` paths are the cases with a block.
- **Exit balance** in test executables: blocks allocated minus blocks released
  equals the documented retentions.

## Part 3. Windows-only facts (MSYS2 CLANG64 shell; LLVM 22.1.8)

Measured on Windows 11 x86-64, MSYS2 CLANG64 LLVM 22.1.8 (records in the
private projects repository, `tracks/gui_forms/hot_paths.md`, 2026-11-02). The
repro is the handoff repro: an executable with an executable-only replacement
of operator new and delete, starting and joining a std::thread.

| # | Fact | Command | Result |
|---|---|---|---|
| W1 | The dynamic-runtime repro imports no operator new or delete from libc++.dll | `llvm-readobj --coff-imports`; `grep -E '_Znw\|_Zdl'` over the link map | **Confirmed.** It imports `__thread_struct`'s constructor and destructor, `__thread_local_data`, `__libcpp_tls_set`, `__libcpp_thread_create`, `thread::join` and the exception ABI, and no `_Znw`/`_Zdl`; the map has no operator new or delete from libc++.dll. (It does not import `__libcpp_tls_create`.) |
| W2 | libc++.dll exports `_Znwy`/`_ZdlPv*`, imports its heap from the UCRT, and imports nothing from the executable | `llvm-readobj --coff-exports`/`--coff-imports` on `clang64/bin/libc++.dll` | **Confirmed.** Every `_Znw*`, `_Zna*`, `_Zdl*` and `_Zda*` form is exported; the imports are KERNEL32 and `api-ms-win-crt-*` (heap among them) only. |
| W3 | The faulting free is the UCRT's, called from libc++.dll's `__at_thread_exit`, and raises 0xC0000374 | a debugger's backtrace at the stop | **Fault code measured** (0xC0000374 at thread exit). The backtrace is pending: no debugger is installed on the Windows host (no gdb, lldb or cdb). |
| W4 | `__at_thread_exit` calls its own sized operator delete | `llvm-objdump -dr --disassemble-symbols=…__at_thread_exit… clang64/lib/libc++.a` | **Confirmed.** Its relocations name `__thread_struct_imp`'s destructor and `_ZdlPvy` (sized delete) twice. |
| W5 | Extern-template string storage crosses the boundary | `crossing.cpp` | **Measured**: TRAP with the DLL, balanced when static. |
| W6 | With the static runtime the executable imports no libc++.dll or libunwind.dll | `llvm-readobj --coff-imports` after a static link | **Confirmed.** The imports are KERNEL32, ADVAPI32 and `api-ms-win-crt-*` only. |
| W7 | With the static runtime, operator new and delete have one definition per module | `-Wl,-Map`; operator new and delete in the map | **Confirmed.** Every reference to `operator new(unsigned long long)` resolves to one address, the replacement's object; libc++.a's own definitions are not linked. |
| W8 | CLANG64 has no static UCRT | `llvm-nm clang64/lib/libucrt.a` | **Confirmed.** `libucrt.a` defines `malloc` only beside `__imp_malloc`: it is an import library for the UCRT DLL (likewise `libucrtbase.a`). |
| W9-W11 | UCRT `_*_base` exports, ucrtbase back-binding, thread-exit order | (see the earlier draft) | Not needed under Part 2. |

MSVC (Visual Studio 2026, MSVC 14.51.36231, `cl /O1 /EHsc /std:c++20`), the
same `crossing.cpp` with the same executable-only tagged replacement:

| # | Fact | Result |
|---|---|---|
| M1 | The MSVC STL with `/MD` (msvcp140.dll) crosses no allocator boundary in the string, stream, facet, async, promise, thread and exception cases | **Confirmed.** No trap; no foreign block released by the replacement; balanced. |
| M2 | The same with `/MT` | **Confirmed.** No trap and no foreign free; 4 (stream) and 2 (facet) of the replacement's blocks are still held by the static CRT's locale objects when the report runs. |
| M3 | Our own DLL boundary crosses when one module replaces operator new, on every toolchain | **Confirmed** on MSVC `/MD`, MSVC `/MT` and MinGW static libc++: a block allocated by the executable and freed by the DLL traps (0xC0000374); a block allocated by the DLL and freed by the executable is a foreign block released by the replacement. |

So the MinGW shared-libc++ crossing has no MSVC counterpart, and M3 is why no
module of ours may replace operator new (2.3).

## Part 4. Decisions

Decided (owner, 2026-11-02):

1. **The ownership-boundary model** (Part 2.1). No process-wide replacement of
   operator new/delete or malloc on any platform. The macOS default-zone design
   is withdrawn.
2. **Static libc++ in every module we build**, enforced in stx_auxlib's CMake
   on MinGW (2.3). The Windows side owns Windows execution and facts W1-W8.
3. **Tagging check mode and must-trap death tests** (2.6), tested on the M4 for
   macOS. Windows execution is theirs.
4. **No whole-process redirection.** mimalloc-redirect.dll (prebuilt) and
   import patching are not adopted.

Decided (owner and coordinator, 2026-11-02):

5. How our containers and new-expressions reach stx_auxlib's allocator (2.4).
   Recommended: named calls now; allocator-aware containers on measured paths;
   class-level operators for numerous retained families; pmr only for arenas.
   **Decided: named calls now; allocator-aware containers only on measured
   paths.**
6. `stx_auxlib_global_allocator`: remove (recommended) or keep as an opt-in for
   whole single-module programs (2.5).
   **Decided: removed.**

Open, for the owner:

7. Whether FreeType, HarfBuzz and miniaudio should take our allocator through
   their hooks (2.2), for inspection only. They are already safe without it.
8. The MSVC ABI with a static CRT (clang-cl `/MT`) is not needed under this
   model. It is recorded as an option only.
