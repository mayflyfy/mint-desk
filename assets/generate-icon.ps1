# Development utility; only the generated ICO is compiled into MintDesk.exe.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedRectangle([single]$X, [single]$Y, [single]$Width, [single]$Height, [single]$Radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $diameter = $Radius * 2
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return ,$path
}

$images = @()
foreach ($size in @(16, 24, 32, 48, 64, 128, 256)) {
    $bitmap = New-Object System.Drawing.Bitmap($size, $size)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.ScaleTransform($size / 256.0, $size / 256.0)
    $background = New-RoundedRectangle 8 8 240 240 50
    $backgroundBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 18, 27, 33))
    $graphics.FillPath($backgroundBrush, $background)
    $shadowBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 25, 80, 82))
    $backCard = New-RoundedRectangle 49 47 128 143 13
    $graphics.FillPath($shadowBrush, $backCard)
    $frontBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 89, 221, 196))
    $frontCard = New-RoundedRectangle 79 73 132 142 13
    $graphics.FillPath($frontBrush, $frontCard)
    $detailBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 18, 64, 63))
    foreach ($lineY in @(105, 137, 169)) {
        $dot = New-RoundedRectangle 98 $lineY 12 12 3
        $line = New-RoundedRectangle 121 ($lineY + 2) 66 8 4
        $graphics.FillPath($detailBrush, $dot)
        $graphics.FillPath($detailBrush, $line)
        $dot.Dispose()
        $line.Dispose()
    }
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += ,@($size, $stream.ToArray())
    $stream.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
    $background.Dispose()
    $backCard.Dispose()
    $frontCard.Dispose()
    $backgroundBrush.Dispose()
    $shadowBrush.Dispose()
    $frontBrush.Dispose()
    $detailBrush.Dispose()
}

$iconPath = Join-Path $PSScriptRoot 'mintdesk.ico'
$fileStream = [System.IO.File]::Create($iconPath)
$writer = New-Object System.IO.BinaryWriter($fileStream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$images.Count)
    $offset = 6 + $images.Count * 16
    foreach ($entry in $images) {
        $dimension = $(if ($entry[0] -eq 256) { 0 } else { $entry[0] })
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$entry[1].Length)
        $writer.Write([uint32]$offset)
        $offset += $entry[1].Length
    }
    foreach ($entry in $images) { $writer.Write([byte[]]$entry[1]) }
}
finally {
    $writer.Dispose()
    $fileStream.Dispose()
}
Write-Host "Generated $iconPath"
