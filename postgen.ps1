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
    if ($txt -ne $orig) {
        [IO.File]::WriteAllText($conf, $txt, [Text.Encoding]::ASCII)
        $msg += "[hal_conf.h] 已修复 SRAM/TIM 宏"
    } else {
        $msg += "[hal_conf.h] SRAM/TIM 宏已是开启状态，跳过"
    }
} else {
    $msg += "[hal_conf.h] 文件不存在！"
}

# ---------------------------------------------------------------
# 2. uvprojx：恢复 App 组（cursor.c / app_config.c / input_task.c / ui_task.c）+ 包含路径
# ---------------------------------------------------------------
if (Test-Path $uv) {
    $txt = [IO.File]::ReadAllText($uv, [Text.Encoding]::UTF8)

    # 2a. App 组
    if ($txt.Contains("<GroupName>App</GroupName>")) {
        $msg += "[uvprojx] App 组已存在，跳过"
    } else {
        $appGroup = @"
        <Group>
          <GroupName>App</GroupName>
          <Files>
            <File>
              <FileName>cursor.c</FileName>
              <FileType>1</FileType>
              <FilePath>../App/Src/cursor.c</FilePath>
            </File>
            <File>
              <FileName>app_config.c</FileName>
              <FileType>1</FileType>
              <FilePath>../App/Src/app_config.c</FilePath>
            </File>
            <File>
              <FileName>input_task.c</FileName>
              <FileType>1</FileType>
              <FilePath>../App/Src/input_task.c</FilePath>
            </File>
            <File>
              <FileName>ui_task.c</FileName>
              <FileType>1</FileType>
              <FilePath>../App/Src/ui_task.c</FilePath>
            </File>
          </Files>
        </Group>
"@
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

    # 2b. IncludePath 加 ../App/Inc
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
$msg | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "完成。重复运行无副作用。"
