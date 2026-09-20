// 域类型表：画质预设、音频源档位、路由名、事件类型、错误码中文文案
// 本文件仅承载纯数据定义，供 data / service / viewmodel / views 各层共享引用
import { ErrorCode } from 'media_stream';

// —— 录制列表页路由名（Navigation NavPathStack push 用）——
export const ROUTE_RECORDINGS = 'recordings';

// —— 画质预设表：value 与原生 ResolutionPreset 对齐，shortEdge 为屏幕短边目标值 ——
export interface PresetOption {
  value: string;
  label: string;
  shortEdge: number;
  bitrateKbps: number;
}

export const PRESETS: Array<PresetOption> = [
  { value: '1080p', label: '1080p', shortEdge: 1080, bitrateKbps: 8000 },
  { value: '720p', label: '720p', shortEdge: 720, bitrateKbps: 4000 },
  { value: '480p', label: '480p', shortEdge: 480, bitrateKbps: 2000 },
];

// —— 音频源档位表：value 与原生 AudioSourceMode 对齐 ——
export interface AudioOption {
  value: string;
  label: string;
}

export const AUDIO_SOURCES: Array<AudioOption> = [
  { value: 'inner', label: '内录' },
  { value: 'mic', label: '麦克风' },
  { value: 'micInner', label: '混合' },
];

// —— 事件类型映射（与原生 MediaStreamEvent.type 判别值对齐）——
export interface EventTypeMap {
  captureState: string;
  streamState: string;
  recordState: string;
  stats: string;
  recordFinished: string;
  error: string;
  micDegraded: string;
}

export const EVENT_TYPES: EventTypeMap = {
  captureState: 'captureState',
  streamState: 'streamState',
  recordState: 'recordState',
  stats: 'stats',
  recordFinished: 'recordFinished',
  error: 'error',
  micDegraded: 'micDegraded',
};

// —— ErrorCode → 中文用户可读文案映射表（US5/T030）——
export function errorMessage(code: number): string {
  switch (code) {
    case ErrorCode.INVALID_ARG:
      return '参数非法，请检查输入';
    case ErrorCode.BUSY:
      return '当前操作进行中，请稍后再试';
    case ErrorCode.CAPTURE_INIT_FAIL:
      return '屏幕采集初始化失败';
    case ErrorCode.CAPTURE_PERMISSION_DENIED:
      return '未获得录屏授权，无法开始';
    case ErrorCode.MIC_PERMISSION_DENIED:
      return '未获得麦克风权限，已切换为仅内录';
    case ErrorCode.ENCODER_INIT_FAIL:
      return '视频编码器初始化失败';
    case ErrorCode.ENCODER_ERROR:
      return '视频编码发生异常';
    case ErrorCode.RTMP_URL_INVALID:
      return '推流地址格式不正确，需以 rtmp:// 开头';
    case ErrorCode.RTMP_CONNECT_FAIL:
      return '无法连接到推流服务器';
    case ErrorCode.RTMP_TIMEOUT:
      return '推流连接超时';
    case ErrorCode.RTMP_AUTH_FAIL:
      return '推流服务器鉴权失败';
    case ErrorCode.STORAGE_FULL:
      return '存储空间不足，录制已停止';
    case ErrorCode.INTERNAL_ERROR:
      return '内部异常，请重试';
    default:
      return '未知错误';
  }
}