<#
    tools\make_icons.ps1 — генерирует bin\save.ico и bin\restore.ico.

    Запускается автоматически из build.bat (если иконок ещё нет) или вручную:
        powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icons.ps1

    Записи 16/32/48 px сохраняются в ICO как BMP (32 бита + маска), 256 px — как PNG:
    так Windows правильно рисует и в трее/заголовке, и в крупном предпросмотре.
    Зависимостей нет — только System.Drawing из состава Windows.
#>
[CmdletBinding()]
param(
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$Canvas = 256.0            # рисуем в системе координат 256x256 и масштабируем под размер
$Sizes = @(16, 32, 48, 256)
$PngFrom = 256             # начиная с этого размера — PNG внутри ICO

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $ProjectRoot 'bin'
}
if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

function New-RoundRect {
    param([single]$X, [single]$Y, [single]$W, [single]$H, [single]$R)
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $d = $R * 2
    $path.AddArc($X, $Y, $d, $d, 180, 90)
    $path.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $path.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $path.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-Solid {
    param([int]$A, [int]$R, [int]$G, [int]$B)
    return [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb($A, $R, $G, $B))
}

function Fill-RoundRect {
    param([System.Drawing.Graphics]$G, [single]$X, [single]$Y, [single]$W, [single]$H,
          [single]$R, [System.Drawing.Brush]$Brush)
    $path = New-RoundRect $X $Y $W $H $R
    $G.FillPath($Brush, $path)
    $path.Dispose()
}

function Draw-Background {
    param([System.Drawing.Graphics]$G, [int]$R1, [int]$G1, [int]$B1, [int]$R2, [int]$G2, [int]$B2)
    $path = New-RoundRect 8 8 240 240 52
    $rect = [System.Drawing.RectangleF]::new(8, 8, 240, 240)
    $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        $rect,
        [System.Drawing.Color]::FromArgb(255, $R1, $G1, $B1),
        [System.Drawing.Color]::FromArgb(255, $R2, $G2, $B2),
        90.0)
    $G.FillPath($brush, $path)
    $brush.Dispose()
    $path.Dispose()
}

# «Сохранить»: зелёная плитка, белая дискета, зелёная стрелка вниз на этикетке.
function Draw-SaveIcon {
    param([System.Drawing.Graphics]$G)
    Draw-Background $G 38 166 86 15 108 55

    $body = New-Solid 255 244 250 246
    Fill-RoundRect $G 56 46 144 164 14 $body
    $body.Dispose()

    $shutter = New-Solid 255 24 92 52
    Fill-RoundRect $G 104 60 48 40 8 $shutter
    $shutter.Dispose()

    $label = New-Solid 255 190 232 203
    Fill-RoundRect $G 84 126 88 64 10 $label
    $label.Dispose()

    $arrow = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $arrow.AddPolygon([System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(120, 130),
        [System.Drawing.PointF]::new(136, 130),
        [System.Drawing.PointF]::new(136, 156),
        [System.Drawing.PointF]::new(152, 156),
        [System.Drawing.PointF]::new(128, 182),
        [System.Drawing.PointF]::new(104, 156),
        [System.Drawing.PointF]::new(120, 156)))
    $brush = New-Solid 255 15 108 55
    $G.FillPath($brush, $arrow)
    $brush.Dispose()
    $arrow.Dispose()
}

# «Восстановить»: синяя плитка, два окна (заднее/переднее) и белая стрелка влево.
function Draw-RestoreIcon {
    param([System.Drawing.Graphics]$G)
    Draw-Background $G 46 124 224 13 72 140

    $back = New-Solid 255 186 212 246
    Fill-RoundRect $G 48 44 116 96 12 $back
    $back.Dispose()

    $front = New-Solid 255 250 252 255
    Fill-RoundRect $G 92 92 116 96 12 $front
    $front.Dispose()

    $title = New-Solid 255 46 124 224
    Fill-RoundRect $G 92 92 116 28 12 $title
    $title.Dispose()

    $pen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(255, 250, 252, 255), 18.0)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $G.DrawLine($pen, 204.0, 212.0, 160.0, 212.0)
    $pen.Dispose()

    $head = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $head.AddPolygon([System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(160, 196),
        [System.Drawing.PointF]::new(160, 228),
        [System.Drawing.PointF]::new(130, 212)))
    $brush = New-Solid 255 250 252 255
    $G.FillPath($brush, $head)
    $brush.Dispose()
    $head.Dispose()
}

