// 主播端服务：封装 media_stream 原生模块调用，向 UI 提供统一的会话状态与事件
// 状态由原生 TSFN 事件通道回传，本服务负责转译进 UI 可读的枚举并做错误归类
import { hilog } from '@kit.PerformanceAnalysisKit';
import native, {
  MediaStreamEvent as NativeEvent,
  SessionStats,
  ResolutionPreset,
  AudioSourceMode,
} from 'media_stream';

// 供 UI 侧直接引用的原生统计类型
export type { SessionStats } from 'media_stream';

const DOMAIN = 0x0000;
const TAG = 'MediaStreamService';

// UI 侧会话状态（由原生 capture/stream/record 三平面状态演变）
export enum CaptureStatus {
  IDLE = 'idle',
  STARTING = 'starting',
  ACTIVE = 'active',
  STOPPING = 'stopping',
  ERROR = 'error',
}

export enum StreamStatus {
  IDLE = 'idle',
  CONNECTING = 'connecting',
  STREAMING = 'streaming',
  RECONNECTING = 'reconnecting',
  STOPPED = 'stopped',
  ERROR = 'error',
}

export enum RecordStatus {
  IDLE = 'idle',
  RECORDING = 'recording',
  STOPPED = 'stopped',
  ERROR = 'error',
}

// 录音配置（持久化于 Preferences）
export class Browser {
  first: number = 0;
}
export class StreamSettings {
  rtmpUrl: string = '';   // 推流地址（到 app 段，如 rtmp://host/live）
  streamKey: string = ''; // 推流码（流名，如 stream）
  preset: string = '720p';
  fps: number = 30;
  videoBitrateKbps: number = 4000;
  audioMode: string = 'inner';
}

// 去除首尾空白与尾部斜杠（地址侧）
function trimEndSlash(s: string): string {
  let r = s.trim();
  while (r.length > 0 && r.charAt(r.length - 1) === '/') {
    r = r.substring(0, r.length - 1);
  }
  return r;
}

// 去除首尾空白与前导斜杠（推流码侧）
function trimStartSlash(s: string): string {
  let r = s.trim();
  while (r.length > 0 && r.charAt(0) === '/') {
    r = r.substring(1);
  }
  return r;
}

// 拼接完整 RTMP URL：推流地址 + '/' + 推流码（对齐抖音/B站「推流地址 + 推流码」的分段给法）。
// 两侧斜杠做规整，避免出现 //；推流码为空时按地址原样使用（兼容地址里已含完整路径的老配置）。
// 原生 RtmpClient 只认完整 URL，拆分与拼接全部在本层完成，原生侧无需改动。
export function composeRtmpUrl(rtmpUrl: string, streamKey: string): string {
  const addr = trimEndSlash(rtmpUrl);
  const key = trimStartSlash(streamKey);
  if (key.length === 0) {
    return addr;
  }
  return `${addr}/${key}`;
}

// 录制产物元数据（用于列表展示）
export class RecordingMeta {
  fileName: string = '';
  filePath: string = '';
  durationMs: number = 0;
  sizeBytes: number = 0;
  createdAtMs: number = 0;
}

// 事件载荷（与原生 MediaStreamEvent 判别联合对齐；字段可选，type 判别）
export interface MediaStreamEvent {
  type: string;
  state?: string;
  meta?: RecordingMeta;
  reason?: string;
  code?: number;
  message?: string;
  source?: string;
  stats?: Record<string, Object>;
  degradedTo?: string;
}

export interface MediaStreamEventListener {
  onCaptureState(state: CaptureStatus): void;
  onStreamState(state: StreamStatus): void;
  onRecordState(state: RecordStatus): void;
  onStats(stats: SessionStats): void;
  onRecordFinished(meta: RecordingMeta, reason: string): void;
  onError(code: number, message: string, source: string): void;
  onMicDegraded(message: string): void;
}

export class MediaStreamService {
  private static instance_: MediaStreamService | null = null;
  private listeners: Array<MediaStreamEventListener> = [];
  private inited_ = false;

  // 当前可观测状态
  captureStatus_: CaptureStatus = CaptureStatus.IDLE;
  streamStatus_: StreamStatus = StreamStatus.IDLE;
  recordStatus_: RecordStatus = RecordStatus.IDLE;
  captureError_ = '';
  streamError_ = '';
  recordError_ = '';

  static getInstance(): MediaStreamService {
    if (MediaStreamService.instance_ === null) {
      MediaStreamService.instance_ = new MediaStreamService();
    }
    return MediaStreamService.instance_;
  }

  private constructor() {
  }

