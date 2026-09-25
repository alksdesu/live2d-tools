# live2d2psd

[English](#english) | [中文](#中文)

---

<a id="english"></a>

## English

A Live2D Cubism model conversion toolbox: convert runtime models (`.moc3`) to Adobe Photoshop (PSD) files, or restore them to editable Cubism Editor projects (`.cmo3`).

PSD export renders every drawable (art mesh) individually into its own layer, preserving the original Part group hierarchy and names. cmo3 generation restores all geometry, deformers, parameters and physics from the moc3 binary — for cases where the source project is lost and only runtime files remain.

### Features

#### Model Viewer

- Open `.model3.json` or `.moc3` files (drag & drop or file dialog; opening a moc3 directly auto-generates the config)
- Live preview with sliders for all model parameters
- Play motions, switch expressions, toggle auto eye-blink / breathing, Pose switching
- Model structure tree (Part / ArtMesh / Deformer hierarchy) with click-to-highlight
- Searchable multi-select structure tree with visibility controls and a Cubism-style Inspector/diagnostics view
- Inspector edits for ArtMesh visibility and opacity are applied to preview, PSD and cmo3 export
- Inspector draw-order edits use drawable order values, with native render order breaking ties; preview, picking and PSD share the same ordering, and overrides are saved to cmo3/sessions. Editing models with Draw Order Groups remains incomplete.
- Searchable and collapsible parameter groups with undo/redo history
- Edit parameter names, ranges, defaults, decimal precision and repeat settings; range changes and the current base value share one undo step and session restore path
- Versioned editor sessions (parameters, visibility and camera) with recent-model shortcuts and persistent workspace layout/theme
- Texture atlas viewer: zoom/pan, UV wireframe overlay, click the atlas to locate an ArtMesh (three-way linkage with the tree and main view), PNG export

#### PSD Export

- Two layer modes:
  - **All layers** — every drawable flattened in render order
  - **Grouped** — contiguous layers follow the Part hierarchy; interleaved Parts are split into multiple folders to preserve layer stacking
- **Parameter snapshot export** — export the pose currently set via sliders, with automatic effects disabled
- **Texture extraction mode** — extract original textures without deformation, for high-quality asset ripping
- **Cubism-importable mode** — embeds an sRGB ICC profile, Unicode layer names (luni) and deduplicated layer names, so the exported PSD can be imported back into Cubism Editor
- Batch export: pick a folder, all models are converted automatically; failed files are listed individually
- Atomic PSD saves preserve existing files on failure; generated layer pixels, Unicode names and ICC data have independent binary regression tests

#### cmo3 Project Recovery (moc3 → cmo3)

The "Export cmo3 project" toolbar action (Ctrl+E) restores the current model as a Cubism Editor 5.0 project:

- **Recoverable**: mesh geometry (vertices/UVs/triangles), keyform deformations, Warp/Rotation deformers, Part hierarchy, parameter definitions and bindings, clipping masks, blend modes, Glue, physics (physics3.json), display names (cdi3.json)
- **Not recoverable** (absent from moc3): original PSD association, deformer path organization, artist notes and other editor metadata
- Textures are rebuilt in "single PSD document + one layer per mesh" mode (MODEL_IMAGE); artwork can be replaced directly in the Editor and re-exported to moc3
- Export preserves model parameter defaults unless "Use current parameter values as defaults" is selected; sessions store the current values separately

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
psd/          PSD file writer
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

PSD 导出逐个渲染模型中的每个 drawable（art mesh），写入独立图层，保留原始 Part 分组层级结构和名称。cmo3 生成从 moc3 二进制还原全部几何、变形器、参数与物理数据，适用于丢失源工程只剩运行时文件的场景。

### 功能

#### 模型查看

- 打开 `.model3.json` 或 `.moc3` 文件（拖拽或文件对话框；moc3 直开自动生成配置）
- 实时预览模型，通过滑条调整所有参数
- 播放动作、切换表情，支持自动眨眼和呼吸开关，Pose 切换
- 模型结构树查看（Part / ArtMesh / Deformer 层级），点击高亮对应网格
- 结构树支持名称/ID 搜索、多选和可见性控制，Inspector 显示几何、Keyform、绘制顺序、混合/剔除、蒙版等信息，模型检查面板报告结构错误
- Inspector 可直接修改 ArtMesh 可见性和透明度，结果同步到实时预览、PSD 与 cmo3 导出
- Inspector 绘制顺序编辑按 draw order 数值排序，同值沿用 Core 前后关系；预览、选取、PSD 共用排序，覆盖值保存至 cmo3 和工作会话。含 Draw Order Group 模型的编辑语义尚未完整支持。
- 参数分组可折叠，支持名称/ID 搜索、撤销/重做；可保存和载入版本化工作会话（参数、可见性、视图）
- 可编辑参数名称、范围、默认值、小数位和循环设置；范围变化与当前基线值一起撤销，并通过会话恢复
- 支持最近模型、持久化窗口布局，以及浅色/深色工作区主题
- 纹理图集查看：缩放/平移、UV 线框叠加、点击图集反查 ArtMesh（与结构树/主视图三方联动）、导出 PNG

#### PSD 导出

- 两种图层模式：
  - **全部图层** — 按渲染顺序平铺所有 drawable
  - **按分组导出** — 连续图层按 Part 层级分组；前后顺序交错的 Part 拆成多个文件夹，保留图层叠放顺序
- **参数快照导出** — 用当前滑条调整的姿态导出，禁用自动效果
- **纹理提取模式** — 直接提取原始纹理，无变形，适合高质量素材提取
- **Cubism 可导入模式** — 嵌入 sRGB ICC 配置、Unicode 图层名（luni）、图层名去重，保证导出的 PSD 能再导回 Cubism Editor
- 批量导出：选择文件夹，自动遍历所有模型生成 PSD，失败文件逐个列出
- PSD 原子保存，失败时保留已有文件；图层像素、Unicode 名称与 ICC 数据有独立二进制回归测试

#### cmo3 工程恢复（moc3 → cmo3）

工具栏"导出cmo3工程"（Ctrl+E）将当前模型恢复为 Cubism Editor 5.0 工程：

- **可恢复**：网格几何（顶点/UV/三角形）、关键帧变形、Warp/Rotation 变形器、Part 层级、参数定义与绑定、裁剪蒙版、混合模式、Glue、物理设置（physics3.json）、显示名汉化（cdi3.json）
- **不可恢复**（moc3 中不存在）：原始 PSD 关联、变形器路径整理、画师备注等编辑器元数据
- 贴图以"单 PSD 文档 + 每网格一图层"模式重建（MODEL_IMAGE），在 Editor 中可直接换皮再导出 moc3
- 默认保留模型参数的默认值；勾选“将当前参数值作为默认值”可改用当前姿态，工作会话另行保存当前值

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
psd/          PSD 文件写入
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
