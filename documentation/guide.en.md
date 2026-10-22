# User guide

[Project home](../README.md) · [中文](guide.zh-CN.md)

A desktop toolbox for editing Live2D Cubism runtime models, recovering source artwork, and exporting PSD, CMO3 and runtime model packages. It combines a model canvas, hierarchy and parameter tools, mesh/deformer editing, animation curves, physics preview and source/texture management.

The project edits information retained in `.moc3` and supported companion files. It is **not a complete replacement for Cubism Editor**: a runtime model cannot recover source pixels, names or authoring metadata that were discarded during export. Supplying the original PSD or CMO3 allows substantially more complete artwork recovery.

### Getting started

1. Open a `.model3.json`, or open a `.moc3` directly and select the accompanying textures/resources as needed. Drag and drop is supported.
2. Use the model tree, canvas and Inspector to select objects; adjust parameters or switch to the animation workspace for motion editing.
3. Save a **work session** to retain supported edits, source revisions, animation drafts and editor state. Export a **runtime model package** when you need a portable compiled model and its resources. Neither a PSD nor a reconstructed CMO3 substitutes for the work session.
4. Choose the PSD workflow below according to whether you need original artwork or the current model appearance.

### Choose the right PSD workflow

| Available input / intended result | Workflow | What it preserves |
|---|---|---|
| A retained original PSD | **Recover PSD from saved sources…** (`从已保存来源恢复 PSD…`) → **Save original PSD as…** (`原样另存 PSD…`) | The validated original file bytes, including its masks, effects, profiles and metadata. Current model edits are not written into this copy. |
| A CMO3 containing source artwork | **Recover PSD from project…** (`从工程恢复 PSD…`), or recover an archived CMO3 source | Embedded source images and the document/layer information actually retained in the project. Separate source canvases remain separate PSDs. |
| A runtime model and textures, or the edited current model | **Export PSD** (`输出psd`, `Ctrl+M`) | Layers reconstructed from runtime drawables, their available hierarchy/names and the chosen rendering or texture-sampling mode. This is not the original PSD. |

**For maximum artwork completeness, prefer source recovery.** These are currently separate entry points; the ordinary PSD export dialog does not automatically switch to the retained original.

### Workspace and model inspection

- Light/dark themes, persistent dock layouts, recent models, drag-and-drop loading and unsaved-change protection.
- Searchable Part / ArtMesh / Deformer tree with multi-selection, visibility, solo and object locks; linked selection between the tree, canvas, Inspector and texture view.
- Parameter sliders, numeric entry, defaults, clipboard/presets and undo/redo. Parameter names, ranges, defaults, decimal precision and repeat settings are editable.
- Motion, expression and Pose preview; automatic eye-blink, breathing, physics and mouse-follow controls, plus WAV lip-sync playback controls.
- Inspector editing for visibility, opacity, draw order, clipping relationships/inverted masks and supported keyform/deformer properties. Preview, picking and exports use the edited document state.
- Diagnostics locate unambiguous affected objects and export versioned JSON reports. Export results retain per-file errors, warnings and cancellation status.

### Modeling and keyforms

