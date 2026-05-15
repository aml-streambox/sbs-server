param(
    [string]$Url = $env:SBS_SRT_URL,
    [string]$HostName = $(if ($env:SBS_TARGET_HOST) { $env:SBS_TARGET_HOST } else { $env:TARGET }),
    [int]$Port = $(if ($env:SBS_SRT_PORT) { [int]$env:SBS_SRT_PORT } else { 8888 }),
    [int]$LatencyUs = $(if ($env:SBS_SRT_LATENCY_US) { [int]$env:SBS_SRT_LATENCY_US } else { 600000 }),
    [switch]$UseSrtQuery,
    [double]$DurationSeconds = 20,
    [int]$SampleMs = 50,
    [double]$SilenceDbfs = -55.0,
    [int]$MinDropoutMs = 120,
    [string]$OutDir = "",
    [string]$VlcPath = "C:\Program Files\VideoLAN\VLC\vlc.exe",
    [switch]$KeepVlcOpen,
    [switch]$StartMinimized
)

$ErrorActionPreference = "Stop"

if (-not $Url) {
    if (-not $HostName) {
        throw "Provide -Url, -HostName, SBS_SRT_URL, SBS_TARGET_HOST, or TARGET"
    }
    $Url = "srt://${HostName}:${Port}"
    if ($UseSrtQuery) {
        $Url = "${Url}?mode=caller&latency=${LatencyUs}"
    }
}

if (-not (Test-Path -LiteralPath $VlcPath)) {
    throw "VLC not found: $VlcPath"
}

