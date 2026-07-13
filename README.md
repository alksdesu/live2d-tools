# live2d2psd

Live2D Cubism 模型转换工具箱：将运行时模型（`.moc3`）转换为 Adobe Photoshop（PSD）文件，或恢复为 Cubism Editor 可编辑工程（`.cmo3`）。

PSD 导出逐个渲染模型中的每个 drawable（art mesh），写入独立图层，保留原始 Part 分组层级结构和名称。cmo3 生成从 moc3 二进制还原全部几何、变形器、参数与物理数据，适用于丢失源工程只剩运行时文件的场景。

## 功能

### 模型查看

- 打开 `.model3.json` 或 `.moc3` 文件（拖拽或文件对话框；moc3 直开自动生成配置）
- 实时预览模型，通过滑条调整所有参数
- 播放动作、切换表情，支持自动眨眼和呼吸开关，Pose 切换
- 模型结构树查看（Part / ArtMesh / Deformer 层级），点击高亮对应网格
- 纹理图集查看：缩放/平移、UV 线框叠加、点击图集反查 ArtMesh（与结构树/主视图三方联动）、导出 PNG

### PSD 导出

- 两种图层模式：
  - **全部图层** — 按渲染顺序平铺所有 drawable
  - **按分组导出** — 保留 Part 层级结构为 PSD 图层组
- **参数快照导出** — 用当前滑条调整的姿态导出，禁用自动效果
- **纹理提取模式** — 直接提取原始纹理，无变形，适合高质量素材提取
- **Cubism 可导入模式** — 嵌入 sRGB ICC 配置、Unicode 图层名（luni）、图层名去重，保证导出的 PSD 能再导回 Cubism Editor
- 批量导出：选择文件夹，自动遍历所有模型生成 PSD，失败文件逐个列出

### cmo3 工程恢复（moc3 → cmo3）

工具栏"导出cmo3工程"（Ctrl+E）将当前模型恢复为 Cubism Editor 5.0 工程：

- **可恢复**：网格几何（顶点/UV/三角形）、关键帧变形、Warp/Rotation 变形器、Part 层级、参数定义与绑定、裁剪蒙版、混合模式、Glue、物理设置（physics3.json）、显示名汉化（cdi3.json）
- **不可恢复**（moc3 中不存在）：原始 PSD 关联、变形器路径整理、画师备注等编辑器元数据
- 贴图以"单 PSD 文档 + 每网格一图层"模式重建（MODEL_IMAGE），在 Editor 中可直接换皮再导出 moc3

```
输入: model.moc3 + textures/*.png [+ physics3.json + cdi3.json]
输出: model.cmo3（CAFF 容器：main.xml + 每网格 PNG）
```

## 截图

<!-- 在此放置截图 -->

## 构建

**环境要求：**

- Windows x64
- Qt 6.x（需要模块：core gui opengl openglwidgets core5compat widgets gui-private network）
- MSVC 编译器
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

## 依赖

| 库 | 用途 |
|---|---|
| [Live2D Cubism SDK](https://www.live2d.com/en/sdk/about/) | 模型加载、参数控制、渲染 |
| Qt 6 | GUI 框架 |
| OpenCV 4.10 | 图像处理 |
| [psd_sdk](https://molecular-matters.com/products_psd_sdk.html) | PSD 文件读写（已扩展 luni/ICC 支持） |
| miniz | cmo3 CAFF 容器的 ZIP 压缩 |
| stb_image | 图像加载 |

## 目录说明

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

## 注意

- `framework/` 目录下的代码基于 Live2D Cubism Native Framework 修改，`CubismRenderer_OpenGLES2.cpp` 中的 `DrawModel()` 有大幅改动以支持 PSD 逐层导出。升级 SDK 时需要手动合并。
- Live2D Cubism SDK 使用需遵守其[许可协议](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html)。

## 许可证

本项目代码以 [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) 许可证发布。

本项目仅用于个人研究和学习目的。

第三方依赖各自遵循其原始许可证（Live2D SDK、OpenCV、Qt 等）。
