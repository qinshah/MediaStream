// 控制台视图模型：@ObservedV2 状态镜像三平面、事件订阅、配置加载保存、操作编排
// 编排链路：权限(麦克风) → 长时任务 → 引擎指令；会话结束后长时任务自动取消（T015/T020/T028/T031）
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import {
  MediaStreamService,
  MediaStreamEventListener,
  CaptureStatus,
  StreamStatus,
  RecordStatus,
  StreamSettings,
  RecordingMeta,
  SessionStats,
} from '../service/MediaStreamService';
import { PreferencesRepository } from '../data/PreferencesRepository';
import { RecordingsRepository } from '../data/RecordingsRepository';
import { PermissionService } from '../service/PermissionService';
import { BackgroundTaskService } from '../service/BackgroundTaskService';
import { errorMessage } from '../model/Types';

const DOMAIN = 0x0000;
const TAG = 'ConsoleViewModel';

// 默认推流地址：指向 Mac 上 node-media-server 起的局域网 RTMP 服务（rtmp://<mac-ip>:1935/live/stream）。
// 便于“打开即点推流”完成闭环测试；用户仍可在输入框改成任意地址，改后持久化覆盖默认值。
const DEFAULT_RTMP_URL = 'rtmp://192.168.43.10/live/stream';

// 事件监听器：把原生/服务的状态事件镜像进 viewmodel 的可观测字段
class ConsoleListener implements MediaStreamEventListener {
  private owner: ConsoleViewModel;

  constructor(owner: ConsoleViewModel) {
    this.owner = owner;
  }

  onCaptureState(state: CaptureStatus): void {
    this.owner.applyCaptureState(state);
  }

  onStreamState(state: StreamStatus): void {
    this.owner.applyStreamState(state);
  }

  onRecordState(state: RecordStatus): void {
    this.owner.applyRecordState(state);
  }

  onStats(stats: SessionStats): void {
    this.owner.applyStats(stats);
  }

  onRecordFinished(meta: RecordingMeta, reason: string): void {
    this.owner.applyRecordFinished(meta, reason);
  }

  onError(code: number, message: string, source: string): void {
    this.owner.applyError(code, message, source);
  }

  onMicDegraded(message: string): void {
    this.owner.applyMicDegraded(message);
  }
}

@ObservedV2
export class ConsoleViewModel {
  // —— 配置（可编辑，变更即持久化；会话进行中锁定）——
  @Trace rtmpUrl: string = DEFAULT_RTMP_URL;
  @Trace preset: string = '720p';
  @Trace fps: number = 30;
  @Trace videoBitrateKbps: number = 4000;
  @Trace audioMode: string = 'inner';
  @Trace settingsLocked: boolean = false;

  // —— 三平面状态镜像 ——
  @Trace captureStatus: string = CaptureStatus.IDLE;
  @Trace streamStatus: string = StreamStatus.IDLE;
  @Trace recordStatus: string = RecordStatus.IDLE;

  // —— 统计面板 ——
  @Trace streamFps: string = '0';
  @Trace streamBitrate: string = '0';
  @Trace sentBytes: string = '0';
  @Trace streamDuration: string = '0s';
  @Trace recordDuration: string = '0s';
  @Trace reconnectAttempt: number = 0;

  // —— 错误/提示 ——
  @Trace errorText: string = '';
  @Trace errorCode: number = -1;
  @Trace showErrorDialog: boolean = false;

  // —— 麦克风降级编排 ——
  @Trace pendingMicDegradeConfirm: boolean = false;
  @Trace micDegraded: boolean = false;

  context: common.UIAbilityContext | null = null;
  // 弹窗回调（由视图注册，vm 事件驱动触发错误/降级确认弹窗）
  dialogErrorHandler: (() => void) | null = null;
  dialogDegradeHandler: (() => void) | null = null;
  private service: MediaStreamService;
  private listener: MediaStreamEventListener | null = null;
  private pendingAction: string = 'stream';

  constructor() {
    this.service = MediaStreamService.getInstance();
    this.listener = new ConsoleListener(this);
    this.service.addListener(this.listener);
  }