if (-not $OutDir) {
    $OutDir = Join-Path $env:TEMP ("sbs-vlc-audio-meter-" + [guid]::NewGuid().ToString("N"))
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Redact-SrtUrl([string]$value) {
    return ($value -replace 'srt://[^/:\s]+', 'srt://<host>')
}

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

namespace SbsCoreAudio {
    public enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
    public enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }
    public enum AudioSessionState { Inactive = 0, Active = 1, Expired = 2 }

    [ComImport]
    [Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    public class MMDeviceEnumeratorComObject { }

    [ComImport]
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDeviceEnumerator {
        int NotImpl1();
        [PreserveSig]
        int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice ppDevice);
    }

    [ComImport]
    [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMMDevice {
        [PreserveSig]
        int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams, [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);
    }

    [ComImport]
    [Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionManager2 {
        int NotImpl1();
        int NotImpl2();
        [PreserveSig]
        int GetSessionEnumerator(out IAudioSessionEnumerator SessionEnum);
    }

    [ComImport]
    [Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionEnumerator {
        [PreserveSig]
        int GetCount(out int SessionCount);
        [PreserveSig]
        int GetSession(int SessionCount, out IAudioSessionControl Session);
    }

    [ComImport]
    [Guid("F4B1A599-7266-4319-A8CA-E70ACB11E8CD")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionControl {
        [PreserveSig]
        int GetState(out AudioSessionState state);
        [PreserveSig]
        int GetDisplayName([MarshalAs(UnmanagedType.LPWStr)] out string displayName);
        [PreserveSig]
        int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string displayName, ref Guid eventContext);
        [PreserveSig]
        int GetIconPath([MarshalAs(UnmanagedType.LPWStr)] out string iconPath);
        [PreserveSig]
        int SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string iconPath, ref Guid eventContext);
        [PreserveSig]
        int GetGroupingParam(out Guid groupingId);
        [PreserveSig]
        int SetGroupingParam(ref Guid groupingId, ref Guid eventContext);
        [PreserveSig]
        int RegisterAudioSessionNotification(IntPtr notification);
        [PreserveSig]
        int UnregisterAudioSessionNotification(IntPtr notification);
    }

    [ComImport]
    [Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioSessionControl2 {
        [PreserveSig]
        int GetState(out AudioSessionState state);
        [PreserveSig]
        int GetDisplayName([MarshalAs(UnmanagedType.LPWStr)] out string displayName);
        [PreserveSig]
        int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string displayName, ref Guid eventContext);
        [PreserveSig]
        int GetIconPath([MarshalAs(UnmanagedType.LPWStr)] out string iconPath);
        [PreserveSig]
        int SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string iconPath, ref Guid eventContext);
        [PreserveSig]
        int GetGroupingParam(out Guid groupingId);
        [PreserveSig]
        int SetGroupingParam(ref Guid groupingId, ref Guid eventContext);
        [PreserveSig]
        int RegisterAudioSessionNotification(IntPtr notification);
        [PreserveSig]
        int UnregisterAudioSessionNotification(IntPtr notification);
        [PreserveSig]
        int GetSessionIdentifier([MarshalAs(UnmanagedType.LPWStr)] out string retVal);
        [PreserveSig]
        int GetSessionInstanceIdentifier([MarshalAs(UnmanagedType.LPWStr)] out string retVal);
        [PreserveSig]
        int GetProcessId(out uint retVal);
        [PreserveSig]
        int IsSystemSoundsSession();
        [PreserveSig]
        int SetDuckingPreference(bool optOut);
    }

    [ComImport]
    [Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioMeterInformation {
        [PreserveSig]
        int GetPeakValue(out float peak);
        [PreserveSig]
        int GetMeteringChannelCount(out int channelCount);
        [PreserveSig]
        int GetChannelsPeakValues(int channelCount, [Out] float[] peaks);
        [PreserveSig]
        int QueryHardwareSupport(out int hardwareSupportMask);
    }

    public class SessionMeter {
        private IAudioMeterInformation meter;
        public uint ProcessId { get; private set; }
        public SessionMeter(uint processId, IAudioMeterInformation meterInfo) {
            ProcessId = processId;
            meter = meterInfo;
        }
        public float GetPeak() {
            float peak;
            int hr = meter.GetPeakValue(out peak);
            if (hr != 0) Marshal.ThrowExceptionForHR(hr);
            return peak;
        }
    }

    public static class AudioSessions {
        public static SessionMeter FindByProcessId(uint processId) {
            var enumerator = (IMMDeviceEnumerator)(new MMDeviceEnumeratorComObject());
            IMMDevice device;
            int hr = enumerator.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eMultimedia, out device);
            if (hr != 0) Marshal.ThrowExceptionForHR(hr);

            Guid iid = typeof(IAudioSessionManager2).GUID;
            object managerObject;
            hr = device.Activate(ref iid, 23, IntPtr.Zero, out managerObject);
            if (hr != 0) Marshal.ThrowExceptionForHR(hr);
            var manager = (IAudioSessionManager2)managerObject;

            IAudioSessionEnumerator sessions;
            hr = manager.GetSessionEnumerator(out sessions);
            if (hr != 0) Marshal.ThrowExceptionForHR(hr);

            int count;
            hr = sessions.GetCount(out count);
            if (hr != 0) Marshal.ThrowExceptionForHR(hr);

            for (int i = 0; i < count; i++) {
                IAudioSessionControl session;
                hr = sessions.GetSession(i, out session);
                if (hr != 0 || session == null) continue;
                var control2 = session as IAudioSessionControl2;
                if (control2 == null) continue;
                uint pid;
                hr = control2.GetProcessId(out pid);
                if (hr != 0 || pid != processId) continue;
                var meter = session as IAudioMeterInformation;
                if (meter == null) continue;
                return new SessionMeter(pid, meter);
            }
            return null;
        }
    }
}
"@

function Peak-ToDbfs([double]$peak) {
    if ($peak -le 0.0) { return $null }
    return 20.0 * [Math]::Log10($peak)
}

$vlcLogPath = Join-Path $OutDir "vlc.log"
$vlcLogSourcePath = Join-Path $env:TEMP ("sbs-vlc-" + [guid]::NewGuid().ToString("N") + ".log")
$arguments = @(
    "--no-one-instance",
    "--play-and-exit",
    "--no-video-title-show",
    "--extraintf=logger",
    "--verbose=2",
    "--logfile=$vlcLogSourcePath",
    $Url
)
if ($StartMinimized) {
    $arguments = @($arguments[0], $arguments[1], $arguments[2], "--qt-start-minimized") + $arguments[3..($arguments.Count - 1)]
}

$process = Start-Process -FilePath $VlcPath -ArgumentList $arguments -PassThru

$samples = New-Object 'System.Collections.Generic.List[object]'
$dropouts = New-Object 'System.Collections.Generic.List[object]'
$meter = $null
$meterFoundAt = $null
$start = [DateTimeOffset]::UtcNow
$deadline = $start.AddSeconds($DurationSeconds)
$nextSample = $start
$vlcExitedEarly = $false

try {
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        if ($process.HasExited) {
            $vlcExitedEarly = $true
            break
        }

        if ($null -eq $meter) {
            $meter = [SbsCoreAudio.AudioSessions]::FindByProcessId([uint32]$process.Id)
            if ($null -ne $meter -and $null -eq $meterFoundAt) {
                $meterFoundAt = [DateTimeOffset]::UtcNow
            }
        }

        $now = [DateTimeOffset]::UtcNow
        if ($now -lt $nextSample) {
            Start-Sleep -Milliseconds ([Math]::Max(1, [int](($nextSample - $now).TotalMilliseconds)))
            continue
        }

        $peak = 0.0
        if ($null -ne $meter) {
            try { $peak = [double]$meter.GetPeak() } catch { $peak = 0.0 }
        }
        $db = Peak-ToDbfs $peak
        $wall = ([DateTimeOffset]::UtcNow - $start).TotalSeconds
        $samples.Add([pscustomobject]@{
            index = $samples.Count
            wall_time_s = [Math]::Round($wall, 6)
            peak = [Math]::Round($peak, 8)
            peak_dbfs = $(if ($null -eq $db) { $null } else { [Math]::Round($db, 3) })
            silent = $(($null -eq $db) -or ($db -le $SilenceDbfs))
            audio_session_found = $null -ne $meter
        }) | Out-Null
        $nextSample = $nextSample.AddMilliseconds($SampleMs)
    }
}
finally {
    if (-not $KeepVlcOpen -and -not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit(3000) | Out-Null
    }
}

