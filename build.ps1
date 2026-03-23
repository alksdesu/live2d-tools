<#
 .SYNOPSIS
  一键编译脚本（Windows / PowerShell）

 .PARAMETER Configuration
  编译配置：Debug 或 Release（默认 Release）

 .PARAMETER QtDir
  Qt 安装根目录（例如 C:\Qt\6.6.3\msvc2019_64）。若不传则自动搜索/使用 QTDIR。

 .PARAMETER QMakePath
  直接指定 qmake.exe 的完整路径（优先级最高）。

 .PARAMETER Arch
  VS 工具链架构：x64 或 x86（默认 x64）

 .PARAMETER Clean
  编译前清理生成文件

 .PARAMETER Jom
  等效于 -MakeTool jom（兼容旧参数）。

 .PARAMETER MakeTool
  指定构建工具：nmake 或 jom。若不指定，自动优先 jom（有则用），否则使用 nmake。

 .PARAMETER OpenOutput
  编译成功后打开输出目录

 用法示例：
  - 默认发布版：  .\build.ps1
  - 调试版：      .\build.ps1 -Configuration Debug
  - 指定Qt：       .\build.ps1 -QtDir "C:\Qt\6.6.3\msvc2019_64"
  - 清理重建：    .\build.ps1 -Clean
  - 指定qmake与nmake：powershell -ExecutionPolicy Bypass -File .\build.ps1 -QMakePath "<Qt安装路径>\bin\qmake.exe" -MakeTool nmake -Clean
#>

