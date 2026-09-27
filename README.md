# live2d2psd

[English](#english) | [中文](#中文)

---

<a id="english"></a>

## English

A Live2D Cubism model conversion toolbox: convert runtime models (`.moc3`) to Adobe Photoshop (PSD) files, or restore them to editable Cubism Editor projects (`.cmo3`).

PSD export writes each drawable (art mesh) to its own layer and can group layers using the available Part hierarchy and display names. CMO3 generation reconstructs the model data retained in moc3 and supported companion files when the source project is lost. A runtime model alone cannot recover artwork or authoring metadata that was discarded during export.

### Features

#### Model Viewer

- Open `.model3.json` or `.moc3` files (drag & drop or file dialog; opening a moc3 directly auto-generates the config)
- Live preview with sliders for all model parameters
- Play motions, switch expressions, toggle auto eye-blink / breathing, Pose switching
- Model structure tree (Part / ArtMesh / Deformer hierarchy) with click-to-highlight
- Searchable multi-select structure tree with visibility controls and a Cubism-style Inspector/diagnostics view
- Model diagnostics show the affected object, support locating unambiguous IDs, and export a versioned JSON report with the source identity and document revision.
- Clipping edits retain proven source repetitions and their order. Removing another mask does not silently deduplicate the remaining masks; deduplication is an explicit option. New repetitions, self references and invalid targets are rejected. Edited lists with inherited repetitions use session v14; earlier sessions remain readable.
- Inspector edits for ArtMesh visibility and opacity are applied to preview, PSD and cmo3 export
- Inspector draw-order edits use drawable order values, with native render order breaking ties; preview, picking and PSD share the same ordering, and overrides are saved to cmo3/sessions. Editing models with Draw Order Groups remains incomplete.
- Searchable and collapsible parameter groups with undo/redo history
- Edit parameter names, ranges, defaults, decimal precision and repeat settings; range changes and the current base value share one undo step and session restore path
- Versioned editor sessions (parameters, visibility and camera) with recent-model shortcuts and persistent workspace layout/theme
- Rename Warp and Rotation deformers from the Inspector or structure tree while retaining their IDs, bindings and active keyform. Names support undo/redo, v13 sessions, this tool's CDI extension, runtime packages and CMO3 authoring objects.
- Resize an existing Warp conversion grid across all ordinary and Blend Shape keyforms. Preview the selected control grid and all mesh boundaries, including hidden meshes, at the captured pose before applying one undo step. Resampling can change deformation, especially outside the grid or when reducing divisions; the preview reports displacement in original canvas pixels. This edits conversion divisions, not Bézier authoring controllers.
- ArtMesh and Warp point editing offers move, rectangle, lasso, distance falloff and an influence brush. Hard selections alone control topology operations; soft and painted weights scale movement. Brush strokes have separate undo/redo, while geometric edits use document history.
- Mesh editing, pairing, sessions, previews and exports preserve unsigned 16-bit indices through 65535. Topology structure edits support up to 65536 vertices; larger source vertex arrays remain readable when their indices are valid. Dense mesh checks use spatial filtering, and repeated edits reuse free storage instead of exhausting it through accumulated copies.
- Distance influence can follow mesh edges, stay within connected islands, or cross islands within the object. Brush radius, hardness and per-stroke strength are adjustable; retracing one stroke does not repeatedly accumulate weight. Painted fields survive keyform changes but are temporary and are cleared when loading a session.
- Generate a mesh from artwork transparency for one selected ArtMesh, with three density presets, point spacing, alpha threshold and boundary margin. Inspect the candidate over the artwork before applying it as one undo step; UVs, ordinary and Blend Shape keyforms, and linked artwork coordinates migrate together.
- Automatic generation stays within the existing UV domain and retains its triangle constraints. Coarse presets need not reduce the original vertex count, and nonlinear deformation can change after resampling. Existing Glue endpoints stay pinned; new vertices do not acquire Glue pairs. Linked artwork uses its pinned source revision; an atlas fallback cannot separate other artwork already inside the UV domain.
- Texture atlas viewer: zoom/pan, UV wireframe overlay, click the atlas to locate an ArtMesh (three-way linkage with the tree and main view), PNG export
- UV editor: point, box and connected-island selection, pixel coordinates, transforms, axis constraints and snapping. Assign meshes to existing atlases while preserving normalized UVs or pixel coordinates; preview a draft, cancel it, or save it as one undo step. This changes texture sampling, without repacking PNG pixels.
- UVs and texture assignments persist in sessions and exports. CMO3 materials and extracted PSD textures repeat samples outside atlas bounds, with explicit size limits; original texture files remain unchanged.
- Atlas workspace: create and rename atlases, resize the canvas while preserving pixel density, move or copy material regions across atlases, rotate by 90°, and automatically pack overlapping material groups. PNG replacement, atlas changes, and UV edits share document undo/redo. Sessions preserve generated atlases; unused generated tail slots can be removed.
- Repacking copies RGBA pixels and sampling margins. Existing source atlases stay available, and minified previews may differ after mipmaps are rebuilt.
- PSD sources: import RGB 8-bit PSD layers, align them against existing atlas pixels, explicitly associate ArtMeshes, and selectively update them from another source revision. Unique Photoshop layer IDs support matching across renames and reordering; ambiguous matches require confirmation. Missing layers can keep their previous artwork.
- Source revisions, original bytes, and mesh associations are preserved in versioned work sessions, with earlier sessions still readable. Atlas movement and supported vertex edits migrate associations; manual texture replacement detaches affected links. Reimport samples the frozen source rather than repeatedly resampling edited atlases.
- Runtime package export can optionally include source artwork, including old revisions and unassociated sources. Reopening the moved package in this tool restores source links against the exported model and texture slots. Damaged or mismatched artwork attachments produce a persistent message while the runtime model remains usable; save a work session to retain view settings and object locks.
- PSD import supports raw, RLE, ZIP and ZIP prediction. PSB, other color modes/depths, and pixel-equivalent reconstruction of masks, effects or unsupported blends are not supported. Color policy and layer-opacity baking are explicit. CMO3 preserves the resulting model and textures but currently does not preserve these PSD source revisions or associations.

#### PSD Export

- Two layer modes:
  - **All layers** — every drawable flattened in render order
  - **Grouped** — contiguous layers follow the Part hierarchy; interleaved Parts are split into multiple folders to preserve layer stacking
- **Parameter snapshot export** — export the pose currently set via sliders, with automatic effects disabled
- **Texture sampling mode** — sample the effective atlases onto the frozen model mesh at canvas resolution. Preserves mesh shape, layer opacity and blend mode; does not bake clipping masks or multiply/screen colors. Supports parameter snapshots and does not reconstruct the original PSD artwork.
- **Cubism-importable mode** — embeds an sRGB ICC profile, Unicode layer names (luni) and deduplicated layer names for Cubism import. Binary output is tested. Photoshop 2022 read/save/reopen has been verified for a 30-layer texture-sampling export; this does not establish equivalence for every mask, effect or color profile. Cubism Editor GUI import remains unverified.
- ArtMesh layers retain drawable IDs; Part folders use their own display names. A Part sharing an ArtMesh ID no longer supplies an unrelated name to the texture layer.
- Batch export: pick a folder, all models are converted automatically; failed files are listed individually
- Source mask repetitions produce a detailed warning instead of blocking export. Batch results retain per-file diagnostics, including successful exports with warnings, in the v2 JSON report. Rendered PSD preserves mask multiplicities; texture sampling still does not bake clipping masks.
- Atomic PSD saves preserve existing files on failure; generated layer pixels, Unicode names and ICC data have independent binary regression tests

#### cmo3 Project Recovery (moc3 → cmo3)

The "Export cmo3 project" toolbar action (Ctrl+E) restores the current model as a Cubism Editor 5.0 project:

- **Recoverable**: mesh geometry (vertices/UVs/triangles), keyform deformations, Warp/Rotation deformers, Part hierarchy, parameter definitions and bindings, clipping masks, blend modes, Glue, physics (physics3.json), display names (cdi3.json)
- **Not recoverable** (absent from moc3): original PSD association, deformer path organization, artist notes and other editor metadata
- Textures are rebuilt in "single PSD document + one layer per mesh" mode (MODEL_IMAGE); artwork can be replaced directly in the Editor and re-exported to moc3
- Export preserves model parameter defaults unless "Use current parameter values as defaults" is selected; sessions store the current values separately
- Physics export preserves normalization and integer calculation FPS, including `0`; an unspecified FPS stays unspecified. Fractional FPS, non-default forces and X/Y outputs require an explicit conversion review. Export a physics3 configuration or runtime model package to retain those settings in full.

```
Input:  model.moc3 + textures/*.png [+ physics3.json + cdi3.json]
Output: model.cmo3 (CAFF container: main.xml + one PNG per mesh)
```

### Screenshots

<!-- Place screenshots here -->

### Building

**Requirements:**

- Windows x64 or macOS ARM64
- Qt 6.x (modules: core gui opengl openglwidgets core5compat widgets gui-private network)
- MSVC on Windows; Apple Clang on macOS
- C++17

**Compile:**

```powershell
.\build.ps1                    # Release build
.\build.ps1 -Clean             # Clean rebuild
.\build.ps1 -Configuration Debug
```

If Qt is not in the default location, specify qmake:

```powershell
.\build.ps1 -QMakePath "C:\Qt\6.10.0\msvc2022_64\bin\qmake.exe" -MakeTool nmake -Clean
```

Output: `softwareEnv/physicsV1/live2d2psd.exe`

For an external Core library, pass `LIVE2D_CORE_LIBRARY` to qmake or set the environment variable of the same name. Select a library compatible with `core/Live2DCubismCore.h`. Native SDK 5-r.4.1 has been verified on macOS ARM64; the 5-r.5 Core removes an API used by this framework and is not a drop-in replacement. SDK packages are available from the [official download page](https://www.live2d.com/en/sdk/download/native/).

On macOS, configure from the repository directory using your Qt qmake:

```sh
/path/to/Qt/bin/qmake App.pro CONFIG+=release \
  LIVE2D_CORE_LIBRARY=/path/to/Core/lib/macos/arm64/libLive2DCubismCore.a \
  QMAKE_MACOSX_DEPLOYMENT_TARGET=14.0
make -j4
```

The deployment target must meet the selected Core library's minimum OS requirement; 14.0 matches the tested ARM64 library. The project handles the tested Qt 6.8.3 ARM declaration and missing AGL framework compatibility without editing Qt or the generated Makefile. Output: `softwareEnv/physicsV1/live2d2psd.app`.

### Component tests

CMake builds the data, Qt panel and PSD tests independently of Cubism Core. Set `CMAKE_PREFIX_PATH` to your Qt installation, build with `BUILD_TESTING=ON`, then run `ctest --test-dir <build-directory> --output-on-failure` (add `-C Release` for multi-configuration builds). These tests do not replace full application or Photoshop/Cubism Editor validation. Detailed coverage and remaining work are recorded in [DEVELOPMENT.md](DEVELOPMENT.md).

Adding `-DLIVE2D_CORE_LIBRARY=/absolute/path/to/the/library` enables `real_core_preview`, which loads candidate MOC data through the actual Core library and compares model outputs across poses. Set `LIVE2D_REAL_CORE_EXTRA_MOC` when running that test to include another local model, such as the SDK's Mao sample. The SDK and additional sample are not bundled with these tests.

### Dependencies

| Library | Purpose |
|---|---|
| [Live2D Cubism SDK](https://www.live2d.com/en/sdk/about/) | Model loading, parameter control, rendering |
| Qt 6 | GUI framework |
| [psd_sdk](https://molecular-matters.com/products_psd_sdk.html) | PSD file I/O (extended with luni/ICC support) |
| miniz | ZIP compression for the cmo3 CAFF container |
| stb_image | Image loading |

### Directory Layout

```
ui/           UI layer (main window, panels, dialogs)
opengl/       OpenGL offscreen rendering + PSD export pipeline + texture extraction
live2d/       Live2D SDK application layer
framework/    Live2D Cubism Native Framework (modified)
live2dstruct/ MOC3 binary parser + hierarchy tree builder
psd/          PSD file reader and writer
cmo3/         moc3 → cmo3 project generator (CAFF container + xs XML serialization)
edit/         Editing/drawing tools
std/          Common utilities
```

### Notes

- Code under `framework/` is based on a modified Live2D Cubism Native Framework; `DrawModel()` in `CubismRenderer_OpenGLES2.cpp` is heavily changed to support per-layer PSD export. Merge manually when upgrading the SDK.
- Use of the Live2D Cubism SDK is subject to its [license agreement](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html).

### License

This project's code is released under [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/).

This project is for personal research and study purposes only.

Third-party dependencies remain under their original licenses (Live2D SDK, OpenCV, Qt, etc.).

---

<a id="中文"></a>

## 中文

Live2D Cubism 模型转换工具箱：将运行时模型（`.moc3`）转换为 Adobe Photoshop（PSD）文件，或恢复为 Cubism Editor 可编辑工程（`.cmo3`）。

PSD 导出将每个 drawable（art mesh）写入独立图层，并可按现有 Part 层级与显示名分组。CMO3 生成从 moc3 及支持的配套文件重建仍被保留的模型数据，适用于丢失源工程的场景。运行时模型无法单独找回导出时已经丢弃的原画和编辑器元数据。

### 功能

#### 模型查看

- 打开 `.model3.json` 或 `.moc3` 文件（拖拽或文件对话框；moc3 直开自动生成配置）
- 实时预览模型，通过滑条调整所有参数
- 播放动作、切换表情，支持自动眨眼和呼吸开关，Pose 切换
- 模型结构树查看（Part / ArtMesh / Deformer 层级），点击高亮对应网格
- 结构树支持名称/ID 搜索、多选和可见性控制，Inspector 显示几何、Keyform、绘制顺序、混合/剔除、蒙版等信息，模型检查面板报告结构错误
- 模型检查显示关联对象，支持定位无歧义的 ID，并导出包含来源指纹和文档修订号的版本化 JSON 报告
- 蒙版编辑保留可证明来源的重复次数与当前顺序，删除其它蒙版不会顺带去重；去重需明确勾选。新增重复、自引用和无效目标仍被拒绝。含来源重复的编辑列表使用会话 v14 保存，旧版会话继续可读
- Inspector 可直接修改 ArtMesh 可见性和透明度，结果同步到实时预览、PSD 与 cmo3 导出
- Inspector 绘制顺序编辑按 draw order 数值排序，同值沿用 Core 前后关系；预览、选取、PSD 共用排序，覆盖值保存至 cmo3 和工作会话。含 Draw Order Group 模型的编辑语义尚未完整支持。
- 参数分组可折叠，支持名称/ID 搜索、撤销/重做；可保存和载入版本化工作会话（参数、可见性、视图）
- 可编辑参数名称、范围、默认值、小数位和循环设置；范围变化与当前基线值一起撤销，并通过会话恢复
- 模型结构面板支持为同部件、同直接父级的选区创建 Warp／Rotation；可设置 ID、Warp 行列和插值方式，也可在部件或根级创建空变形器。删除前检查子对象全部关键形的保形转换，成功后支持撤销、会话保存和运行时模型包导出
- 变形器创建／删除的保形转换只接受已验证的变换；不支持的动画父级、非线性转换或绑定会给出原因。部分旋转／反射转换可能产生微小的浮点舍入差异，不保证逐位一致
- 可从属性面板或结构树修改 Warp／Rotation 显示名，保留 ID、绑定与活动关键形。名称支持撤销／重做、会话 v13、本工具的 CDI 扩展、运行时模型包及 CMO3 工程对象
- 已有 Warp 可调整转换分段，全部普通／融合关键形一并重采样。应用前可查看所选控制网格与打开窗口时姿态的全部网格边界（含隐藏网格），位移按原始画布像素显示；减少分段或网格外外推可能改变形状。这里编辑的是转换分段，不是 Bézier 创作控制器
- 支持最近模型、持久化窗口布局，以及浅色/深色工作区主题
- ArtMesh 与 Warp 共用选择／移动、框选、套索、距离柔化和影响笔刷。只有硬选点参与删除、连边等拓扑操作，其余权重只决定移动幅度；笔触有独立撤回／重做，模型形变仍使用文档历史
- 网格编辑、配对、会话、预览和导出完整保留 0–65535 的无符号索引，拓扑结构编辑最多支持 65536 个顶点；更大来源顶点数组在索引合法时仍可读取。稠密网格校验使用空间筛选，连续编辑会复用空闲存储，避免旧数据累积导致过早失败
- 距离柔化可沿网格边、限制在连通区域，或允许同一对象跨岛影响。笔刷可调半径、硬度与每笔强度，同一笔重复经过不会反复叠加。切换关键形保留手绘权重；权重属于临时编辑状态，加载会话会清除
- 选中单个 ArtMesh 后可按素材透明轮廓生成网格，提供低／标准／高密度、点间距、透明度阈值和边缘余量。先在素材上查看候选，应用只增加一条撤销记录；UV、普通与 Blend Shape 关键形、关联素材坐标一起迁移
- 自动生成限定在原 UV 覆盖域内并保留原三角约束，因此低密度不保证减少原有顶点，重采样后的非线性变形外观可能改变。保留已有 Glue 端点，新增顶点不会自动获得接缝配对；关联素材使用固定来源版本，回退到图集时无法分离已混入 UV 覆盖域的其它图案
- 纹理图集查看：缩放/平移、UV 线框叠加、点击图集反查 ArtMesh（与结构树/主视图三方联动）、导出 PNG
- UV 编辑支持点选、框选、连通岛、像素坐标、平移／旋转／缩放、轴约束和吸附；可保持归一化 UV 或像素坐标切换到已有图集。草稿实时预览，可取消，保存只增加一条撤销记录。此操作改变纹理采样区域，不重新排布 PNG 像素
- UV 与纹理槽修改随会话保存并用于导出；CMO3 素材和纹理采样式 PSD 支持越界重复采样，超过素材尺寸预算时明确报错
- 图集编辑支持新建、命名、保持像素密度的画布尺寸调整、跨图集素材搬移、90°旋转和自动排布；重叠素材成组处理，保护未选对象仍使用的像素。PNG 替换、图集变化与 UV 编辑共用文档撤销／重做，生成图集可保存到会话与模型包，可删除未引用的生成尾槽
- 重排复制 RGBA 像素及采样边带；原图集保持可用，重建 mipmap 后的缩小显示可能存在差异
- PSD 来源导入：读取 RGB 8-bit 图层，对照现有图集配准，明确关联 ArtMesh，再从新来源版本选择性更新。唯一 Photoshop 图层 ID 支持改名和调序后的匹配；歧义匹配需要确认，缺失图层可保留旧素材
- 版本化工作会话保存原始来源字节、版本和网格关联，并继续读取旧版会话；图集搬移及支持的顶点编辑迁移关联，手工替换纹理会断开受影响的链接。重新导入从冻结原图采样，避免对已编辑图集反复重采样
- 导出运行时模型包时，可选择包含来源素材，包括旧版本与未关联来源。把包移到新目录后用本工具打开，会按导出的模型和纹理槽恢复来源关联；附件损坏或不匹配时保留明确提示，运行模型仍可使用。对象锁定、视图等编辑状态仍通过工作会话保存
- PSD 导入支持 RAW、RLE、ZIP 和 ZIP prediction；不支持 PSB、其它色彩模式／位深，以及蒙版、效果或不支持混合模式的像素等价重建。颜色策略和图层透明度烘焙需明确选择。CMO3 保留最终模型和贴图，当前不保留这些 PSD 来源版本和关联
- 图集支持同尺寸 PNG 替换、恢复原图和撤销／重做；覆盖随工作会话保存，并用于画布、PNG、PSD、cmo3 与运行时模型包。原素材文件保持不变，共用同一路径的多个纹理槽可独立替换
- 纹理使用首次成功加载的字节快照，外部修改或删除源 PNG 不会造成画布、图集和导出内容分裂；会话重开时核对原始素材摘要，避免把覆盖应用到不同来源

#### PSD 导出

- 两种图层模式：
  - **全部图层** — 按渲染顺序平铺所有 drawable
  - **按分组导出** — 连续图层按 Part 层级分组；前后顺序交错的 Part 拆成多个文件夹，保留图层叠放顺序
- **参数快照导出** — 用当前滑条调整的姿态导出，禁用自动效果
- **纹理采样模式** — 按冻结的模型网格在画布分辨率采样有效图集，保留网格形状、图层透明度与混合模式；不烘焙裁剪蒙版、乘色或屏幕色。支持参数快照，不表示恢复原始 PSD 素材
- **Cubism 可导入模式** — 嵌入 sRGB ICC 配置、Unicode 图层名（luni）并去重图层名，供 Cubism 导入使用；已有二进制输出测试。Photoshop 2022 已验证一个 30 层纹理采样文件的打开、另存与重开，不代表所有蒙版、效果和色彩配置均等价；Cubism Editor GUI 导入仍待验证
- ArtMesh 图层保留 drawable ID，Part 文件夹使用自己的显示名；修复同 ID 的 Part 名称误用于纹理图层的问题
- 批量导出：选择文件夹，自动遍历所有模型生成 PSD，失败文件逐个列出
- 源模型的重复蒙版改为具体提示，不再直接阻止导出；批量结果与 v2 JSON 报告保留每个文件的诊断，包括成功文件的提示。渲染式 PSD 保留蒙版重复次数，纹理采样仍不烘焙裁剪蒙版
- PSD 原子保存，失败时保留已有文件；图层像素、Unicode 名称与 ICC 数据有独立二进制回归测试

#### cmo3 工程恢复（moc3 → cmo3）

工具栏"导出cmo3工程"（Ctrl+E）将当前模型恢复为 Cubism Editor 5.0 工程：

- **可恢复**：网格几何（顶点/UV/三角形）、关键帧变形、Warp/Rotation 变形器、Part 层级、参数定义与绑定、裁剪蒙版、混合模式、Glue、物理设置（physics3.json）、显示名汉化（cdi3.json）
- **不可恢复**（moc3 中不存在）：原始 PSD 关联、变形器路径整理、画师备注等编辑器元数据
- 贴图以"单 PSD 文档 + 每网格一图层"模式重建（MODEL_IMAGE），在 Editor 中可直接换皮再导出 moc3
- 默认保留模型参数的默认值；勾选“将当前参数值作为默认值”可改用当前姿态，工作会话另行保存当前值
- 物理导出保留归一化和整数计算 FPS（包括 `0`），未指定的 FPS 仍保持未指定。小数 FPS、非默认力场及 X/Y 输出需先确认具体转换；完整配置可通过 physics3 或运行时模型包保存。

```
输入: model.moc3 + textures/*.png [+ physics3.json + cdi3.json]
输出: model.cmo3（CAFF 容器：main.xml + 每网格 PNG）
```

### 截图

<!-- 在此放置截图 -->

### 构建

**环境要求：**

- Windows x64 或 macOS ARM64
- Qt 6.x（需要模块：core gui opengl openglwidgets core5compat widgets gui-private network）
- Windows 使用 MSVC；macOS 使用 Apple Clang
- C++17

**编译：**

```powershell
.\build.ps1                    # Release 构建
.\build.ps1 -Clean             # 清理后重新构建
.\build.ps1 -Configuration Debug
```

如果 Qt 不在默认路径，指定 qmake：

```powershell
.\build.ps1 -QMakePath "C:\Qt\6.10.0\msvc2022_64\bin\qmake.exe" -MakeTool nmake -Clean
```

输出位置：`softwareEnv/physicsV1/live2d2psd.exe`

外部 Core 库可通过 qmake 参数 `LIVE2D_CORE_LIBRARY` 或同名环境变量指定，必须与 `core/Live2DCubismCore.h` 的 API 匹配。本机 macOS ARM64 已验证 Native SDK 5-r.4.1；5-r.5 Core 删除了当前框架使用的 API，不能直接替换。SDK 可从[官方下载页](https://www.live2d.com/en/sdk/download/native/)获取。

macOS 在仓库目录使用对应 Qt 的 qmake：

```sh
/path/to/Qt/bin/qmake App.pro CONFIG+=release \
  LIVE2D_CORE_LIBRARY=/path/to/Core/lib/macos/arm64/libLive2DCubismCore.a \
  QMAKE_MACOSX_DEPLOYMENT_TARGET=14.0
make -j4
```

部署目标应满足所选 Core 库的最低系统要求，14.0 对应本轮验证的 ARM64 库。项目已处理本机 Qt 6.8.3 的 ARM 声明和缺失 AGL 框架问题，无需修改 Qt 安装或生成的 Makefile。输出为 `softwareEnv/physicsV1/live2d2psd.app`。

### 组件测试

CMake 可独立构建数据层、Qt 面板和 PSD 测试，不依赖 Cubism Core。将 `CMAKE_PREFIX_PATH` 指向 Qt 安装目录，启用 `BUILD_TESTING=ON` 构建，然后执行 `ctest --test-dir <构建目录> --output-on-failure`（多配置构建补充 `-C Release`）。这些测试不替代完整应用或 Photoshop/Cubism Editor 验证；具体覆盖与剩余工作见 [DEVELOPMENT.md](DEVELOPMENT.md)。

配置时增加 `-DLIVE2D_CORE_LIBRARY=/库的绝对路径` 可启用 `real_core_preview`，使用实际 Core 库加载候选 MOC 并比较多姿态结果。运行该测试时设置 `LIVE2D_REAL_CORE_EXTRA_MOC`，可额外验证本地模型，例如 SDK 的 Mao 样本。测试不附带 SDK 或额外样本文件。

### 依赖

| 库 | 用途 |
|---|---|
| [Live2D Cubism SDK](https://www.live2d.com/en/sdk/about/) | 模型加载、参数控制、渲染 |
| Qt 6 | GUI 框架 |
| [psd_sdk](https://molecular-matters.com/products_psd_sdk.html) | PSD 文件读写（已扩展 luni/ICC 支持） |
| miniz | cmo3 CAFF 容器的 ZIP 压缩 |
| stb_image | 图像加载 |

### 目录说明

```
ui/           UI 层（主窗口、面板、对话框）
opengl/       OpenGL 离屏渲染 + PSD 导出管线 + 纹理提取
live2d/       Live2D SDK 应用层封装
framework/    Live2D Cubism Native Framework（已修改）
live2dstruct/ MOC3 二进制解析器 + 层级树构建
psd/          PSD 文件读取与写入
cmo3/         moc3 → cmo3 工程生成器（CAFF 容器 + xs XML 序列化）
edit/         编辑/绘制工具
std/          通用工具
```

### 注意

- `framework/` 目录下的代码基于 Live2D Cubism Native Framework 修改，`CubismRenderer_OpenGLES2.cpp` 中的 `DrawModel()` 有大幅改动以支持 PSD 逐层导出。升级 SDK 时需要手动合并。
- Live2D Cubism SDK 使用需遵守其[许可协议](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html)。

### 许可证

本项目代码以 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) 许可证发布。

本项目仅用于个人研究和学习目的。

第三方依赖各自遵循其原始许可证（Live2D SDK、OpenCV、Qt 等）。
