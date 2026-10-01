<#
.SYNOPSIS
  Regenerates the placeholder images shown by the virtual camera when no live video is available.

.DESCRIPTION
  Renders 1920x1080 PNGs into host/assets/placeholders with ffmpeg's drawtext filter and the
  Segoe UI fonts that ship with Windows. The host converts them to NV12 at runtime for the
  installed camera mode. Run this only when the wording or design changes.
#>
[CmdletBinding()]
param(
    [string] $Ffmpeg = 'ffmpeg'
)

$ErrorActionPreference = 'Stop'
$outDir = Join-Path $PSScriptRoot '..\assets\placeholders'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ffmpeg filter syntax needs the drive colon escaped.
$regular = 'C\:/Windows/Fonts/segoeui.ttf'
$bold = 'C\:/Windows/Fonts/segoeuib.ttf'

$placeholders = [ordered]@{
    'no-device'  = @{ Title = 'Connect your phone'; Detail = 'iPhone - unlock it and tap Trust  /  Android - allow USB debugging' }
    'app-closed' = @{ Title = 'Open Mobile Webcam'; Detail = 'Start the Mobile Webcam app on your phone' }
    'background' = @{ Title = 'Mobile Webcam is in the background'; Detail = 'Bring the app back to the foreground to resume the camera' }
    'paused'     = @{ Title = 'Camera paused'; Detail = 'The phone camera is busy or interrupted. It resumes automatically' }
    'stopped'    = @{ Title = 'Starting camera...'; Detail = 'Waiting for video from your phone' }
}

foreach ($entry in $placeholders.GetEnumerator()) {
    $title = $entry.Value.Title
    $detail = $entry.Value.Detail
    $filter = @(
        "drawbox=x=0:y=ih-12:w=iw:h=12:color=0x3D7BFF@1:t=fill",
        "drawtext=fontfile='$bold':text='$title':fontcolor=white:fontsize=84:x=(w-text_w)/2:y=(h-text_h)/2-60",
        "drawtext=fontfile='$regular':text='$detail':fontcolor=0xB8C0CC:fontsize=40:x=(w-text_w)/2:y=(h/2)+40",
        "drawtext=fontfile='$regular':text='Mobile Webcam Bridge':fontcolor=0x5A6372:fontsize=32:x=60:y=h-90"
    ) -join ','
    $target = Join-Path $outDir "$($entry.Key).png"
    & $Ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'color=c=0x15181E:s=1920x1080:d=1' -vf $filter -frames:v 1 $target
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed for $($entry.Key)" }
    Write-Host "wrote $target"
}