  // 页面销毁时解绑监听，但不再误停会话（后台采集预期）
  dispose(): void {
    if (this.listener) {
      this.service.removeListener(this.listener);
      this.listener = null;
    }
  }

  // 初始化 async：绑定上下文 → 初始化配置仓库 → 加载配置 → 初始化原生
  async initAsync(context: common.UIAbilityContext): Promise<void> {
    this.context = context;
    await PreferencesRepository.getInstance().init(context);
    await this.loadConfig();
    this.service.init(context.filesDir);
  }

  async loadConfig(): Promise<void> {
    const loaded = await PreferencesRepository.getInstance().load();
    // 未配置过（或存的是空串）时回落到局域网默认地址，保证“直接点推流”可测
    this.rtmpUrl = loaded.rtmpUrl.trim().length === 0 ? DEFAULT_RTMP_URL : loaded.rtmpUrl;
    this.preset = loaded.preset;
    this.fps = loaded.fps;
    this.videoBitrateKbps = loaded.videoBitrateKbps;
    this.audioMode = loaded.audioMode === '' ? 'inner' : loaded.audioMode;
  }

  // 变更即持久化（US5）
  persistConfig(): void {
    if (this.settingsLocked) {
      return;
    }
    const s = new StreamSettings();
    s.rtmpUrl = this.rtmpUrl;
    s.preset = this.preset;
    s.fps = this.fps;
    s.videoBitrateKbps = this.videoBitrateKbps;
    s.audioMode = this.audioMode;
    PreferencesRepository.getInstance().save(s).catch((e: Error) => {
      hilog.error(DOMAIN, TAG, 'persist config err %{public}s', JSON.stringify(e));
    });
  }

  // —— 操作编排 ——

  isStreamingActive(): boolean {
    return this.streamStatus === StreamStatus.STREAMING ||
      this.streamStatus === StreamStatus.CONNECTING ||
      this.streamStatus === StreamStatus.RECONNECTING;
  }

  isRecordingActive(): boolean {
    return this.recordStatus === RecordStatus.RECORDING;
  }

  toggleStream(): void {
    if (this.isStreamingActive()) {
      this.stopStream();
    } else {
      this.startStream();
    }
  }

  toggleRecord(): void {
    if (this.isRecordingActive()) {
      this.stopRecord();
    } else {
      this.startRecord();
    }
  }

  startStream(): void {
    if (this.rtmpUrl.trim().length === 0) {
      this.errorText = errorMessage(7); // RTMP_URL_INVALID
      return;
    }
    if (!this.rtmpUrl.startsWith('rtmp://')) {
      this.errorText = errorMessage(7);
      return;
    }
    this.errorText = '';
    this.persistConfig();
    this.gateMic('stream');
  }

  startRecord(): void {
    this.errorText = '';
    this.persistConfig();
    this.gateMic('record');
  }

  private gateMic(action: string): void {
    const needsMic = this.audioMode === 'mic' || this.audioMode === 'micInner';
    if (needsMic && this.context) {
      if (!PermissionService.getInstance().hasMicPermission(this.context)) {
        // 未授权：先自动发起麦克风权限申请（系统弹窗），授予后继续；拒绝再降级为"仅内录继续/取消"
        this.pendingAction = action;
        PermissionService.getInstance().requestMicPermission(this.context).then(granted => {
          if (!granted) {
            // 用户拒绝授权 + 未勾选"不再询问"：弹出仅内录继续/取消
            this.pendingMicDegradeConfirm = true;
            if (this.dialogDegradeHandler) {
              this.dialogDegradeHandler();
            }
          } else {
            // 授权成功，直接开始
            this.pendingMicDegradeConfirm = false;
            this.applyAction(action);
          }
        });
        return;
      }
    }
    this.applyAction(action);
  }

  // 用户选择"继续=仅内录"：降级音频源为 inner 后开始
  confirmDegrade(): void {
    this.pendingMicDegradeConfirm = false;
    this.audioMode = 'inner';
    this.micDegraded = true;
    this.persistConfig();
    const action = this.pendingAction;
    this.pendingAction = 'stream';
    this.applyAction(action);
  }

  // 用户选择"取消"：中止
  cancelDegrade(): void {
    this.pendingMicDegradeConfirm = false;
  }

