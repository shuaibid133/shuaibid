# postgen.ps1 — CubeMX 重新生成代码后，一键恢复被覆盖的手工改动
# 幂等：已修复的项目自动跳过，可重复运行
# 用法：双击 postgen.bat（或在 CubeMX 生成代码后于终端执行 powershell -File postgen.ps1）

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$conf = Join-Path $root "Core\Inc\stm32f1xx_hal_conf.h"
$uv   = Join-Path $root "MDK-ARM\freertos_test.uvprojx"
$msg  = @()

# ---------------------------------------------------------------
# 1. hal_conf.h：开启 HAL_SRAM_MODULE_ENABLED / HAL_TIM_MODULE_ENABLED
#    CubeMX 每次生成都会把 SRAM 还原成注释（LCD FSMC 需要 SRAM 头文件）；
#    TIM 同理（SYS Timebase = TIM6，HAL 基准时钟，main.c/it.c 引用 htim6）。
#    hal_conf.h 是纯 ASCII 文件，直接文本级替换（不要再用字节级等长替换，
#    旧版 repl 比 needle 长 3 字节，覆盖了下一行的 \r\n 和 '#'，
#    把 SRAM/TIM 两行挤成一条脏行导致 HAL_TIM_MODULE_ENABLED 丢失）。
# ---------------------------------------------------------------
if (Test-Path $conf) {
    $txt  = [IO.File]::ReadAllText($conf, [Text.Encoding]::ASCII)
    $orig = $txt
    # 自愈：历史 bug 产生的脏行（两行被合并成一行）
    $txt = $txt.Replace("#define HAL_SRAM_MODULE_ENABLED          define HAL_TIM_MODULE_ENABLED",
                        "#define HAL_SRAM_MODULE_ENABLED`r`n#define HAL_TIM_MODULE_ENABLED")
    # 开启 SRAM
    $txt = $txt.Replace("/*#define HAL_SRAM_MODULE_ENABLED   */", "#define HAL_SRAM_MODULE_ENABLED")
    # 开启 TIM
    $txt = $txt.Replace("/*#define HAL_TIM_MODULE_ENABLED   */", "#define HAL_TIM_MODULE_ENABLED")
    # 开启 RTC（时间源 rtc_app.c 手动启用，CubeMX 未开 RTC 中间件）
    $txt = $txt.Replace("/*#define HAL_RTC_MODULE_ENABLED   */", "#define HAL_RTC_MODULE_ENABLED   /* 手动启用：RTC 时间源 */")
    if ($txt -ne $orig) {
        [IO.File]::WriteAllText($conf, $txt, [Text.Encoding]::ASCII)
        $msg += "[hal_conf.h] 已修复 SRAM/TIM/RTC 宏"
    } else {
        $msg += "[hal_conf.h] SRAM/TIM/RTC 宏已是开启状态，跳过"
    }
} else {
    $msg += "[hal_conf.h] 文件不存在！"
}