[CmdletBinding()]
param(
  [ValidateSet('Debug','Release')]
  [string]$Configuration = 'Release',

  [string]$QtDir,

  [string]$QMakePath,

  [ValidateSet('x64','x86')]
  [string]$Arch = 'x64',

  [switch]$Clean,
  [ValidateSet('nmake','jom')]
  [string]$MakeTool,
  [switch]$Jom,
  [switch]$OpenOutput
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-Info($msg){ Write-Host "[INFO] $msg" -ForegroundColor Cyan }
function Write-Warn($msg){ Write-Host "[WARN] $msg" -ForegroundColor Yellow }
function Write-Err($msg){ Write-Host "[ERR ] $msg" -ForegroundColor Red }

function Import-EnvFromBatch {
  param(
    [Parameter(Mandatory=$true)][string]$BatchFile,
    [Parameter(Mandatory=$true)][string]$Arguments
  )
  if(!(Test-Path $BatchFile)){ throw "找不到批处理文件: $BatchFile" }
  $cmd = "`"$BatchFile`" $Arguments && set"
  $output = cmd /c $cmd
  foreach($line in $output){
    if($line -match '^(.*?)=(.*)$'){
      $name = $matches[1]
      $value = $matches[2]
      # 设置当前进程环境变量
      [System.Environment]::SetEnvironmentVariable($name, $value, 'Process')
      Set-Item -Path ("Env:" + $name) -Value $value -ErrorAction SilentlyContinue
    }
  }
}

function Find-VSVCVarsBat {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\\Installer\\vswhere.exe'
  if(Test-Path $vswhere){
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if($LASTEXITCODE -eq 0 -and $vsPath){
      $vcvars = Join-Path $vsPath 'VC\\Auxiliary\\Build\\vcvarsall.bat'
      if(Test-Path $vcvars){ return $vcvars }
    }
  }
  # 常见路径回退
  $candidates = @(
    'C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat',
    'C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvarsall.bat'
  )
  foreach($c in $candidates){ if(Test-Path $c){ return $c } }
  return $null
}

function Resolve-QMake {
  param([string]$QtDirParam)
  if($QtDirParam){
    $q = Join-Path $QtDirParam 'bin\\qmake.exe'
    if(Test-Path $q){ return $q }
    throw "qmake not found under -QtDir: $q"
  }
  if($env:QTDIR){
    $q = Join-Path $env:QTDIR 'bin\\qmake.exe'
    if(Test-Path $q){ return $q }
  }
  # 搜索常见 Qt 安装
  $roots = @('C:\\Qt','D:\\Qt','E:\\Qt') | Where-Object { Test-Path $_ }
  $hits = @()
  foreach($r in $roots){
    $hits += Get-ChildItem -Path $r -Recurse -Filter 'qmake.exe' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'msvc.*\\bin\\qmake.exe' }
  }
  if($hits.Count -gt 0){
    # 简单选择：按路径排序，优先最新版
    return ($hits | Sort-Object FullName -Descending | Select-Object -First 1).FullName
  }
  throw "qmake not found. Set -QtDir or QTDIR environment variable."
}

function Resolve-Builder([string]$qmakePath, [switch]$PreferJom){
  $qBin = Split-Path $qmakePath -Parent
  $jom = Join-Path $qBin 'jom.exe'
  if($PreferJom -and (Test-Path $jom)){ return $jom }
  if(Test-Path $jom){ return $jom }
  return 'nmake.exe'
}

function Ensure-OutDir([string]$path){
  if(!(Test-Path $path)){
    New-Item -ItemType Directory -Force -Path $path | Out-Null
  }
}

Push-Location $PSScriptRoot
try{
  chcp 65001 > $null
  Write-Info "Working dir: $PSScriptRoot"

  # 1) 配置 VS 编译环境
  $vcvars = Find-VSVCVarsBat
  if($vcvars){
    Write-Info "Import VS toolset: $vcvars ($Arch)"
    Import-EnvFromBatch -BatchFile $vcvars -Arguments $Arch
  } else {
    Write-Warn "VS toolchain env script not found. Use Developer Command Prompt or ensure cl.exe/nmake.exe in PATH."
  }

  # 2) 定位 qmake 与构建器
  if($QMakePath){
    if(!(Test-Path $QMakePath)){ throw "qmake not found: $QMakePath" }
    $qmake = $QMakePath
  } else {
    $qmake = Resolve-QMake -QtDirParam $QtDir
  }
  Write-Info "qmake: $qmake"
  if($MakeTool){
    if($MakeTool -eq 'jom'){
      $qBin = Split-Path $qmake -Parent
      $jomPath = Join-Path $qBin 'jom.exe'
      if(Test-Path $jomPath){ $builder = $jomPath }
      else {
        $cmd = Get-Command jom -ErrorAction SilentlyContinue
        if($cmd){ $builder = $cmd.Source } else { throw "jom not found. Install jom.exe or omit -MakeTool jom" }
      }
    } else {
      $builder = 'nmake.exe'
    }
  } else {
    $builder = Resolve-Builder -qmakePath $qmake -PreferJom:$Jom
  }
  Write-Info "Builder: $builder"

  # 3) 清理
  if($Clean){
    Write-Info "Cleaning build artifacts..."
    @('Makefile','Makefile.Debug','Makefile.Release') | ForEach-Object { if(Test-Path $_){ Remove-Item -Force -ErrorAction SilentlyContinue $_ } }
    if(Test-Path '.\\build') { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue '.\\build' }
  }

  # 3.1) 确保输出目录存在，并终止正在运行的 live2d2psd.exe，避免链接失败 LNK1104
  $outDir = Join-Path $PSScriptRoot 'softwareEnv\\physicsV1'
  Ensure-OutDir $outDir
  $existing = Get-Process -Name 'live2d2psd' -ErrorAction SilentlyContinue
  if($existing){
    Write-Warn 'Killing running live2d2psd.exe to avoid LNK1104...'
    $existing | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
  }

  # 选择构建配置（PowerShell 不支持 ?: 运算符）
  if ($Configuration -ieq 'Release') { $conf = 'release' } else { $conf = 'debug' }

  # 4) 生成 Makefile
  Write-Info "Generate Makefile ($Configuration)..."
  & $qmake "App.pro" "-spec" "win32-msvc" "CONFIG+=$conf"
  if($LASTEXITCODE -ne 0){ throw "qmake failed, code: $LASTEXITCODE" }

  # 5) 编译
  Write-Info "Start build ($builder $conf)..."
  & $builder $conf
  if($LASTEXITCODE -ne 0){ throw "build failed, code: $LASTEXITCODE" }

  # 6) 完成 & 打开输出目录
  Write-Host ""
  Write-Host "========== Build Succeeded ==========" -ForegroundColor Green
  Write-Host "Output: $outDir"
  if($OpenOutput){
    if(Test-Path $outDir){ Start-Process explorer.exe $outDir }
  }
}
catch{
  Write-Err $_
  exit 1
}
finally{
  Pop-Location
}
