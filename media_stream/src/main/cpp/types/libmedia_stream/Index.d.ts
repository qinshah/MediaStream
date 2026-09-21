/**
 * media_stream 原生模块 ArkTS API 契约
 *
 * 全部方法由 libmedia_stream.so 原生实现（NAPI 模块名 media_stream）。
 * entry 模块仅通过本契约调用，不包含任何媒体处理原生实现。
 */

// —— 画质预设：解释为屏幕短边目标值，长边按屏幕宽高比等比缩放 ——
export type ResolutionPreset = '1080p' | '720p' | '480p';

// —— 音频源档位：仅麦克风 / 仅系统内录 / 麦克风+内录混音 ——
export type AudioSourceMode = 'mic' | 'inner' | 'micInner';

// —— 会话输出配置 ——
export interface OutputConfig {
  rtmpUrl: string;            // 推流地址，须为 rtmp:// 前缀
  preset: ResolutionPreset;   // 画质预设（短边目标值）
  fps: number;                // 帧率，默认 30
  videoBitrateKbps: number;   // 视频码率：1080p→8000 / 720p→4000 / 480p→2000
  audioMode: AudioSourceMode; // 音频源档位
}

// —— 会话状态三平面 ——
export type CaptureState = 'idle' | 'starting' | 'active' | 'stopping' | 'error';
export type StreamState = 'idle' | 'connecting' | 'streaming' | 'reconnecting' | 'stopped' | 'error';
export type RecordState = 'idle' | 'recording' | 'stopped' | 'error';

// —— 会话统计（节流 500ms 上报；字段按输出活跃与否可选）——
export interface SessionStats {
  streamDurationMs?: number;
  recordDurationMs?: number;
  sentBytes?: number;
  videoFps?: number;
  videoBitrateKbps?: number;
  reconnectAttempt?: number;
  droppedVideoFrames?: number;
}

// —— 录制产物元数据 ——
export interface RecordingMeta {
  fileName: string;
  filePath: string;
  durationMs: number;
  sizeBytes: number;
  createdAtMs: number;
}

// —— 错误码 ——
export enum ErrorCode {
  INVALID_ARG = 0,
  BUSY = 1,
  CAPTURE_INIT_FAIL = 2,
  CAPTURE_PERMISSION_DENIED = 3,
  MIC_PERMISSION_DENIED = 4,
  ENCODER_INIT_FAIL = 5,
  ENCODER_ERROR = 6,
  RTMP_URL_INVALID = 7,
  RTMP_CONNECT_FAIL = 8,
  RTMP_TIMEOUT = 9,
  RTMP_AUTH_FAIL = 10,
  STORAGE_FULL = 11,
  INTERNAL_ERROR = 12,
}

// —— 事件载荷（TSFN 唯一事件通道；type 判别）——
export interface MediaStreamEvent {
  type: string;
  state?: string;
  meta?: RecordingMeta;
  reason?: string;
  code?: number;
  message?: string;
  source?: string;
  stats?: SessionStats;
  degradedTo?: AudioSourceMode;
}

// —— NAPI 方法签名 ——
// 所有 start/stop 幂等防重入；配置非法时抛出带 ErrorCode 的异常
export const init: (filesDir: string) => void;
export const startStreaming: (config: OutputConfig) => void;
export const stopStreaming: () => void;
export const startRecording: (config?: OutputConfig) => void;
export const stopRecording: () => void;
export const onEvent: (callback: (event: MediaStreamEvent) => void) => void;
export const destroy: () => void;

declare const _default: {
  init: typeof init;
  startStreaming: typeof startStreaming;
  stopStreaming: typeof stopStreaming;
  startRecording: typeof startRecording;
  stopRecording: typeof stopRecording;
  onEvent: typeof onEvent;
  destroy: typeof destroy;
};
export default _default;
