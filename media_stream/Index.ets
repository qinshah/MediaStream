// media_stream HAR 对外出口：re-export 原生 NAPI 模块 API 与类型契约
// 原生实现位于 src/main/cpp（NAPI 模块名 media_stream，产物 libmedia_stream.so）
import native from 'libmedia_stream.so';

export default native;

// 类型 re-export，便于 entry 侧以 `import { ... } from 'media_stream'` 使用
export {
  ResolutionPreset,
  AudioSourceMode,
  OutputConfig,
  CaptureState,
  StreamState,
  RecordState,
  SessionStats,
  RecordingMeta,
  ErrorCode,
  MediaStreamEvent,
} from 'libmedia_stream.so';
