$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# Flat vector master: a camera over a second frame represents shared video.
$iconSvg = @'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" fill="none">
  <title>MultiCam</title>
  <rect x="8" y="8" width="240" height="240" rx="56" fill="#142239"/>
  <rect x="84" y="58" width="116" height="88" rx="20" fill="#2DD4BF"/>
  <rect x="44" y="96" width="132" height="108" rx="28" fill="#142239"/>
  <rect x="50" y="102" width="120" height="96" rx="22" fill="#F8FAFC"/>
  <path d="M182 131L205 117Q216 110 216 123V177Q216 190 205 183L182 169Z" fill="#F8FAFC"/>
  <circle cx="110" cy="150" r="25" fill="#142239"/>
  <circle cx="110" cy="150" r="10" fill="#2DD4BF"/>
</svg>
'@
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'multicam.svg'), $iconSvg, [Text.UTF8Encoding]::new($false))

function Fill-RoundedRect($Graphics, [string]$Color, [single]$X, [single]$Y, [single]$Width, [single]$Height, [single]$Radius) {
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $brush = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($Color))
    try {
        $diameter = $Radius * 2
        $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
        $path.AddArc(($X+$Width-$diameter), $Y, $diameter, $diameter, 270, 90)
        $path.AddArc(($X+$Width-$diameter), ($Y+$Height-$diameter), $diameter, $diameter, 0, 90)
        $path.AddArc($X, ($Y+$Height-$diameter), $diameter, $diameter, 90, 90)
        $path.CloseFigure()
        $Graphics.FillPath($brush, $path)
    } finally { $brush.Dispose(); $path.Dispose() }
}

function New-IconBitmap([int]$Size) {
    $large = [Drawing.Bitmap]::new(1024, 1024)
    $canvas = [Drawing.Graphics]::FromImage($large)
    $white = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#F8FAFC'))
    $navy = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#142239'))
    $mint = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#2DD4BF'))
    $cone = [Drawing.Drawing2D.GraphicsPath]::new()
    try {
        $canvas.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $canvas.ScaleTransform(4, 4)
        Fill-RoundedRect $canvas '#142239' 8 8 240 240 56
        Fill-RoundedRect $canvas '#2DD4BF' 84 58 116 88 20
        Fill-RoundedRect $canvas '#142239' 44 96 132 108 28
        Fill-RoundedRect $canvas '#F8FAFC' 50 102 120 96 22
        $cone.AddLine(182, 131, 205, 117)
        $cone.AddBezier(205, 117, 212.3333, 112.3333, 216, 114.3333, 216, 123)
        $cone.AddLine(216, 123, 216, 177)
        $cone.AddBezier(216, 177, 216, 185.6667, 212.3333, 187.6667, 205, 183)
        $cone.AddLine(205, 183, 182, 169)
        $cone.CloseFigure()
        $canvas.FillPath($white, $cone)
        $canvas.FillEllipse($navy, 85, 125, 50, 50)
        $canvas.FillEllipse($mint, 100, 140, 20, 20)
        $bitmap = [Drawing.Bitmap]::new($Size, $Size)
        $scaled = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $scaled.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $scaled.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
            $scaled.DrawImage($large, [Drawing.Rectangle]::new(0, 0, $Size, $Size), 0, 0, 1024, 1024, [Drawing.GraphicsUnit]::Pixel)
        } finally { $scaled.Dispose() }
        return ,$bitmap
    } finally { $cone.Dispose(); $white.Dispose(); $navy.Dispose(); $mint.Dispose(); $canvas.Dispose(); $large.Dispose() }
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$frames = @()
foreach ($size in $sizes) {
    $bitmap = New-IconBitmap $size
    $stream = [IO.MemoryStream]::new()
    try {
        $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        $frames += ,$stream.ToArray()
        if ($size -eq 256) { $bitmap.Save((Join-Path $PSScriptRoot 'multicam.png'), [Drawing.Imaging.ImageFormat]::Png) }
    } finally { $stream.Dispose(); $bitmap.Dispose() }
}
$file = [IO.File]::Create((Join-Path $PSScriptRoot 'multicam.ico'))
$writer = [IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i=0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$i].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$i].Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame) }
} finally { $writer.Dispose(); $file.Dispose() }
Write-Output 'Created multicam.svg, multicam.png and multicam.ico (16 to 256 px).'