# ---------------------------------------------------------------
# 2. uvprojx：恢复 App 组（cursor.c / app_config.c / input_task.c / ui_task.c）+ 包含路径
# ---------------------------------------------------------------
if (Test-Path $uv) {
    $txt = [IO.File]::ReadAllText($uv, [Text.Encoding]::UTF8)

    # 2a. App 组：逐文件检查，缺哪个补哪个（组不存在则整体插入到 Drivers/CMSIS 前）
    #     目录：joystick.c 在 App/Dev/（dev 层），FATFS 库在 App/FatFs/，其余在 App/Src/
    $appFiles = @(
        @{ n = "cursor.c";      d = "Src" },
        @{ n = "app_config.c";  d = "Src" },
        @{ n = "input_task.c";  d = "Src" },
        @{ n = "ui_task.c";     d = "Src" },
        @{ n = "desktop.c";     d = "Src" },
        @{ n = "rtc_app.c";     d = "Src" },
        @{ n = "app.c";         d = "Src" },
        @{ n = "app_monitor.c"; d = "Src" },
        @{ n = "app_files.c";   d = "Src" },
        @{ n = "sys_stats.c";   d = "Src" },
        @{ n = "w25q128.c";     d = "Src" },
        @{ n = "joystick.c";    d = "Dev" },
        @{ n = "ff.c";          d = "FatFs" },
        @{ n = "diskio.c";      d = "FatFs" }
    )
    # 按文件列表生成一组 <File> 条目（列表为 @{n=文件名; d=子目录}）
    function Get-FileItems($list) {
        ($list | ForEach-Object {
            "        <File>`r`n          <FileName>$($_.n)</FileName>`r`n          <FileType>1</FileType>`r`n          <FilePath>../App/$($_.d)/$($_.n)</FilePath>`r`n        </File>"
        }) -join "`r`n"
    }
    $missing = @()
    foreach ($f in $appFiles) {
        if (-not $txt.Contains("<FileName>$($f.n)</FileName>")) { $missing += $f }
    }
    if ($missing.Count -eq 0) {
        $msg += "[uvprojx] App 组文件齐全，跳过"
    } elseif ($txt.Contains("<GroupName>App</GroupName>")) {
        # 组存在：把缺失文件插到组内第一个 </Files> 之前
        $items = Get-FileItems $missing
        # 定位 App 组内的 </Files>（不能用第一个：新 CubeMX 模板最前是 Application/MDK-ARM 组）
        $gi = $txt.IndexOf("<GroupName>App</GroupName>")
        $anchor = "</Files>"
        $idx = $txt.IndexOf($anchor, $gi)
        if ($idx -ge 0) {
            $newTxt = $txt.Substring(0, $idx) + $items + "`r`n" + $anchor + $txt.Substring($idx + $anchor.Length)
            [IO.File]::WriteAllText($uv, $newTxt, [Text.Encoding]::UTF8)
            $msg += "[uvprojx] App 组补齐: " + (($missing | ForEach-Object { $_.n }) -join ", ")
        } else {
            $msg += "[uvprojx] 未找到 </Files>！请手动检查"
        }
    } else {
        # 组不存在：整体插入（文件列表与增量分支共用 Get-FileItems）
        $appGroup = "        <Group>`r`n          <GroupName>App</GroupName>`r`n          <Files>`r`n" +
                    (Get-FileItems $appFiles) +
                    "`r`n          </Files>`r`n        </Group>"
        $anchor = "        <Group>`r`n          <GroupName>Drivers/CMSIS</GroupName>"
        $newTxt = $txt.Replace($anchor, $appGroup + "`r`n" + $anchor)
        if ($newTxt -eq $txt) {
            # 换行格式不同（LF）时的兜底
            $anchor2 = "<Group>`n          <GroupName>Drivers/CMSIS</GroupName>"
            $newTxt = $txt.Replace($anchor2, $appGroup + "`n" + $anchor2)
        }
        if ($newTxt -ne $txt) {
            [IO.File]::WriteAllText($uv, $newTxt, [Text.Encoding]::UTF8)
            $msg += "[uvprojx] App 组已插入"
        } else {
            $msg += "[uvprojx] 未找到插入位置！请手动检查"
        }
    }

    # 2b. IncludePath 加 ../App/Inc + ../App/Dev（dev 层目录）
    $txt = [IO.File]::ReadAllText($uv, [Text.Encoding]::UTF8)
    if ($txt.Contains("../App/Inc")) {
        $msg += "[uvprojx] 包含路径已含 ../App/Inc，跳过"
    } else {
        $newTxt = $txt.Replace("../Drivers;", "../Drivers;../App/Inc;")
        if ($newTxt -ne $txt) {
            [IO.File]::WriteAllText($uv, $newTxt, [Text.Encoding]::UTF8)
            $msg += "[uvprojx] 包含路径已加 ../App/Inc"
        } else {
            $msg += "[uvprojx] 未找到 ../Drivers; 包含路径！"
        }
    }
    $txt = [IO.File]::ReadAllText($uv, [Text.Encoding]::UTF8)
    # 注意：不能只查 "../App/FatFs"——组里的 <FilePath>../App/FatFs/ff.c</FilePath>
    # 也会匹配（2026-08-18 误判坑），必须带分号只认 IncludePath 条目
    if ($txt.Contains("../App/FatFs;")) {
        $msg += "[uvprojx] 包含路径已含 ../App/FatFs;，跳过"
    } else {
        $newTxt = $txt.Replace("../App/Inc;", "../App/Inc;../App/FatFs;")
        if ($newTxt -ne $txt) {
            [IO.File]::WriteAllText($uv, $newTxt, [Text.Encoding]::UTF8)
            $msg += "[uvprojx] 包含路径已加 ../App/FatFs;"
        } else {
            $msg += "[uvprojx] 未找到 ../App/Inc; 包含路径！"
        }
    }
    $txt = [IO.File]::ReadAllText($uv, [Text.Encoding]::UTF8)
    if ($txt.Contains("../App/Dev")) {
        $msg += "[uvprojx] 包含路径已含 ../App/Dev，跳过"
    } else {
        $newTxt = $txt.Replace("../App/Src;", "../App/Src;../App/Dev;")
        if ($newTxt -ne $txt) {
            [IO.File]::WriteAllText($uv, $newTxt, [Text.Encoding]::UTF8)
            $msg += "[uvprojx] 包含路径已加 ../App/Dev"
        } else {
            $msg += "[uvprojx] 未找到 ../App/Src; 包含路径！"
        }
    }

    # 2c. uvprojx 必须是"无 BOM 的 UTF-8"——uVision4 5.24 的老解析器读带 BOM
    #     的工程文件会报 "cannot read project file"、工程树为空（2026-08-17 bug 根因）。
    #     CubeMX 生成的是无 BOM 版本；若任何工具（包括本脚本的 UTF8 写入）或
    #     手动编辑引入了 BOM，Keil 就打不开工程，这里兜底剥离。
    $b = [IO.File]::ReadAllBytes($uv)
    if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) {
        [IO.File]::WriteAllBytes($uv, $b[3..($b.Length-1)])
        $msg += "[uvprojx] 已剥离 BOM（Keil4 不认带 BOM 的工程文件）"
    } else {
        $msg += "[uvprojx] 无 BOM，Keil4 可正常读取"
    }

    # 2d. HAL 组兜底：stm32f1xx_hal_rtc.c / _rtc_ex.c——CubeMX 未开 RTC 中间件
    #     生成时不带这两个文件，但 rtc_app.c 需要（hal_conf.h 已手动开 RTC 宏）
    $txt = [IO.File]::ReadAllText($uv, [Text.Encoding]::UTF8)
    $halMissing = @()
    foreach ($hf in @("stm32f1xx_hal_rtc.c", "stm32f1xx_hal_rtc_ex.c")) {
        if (-not $txt.Contains("<FileName>$hf</FileName>")) { $halMissing += $hf }
    }
    if ($halMissing.Count -eq 0) {
        $msg += "[uvprojx] HAL RTC 文件齐全，跳过"
    } else {
        $items = ($halMissing | ForEach-Object {
            "        <File>`r`n          <FileName>$_</FileName>`r`n          <FileType>1</FileType>`r`n          <FilePath>../Drivers/STM32F1xx_HAL_Driver/Src/$_</FilePath>`r`n        </File>"
        }) -join "`r`n"
        $gi = $txt.IndexOf("<GroupName>Drivers/STM32F1xx_HAL_Driver</GroupName>")
        $anchor = "</Files>"
        $idx = $txt.IndexOf($anchor, $gi)
        if ($gi -ge 0 -and $idx -ge 0) {
            $newTxt = $txt.Substring(0, $idx) + $items + "`r`n" + $anchor + $txt.Substring($idx + $anchor.Length)
            [IO.File]::WriteAllText($uv, $newTxt, [Text.Encoding]::UTF8)
            $msg += "[uvprojx] HAL 组补齐: " + ($halMissing -join ", ")
        } else {
            $msg += "[uvprojx] 未找到 HAL 驱动组！请手动检查"
        }
    }
} else {
    $msg += "[uvprojx] 文件不存在！"
}