  dismissError(): void {
    this.showErrorDialog = false;
  }

  private async applyAction(action: string): Promise<void> {
    const ctx = this.context;
    if (!ctx) {
      return;
    }
    // 长时任务 → 引擎指令
    await BackgroundTaskService.getInstance().start(ctx);
    const s = new StreamSettings();
    s.rtmpUrl = this.rtmpUrl;
    s.preset = this.preset;
    s.fps = this.fps;
    s.videoBitrateKbps = this.videoBitrateKbps;
    s.audioMode = this.audioMode;
    if (action === 'record') {
      this.service.startRecording(s);
    } else {
      this.service.startStreaming(s);
    }
  }

  private async releaseBackgroundIfIdle(): Promise<void> {
    const ctx = this.context;
    if (!ctx) {
      return;
    }
    if (!this.isStreamingActive() && !this.isRecordingActive()) {
      await BackgroundTaskService.getInstance().stop(ctx);
    }
  }

  stopStream(): void {
    this.service.stopStreaming();
    // 推流已停止，若录制也未运行则无需再挂长时任务
    this.releaseBackgroundIfIdle();
  }

  stopRecord(): void {
    this.service.stopRecording();
    this.releaseBackgroundIfIdle();
  }

  // —— 事件镜像 ——

  applyCaptureState(state: CaptureStatus): void {
    this.captureStatus = state;
    this.updateLocked();
  }

  applyStreamState(state: StreamStatus): void {
    this.streamStatus = state;
    this.updateLocked();
  }

  applyRecordState(state: RecordStatus): void {
    this.recordStatus = state;
    this.updateLocked();
  }

  applyStats(stats: SessionStats): void {
    const fpsVal = stats.videoFps;
    if (typeof fpsVal === 'number') {
      this.streamFps = fpsVal.toFixed(0);
    }
    const bitVal = stats.videoBitrateKbps;
    if (typeof bitVal === 'number') {
      this.streamBitrate = bitVal.toFixed(0);
    }
    const sentVal = stats.sentBytes;
    if (typeof sentVal === 'number') {
      this.sentBytes = (sentVal / 1024).toFixed(1);
    }
    const sd = stats.streamDurationMs;
    if (typeof sd === 'number' && sd > 0) {
      this.streamDuration = this.formatMs(sd);
    }
    const rd = stats.recordDurationMs;
    if (typeof rd === 'number' && rd > 0) {
      this.recordDuration = this.formatMs(rd);
    }
    const ra = stats.reconnectAttempt;
    if (typeof ra === 'number' && ra > 0) {
      this.reconnectAttempt = ra;
      this.errorText = `网络异常，重连中 (${ra}/3)`;
    }
  }

  applyRecordFinished(meta: RecordingMeta, reason: string): void {
    if (reason === 'normal') {
      try {
        RecordingsRepository.getInstance().add(meta);
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'index record err %{public}s', JSON.stringify(e));
      }
    }
    hilog.info(DOMAIN, TAG, 'record finished %{public}s reason=%{public}s', meta.fileName, reason);
  }

  applyError(code: number, message: string, source: string): void {
    this.errorCode = code;
    // 优先使用 Type 中错误码中文文案表；无则回退原生 message
    const text = errorMessage(code);
    this.errorText = text.length > 0 && text !== '未知错误' ? text : message;
    this.showErrorDialog = true;
    if (this.dialogErrorHandler) {
      this.dialogErrorHandler();
    }
    hilog.error(DOMAIN, TAG, 'error code=%{public}d src=%{public}s msg=%{public}s', code, source, message);
  }

  applyMicDegraded(message: string): void {
    this.micDegraded = true;
    this.errorText = message.length > 0 ? message : '麦克风无数据，已切换为仅内录';
  }

  private updateLocked(): void {
    this.settingsLocked = this.isStreamingActive() || this.isRecordingActive();
  }

  private formatMs(ms: number): string {
    const s = Math.floor(ms / 1000);
    const mm = Math.floor(s / 60);
    const ss = s % 60;
    return `${mm}:${ss < 10 ? '0' : ''}${ss}`;
  }
}