- **Canvas editing:** ArtMesh points, Warp control points and Rotation handles; move, box/lasso selection, axis constraints, transforms, distance falloff and influence painting. Hard selections control topology; soft/painted weights scale movement. Brush strokes have their own undo/redo; painted fields are temporary and are cleared when loading a session.
- **Mesh topology:** insert/delete vertices, connect edges and retriangulate a local selection or whole mesh. Boundary and hole protection is enabled by default. Supported operations migrate UVs, ordinary/BlendShape keyforms and source coordinates together instead of changing only the visible mesh.
- **Automatic mesh:** generate a candidate from artwork transparency with density presets, spacing, alpha threshold and boundary margin. Preview before applying one undo step. Existing UV domains, triangle constraints and Glue endpoints restrict the result; coarse presets need not reduce vertex count, and nonlinear deformation may change after resampling.
- **ArtMesh cloning:** duplicate an existing mesh into an independently editable object, with transactional preview, undo/redo and session restoration. This is different from creating a new model from unassociated PSD layers.
- **Deformers:** create Warp/Rotation deformers from selections sharing one Part and direct parent, or create an empty deformer at a supported root/Part location. Edit their display names without changing IDs, and resize an existing Warp conversion grid across ordinary and BlendShape keyforms. Deletion checks shape-preserving reparenting; unsupported transforms/bindings are rejected. Conversion-grid editing is not Bézier authoring-controller editing.
- **Parameters and binding keys:** create ordinary parameters, bind selected ArtMesh/Warp/Rotation objects, remove unreferenced parameters or freeze supported bound parameters at a chosen value. Edit existing binding keys, including multi-axis batches. Search, single-level folders, ordering, pairing and selection-related filtering organize the parameter workspace.
- **Keyform matrix:** inspect available/missing corners, mark a source, copy to another valid target or interpolate with an adjustable source weight. Adjacent copy/interpolation is also available. ArtMesh transfers positions; Warp/Rotation transfers complete supported keyform fields. Conflicting Rotation reflection flags prevent interpolation.
- **Pairing and symmetry:** configure object correspondence for mirrored copies and synchronized geometry editing. Combined geometry/source-opacity edits are atomic; partner opacity is not implicitly synchronized.
- **BlendShape and Glue:** inspect existing bindings, edit supported ArtMesh BlendShape deltas and constraint curves, and edit existing Glue A/B endpoint weights. Supported non-Glue BlendShape deformer/parent mappings can be edited; unsafe inverse mappings and reflection conflicts remain guarded. The Glue panel does not create arbitrary new seams or reconnect endpoints.
- **Index limits:** topology editing supports up to 65,536 vertices and preserves unsigned indices through 65,535. Larger source vertex arrays remain readable when indices are valid. Dense checks use spatial filtering; repeated edits reuse available storage.

### Animation and physics

- Import/export `motion3.json`, record parameter motion, scrub and preview curves, and edit parameter, model-opacity and Part-opacity tracks.
- Multi-key selection and curve box selection, copy/cut/paste, time/value translation and scaling, frame snapping and grouped undo/redo.
- Linear, stepped and Bézier interpolation; editable Bézier handles with independent, aligned, horizontal and auto-clamped tangent modes.
- Motion events and draggable event markers, global/per-parameter fades, looping and playback speed controls.
- Onion-skin preview with configurable preceding/following frames, spacing and opacity. It is available in the animation/timeline context and may be temporarily disabled during editing/export operations.
- Edit physics groups, inputs, outputs, pendulum vertices, weights, inversion, gravity and wind; preview drafts, inspect the pendulum and input/output signals, use presets, and save physics configuration.
- Bake physics at a fixed timestep with warmup. Choose scene, physics or custom FPS; merge outputs into the current animation, generate an independent animation, or create a **copy of the current scene**. Resulting animations/scenes can be switched and retained in work sessions.
- Optional error-bounded curve optimization reduces baked keys; diagnostics report output-range clipping. Baking has key-count/resource budgets, and conflicting global eye-blink/lip-sync tracks may require an independent destination rather than a merge.

### Textures and source artwork