# ---------------------------------------------------------------
# 3. freertos.c：确保 UTF-8（带 BOM）——CubeMX/Keil 可能按系统 ANSI 重写导致乱码
#    注：freertos.c 内的用户注释已改英文（CubeMX 每次重写都会把中文按 ANSI
#    重新编码成乱码且无法自动还原），App/ 下自己的代码全部保留中文注释
# ---------------------------------------------------------------
$fr = Join-Path $root "Core\Src\freertos.c"
if (Test-Path $fr) {
    $b = [IO.File]::ReadAllBytes($fr)
    $utf8 = New-Object System.Text.UTF8Encoding($false, $true)   # 严格解码：非法序列抛异常
    try { $null = $utf8.GetString($b); $isUtf8 = $true } catch { $isUtf8 = $false }
    $hasBom = ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
    if ($isUtf8 -and $hasBom) {
        $msg += "[freertos.c] 已是 UTF-8 BOM，跳过"
    } elseif ($isUtf8) {
        [IO.File]::WriteAllBytes($fr, ([byte[]](0xEF, 0xBB, 0xBF)) + $b)
        $msg += "[freertos.c] 已补 UTF-8 BOM"
    } else {
        try {
            $gbk  = [Text.Encoding]::GetEncoding(936)
            $text = $gbk.GetString($b)
            $out  = $utf8.GetBytes($text)
            [IO.File]::WriteAllBytes($fr, ([byte[]](0xEF, 0xBB, 0xBF)) + $out)
            $msg += "[freertos.c] 已从 ANSI/GBK 转回 UTF-8 BOM"
        } catch {
            $msg += "[freertos.c] 编码未知，请手动检查！"
        }
    }
} else {
    $msg += "[freertos.c] 文件不存在！"
}

