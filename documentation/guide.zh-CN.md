# 使用指南

[返回首页](../README.md) · [English](guide.en.md)

用于 Live2D Cubism 运行时模型编辑、来源素材恢复，以及 PSD、CMO3 和运行时模型包导出的桌面工具。包含模型画布、层级和参数工具、网格／变形器编辑、动画曲线、物理预览及来源／纹理管理。

项目处理 `.moc3` 与支持的配套文件中仍然保留的信息，**不是 Cubism Editor 的完整替代品**。运行模型无法找回导出时已丢弃的原始像素、语义名称和创作元数据；提供原始 PSD 或 CMO3 能显著提高素材还原完整度。

### 开始使用

1. 打开 `.model3.json`，或直接打开 `.moc3`，按需指定配套纹理和资源；支持拖拽加载。
2. 在模型树、画布和属性面板中选择对象；调整参数，或切到动画工作区编辑动作。
3. 使用**工作会话**保存已支持的编辑、来源版本、动画草稿和编辑状态。需要可移动的编译模型及资源时，导出**运行时模型包**。PSD 和重建 CMO3 都不能替代工作会话。
4. 根据需要保留“原始素材”还是“当前模型外观”，选择下面的 PSD 工作流。

### 选择合适的 PSD 工作流

| 已有输入／目标 | 使用入口 | 保留内容 |
|---|---|---|
| 已保存的原始 PSD | **从已保存来源恢复 PSD…** → **原样另存 PSD…** | 写出校验后的原文件字节，保留原有蒙版、效果、色彩配置和元数据；当前模型编辑不会写进这份原稿。 |
| 包含来源素材的 CMO3 | **从工程恢复 PSD…**，或恢复已归档的 CMO3 来源 | 工程实际保留的内嵌原图和文档／图层信息；不同来源画布分别保存。 |
| 只有运行模型与纹理，或需要当前编辑结果 | **输出psd**（`Ctrl+M`） | 按运行时 drawable、可用层级／名称及渲染或纹理采样选项重建图层；不等于原始 PSD。 |

**追求素材完整性时，优先使用来源恢复。** 当前这些是独立入口，普通 PSD 导出窗口尚不会自动切换到已保存原稿。

### 工作区与模型检查

- 浅色／深色主题、可保存的停靠布局、最近模型、拖拽加载和未保存修改保护。
- Part／ArtMesh／Deformer 结构树支持搜索、多选、可见性、独显和锁定；结构树、画布、属性与纹理视图选区联动。
- 参数滑条、数值输入、恢复默认、剪贴板／预设和撤销／重做；可编辑名称、范围、默认值、小数位和循环设置。
- 动作、表情与 Pose 预览；自动眨眼、呼吸、物理、鼠标跟随开关，以及 WAV 口型播放控制。
- 属性面板编辑可见性、透明度、绘制顺序、裁剪关系／反向蒙版和支持的关键形／变形器属性；预览、选取与导出使用编辑后的文档状态。
- 模型诊断可定位无歧义对象并导出版本化 JSON；导出记录保留逐文件错误、警告和取消状态。

### 建模与关键形

- **画布操纵**：ArtMesh 顶点、Warp 控制点和 Rotation 手柄；支持移动、框选／套索、轴约束、变换、距离柔化和影响笔刷。硬选点参与拓扑操作，软选与绘制权重只控制移动幅度。笔触有独立撤销／重做；绘制权重是临时状态，加载会话时清除。
- **网格拓扑**：插点、删点、连边、局部或整网格重剖分，默认保护外轮廓与孔洞。已支持的操作同时迁移 UV、普通／BlendShape 关键形和关联素材坐标，不只修改当前可见网格。
- **自动网格**：按素材透明轮廓生成候选，可调密度预设、点间距、透明度阈值和边缘余量，预览后一次提交。原 UV 域、三角约束和既有 Glue 端点会限制结果；低密度不保证减少顶点，重采样后的非线性形变可能改变。
- **克隆 ArtMesh**：从既有网格复制出独立可编辑对象，支持事务预览、撤销／重做和会话恢复；不等于从未关联 PSD 图层新建模型。
- **变形器**：从同一 Part、同一直接父级的选区创建 Warp／Rotation，或在支持的根级／Part 位置创建空变形器；支持修改显示名（不改变 ID），以及跨普通／融合关键形调整 Warp 转换网格。删除会检查子对象保形重挂，不支持的变换／绑定会被拒绝。转换分段不是 Bézier 创作控制器。
- **参数与绑定关键值**：创建普通参数，绑定 ArtMesh／Warp／Rotation 选区；删除无引用参数，或按指定值冻结并删除支持的已绑定参数。支持既有绑定关键值与多轴批量编辑，以及搜索、单层文件夹、排序、配对和仅显示选区关联参数。
- **关键形矩阵**：查看已有／缺失角点，标记来源，复制到其他有效目标或按可调来源权重插值；也提供相邻关键形复制／插值。ArtMesh 转移顶点位置，Warp／Rotation 转移支持的完整关键形字段；Rotation 反射标记冲突时拒绝插值。
- **配对与对称**：明确设置对象对应关系，镜像复制或同步几何编辑。几何与源侧透明度可原子提交，但不会隐式同步对侧透明度。
- **BlendShape 与 Glue**：浏览已有绑定，编辑支持的 ArtMesh 融合增量、限制曲线和既有 Glue A／B 端点权重。已支持的非 Glue 融合变形器／父链可以编辑；不安全的反解和反射冲突仍会拦截。Glue 面板不提供任意新建接缝或重接端点。
- **索引范围**：拓扑编辑最多支持 65,536 个顶点，完整保留到 65,535 的无符号索引；更大来源顶点数组在索引有效时仍可读取。稠密校验采用空间筛选，重复编辑会复用可用存储。