- Texture viewer with zoom/pan, UV wireframes, click-to-locate and PNG export. Same-size PNG replacement, restoration and undo/redo use loaded byte snapshots, so external file changes do not silently change an export.
- UV editing supports points, boxes, connected islands, pixel coordinates, transforms, axis constraints and snapping. Move meshes between existing atlases while retaining normalized UVs or pixel coordinates; preview/cancel before committing. Supported CMO3 material and PSD texture sampling repeats UVs outside atlas bounds within explicit size budgets; the original texture files remain unchanged.
- Atlas workspace supports creation/naming, canvas resizing with preserved pixel density, region move/copy, 90° rotation and automatic packing of overlapping material groups. UV, PNG and atlas edits share document history; generated atlases persist in sessions/packages. Only unused generated tail slots can be removed.
- Packing copies RGBA pixels and sampling margins while preserving supported source associations. Manual texture replacement detaches affected links. Minified appearance can differ when mipmaps are rebuilt.
- Import RGB 8-bit PSD sources, match their layers against existing atlas pixels, explicitly associate existing ArtMeshes and selectively update from another source revision. Unique layer IDs help match renamed/reordered layers; ambiguous matches require confirmation, and missing layers can retain previous artwork.
- Import retained CMO3 source documents without changing the current model appearance. Saving a source or displaying a material relationship does not by itself prove or apply an exact native binding.
- Work sessions preserve source bytes, revisions and supported associations. Runtime package export can optionally include old/unassociated source artwork; reopening a moved package restores validated links. Damaged or mismatched attachments are reported without making the runtime model unusable.
- **Import names from CMO3…** checks typed IDs and relevant structure before offering display-name changes. It does not change IDs, hierarchy or bindings, and structural compatibility alone is not proof of common provenance.
- PSD raster import supports RAW, RLE, ZIP and ZIP prediction. Color policy and opacity baking are explicit. PSB, other color modes/depths and pixel-equivalent reconstruction of arbitrary masks/effects are not supported by the raster importer; retaining the original PSD bytes is a separate capability.

### Export and recovery details

#### Current model → PSD

- Flat drawable layers or Part-based groups; interleaved Parts may be split into several folders to preserve stacking. These folders reflect the available model hierarchy, not necessarily the original Photoshop tree.
- Current-parameter snapshot export freezes the selected pose instead of advancing automatic effects.
- Rendered export bakes supported Cubism clipping into layer alpha; it does **not** generally recreate native Photoshop clipping relationships.
- Texture sampling samples effective atlases onto the frozen mesh at canvas resolution. It retains mesh shape and supported opacity/blend metadata, but does not bake clipping or multiply/screen colors and does not recover undeformed source slices.
- Cubism-import-compatible output includes an sRGB profile, Unicode layer names and deduplicated names. File-level regression coverage is not a guarantee of full Cubism Editor GUI compatibility.
- Batch export has per-file results, collision protection, cancellation and JSON reports. Atomic saves preserve an existing file on failure. Inherited repeated masks are diagnosed rather than silently deduplicated.

#### CMO3 / retained sources → PSD

- Recover names, folders, positions, stacking, hidden/empty layers, opacity, supported blend keys, clipping flags and layer IDs where the source document contains them.
- Embedded PNG pixels are not resized to reverse model deformation; transparent RGB values are retained. Missing artwork and unsupported/conflicting source metadata are reported rather than invented.
- Reconstructed source PSDs do not claim a precomputed merged preview; Photoshop can rebuild it on save. Missing profiles are reported without assigning an invented profile.
- Original-PSD byte preservation and CMO3 artwork reconstruction are different promises. The latter cannot restore PSD features absent from the CMO3.

#### Current model → CMO3 / runtime package

- **Export cmo3 project** (`Ctrl+E`) generates a CMO3 project from supported retained data: geometry, keyforms, Warp/Rotation deformers, Parts, parameter definitions/bindings, masks, Glue, supported BlendShape MorphTargets, physics and display names.
- Materials are reconstructed from effective runtime atlases, with a generated artwork document and mesh images. This is not the original PSD association or a complete import of the original Editor project.
- Parameter defaults remain unchanged unless the current-values-as-defaults option is selected; work sessions store current values separately.
- CMO3 physics output preserves supported normalization and integer FPS, including `0`. Fractional FPS, non-default forces and X/Y outputs require explicit conversion review. Use `physics3.json` or a runtime package to retain the runtime configuration in full.
- This tool’s current CMO3 export does not retain PSD source revisions/associations or parameter organization/pairing settings; retain the work session and, where supported, packaged source attachments.

### Current limitations