$analysisStarted = $false
$current = $null
foreach ($sample in $samples) {
    if (-not $analysisStarted) {
        if ($sample.silent) { continue }
        $analysisStarted = $true
    }
    if ($sample.silent) {
        if ($null -eq $current) {
            $current = [ordered]@{ start_s = $sample.wall_time_s; end_s = $sample.wall_time_s; sample_count = 1; min_peak_dbfs = $sample.peak_dbfs }
        } else {
            $current.end_s = $sample.wall_time_s
            $current.sample_count += 1
            if ($null -eq $sample.peak_dbfs -or $null -eq $current.min_peak_dbfs) {
                $current.min_peak_dbfs = $null
            } else {
                $current.min_peak_dbfs = [Math]::Min([double]$current.min_peak_dbfs, [double]$sample.peak_dbfs)
            }
        }
    } elseif ($null -ne $current) {
        $duration = [double]$current.end_s - [double]$current.start_s
        if ($duration * 1000.0 -ge $MinDropoutMs) {
            $current.duration_s = [Math]::Round($duration, 6)
            $dropouts.Add([pscustomobject]$current) | Out-Null
        }
        $current = $null
    }
}
if ($null -ne $current) {
    $duration = [double]$current.end_s - [double]$current.start_s
    if ($duration * 1000.0 -ge $MinDropoutMs) {
        $current.duration_s = [Math]::Round($duration, 6)
        $dropouts.Add([pscustomobject]$current) | Out-Null
    }
}

$activeSamples = @($samples | Where-Object { $_.audio_session_found })
$nonSilentSamples = @($samples | Where-Object { -not $_.silent })
$peakValues = @($samples | ForEach-Object { $_.peak })
$dbValues = @($samples | Where-Object { $null -ne $_.peak_dbfs } | ForEach-Object { $_.peak_dbfs })