### 动画与物理

- 导入／导出 `motion3.json`，录制参数动作、拖动时间位置、预览曲线，编辑参数、模型透明度与 Part 透明度轨道。
- 多关键点选择、曲线框选、复制／剪切／粘贴、时间与数值平移缩放、帧吸附和成组撤销／重做。
- 线性、阶跃和 Bézier 插值；可编辑 Bézier 手柄，支持独立、对齐、水平和自动钳制切线模式。
- 动作事件、可拖动事件标记、全局／参数淡入淡出、循环与播放速度设置。
- 洋葱皮可设置前后帧数、间隔和透明度；需要动画／时间轴预览上下文，编辑或导出期间可能暂时禁用。
- 编辑物理组、输入输出、摆锤顶点、权重、反转、重力与风力；草稿预览、摆锤与输入输出监视、预设及配置保存。
- 物理烘焙使用固定步长与预热，可选择场景、物理或自定义 FPS；支持合并到当前动画、生成独立动画或创建**当前场景副本**。生成的动画／场景可切换，并保存进工作会话。
- 可选误差限曲线优化减少烘焙关键点，诊断提示输出范围裁剪。烘焙有关键点与资源预算；全局眨眼／口型轨道冲突时，可能必须选择独立动画而不能直接合并。

### 纹理与来源素材

- 图集查看支持缩放／平移、UV 线框、点击定位和 PNG 导出。同尺寸 PNG 替换、恢复与撤销／重做基于已加载字节快照，外部改动文件不会悄悄改变导出内容。
- UV 编辑支持点选、框选、连通岛、像素坐标、变换、轴约束和吸附；可保持归一化 UV 或像素坐标切换到已有图集，先预览／取消再提交。已支持的 CMO3 材质和 PSD 纹理采样可在明确尺寸预算内处理图集外重复 UV，原纹理文件保持不变。
- 图集工作区支持新建／命名、保持像素密度的画布调整、区域移动／复制、90°旋转和重叠素材分组排布。UV、PNG 和图集编辑共用文档历史；生成图集保存在会话／模型包中，只能删除未引用的生成尾槽。
- 排布复制 RGBA 像素和采样边带，并迁移支持的来源关联；手工替换纹理会断开受影响链接。重建 mipmap 后的缩小显示可能不同。
- 导入 RGB 8-bit PSD 来源，对照既有图集匹配图层、明确关联已有 ArtMesh，并从新来源版本选择性更新。唯一图层 ID 帮助匹配改名／调序；歧义需确认，缺失图层可保留旧素材。
- 导入 CMO3 保留的来源文档，不改变当前模型外观；保存来源或展示素材关系，不等于已证明并应用精确的原生绑定。
- 工作会话保存来源字节、版本和支持的关联；模型包可选包含旧版／未关联素材，移动后重开会恢复验证通过的链接。附件损坏或不匹配会提示，不会因此让运行模型无法使用。
- **从 CMO3 导入名称…** 按类型、ID 与相关结构检查候选后提供显示名修改，不改变 ID、层级或绑定；结构兼容本身不是同源证明。
- PSD 栅格导入支持 RAW、RLE、ZIP 和 ZIP prediction，颜色策略与透明度烘焙需明确选择。不支持 PSB、其他色彩模式／位深和任意蒙版／效果的像素等价重建；保留原 PSD 字节是另一项独立能力。

### 导出与恢复细节

#### 当前模型 → PSD