  // 绑定原生事件回调（TSFN 在 JS 线程回调，可安全触发监听器）
  init(filesDir: string): void {
    if (this.inited_) {
      return;
    }
    this.inited_ = true;
    native.onEvent((event: NativeEvent) => {
      this.dispatchEvent(event);
    });
    try {
      native.init(filesDir);
      hilog.info(DOMAIN, TAG, 'init ok, filesDir=%{public}s', filesDir);
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'init failed: %{public}s', JSON.stringify(err));
    }
  }

  addListener(listener: MediaStreamEventListener): void {
    this.listeners.push(listener);
  }

  removeListener(listener: MediaStreamEventListener): void {
    const idx = this.listeners.indexOf(listener);
    if (idx >= 0) {
      this.listeners.splice(idx, 1);
    }
  }

  startStreaming(settings: StreamSettings): void {
    this.streamStatus_ = StreamStatus.CONNECTING;
    this.streamError_ = '';
    try {
      // preset 映射短边目标值（原生侧已按预设解释）
      native.startStreaming({
        rtmpUrl: composeRtmpUrl(settings.rtmpUrl, settings.streamKey),
        preset: settings.preset as ResolutionPreset,
        fps: settings.fps,
        videoBitrateKbps: settings.videoBitrateKbps,
        audioMode: settings.audioMode as AudioSourceMode,
      });
      this.notifyStream();
    } catch (err) {
      this.streamStatus_ = StreamStatus.ERROR;
      this.streamError_ = `启动失败: ${JSON.stringify(err)}`;
      this.notifyStream();
    }
  }

  stopStreaming(): void {
    try {
      native.stopStreaming();
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'stopStreaming err %{public}s', JSON.stringify(err));
    }
  }

  startRecording(settings?: StreamSettings): void {
    this.recordStatus_ = RecordStatus.RECORDING;
    this.recordError_ = '';
    this.notifyRecord();
    try {
      // 录制必须携带会话配置（尤其 audioMode），否则原生侧沿用陈旧/默认配置导致麦克风等音频源未启用
      if (settings) {
        native.startRecording({
          rtmpUrl: composeRtmpUrl(settings.rtmpUrl, settings.streamKey),
          preset: settings.preset as ResolutionPreset,
          fps: settings.fps,
          videoBitrateKbps: settings.videoBitrateKbps,
          audioMode: settings.audioMode as AudioSourceMode,
        });
      } else {
        native.startRecording();
      }
    } catch (err) {
      this.recordStatus_ = RecordStatus.ERROR;
      this.recordError_ = `启动录制失败: ${JSON.stringify(err)}`;
      this.notifyRecord();
    }
  }

  stopRecording(): void {
    try {
      native.stopRecording();
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'stopRecording err %{public}s', JSON.stringify(err));
    }
  }

  destroy(): void {
    try {
      native.destroy();
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'destroy err %{public}s', JSON.stringify(err));
    }
  }

  // 事件转译分发
  private dispatchEvent(event: NativeEvent): void {
    switch (event.type) {
      case 'captureState': {
        const s = event.state ?? 'idle';
        const st = this.toCapture(s);
        this.captureStatus_ = st;
        for (const l of this.listeners) {
          l.onCaptureState(st);
        }
        break;
      }
      case 'streamState': {
        const s = event.state ?? 'idle';
        const st = this.toStream(s);
        this.streamStatus_ = st;
        for (const l of this.listeners) {
          l.onStreamState(st);
        }
        break;
      }
      case 'recordState': {
        const s = event.state ?? 'idle';
        const st = this.toRecord(s);
        this.recordStatus_ = st;
        for (const l of this.listeners) {
          l.onRecordState(st);
        }
        break;
      }
      case 'stats': {
        const stats = event.stats;
        if (stats) {
          for (const l of this.listeners) {
            l.onStats(stats);
          }
        }
        break;
      }
      case 'recordFinished': {
        const nm = event.meta;
        const meta = new RecordingMeta();
        if (nm) {
          meta.fileName = nm.fileName;
          meta.filePath = nm.filePath;
          meta.durationMs = nm.durationMs;
          meta.sizeBytes = nm.sizeBytes;
          meta.createdAtMs = nm.createdAtMs;
        }
        const reason = event.reason ?? 'normal';
        for (const l of this.listeners) {
          l.onRecordFinished(meta, reason);
        }
        break;
      }
      case 'error': {
        const code = event.code ?? -1;
        const source = event.source ?? '';
        const message = event.message ?? '';
        // 按来源归类错误文本
        if (source === 'capture') {
          this.captureError_ = message;
        } else if (source === 'rtmp') {
          this.streamError_ = message;
        } else {
          this.recordError_ = message;
        }
        for (const l of this.listeners) {
          l.onError(code, message, source);
        }
        break;
      }
      case 'micDegraded': {
        const message = event.message ?? '';
        for (const l of this.listeners) {
          l.onMicDegraded(message);
        }
        break;
      }
      default:
        hilog.warn(DOMAIN, TAG, 'unknown event type %{public}s', event.type);
        break;
    }
  }

  private toCapture(s: string): CaptureStatus {
    if (s === 'active') return CaptureStatus.ACTIVE;
    if (s === 'starting') return CaptureStatus.STARTING;
    if (s === 'stopping') return CaptureStatus.STOPPING;
    if (s === 'error') return CaptureStatus.ERROR;
    return CaptureStatus.IDLE;
  }

  private toStream(s: string): StreamStatus {
    if (s === 'connecting') return StreamStatus.CONNECTING;
    if (s === 'streaming') return StreamStatus.STREAMING;
    if (s === 'reconnecting') return StreamStatus.RECONNECTING;
    if (s === 'stopped') return StreamStatus.STOPPED;
    if (s === 'error') return StreamStatus.ERROR;
    return StreamStatus.IDLE;
  }

  private toRecord(s: string): RecordStatus {
    if (s === 'recording') return RecordStatus.RECORDING;
    if (s === 'stopped') return RecordStatus.STOPPED;
    if (s === 'error') return RecordStatus.ERROR;
    return RecordStatus.IDLE;
  }

  private notifyStream(): void {
    for (const l of this.listeners) {
      l.onStreamState(this.streamStatus_);
    }
  }

  private notifyRecord(): void {
    for (const l of this.listeners) {
      l.onRecordState(this.recordStatus_);
    }
  }
}