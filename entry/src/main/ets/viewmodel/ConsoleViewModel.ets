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
  composeRtmpUrl,
} from '../service/MediaStreamService';
import { ErrorCode } from 'media_stream';
import { PreferencesRepository } from '../data/PreferencesRepository';
import { RecordingsRepository } from '../data/RecordingsRepository';
import { PermissionService, MicAuthResult } from '../service/PermissionService';
import { parseRtmpTarget, probeRtmpServer, RtmpTarget } from '../service/RtmpProbe';
import { BackgroundTaskService } from '../service/BackgroundTaskService';
import { errorMessage } from '../model/Types';

const DOMAIN = 0x0000;
const TAG = 'ConsoleViewModel';

// 默认推流地址：指向 Mac 上 node-media-server 起的局域网 RTMP 服务。
// 按「推流地址 + 推流码」拆开给（对齐抖音/B站），拼接后即 rtmp://192.168.43.10/live/stream。
// 便于“打开即点推流”完成闭环测试；用户仍可在输入框改成任意地址，改后持久化覆盖默认值。
const DEFAULT_RTMP_ADDR = 'rtmp://192.168.43.10/live';
const DEFAULT_STREAM_KEY = 'stream';

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
  @Trace rtmpUrl: string = DEFAULT_RTMP_ADDR;
  // 推流码（流名）：与推流地址分开填写，推流时拼成完整 RTMP URL 交给原生。
  @Trace streamKey: string = DEFAULT_STREAM_KEY;
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
  @Trace streamDuration: string = '0:00';
  @Trace recordDuration: string = '0:00';
  @Trace reconnectAttempt: number = 0;
  // 推流地址连通性预检中（按钮显示「检测服务器…」并禁止重复点击）
  @Trace probing: boolean = false;
  // 麦克风授权镜像：用于在音频源下方给出「未授权 → 点击授权」入口
  @Trace micGranted: boolean = false;
  // 上一次麦克风申请走到哪一级（应用级/设置级），决定降级弹窗文案
  @Trace micAuthStage: string = '';

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
    this.refreshMicState();
  }

  async loadConfig(): Promise<void> {
    const loaded = await PreferencesRepository.getInstance().load();
    let addr = loaded.rtmpUrl.trim();
    let key = loaded.streamKey.trim();
    // 兼容旧配置：加入推流码之前，「地址 + 推流码」是整串存在 rtmpUrl 里的
    // （如 rtmp://host/live/stream）。若尚未单独存过推流码，且地址带两段以上路径，
    // 就把最后一段拆成推流码，避免升级后拼成 .../stream/stream 推错流。
    if (key.length === 0 && addr.length > 0) {
      const schemeEnd = addr.indexOf('://');
      const pathStart = schemeEnd >= 0 ? addr.indexOf('/', schemeEnd + 3) : -1;
      const lastSlash = addr.lastIndexOf('/');
      if (pathStart > 0 && lastSlash > pathStart && lastSlash < addr.length - 1) {
        key = addr.substring(lastSlash + 1);
        addr = addr.substring(0, lastSlash);
      }
    }
    // 未配置过（或存的是空串）时回落到局域网默认地址，保证“直接点推流”可测
    this.rtmpUrl = addr.length === 0 ? DEFAULT_RTMP_ADDR : addr;
    this.streamKey = key.length === 0 ? DEFAULT_STREAM_KEY : key;
    this.preset = loaded.preset;
    this.fps = loaded.fps;
    this.videoBitrateKbps = loaded.videoBitrateKbps;
    this.audioMode = loaded.audioMode === '' ? 'inner' : loaded.audioMode;
  }

  // 拼接后的完整推流地址（推流地址 + 推流码），预检与展示都以它为准
  buildRtmpUrl(): string {
    return composeRtmpUrl(this.rtmpUrl, this.streamKey);
  }

  // 变更即持久化（US5）
  persistConfig(): void {
    if (this.settingsLocked) {
      return;
    }
    const s = new StreamSettings();
    s.rtmpUrl = this.rtmpUrl;
    s.streamKey = this.streamKey;
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
    // 预检与推流都以「地址 + 推流码」拼接后的完整 URL 为准
    const target: RtmpTarget | null = parseRtmpTarget(this.buildRtmpUrl());
    if (target === null) {
      this.errorText = errorMessage(ErrorCode.RTMP_URL_INVALID);
      return;
    }
    if (this.probing) {
      return; // 预检进行中，忽略重复点击
    }
    // 取出标量，避免在回调闭包里依赖可空对象的类型收窄
    const host = target.host;
    const port = target.port;
    this.errorText = '';
    this.persistConfig();
    this.probing = true;
    // 「不存在的服务器不应能开始推流」：先做一次 TCP 可达性预检，不可达直接中止，
    // 不进入采集/推流（否则会白弹一次录屏授权、界面先切到「连接中」再报错）。
    probeRtmpServer(target).then((reachable: boolean) => {
      this.probing = false;
      if (!reachable) {
        this.failWith(ErrorCode.RTMP_CONNECT_FAIL,
          `无法连接到服务器 ${host}:${port}，请确认服务器已启动且与本机网络互通`);
        return;
      }
      this.gateMic('stream');
    }).catch((e: Error) => {
      this.probing = false;
      hilog.error(DOMAIN, TAG, 'probe server err %{public}s', JSON.stringify(e));
      this.failWith(ErrorCode.RTMP_CONNECT_FAIL, '推流服务器连通性检查失败，请重试');
    });
  }

  // 当前音频源是否需要麦克风
  needsMic(): boolean {
    return this.audioMode === 'mic' || this.audioMode === 'micInner';
  }

  // 同步麦克风授权镜像（授权弹窗/设置页返回后调用）
  refreshMicState(): void {
    const ctx = this.context;
    if (!ctx) {
      return;
    }
    try {
      this.micGranted = PermissionService.getInstance().hasMicPermission(ctx);
    } catch (e) {
      this.micGranted = false;
    }
  }

  // 用户点击「麦克风未授权」提示时手动发起授权，链路与开始推流时一致（应用级 → 设置级）
  requestMicAuth(): void {
    const ctx = this.context;
    if (!ctx) {
      return;
    }
    PermissionService.getInstance().ensureMicPermission(ctx).then((auth: MicAuthResult) => {
      this.micAuthStage = auth;
      this.micGranted = auth === MicAuthResult.GRANTED;
      if (auth === MicAuthResult.GUIDED_TO_SETTINGS) {
        this.errorText = '请在系统设置中开启麦克风权限';
      }
    }).catch((e: Error) => {
      hilog.error(DOMAIN, TAG, 'requestMicAuth err %{public}s', JSON.stringify(e));
    });
  }

  // 与原生 error 事件区分：调用方已给出更具体的文案（如带 host:port），不查错误码表直接展示
  private failWith(code: number, detail: string): void {
    this.errorCode = code;
    this.errorText = detail;
    this.showErrorDialog = true;
    if (this.dialogErrorHandler) {
      this.dialogErrorHandler();
    }
    hilog.error(DOMAIN, TAG, 'fail code=%{public}d msg=%{public}s', code, detail);
  }

  startRecord(): void {
    this.errorText = '';
    this.persistConfig();
    this.gateMic('record');
  }

  private gateMic(action: string): void {
    const ctx = this.context;
    if (!this.needsMic() || !ctx) {
      this.applyAction(action);
      return;
    }
    if (PermissionService.getInstance().hasMicPermission(ctx)) {
      this.micGranted = true;
      this.applyAction(action);
      return;
    }
    // 两级申请：应用级系统弹窗 → 仍被拒则拉起权限设置弹窗（把用户直接送到麦克风开关前）。
    // 弹窗异步返回，任一级通过即自动继续；两级都不通过才弹「仅内录继续/取消」。
    this.pendingAction = action;
    PermissionService.getInstance().ensureMicPermission(ctx).then((auth: MicAuthResult) => {
      this.refreshMicState();
      this.micAuthStage = auth;
      if (auth === MicAuthResult.GRANTED) {
        this.pendingMicDegradeConfirm = false;
        this.applyAction(action);
        return;
      }
      if (auth === MicAuthResult.GUIDED_TO_SETTINGS) {
        // 已把用户送到系统设置页：此刻不能判定失败（用户尚未操作），也不弹降级确认，
        // 只留一行提示；用户在设置里打开麦克风后重新点开始即可。
        this.pendingMicDegradeConfirm = false;
        this.errorText = '请在系统设置中开启麦克风权限后重新开始';
        return;
      }
      this.pendingMicDegradeConfirm = true;
      if (this.dialogDegradeHandler) {
        this.dialogDegradeHandler();
      }
    }).catch((e: Error) => {
      hilog.error(DOMAIN, TAG, 'ensure mic permission err %{public}s', JSON.stringify(e));
      this.micAuthStage = MicAuthResult.DENIED_SETTING_LEVEL;
      this.pendingMicDegradeConfirm = true;
      if (this.dialogDegradeHandler) {
        this.dialogDegradeHandler();
      }
    });
  }

  // 用户选择"继续=仅内录"：降级音频源为 inner 后开始
  confirmDegrade(): void {
    this.pendingMicDegradeConfirm = false;
    this.micAuthStage = '';
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
    s.streamKey = this.streamKey;
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
    const wasActive = this.isStreamingActive(); // 用「变更前」的状态判断这是否是一轮新会话
    this.streamStatus = state;
    if (state === StreamStatus.CONNECTING && !wasActive) {
      // 新一轮推流：时长与速率归零（时长终值由停止回调冻结后推来）。
      // 重连时不做归零，否则每次重连显示都会闪回 0:00。
      this.streamDuration = '0:00';
      this.streamFps = '0';
      this.streamBitrate = '0';
    }
    // 推流结束后统计线程不再上报，fps/码率会停在最后一帧的值（真机现象：已停止仍显示 12fps/617kbps）。
    // 但录制还在跑时编码器仍在出帧，此时不能清零（否则纯录制场景的速率显示会一直被推流状态带没了）。
    if ((state === StreamStatus.IDLE || state === StreamStatus.STOPPED) && !this.isRecordingActive()) {
      this.streamFps = '0';
      this.streamBitrate = '0';
    }
    this.updateLocked();
  }

  applyRecordState(state: RecordStatus): void {
    this.recordStatus = state;
    if (state === RecordStatus.RECORDING) {
      this.recordDuration = '0:00'; // 新一段录制从 0 起算（终值由 onFinished 推来）
    }
    this.updateLocked();
  }

  applyStats(stats: SessionStats): void {
    // 瞬时速率只在「有输出在跑」时更新：只要推流或录制任一路活跃，编码器就在出帧，速率就该刷新
    // （此前只认 isStreamingActive()，导致纯录制时下方 fps/码率永远不动）。
    // 两路都停时清零，顺带处理停止后那个「在途」的 stats 事件（否则会把刚清零的值又填回去）。
    if (this.isStreamingActive() || this.isRecordingActive()) {
      const fpsVal = stats.videoFps;
      if (typeof fpsVal === 'number') {
        this.streamFps = fpsVal.toFixed(0);
      }
      const bitVal = stats.videoBitrateKbps;
      if (typeof bitVal === 'number') {
        this.streamBitrate = bitVal.toFixed(0);
      }
    } else {
      this.streamFps = '0';
      this.streamBitrate = '0';
    }
    // 时长一律照收：数值只由原生在「运行中」或「停止回调里冻结」时给出，
    // 不会出现中断后还在涨的情况，UI 侧无需再加门控。
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