- 可平铺 drawable 或按 Part 分组；交错 Part 可能拆成多个文件夹以保留叠放顺序，不能据此宣称恢复原始 Photoshop 文件夹树。
- 当前参数快照导出会冻结选定姿态，不继续推进自动效果。
- 渲染导出把已支持的 Cubism 裁剪效果烘焙进图层 alpha，通常**不是**重建 Photoshop 原生剪切关系。
- 纹理采样按冻结网格在画布分辨率采样有效图集，保留网格形状与支持的透明度／混合元数据，不烘焙裁剪、乘色和屏幕色，也不恢复未变形原始切片。
- Cubism 导入兼容输出包含 sRGB 配置、Unicode 图层名和重名处理；文件级回归测试不等于完整 Editor GUI 兼容保证。
- 批量导出支持逐文件结果、重名保护、取消和 JSON 报告；原子保存失败时保留已有文件。源模型的重复蒙版会给出诊断，不静默去重。

#### CMO3／已保存来源 → PSD

- 按来源实际保留的信息恢复名称、文件夹、位置、叠放顺序、隐藏／空层、透明度、支持的混合键、剪切标志和图层 ID。
- 不会通过缩放内嵌 PNG 来假装逆向解除模型形变；保留透明像素 RGB，缺失素材与无法处理的来源元数据会明确报告。
- 重建的来源 PSD 不宣称含有预先合成的预览，Photoshop 保存时可重建；缺少色彩配置时提示，不擅自补造。
- 原 PSD 字节保留与 CMO3 素材重建是不同保证；后者无法找回 CMO3 未保留的 PSD 功能。

#### 当前模型 → CMO3／运行时模型包

- **导出cmo3工程**（`Ctrl+E`）从支持的保留数据生成 CMO3 工程：几何、关键形、Warp／Rotation、Part、参数定义／绑定、蒙版、Glue、支持的 BlendShape MorphTarget、物理和显示名。
- 根据有效图集重建材质、生成素材文档与网格图像，不等于恢复原 PSD 关联或完整导入原 Editor 工程。
- 除非选择用当前参数值作为默认值，否则保留参数默认值；工作会话另存当前值。
- CMO3 物理输出保留支持的归一化与整数 FPS（包括 `0`）；小数 FPS、非默认力场及 X/Y 输出需要明确的转换确认。完整运行配置可通过 `physics3.json` 或运行时模型包保存。
- 本工具当前的 CMO3 导出不保留 PSD 来源版本／关联和参数组织／配对设置；请同时保留工作会话及支持的模型包来源附件。

### 当前限制

- 仅凭 `.moc3` 无法恢复已丢弃的原始像素、语义名称或精确 Photoshop 文件夹结构，也不能保证重建真正未变形的 rest/bind pose。
- 从未关联素材独立创建 ArtMesh 和 Part **尚未形成完整编辑器工作流**。现有 ArtMesh 克隆已可用；独立新建仍在接入和验证中。
- CMO3 生成可保留 Draw Order Group 标志，但完整绘制组创作／编辑语义尚未补齐。
- Core 6 已接入扩展混合、offscreen 数据和渲染路径；**并非所有 PSD／CMO3 导出路径都能等价保留每一种 Core 6 颜色、alpha 和离屏模式**。需要精确保留来源时优先使用原稿。
- 可以编辑既有 Glue 权重和支持的 BlendShape 操作，但未实现任意 Glue 创作、全部逆变形场景或 Cubism 完整建模／Animator 功能。
- 文件级测试和部分真实 Core／Photoshop 样本检查，不证明所有 Photoshop／Cubism Editor 往返、Windows GUI、高 DPI 或旧版 macOS 均已验收。

### 构建

桌面应用使用 **qmake**；**CMake** 构建库与测试，不是应用打包入口。

要求 C++17；Qt 6 的 Core、Gui、Widgets、Network、OpenGL、OpenGLWidgets、Core5Compat；组件测试需要 CMake 3.21+ 和 Qt Test。Windows x64 使用 MSVC，macOS ARM64 使用 Apple Clang／Xcode；Linux 与 Windows x86 尚不是已确认的发布目标。

#### 选择匹配的 Cubism Core

| 库／API 组合 | 配置 |
|---|---|
| Native SDK 5-r.4.1／旧 Core API | `LIVE2D_CORE_API_VERSION=5`，默认值 |
| Native SDK 5-r.5／Core 6 API | `LIVE2D_CORE_API_VERSION=6` |