# ---------------------------------------------------------------
# 4. FreeRTOSConfig.h：堆扩容 + malloc 失败钩子
#    CubeMX 每次生成都会把堆还原成 3072——装不下 5 个任务 + uiTask 2KB 栈，
#    曾导致 uiTask 创建失败 → 任务没起来 → 屏幕全黑（2026-08-18 bug 根因）。
#    钩子打开后若堆耗尽会停在 for(;;)，LED 停闪 = 分配失败信号，不再静默。
# ---------------------------------------------------------------
$fc = Join-Path $root "Core\Inc\FreeRTOSConfig.h"
if (Test-Path $fc) {
    $txt = [IO.File]::ReadAllText($fc, [Text.Encoding]::ASCII)
    $orig = $txt
    $txt = $txt.Replace("#define configTOTAL_HEAP_SIZE                    ((size_t)3072)",
                        "#define configTOTAL_HEAP_SIZE                    ((size_t)8192)")
    if (-not $txt.Contains("configUSE_MALLOC_FAILED_HOOK")) {
        $txt = $txt.Replace("#define configUSE_TICK_HOOK                      0",
                            "#define configUSE_TICK_HOOK                      0`r`n#define configUSE_MALLOC_FAILED_HOOK             1")
    }
    if ($txt -ne $orig) {
        [IO.File]::WriteAllText($fc, $txt, [Text.Encoding]::ASCII)
        $msg += "[FreeRTOSConfig.h] 已修复堆大小(8192) + malloc 失败钩子"
    } else {
        $msg += "[FreeRTOSConfig.h] 已是修复状态，跳过"
    }
} else {
    $msg += "[FreeRTOSConfig.h] 文件不存在！"
}

# ---------------------------------------------------------------
$msg | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "完成。重复运行无副作用。"