- A `.moc3` alone cannot recover discarded original pixels, semantic names or exact Photoshop layer organization, nor guarantee a true undeformed rest/bind-pose reconstruction.
- Creating new ArtMeshes and Parts from unassociated artwork is **not yet an end-to-end editor workflow**. Existing ArtMesh cloning is available; independent creation is still being integrated and verified.
- Draw Order Group flags can be preserved during CMO3 generation, but full group-authoring/editing semantics are incomplete.
- Core 6 runtime support includes extended blend/offscreen data and rendering paths. **Not all PSD and CMO3 export paths preserve every Core 6 color/alpha/offscreen mode equivalently.** Prefer retained originals when exact source fidelity matters.
- Existing Glue weights and supported BlendShape operations are editable; arbitrary Glue construction, all inverse-deformation cases and the full Cubism authoring/Animator feature set are not implemented.
- File-level tests and selected real-Core/Photoshop checks do not prove universal Photoshop/Cubism Editor round trips, Windows GUI behavior, high-DPI behavior or compatibility with every older macOS release.

### Building

The desktop application uses **qmake**; **CMake** builds libraries and tests. CMake is not the application packaging entry point.

Requirements: C++17; Qt 6 with Core, Gui, Widgets, Network, OpenGL, OpenGLWidgets and Core5Compat; CMake 3.21+ and Qt Test for component tests; MSVC for Windows x64 or Apple Clang/Xcode for macOS ARM64. Linux and Windows x86 are not established release targets.

#### Select a matching Cubism Core

| Library/API combination | Configuration |
|---|---|
| Native SDK 5-r.4.1 / legacy Core API | `LIVE2D_CORE_API_VERSION=5` (default) |
| Native SDK 5-r.5 / Core 6 API | `LIVE2D_CORE_API_VERSION=6` |

Pass the corresponding library through `LIVE2D_CORE_LIBRARY`; its architecture and ABI must match the toolchain. API generation is **not** the SDK package's major version. Core 6 is not a drop-in library swap while leaving the default API 5 setting. Obtain SDKs from the [official download page](https://www.live2d.com/en/sdk/download/native/) and follow their license terms.

#### Windows

Save your work and close the app first: the build script stops running `live2d2psd` processes.

```powershell
.\build.ps1
.\build.ps1 -Configuration Debug
.\build.ps1 -QMakePath "C:\Qt\6.10.0\msvc2022_64\bin\qmake.exe" -MakeTool nmake
```

For a fresh build using an external Core 6 library, set environment variables before invoking the script. When switching Core/API generation, Qt or architecture, clean the previous objects first. If using the script’s `-Clean` option, back up anything needed from `build/` before proceeding:

```powershell
$env:LIVE2D_CORE_API_VERSION = "6"
$env:LIVE2D_CORE_LIBRARY = "C:\path\to\matching-x64-Core-library.lib"
.\build.ps1
```

Output: `softwareEnv/physicsV1/live2d2psd.exe`. **`build.ps1 -Clean` deletes the entire repository `build/` directory**, not just one configuration. Do not keep the only copy of samples, work or verification records there.

#### macOS

Ensure `xcodebuild -version` and `xcrun --sdk macosx --show-sdk-path` work, and finish any Xcode first-launch/license setup yourself before building. Run the following from the repository root, replacing the paths and deployment target for your selected libraries:

```sh
ROOT="$PWD"
QT_ROOT="/path/to/Qt/6.x/macos"
CORE_LIBRARY="/path/to/Core/lib/macos/arm64/libLive2DCubismCore.a"
MACOS_TARGET="15.7"
mkdir -p build/app-core6
(
  set -e
  cd build/app-core6
  "$QT_ROOT/bin/qmake" "$ROOT/App.pro" CONFIG+=release \
    LIVE2D_CORE_API_VERSION=6 \
    "LIVE2D_CORE_LIBRARY=$CORE_LIBRARY" \
    "QMAKE_MACOSX_DEPLOYMENT_TARGET=$MACOS_TARGET"
  make clean
  make all -j2
)
```