通过 `LIVE2D_CORE_LIBRARY` 指定对应库，其架构和 ABI 必须匹配工具链。API 代际**不是** SDK 压缩包的主版本号；保留默认 API 5 配置时，不能只把库换成 Core 6。SDK 从[官方下载页](https://www.live2d.com/en/sdk/download/native/)获取，并遵守对应许可。

#### Windows

先保存工作并关闭应用：构建脚本会结束正在运行的 `live2d2psd` 进程。

```powershell
.\build.ps1
.\build.ps1 -Configuration Debug
.\build.ps1 -QMakePath "C:\Qt\6.10.0\msvc2022_64\bin\qmake.exe" -MakeTool nmake
```

首次使用外部 Core 6 库时，通过环境变量传入再执行脚本。切换 Core／API 代际、Qt 或架构前必须清理旧对象；如使用脚本的 `-Clean`，先备份 `build/` 中需要保留的内容：

```powershell
$env:LIVE2D_CORE_API_VERSION = "6"
$env:LIVE2D_CORE_LIBRARY = "C:\path\to\matching-x64-Core-library.lib"
.\build.ps1
```

输出为 `softwareEnv/physicsV1/live2d2psd.exe`。**`build.ps1 -Clean` 会删除整个仓库 `build/` 目录**，不是只清理某一配置；不要在那里保存样本、工作文件或验证记录的唯一副本。

#### macOS

先确认 `xcodebuild -version` 与 `xcrun --sdk macosx --show-sdk-path` 可用，并自行完成 Xcode 首次启动／许可设置。从仓库根目录运行，按所用库替换路径和部署目标：

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

示例 `15.7` 对应已测试 Core 6 ARM64 库报告的最低系统，不是所有 SDK 的统一要求。实际部署目标须满足**所选 Core 和 Qt** 的要求；旧 Core 使用 API `5` 和对应最低目标。链接较新系统限定的 Core 库成功，不代表应用兼容 macOS 12／14。

qmake 输出为 `softwareEnv/physicsV1/live2d2psd.app`。上例虽在独立目录生成 Makefile，工程的对象与生成代码仍共用源码根目录下的 `build/RELEASE` 或 `build/DEBUG`。切换 Core 代际、架构或 Qt 时，先用生成的 Makefile 执行 `make clean` 再重建；不要并发构建这些 qmake 变体。工程已处理测试环境中的 Qt ARM 声明与缺失 AGL 框架兼容问题；新版 Xcode SDK／Qt 的兼容警告仍需评估，不能视为支持保证。

### 组件测试

为不同 Core／工具链配置使用独立 CMake 构建目录。许多数据、面板和 stub-Core 测试不需要外部 Core，指定库后会启用额外的真实 Core 测试。

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

旧 Core 在另一构建目录使用 API `5`；不需要真实 Core 的子集应使用新构建目录并显式传入 `-DLIVE2D_CORE_LIBRARY=`；仅省略参数可能继承环境变量或已有库缓存。`LIVE2D_REAL_CORE_EXTRA_MOC` 可为支持的真实 Core 测试追加本地模型；具备独立网格创建测试的版本还可设置 `LIVE2D_CREATE_MAO_MOC`／`LIVE2D_CREATE_REN_MOC`。Core 6 渲染回归还可通过 `LIVE2D_REAL_CORE_EXTRA_TEXTURE` 指定匹配纹理。额外样本需自行依其许可取得；请查看详细输出中的跳过情况，不要把每个 CTest“Passed”都当作已执行样本渲染检查。

**不要为整套测试全局设置 `QT_QPA_PLATFORM=offscreen`。** 部分测试需要真实 OpenGL 上下文，macOS 应使用原生平台后端；仅面板测试会单独配置离屏模式。组件测试、应用启动、实际 GUI 编辑和 Photoshop／Cubism Editor 回读属于不同验收范围。仓库目前没有 CI 自动替代这些本地检查。

### 目录与依赖

| 目录 | 职责 |
|---|---|
| `ui/` | 工作区、面板、对话框和文档／运行态联动 |
| `live2dstruct/` | MOC 解析／重写、模型文档、事务、会话和动画数据 |
| `live2d/`、`framework/` | 应用运行层与修改后的 Cubism Native Framework |
| `opengl/` | 渲染、纹理／图集操作及 PSD 导出 |
| `edit/` | 网格／变形器画布交互和编辑状态 |
| `psd/`、`cmo3/` | PSD 读写及 Cubism 工程读取／生成 |
| `std/`、`tests/` | 公共工具与回归测试 |

依赖包括 [Live2D Cubism SDK](https://www.live2d.com/en/sdk/about/)、Qt 6、OpenCV、[psd_sdk](https://molecular-matters.com/products_psd_sdk.html)、miniz 和 stb_image。Framework 含项目特有的渲染／导出修改，升级 SDK 时应逐项审核兼容性，而不是直接覆盖整个目录。

### 许可证

本项目代码以 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) 发布，仅用于个人研究和学习。第三方依赖保留各自许可；修改后的 Cubism Framework 使用仍须遵守 [Live2D Open Software 许可](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html)。
