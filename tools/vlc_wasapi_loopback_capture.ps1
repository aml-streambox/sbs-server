param(
    [string]$Url = $env:SBS_SRT_URL,
    [string]$HostName = $(if ($env:SBS_TARGET_HOST) { $env:SBS_TARGET_HOST } else { $env:TARGET }),
    [int]$Port = $(if ($env:SBS_SRT_PORT) { [int]$env:SBS_SRT_PORT } else { 8888 }),
    [int]$LatencyUs = $(if ($env:SBS_SRT_LATENCY_US) { [int]$env:SBS_SRT_LATENCY_US } else { 600000 }),
    [switch]$UseSrtQuery,
    [double]$DurationSeconds = 30,
    [int]$WindowMs = 50,
    [double]$SilenceDbfs = -55.0,
    [int]$MinDropoutMs = 120,
    [double]$NearZero = 0.00003,
    [double]$FlatDiffRms = 0.00002,
    [string]$OutDir = "",
    [string]$VlcPath = "C:\Program Files\VideoLAN\VLC\vlc.exe",
    [switch]$StartMinimized,
    [switch]$KeepVlcOpen
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
    $OutDir = Join-Path $env:TEMP ("sbs-vlc-wasapi-loopback-" + [guid]::NewGuid().ToString("N"))
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Redact-SrtUrl([string]$value) {
    return ($value -replace 'srt://[^/:\s]+', 'srt://<host>')
}

Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace SbsWasapiLoopback {
    public enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
    public enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }
    public enum AudioClientShareMode { Shared = 0, Exclusive = 1 }

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
    [Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioClient {
        [PreserveSig]
        int Initialize(AudioClientShareMode shareMode, uint streamFlags, long hnsBufferDuration, long hnsPeriodicity, IntPtr pFormat, ref Guid audioSessionGuid);
        [PreserveSig]
        int GetBufferSize(out uint pNumBufferFrames);
        [PreserveSig]
        int GetStreamLatency(out long phnsLatency);
        [PreserveSig]
        int GetCurrentPadding(out uint pNumPaddingFrames);
        [PreserveSig]
        int IsFormatSupported(AudioClientShareMode shareMode, IntPtr pFormat, out IntPtr ppClosestMatch);
        [PreserveSig]
        int GetMixFormat(out IntPtr ppDeviceFormat);
        [PreserveSig]
        int GetDevicePeriod(out long phnsDefaultDevicePeriod, out long phnsMinimumDevicePeriod);
        [PreserveSig]
        int Start();
        [PreserveSig]
        int Stop();
        [PreserveSig]
        int Reset();
        [PreserveSig]
        int SetEventHandle(IntPtr eventHandle);
        [PreserveSig]
        int GetService(ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
    }

    [ComImport]
    [Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IAudioCaptureClient {
        [PreserveSig]
        int GetBuffer(out IntPtr ppData, out uint pNumFramesToRead, out uint pdwFlags, out ulong pu64DevicePosition, out ulong pu64QPCPosition);
        [PreserveSig]
        int ReleaseBuffer(uint NumFramesRead);
        [PreserveSig]
        int GetNextPacketSize(out uint pNumFramesInNextPacket);
    }

    [StructLayout(LayoutKind.Sequential, Pack = 2)]
    public struct WAVEFORMATEX {
        public ushort wFormatTag;
        public ushort nChannels;
        public uint nSamplesPerSec;
        public uint nAvgBytesPerSec;
        public ushort nBlockAlign;
        public ushort wBitsPerSample;
        public ushort cbSize;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 2)]
    public struct WAVEFORMATEXTENSIBLE {
        public WAVEFORMATEX Format;
        public ushort wValidBitsPerSample;
        public uint dwChannelMask;
        public Guid SubFormat;
    }

    public class WindowMetric {
        public int index;
        public double start_s;
        public double end_s;
        public int frames;
        public double rms;
        public double rms_dbfs;
        public double peak;
        public double peak_dbfs;
        public double near_zero_ratio;
        public double diff_peak;
        public double diff_rms;
        public bool silent;
        public bool flat;
        public bool repeated_exact;
    }

    public class CaptureResult {
        public int sample_rate;
        public int channels;
        public int bits_per_sample;
        public string source_format;
        public long captured_frames;
        public double captured_duration_s;
        public double wall_duration_s;
        public WindowMetric[] windows;
    }

    class WindowAccumulator {
        readonly int sampleRate;
        readonly int channels;
        readonly int windowFrames;
        readonly double silenceDbfs;
        readonly double nearZero;
        readonly double flatDiffRms;
        readonly List<WindowMetric> windows = new List<WindowMetric>();
        readonly double[] previousByChannel;
        int framesInWindow = 0;
        long totalFrames = 0;
        double sumSq = 0;
        double peak = 0;
        long zeroCount = 0;
        double diffPeak = 0;
        double diffSumSq = 0;
        long diffCount = 0;
        ulong hash = 1469598103934665603UL;
        ulong? previousHash = null;

        public WindowAccumulator(int sampleRate, int channels, int windowMs, double silenceDbfs, double nearZero, double flatDiffRms) {
            this.sampleRate = sampleRate;
            this.channels = channels;
            this.windowFrames = Math.Max(1, (int)Math.Round(sampleRate * windowMs / 1000.0));
            this.silenceDbfs = silenceDbfs;
            this.nearZero = nearZero;
            this.flatDiffRms = flatDiffRms;
            previousByChannel = new double[channels];
        }

        public void AddFrame(double[] frameSamples) {
            for (int c = 0; c < channels; c++) {
                double s = frameSamples[c];
                double abs = Math.Abs(s);
                if (abs > peak) peak = abs;
                if (abs <= nearZero) zeroCount++;
                sumSq += s * s;
                double diff = s - previousByChannel[c];
                double ad = Math.Abs(diff);
                if (ad > diffPeak) diffPeak = ad;
                diffSumSq += diff * diff;
                diffCount++;
                previousByChannel[c] = s;

                short q = (short)Math.Max(short.MinValue, Math.Min(short.MaxValue, Math.Round(s * 32767.0)));
                unchecked {
                    hash ^= (byte)(q & 0xff);
                    hash *= 1099511628211UL;
                    hash ^= (byte)((q >> 8) & 0xff);
                    hash *= 1099511628211UL;
                }
            }

            framesInWindow++;
            totalFrames++;
            if (framesInWindow >= windowFrames) FinishWindow();
        }

        public long TotalFrames { get { return totalFrames; } }
        public WindowMetric[] Windows { get { return windows.ToArray(); } }

        void FinishWindow() {
            int sampleCount = framesInWindow * channels;
            double rms = sampleCount == 0 ? 0 : Math.Sqrt(sumSq / sampleCount);
            double rmsDbfs = ToDbfs(rms);
            double peakDbfs = ToDbfs(peak);
            double drms = diffCount == 0 ? 0 : Math.Sqrt(diffSumSq / diffCount);
            bool repeated = previousHash.HasValue && previousHash.Value == hash;
            windows.Add(new WindowMetric {
                index = windows.Count,
                start_s = Math.Round((totalFrames - framesInWindow) / (double)sampleRate, 6),
                end_s = Math.Round(totalFrames / (double)sampleRate, 6),
                frames = framesInWindow,
                rms = Math.Round(rms, 8),
                rms_dbfs = Math.Round(rmsDbfs, 3),
                peak = Math.Round(peak, 8),
                peak_dbfs = Math.Round(peakDbfs, 3),
                near_zero_ratio = Math.Round(zeroCount / (double)Math.Max(1, sampleCount), 6),
                diff_peak = Math.Round(diffPeak, 8),
                diff_rms = Math.Round(drms, 8),
                silent = rmsDbfs <= silenceDbfs,
                flat = rmsDbfs > silenceDbfs && drms <= flatDiffRms,
                repeated_exact = repeated
            });
            previousHash = hash;
            framesInWindow = 0;
            sumSq = 0;
            peak = 0;
            zeroCount = 0;
            diffPeak = 0;
            diffSumSq = 0;
            diffCount = 0;
            hash = 1469598103934665603UL;
        }

        static double ToDbfs(double value) {
            if (value <= 0) return -120.0;
            return 20.0 * Math.Log10(value);
        }
    }

    public static class LoopbackCapture {
        const int CLSCTX_ALL = 23;
        const uint AUDCLNT_STREAMFLAGS_LOOPBACK = 0x00020000;
        const uint AUDCLNT_BUFFERFLAGS_SILENT = 0x00000002;
        static readonly Guid IAudioClientGuid = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
        static readonly Guid IAudioCaptureClientGuid = new Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317");
        static readonly Guid PcmGuid = new Guid("00000001-0000-0010-8000-00aa00389b71");
        static readonly Guid FloatGuid = new Guid("00000003-0000-0010-8000-00aa00389b71");

        public static CaptureResult Capture(string wavPath, int durationMs, int windowMs, double silenceDbfs, double nearZero, double flatDiffRms) {
            var enumerator = (IMMDeviceEnumerator)(new MMDeviceEnumeratorComObject());
            IMMDevice device;
            Check(enumerator.GetDefaultAudioEndpoint(EDataFlow.eRender, ERole.eMultimedia, out device), "GetDefaultAudioEndpoint");

            object audioClientObject;
            Guid audioClientGuid = IAudioClientGuid;
            Check(device.Activate(ref audioClientGuid, CLSCTX_ALL, IntPtr.Zero, out audioClientObject), "IMMDevice.Activate(IAudioClient)");
            var audioClient = (IAudioClient)audioClientObject;

            IntPtr formatPtr;
            Check(audioClient.GetMixFormat(out formatPtr), "IAudioClient.GetMixFormat");
            WAVEFORMATEX wf = Marshal.PtrToStructure<WAVEFORMATEX>(formatPtr);
            string formatName = SourceFormatName(formatPtr, wf);
            int channels = wf.nChannels;
            int sampleRate = (int)wf.nSamplesPerSec;
            int bits = wf.wBitsPerSample;
            int blockAlign = wf.nBlockAlign;

            Guid sessionGuid = Guid.Empty;
            Check(audioClient.Initialize(AudioClientShareMode.Shared, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, formatPtr, ref sessionGuid), "IAudioClient.Initialize");
            object captureObject;
            Guid captureGuid = IAudioCaptureClientGuid;
            Check(audioClient.GetService(ref captureGuid, out captureObject), "IAudioClient.GetService(IAudioCaptureClient)");
            var captureClient = (IAudioCaptureClient)captureObject;

            Directory.CreateDirectory(Path.GetDirectoryName(wavPath));
            var acc = new WindowAccumulator(sampleRate, channels, windowMs, silenceDbfs, nearZero, flatDiffRms);
            var sw = Stopwatch.StartNew();
            using (var fs = new FileStream(wavPath, FileMode.Create, FileAccess.Write, FileShare.Read))
            using (var writer = new BinaryWriter(fs)) {
                WriteWavHeader(writer, channels, sampleRate, 16, 0);
                Check(audioClient.Start(), "IAudioClient.Start");
                try {
                    byte[] packet = null;
                    double[] frame = new double[channels];
                    while (sw.ElapsedMilliseconds < durationMs) {
                        uint packetFrames;
                        Check(captureClient.GetNextPacketSize(out packetFrames), "IAudioCaptureClient.GetNextPacketSize");
                        if (packetFrames == 0) {
                            Thread.Sleep(5);
                            continue;
                        }
                        while (packetFrames > 0) {
                            IntPtr data;
                            uint frames;
                            uint flags;
                            ulong devicePos;
                            ulong qpcPos;
                            Check(captureClient.GetBuffer(out data, out frames, out flags, out devicePos, out qpcPos), "IAudioCaptureClient.GetBuffer");
                            int byteCount = checked((int)(frames * blockAlign));
                            if (packet == null || packet.Length < byteCount) packet = new byte[byteCount];
                            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                            if (silent) {
                                Array.Clear(packet, 0, byteCount);
                            } else {
                                Marshal.Copy(data, packet, 0, byteCount);
                            }
                            for (uint f = 0; f < frames; f++) {
                                for (int c = 0; c < channels; c++) {
                                    frame[c] = ReadSample(packet, ((int)f * blockAlign) + SampleOffset(c, bits), bits, formatPtr, wf);
                                }
                                acc.AddFrame(frame);
                                for (int c = 0; c < channels; c++) {
                                    short q = (short)Math.Max(short.MinValue, Math.Min(short.MaxValue, Math.Round(frame[c] * 32767.0)));
                                    writer.Write(q);
                                }
                            }
                            Check(captureClient.ReleaseBuffer(frames), "IAudioCaptureClient.ReleaseBuffer");
                            Check(captureClient.GetNextPacketSize(out packetFrames), "IAudioCaptureClient.GetNextPacketSize");
                        }
                    }
                } finally {
                    audioClient.Stop();
                    Marshal.FreeCoTaskMem(formatPtr);
                }
                long dataBytes = fs.Length - 44;
                fs.Seek(0, SeekOrigin.Begin);
                WriteWavHeader(writer, channels, sampleRate, 16, dataBytes);
            }

            sw.Stop();
            return new CaptureResult {
                sample_rate = sampleRate,
                channels = channels,
                bits_per_sample = 16,
                source_format = formatName,
                captured_frames = acc.TotalFrames,
                captured_duration_s = Math.Round(acc.TotalFrames / (double)sampleRate, 6),
                wall_duration_s = Math.Round(sw.Elapsed.TotalSeconds, 6),
                windows = acc.Windows
            };
        }

        static void Check(int hr, string operation) {
            if (hr != 0) Marshal.ThrowExceptionForHR(hr);
        }

        static int SampleOffset(int channel, int bits) {
            return channel * (bits / 8);
        }

        static double ReadSample(byte[] data, int offset, int bits, IntPtr formatPtr, WAVEFORMATEX wf) {
            string fmt = SourceFormatName(formatPtr, wf);
            if (fmt == "float32") return Math.Max(-1.0, Math.Min(1.0, BitConverter.ToSingle(data, offset)));
            if (fmt == "pcm16") return BitConverter.ToInt16(data, offset) / 32768.0;
            if (fmt == "pcm24") {
                int value = data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16);
                if ((value & 0x800000) != 0) value |= unchecked((int)0xff000000);
                return value / 8388608.0;
            }
            if (fmt == "pcm32") return BitConverter.ToInt32(data, offset) / 2147483648.0;
            throw new NotSupportedException("Unsupported loopback format: " + fmt + " bits=" + bits);
        }

        static string SourceFormatName(IntPtr formatPtr, WAVEFORMATEX wf) {
            if (wf.wFormatTag == 3 && wf.wBitsPerSample == 32) return "float32";
            if (wf.wFormatTag == 1 && wf.wBitsPerSample == 16) return "pcm16";
            if (wf.wFormatTag == 1 && wf.wBitsPerSample == 24) return "pcm24";
            if (wf.wFormatTag == 1 && wf.wBitsPerSample == 32) return "pcm32";
            if (wf.wFormatTag == 0xfffe) {
                WAVEFORMATEXTENSIBLE ext = Marshal.PtrToStructure<WAVEFORMATEXTENSIBLE>(formatPtr);
                if (ext.SubFormat == FloatGuid && wf.wBitsPerSample == 32) return "float32";
                if (ext.SubFormat == PcmGuid && wf.wBitsPerSample == 16) return "pcm16";
                if (ext.SubFormat == PcmGuid && wf.wBitsPerSample == 24) return "pcm24";
                if (ext.SubFormat == PcmGuid && wf.wBitsPerSample == 32) return "pcm32";
            }
            return "unknown-tag-" + wf.wFormatTag + "-bits-" + wf.wBitsPerSample;
        }

        static void WriteWavHeader(BinaryWriter writer, int channels, int sampleRate, int bits, long dataBytes) {
            int blockAlign = channels * bits / 8;
            int byteRate = sampleRate * blockAlign;
            writer.Write(new char[] { 'R', 'I', 'F', 'F' });
            writer.Write((uint)(36 + dataBytes));
            writer.Write(new char[] { 'W', 'A', 'V', 'E' });
            writer.Write(new char[] { 'f', 'm', 't', ' ' });
            writer.Write((uint)16);
            writer.Write((ushort)1);
            writer.Write((ushort)channels);
            writer.Write((uint)sampleRate);
            writer.Write((uint)byteRate);
            writer.Write((ushort)blockAlign);
            writer.Write((ushort)bits);
            writer.Write(new char[] { 'd', 'a', 't', 'a' });
            writer.Write((uint)dataBytes);
        }
    }
}
"@

function New-Dropouts($windows, [double]$silenceDbfs, [int]$minDropoutMs) {
    $dropouts = @()
    $started = $false
    $current = $null
    foreach ($window in $windows) {
        if (-not $started) {
            if ($window.silent) { continue }
            $started = $true
        }
        if ($window.silent) {
            if ($null -eq $current) {
                $current = [ordered]@{ start_s = $window.start_s; end_s = $window.end_s; window_count = 1; min_rms_dbfs = $window.rms_dbfs }
            } else {
                $current.end_s = $window.end_s
                $current.window_count += 1
                $current.min_rms_dbfs = [Math]::Min([double]$current.min_rms_dbfs, [double]$window.rms_dbfs)
            }
        } elseif ($null -ne $current) {
            $duration = [double]$current.end_s - [double]$current.start_s
            if ($duration * 1000.0 -ge $minDropoutMs) {
                $current.duration_s = [Math]::Round($duration, 6)
                $dropouts += [pscustomobject]$current
            }
            $current = $null
        }
    }
    if ($null -ne $current) {
        $duration = [double]$current.end_s - [double]$current.start_s
        if ($duration * 1000.0 -ge $minDropoutMs) {
            $current.duration_s = [Math]::Round($duration, 6)
            $dropouts += [pscustomobject]$current
        }
    }
    return $dropouts
}

$wavPath = Join-Path $OutDir "loopback.wav"
$windowCsvPath = Join-Path $OutDir "windows.csv"
$jsonPath = Join-Path $OutDir "report.json"

$arguments = @(
    "--no-one-instance",
    "--play-and-exit",
    "--no-video-title-show",
    $Url
)
if ($StartMinimized) {
    $arguments = @($arguments[0], $arguments[1], $arguments[2], "--qt-start-minimized") + $arguments[3..($arguments.Count - 1)]
}

$process = Start-Process -FilePath $VlcPath -ArgumentList $arguments -PassThru
$vlcExitedEarly = $false
$capture = $null
try {
    $capture = [SbsWasapiLoopback.LoopbackCapture]::Capture($wavPath, [int]($DurationSeconds * 1000), $WindowMs, $SilenceDbfs, $NearZero, $FlatDiffRms)
    $vlcExitedEarly = $process.HasExited
}
finally {
    if (-not $KeepVlcOpen -and -not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit(3000) | Out-Null
    }
}

$windows = @($capture.windows)
$windows | Export-Csv -NoTypeInformation -Path $windowCsvPath
$dropouts = @(New-Dropouts $windows $SilenceDbfs $MinDropoutMs)
$nonSilentWindows = @($windows | Where-Object { -not $_.silent })
$firstActiveIndex = if ($nonSilentWindows.Count -gt 0) { [int]$nonSilentWindows[0].index } else { $null }
$activeWindows = if ($null -eq $firstActiveIndex) { @() } else { @($windows | Where-Object { [int]$_.index -ge $firstActiveIndex }) }
$flatWindows = @($activeWindows | Where-Object { $_.flat })
$repeatedWindows = @($activeWindows | Where-Object { $_.repeated_exact -and -not $_.silent })
$rmsValues = @($windows | ForEach-Object { $_.rms_dbfs })
$diffValues = @($windows | ForEach-Object { $_.diff_rms })

$healthy = ($dropouts.Count -eq 0) -and ($nonSilentWindows.Count -gt 0) -and ($flatWindows.Count -eq 0) -and ($repeatedWindows.Count -eq 0) -and (-not $vlcExitedEarly)

$report = [ordered]@{
    schema_version = 1
    tool = "vlc_wasapi_loopback_capture.ps1"
    input = [ordered]@{
        url_redacted = Redact-SrtUrl $Url
        duration_s = $DurationSeconds
        window_ms = $WindowMs
        vlc_path = $VlcPath
        process_id = $process.Id
    }
    format = [ordered]@{
        sample_rate = $capture.sample_rate
        channels = $capture.channels
        bits_per_sample = $capture.bits_per_sample
        source_format = $capture.source_format
    }
    thresholds = [ordered]@{
        silence_dbfs = $SilenceDbfs
        min_dropout_ms = $MinDropoutMs
        near_zero = $NearZero
        flat_diff_rms = $FlatDiffRms
    }
    summary = [ordered]@{
        healthy = $healthy
        captured_frames = $capture.captured_frames
        captured_duration_s = $capture.captured_duration_s
        wall_duration_s = $capture.wall_duration_s
        windows = $windows.Count
        non_silent_windows = $nonSilentWindows.Count
        first_active_window = $firstActiveIndex
        dropout_count = $dropouts.Count
        active_flat_window_count = $flatWindows.Count
        active_repeated_exact_window_count = $repeatedWindows.Count
        rms_dbfs_min = $(if ($rmsValues.Count) { [Math]::Round(($rmsValues | Measure-Object -Minimum).Minimum, 3) } else { $null })
        rms_dbfs_max = $(if ($rmsValues.Count) { [Math]::Round(($rmsValues | Measure-Object -Maximum).Maximum, 3) } else { $null })
        rms_dbfs_avg = $(if ($rmsValues.Count) { [Math]::Round(($rmsValues | Measure-Object -Average).Average, 3) } else { $null })
        diff_rms_max = $(if ($diffValues.Count) { [Math]::Round(($diffValues | Measure-Object -Maximum).Maximum, 8) } else { $null })
        vlc_exited_early = $vlcExitedEarly
    }
    dropouts = @($dropouts)
    artifacts = [ordered]@{
        report_json = $jsonPath
        windows_csv = $windowCsvPath
        loopback_wav = $wavPath
    }
    command = [ordered]@{
        vlc_redacted = @($VlcPath) + @($arguments | ForEach-Object { Redact-SrtUrl $_ })
    }
}

$report | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -Path $jsonPath
Write-Host "report: $jsonPath"
Write-Host ("healthy={0} windows={1} non_silent={2} dropouts={3} flat={4} repeated={5} rms_avg={6}" -f $healthy, $windows.Count, $nonSilentWindows.Count, $dropouts.Count, $flatWindows.Count, $repeatedWindows.Count, $report.summary.rms_dbfs_avg)
if (-not $healthy) { exit 2 }