The example target `15.7` matches the minimum reported by the tested Core 6 ARM64 library; it is not a promise for every SDK. Choose a target meeting **all** selected Core/Qt requirements. For the legacy library use API `5` and its own minimum target. Do not infer macOS 12/14 support from a build that links a newer-only Core library.

qmake output goes to `softwareEnv/physicsV1/live2d2psd.app`. Although the Makefile above is out-of-source, the project shares object/generated-code directories under source-root `build/RELEASE` or `build/DEBUG`. When changing Core generation, architecture or Qt, run `make clean` with the generated Makefile before rebuilding; do not build those qmake variants concurrently. The project handles the tested Qt ARM-declaration and missing AGL framework compatibility. Newer Xcode SDK/Qt compatibility warnings still need evaluation rather than being treated as a support guarantee.

### Component tests

Use an independent CMake build directory for each Core/toolchain configuration. Many data, panel and stub-Core tests do not need an external Core library; supplying one enables additional real-Core suites.

```sh
(
set -e
QT_ROOT="/path/to/Qt/6.x/macos"
CORE_LIBRARY="/path/to/Core/lib/macos/arm64/libLive2DCubismCore.a"
cmake -S . -B build/tests-core6 \
  "-DCMAKE_PREFIX_PATH=$QT_ROOT" -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release -DLIVE2D_CORE_API_VERSION=6 \
  "-DLIVE2D_CORE_LIBRARY=$CORE_LIBRARY"
cmake --build build/tests-core6 --config Release -j2
unset QT_QPA_PLATFORM
ctest --test-dir build/tests-core6 -C Release --output-on-failure -j1
)
```

Use API `5` with a matching legacy Core in a separate directory. For the subset not requiring the actual Core, use a fresh build directory and explicitly pass `-DLIVE2D_CORE_LIBRARY=`; omission alone may inherit an environment variable or cached library. `LIVE2D_REAL_CORE_EXTRA_MOC` optionally adds a local model to supported real-Core tests; independent mesh-creation tests also accept `LIVE2D_CREATE_MAO_MOC` / `LIVE2D_CREATE_REN_MOC` when present in that checkout. The Core 6 renderer regression also accepts `LIVE2D_REAL_CORE_EXTRA_TEXTURE` for the matching texture. Obtain extra samples separately under their licenses; inspect detailed output for skipped cases rather than treating every CTest “Passed” as a rendered sample check.

**Do not globally force `QT_QPA_PLATFORM=offscreen` for the entire suite.** Some tests need a real OpenGL context; on macOS they use the native platform backend. Panel-only tests configure offscreen mode individually. Tests, application startup, actual GUI editing and Photoshop/Cubism Editor readback are separate verification scopes. No repository CI currently replaces these local checks.

### Project layout and dependencies

| Directory | Responsibility |
|---|---|
| `ui/` | Workspaces, panels, dialogs and document/runtime coordination |
| `live2dstruct/` | MOC parsing/rewriting, model document, transactions, sessions and animation data |
| `live2d/`, `framework/` | Application runtime and modified Cubism Native Framework |
| `opengl/` | Rendering, texture/atlas operations and PSD export |
| `edit/` | Canvas mesh/deformer interaction and editor state |
| `psd/`, `cmo3/` | PSD I/O and Cubism project reading/generation |
| `std/`, `tests/` | Shared utilities and regression suites |

Dependencies include [Live2D Cubism SDK](https://www.live2d.com/en/sdk/about/), Qt 6, OpenCV, [psd_sdk](https://molecular-matters.com/products_psd_sdk.html), miniz and stb_image. The framework contains project-specific renderer/export changes; SDK upgrades require a manual compatibility review rather than replacing the directory wholesale.

### License

This project's code is released under [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) for personal research and study. Third-party dependencies retain their own licenses. Use of the modified Cubism Framework remains subject to the [Live2D Open Software license](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html).