function New-IconBitmap {
    param([string]$Kind, [int]$Size)

    $bmp = [System.Drawing.Bitmap]::new($Size, $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.ScaleTransform([single]($Size / $Canvas), [single]($Size / $Canvas))
        if ($Kind -eq 'save') { Draw-SaveIcon $g } else { Draw-RestoreIcon $g }
    }
    finally {
        $g.Dispose()
    }
    return $bmp
}

# BMP-запись ICO: BITMAPINFOHEADER (высота удвоена) + XOR-строки BGRA снизу вверх + маска.
function Get-IconDibBytes {
    param([System.Drawing.Bitmap]$Bmp)

    $w = $Bmp.Width
    $h = $Bmp.Height
    $ms = [System.IO.MemoryStream]::new()
    $bw = [System.IO.BinaryWriter]::new($ms)
    try {
        $bw.Write([int]40)
        $bw.Write([int]$w)
        $bw.Write([int]($h * 2))
        $bw.Write([int16]1)
        $bw.Write([int16]32)
        $bw.Write([int]0)                 # BI_RGB
        $bw.Write([int]($w * $h * 4))
        $bw.Write([int]0)
        $bw.Write([int]0)
        $bw.Write([int]0)
        $bw.Write([int]0)

        for ($y = $h - 1; $y -ge 0; $y--) {
            for ($x = 0; $x -lt $w; $x++) {
                $c = $Bmp.GetPixel($x, $y)
                $bw.Write($c.B)
                $bw.Write($c.G)
                $bw.Write($c.R)
                $bw.Write($c.A)
            }
        }

        $maskStride = [int]([math]::Floor(($w + 31) / 32) * 4)
        $mask = [byte[]]::new($maskStride * $h)   # нули = «непрозрачно», альфа берётся из BGRA
        $bw.Write($mask)
        $bw.Flush()
        # Унарная запятая обязательна: иначе PowerShell «развернёт» массив в object[]
        # и длина записи в каталоге ICO будет посчитана неверно.
        return , $ms.ToArray()
    }
    finally {
        $bw.Dispose()
        $ms.Dispose()
    }
}

function Get-IconPngBytes {
    param([System.Drawing.Bitmap]$Bmp)
    $ms = [System.IO.MemoryStream]::new()
    try {
        $Bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        return , $ms.ToArray()
    }
    finally {
        $ms.Dispose()
    }
}

# ICO: 6-байтовая шапка + 16-байтовые элементы каталога + сами данные.
function Write-IcoFile {
    param([string]$Path, $Entries)

    $fs = [System.IO.File]::Create($Path)
    $bw = [System.IO.BinaryWriter]::new($fs)
    try {
        $bw.Write([int16]0)
        $bw.Write([int16]1)                 # тип 1 = иконка
        $bw.Write([int16]$Entries.Count)

        $offset = 6 + 16 * $Entries.Count
        foreach ($e in $Entries) {
            $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
            $bw.Write([byte]$dim)
            $bw.Write([byte]$dim)
            $bw.Write([byte]0)              # палитра не используется
            $bw.Write([byte]0)
            $bw.Write([int16]1)             # плоскости
            $bw.Write([int16]32)            # бит на пиксель
            $bw.Write([int]$e.Data.Length)
            $bw.Write([int]$offset)
            $offset += $e.Data.Length
        }
        foreach ($e in $Entries) {
            $bw.Write($e.Data)
        }
        $bw.Flush()
    }
    finally {
        $bw.Dispose()
        $fs.Dispose()
    }
}

function New-IconFile {
    param([string]$Kind, [string]$Path)

    $entries = @()
    foreach ($size in $Sizes) {
        $bmp = New-IconBitmap $Kind $size
        try {
            [byte[]]$data = if ($size -ge $PngFrom) { Get-IconPngBytes $bmp } else { Get-IconDibBytes $bmp }
        }
        finally {
            $bmp.Dispose()
        }
        if ($data.Length -lt 100) {
            throw "icon $Kind ${size}px: too little data ($($data.Length) bytes)"
        }
        $entries += [pscustomobject]@{ Size = $size; Data = $data }
    }

    Write-IcoFile $Path $entries
    $bytes = (Get-Item -LiteralPath $Path).Length
    Write-Host ("[icons] {0}  ({1} px, {2} байт)" -f (Split-Path -Leaf $Path), ($Sizes -join '/'), $bytes)
}

New-IconFile 'save'    (Join-Path $OutDir 'save.ico')
New-IconFile 'restore' (Join-Path $OutDir 'restore.ico')
Write-Host "[icons] готово: $OutDir"