$csvPath = Join-Path $OutDir "samples.csv"
$jsonPath = Join-Path $OutDir "report.json"
$samples | Export-Csv -NoTypeInformation -Path $csvPath
if (Test-Path -LiteralPath $vlcLogSourcePath) {
    Copy-Item -LiteralPath $vlcLogSourcePath -Destination $vlcLogPath -Force
    Remove-Item -LiteralPath $vlcLogSourcePath -Force -ErrorAction SilentlyContinue
}
$vlcExitCode = $null
if ($process.HasExited) {
    try { $vlcExitCode = [int]$process.ExitCode } catch { $vlcExitCode = $null }
}
$audioSessionFoundAtS = $null
if ($null -ne $meterFoundAt) {
    $audioSessionFoundAtS = [Math]::Round(($meterFoundAt - $start).TotalSeconds, 6)
}
$peakMax = $null
if ($peakValues.Count -gt 0) {
    $peakMax = [Math]::Round([double](($peakValues | Measure-Object -Maximum).Maximum), 8)
}
$peakDbfsMax = $null
$peakDbfsAvg = $null
if ($dbValues.Count -gt 0) {
    $peakDbfsMax = [Math]::Round([double](($dbValues | Measure-Object -Maximum).Maximum), 3)
    $peakDbfsAvg = [Math]::Round([double](($dbValues | Measure-Object -Average).Average), 3)
}
$sampleCount = [int]$samples.Count
$activeSessionSampleCount = [int]$activeSamples.Count
$nonSilentSampleCount = [int]$nonSilentSamples.Count
$dropoutCount = [int]$dropouts.Count
$healthy = $false
if (($dropoutCount -eq 0) -and ($activeSessionSampleCount -gt 0) -and ($nonSilentSampleCount -gt 0) -and (-not [bool]$vlcExitedEarly)) {
    $healthy = $true
}
$urlRedacted = Redact-SrtUrl $Url
$audioSessionFound = $activeSessionSampleCount -gt 0
$vlcCommandRedacted = @($VlcPath) + @($arguments | ForEach-Object { Redact-SrtUrl $_ })

$inputReport = @{}
$inputReport["url_redacted"] = $urlRedacted
$inputReport["duration_s"] = $DurationSeconds
$inputReport["sample_ms"] = $SampleMs
$inputReport["vlc_path"] = $VlcPath
$inputReport["process_id"] = [int]$process.Id
$inputReport["audio_session_found"] = [bool]$audioSessionFound
$inputReport["audio_session_found_at_s"] = $audioSessionFoundAtS

$thresholdReport = @{}
$thresholdReport["silence_dbfs"] = $SilenceDbfs
$thresholdReport["min_dropout_ms"] = $MinDropoutMs

$summaryReport = @{}
$summaryReport["healthy"] = [bool]$healthy
$summaryReport["samples"] = $sampleCount
$summaryReport["active_session_samples"] = $activeSessionSampleCount
$summaryReport["non_silent_samples"] = $nonSilentSampleCount
$summaryReport["dropout_count"] = $dropoutCount
$summaryReport["peak_max"] = $peakMax
$summaryReport["peak_dbfs_max"] = $peakDbfsMax
$summaryReport["peak_dbfs_avg"] = $peakDbfsAvg
$summaryReport["vlc_exited_early"] = [bool]$vlcExitedEarly
$summaryReport["vlc_exit_code"] = $vlcExitCode

$artifactReport = @{}
$artifactReport["report_json"] = $jsonPath
$artifactReport["samples_csv"] = $csvPath
$artifactReport["vlc_log"] = $vlcLogPath

$commandReport = @{}
$commandReport["vlc_redacted"] = $vlcCommandRedacted

$dropoutReport = @()
foreach ($dropout in $dropouts) {
    $dropoutReport += $dropout
}

$report = @{}
$report["schema_version"] = 1
$report["tool"] = "vlc_audio_meter.ps1"
$report["input"] = $inputReport
$report["thresholds"] = $thresholdReport
$report["summary"] = $summaryReport
$report["dropouts"] = $dropoutReport
$report["artifacts"] = $artifactReport
$report["command"] = $commandReport

$report | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -Path $jsonPath
Write-Host "report: $jsonPath"
Write-Host ("healthy={0} audio_session_found={1} samples={2} dropouts={3} peak_dbfs_avg={4}" -f $report.summary.healthy, $report.input.audio_session_found, $report.summary.samples, $report.summary.dropout_count, $report.summary.peak_dbfs_avg)
if (-not $report.summary.healthy) { exit 2 }
