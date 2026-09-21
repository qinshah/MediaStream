// 控制台视图：推流 + 录制全部交互 UI（T020/T023/T025/T028）
// 含 RTMP 输入 / 开始停止推流 / 开始停止录制 / 画质预设 chips / 音频源 chips /
// 状态徽标 / 统计面板 / 错误 AlertDialog / 授权拒绝引导 / 麦克风降级确认 / 录制列表入口
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { ConsoleViewModel } from '../viewmodel/ConsoleViewModel';
import { PRESETS, AUDIO_SOURCES, ROUTE_RECORDINGS, PresetOption, AudioOption } from '../model/Types';
import { StreamStatus, RecordStatus, CaptureStatus } from '../service/MediaStreamService';

const DOMAIN = 0x0000;
const TAG = 'ConsoleView';

@ComponentV2
export struct ConsoleView {
  @Local vm: ConsoleViewModel = new ConsoleViewModel();
  // 宿主 Navigation 的路由栈（由 Index 传入，用于跳转录制列表）
  @Param pathStack: NavPathStack = new NavPathStack();

  aboutToAppear(): void {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    // 注册弹窗回调：错误/降级确认由 vm 事件驱动触发
    this.vm.dialogErrorHandler = () => this.presentError();
    this.vm.dialogDegradeHandler = () => this.presentDegrade();
    this.vm.initAsync(ctx).then(() => {
      hilog.info(DOMAIN, TAG, 'console vm init ok');
    }).catch((e: Error) => {
      hilog.error(DOMAIN, TAG, 'console vm init err %{public}s', JSON.stringify(e));
    });
  }

  aboutToDisappear(): void {
    // 页面销毁不误停会话（后台采集预期），仅解绑监听
    this.vm.dialogErrorHandler = null;
    this.vm.dialogDegradeHandler = null;
    this.vm.dispose();
  }

  presentError(): void {
    this.getUIContext().showAlertDialog({
      title: '操作失败',
      message: this.vm.errorText,
      confirm: {
        value: '确定',
        action: () => {
          this.vm.dismissError();
        },
      },
    });
  }

  presentDegrade(): void {
    this.getUIContext().showAlertDialog({
      title: '麦克风未授权',
      message: '未获得麦克风权限，是否仅录制系统声音继续？',
      primaryButton: {
        value: '继续',
        action: () => {
          this.vm.confirmDegrade();
        },
      },
      secondaryButton: {
        value: '取消',
        action: () => {
          this.vm.cancelDegrade();
        },
      },
    });
  }

  openRecordings(): void {
    this.pathStack.pushPathByName(ROUTE_RECORDINGS, null, true);
  }

  build() {
    Column() {
      // —— 标题栏 ——
      Row() {
        Text('屏幕录制 · 推流')
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
        Blank()
        Button('录制列表')
          .fontSize(14)
          .height(32)
          .onClick(() => this.openRecordings())
      }
      .width('100%')
      .padding(16)

      Scroll() {
        Column({ space: 16 }) {
          // —— 推流地址 ——
          TextInput({ placeholder: 'rtmp://服务器/app/stream', text: this.vm.rtmpUrl })
            .width('100%')
            .enabled(!this.vm.settingsLocked)
            .onChange((v: string) => {
              this.vm.rtmpUrl = v;
            })

          // —— 画质预设 ——
          Column({ space: 6 }) {
            Row() {
              Text('画质（下次会话生效）')
                .fontSize(12)
                .fontColor('#888888')
              if (this.vm.settingsLocked) {
                Text('会话中锁定')
                  .fontSize(11)
                  .fontColor('#F5A623')
              }
            }
            .width('100%')

            Row({ space: 8 }) {
              ForEach(PRESETS, (it: PresetOption) => {
                this.presetChip(it)
              }, (it: PresetOption) => it.value)
            }
            .width('100%')
          }
          .width('100%')

          // —— 帧率 / 码率 ——
          Row({ space: 8 }) {
            TextInput({ text: `${this.vm.fps}` })
              .type(InputType.Number)
              .width(80)
              .enabled(!this.vm.settingsLocked)
              .onChange((v: string) => {
                this.vm.fps = Math.max(10, Math.min(60, parseInt(v, 10) || 30));
              })
            Text('fps')
            TextInput({ text: `${this.vm.videoBitrateKbps}` })
              .type(InputType.Number)
              .width(100)
              .enabled(!this.vm.settingsLocked)
              .onChange((v: string) => {
                this.vm.videoBitrateKbps = Math.max(200, Math.min(20000, parseInt(v, 10) || 4000));
              })
            Text('kbps')
          }
          .width('100%')

          // —— 音频源 ——
          Row({ space: 8 }) {
            Text('音频')
            this.audioChip()
          }
          .width('100%')

          // —— 状态徽标 ——
          this.statusCard()

          // —— 非弹窗型错误行 ——
          if (this.vm.errorText.length > 0 && !this.vm.showErrorDialog && !this.vm.pendingMicDegradeConfirm) {
            Text(this.vm.errorText)
              .fontSize(13)
              .fontColor('#D93026')
              .width('100%')
          }

          // —— 操作按钮 ——
          Row({ space: 12 }) {
            Button(this.vm.isStreamingActive() ? '停止推流' : '开始推流')
              .width('48%')
              .height(48)
              .backgroundColor(this.vm.isStreamingActive() ? '#D93026' : '#0A59F7')
              .onClick(() => this.vm.toggleStream())

            Button(this.vm.isRecordingActive() ? '停止录制' : '开始录制')
              .width('48%')
              .height(48)
              .backgroundColor(this.vm.isRecordingActive() ? '#D93026' : '#18A058')
              .onClick(() => this.vm.toggleRecord())
          }
          .width('100%')
        }
        .width('100%')
      }
      .layoutWeight(1)
      .padding({ left: 16, right: 16 })

      // —— 统计面板（录制时长单列展示）——
      Column({ space: 4 }) {
        Row({ space: 16 }) {
          // 注意：这里必须用「无参 @Builder」而非 statItem(label, value) 形式。
          // @Builder 的值参数是快照，@Trace 字段变化后不会重算，统计会永远停在初值 0
          // （真机现象：内联的「推流时长」正常刷新，而同区域的 fps/码率/已发恒为 0）。
          this.statFps()
          this.statBitrate()
          this.statSent()
          this.statRecord()
        }
        Text(`推流时长 ${this.vm.streamDuration}`)
          .fontSize(12)
          .fontColor('#888888')
      }
      .width('100%')
      .padding(16)
      .border({ width: { top: 1 }, color: '#EEEEEE' })
    }
    .width('100%')
    .height('100%')
  }

  @Builder
  presetChip(it: PresetOption) {
    Text(it.label)
      .fontSize(14)
      .padding({ left: 14, right: 14, top: 6, bottom: 6 })
      .backgroundColor(this.vm.preset === it.value ? '#0A59F7' : '#F0F0F0')
      .fontColor(this.vm.preset === it.value ? '#FFFFFF' : '#333333')
      .borderRadius(16)
      .enabled(!this.vm.settingsLocked)
      .onClick(() => {
        this.vm.preset = it.value;
        this.vm.videoBitrateKbps = it.bitrateKbps;
      })
  }

  @Builder
  audioChip() {
    Row({ space: 8 }) {
      ForEach(AUDIO_SOURCES, (it: AudioOption) => {
        Text(it.label)
          .fontSize(14)
          .padding({ left: 14, right: 14, top: 6, bottom: 6 })
          .backgroundColor(this.vm.audioMode === it.value ? '#0A59F7' : '#F0F0F0')
          .fontColor(this.vm.audioMode === it.value ? '#FFFFFF' : '#333333')
          .borderRadius(16)
          .enabled(!this.vm.settingsLocked)
          .onClick(() => {
            this.vm.audioMode = it.value;
          })
      }, (it: AudioOption) => it.value)
    }
  }

  @Builder
  statusCard() {
    Column({ space: 8 }) {
      Row({ space: 16 }) {
        this.dotCapture()
        this.dotStream()
        this.dotRecord()
      }
      if (this.vm.reconnectAttempt > 0 && this.vm.streamStatus === StreamStatus.RECONNECTING) {
        Text(`重连中 (${this.vm.reconnectAttempt}/3)`)
          .fontSize(12)
          .fontColor('#F5A623')
      }
    }
    .width('100%')
    .padding(16)
    .borderRadius(12)
    .backgroundColor('#F7F7F7')
  }

  // 三个无参 @Builder：@Builder 的「值参数」在首次构建时即快照，传 this.vm.xxxStatus 后
  // @Trace 变化不会触发重算（真机现象：推流中状态行仍显示「空闲」、圆点恒灰）。
  // 与统计面板同理，必须在 @Builder 内部直读 @Trace 字段才能响应式刷新。
  @Builder
  dotCapture() {
    Row({ space: 6 }) {
      Circle({ width: 10, height: 10 })
        .fill(this.captureColor(this.vm.captureStatus))
      Text(`采集: ${this.statusLabel(this.vm.captureStatus)}`)
        .fontSize(13)
    }
  }

  @Builder
  dotStream() {
    Row({ space: 6 }) {
      Circle({ width: 10, height: 10 })
        .fill(this.streamColor(this.vm.streamStatus))
      Text(`推流: ${this.statusLabel(this.vm.streamStatus)}`)
        .fontSize(13)
    }
  }

  @Builder
  dotRecord() {
    Row({ space: 6 }) {
      Circle({ width: 10, height: 10 })
        .fill(this.recordColor(this.vm.recordStatus))
      Text(`录制: ${this.statusLabel(this.vm.recordStatus)}`)
        .fontSize(13)
    }
  }

  captureColor(status: string): string {
    if (status === CaptureStatus.ACTIVE) {
      return '#18A058';
    }
    if (status === CaptureStatus.ERROR) {
      return '#D93026';
    }
    if (status === CaptureStatus.IDLE) {
      return '#CCCCCC';
    }
    return '#F5A623';
  }

  streamColor(status: string): string {
    if (status === StreamStatus.STREAMING) {
      return '#18A058';
    }
    if (status === StreamStatus.CONNECTING || status === StreamStatus.RECONNECTING) {
      return '#F5A623';
    }
    if (status === StreamStatus.ERROR) {
      return '#D93026';
    }
    return '#CCCCCC';
  }

  recordColor(status: string): string {
    if (status === RecordStatus.RECORDING) {
      return '#D93026';
    }
    if (status === RecordStatus.ERROR) {
      return '#D93026';
    }
    return '#CCCCCC';
  }

  statusLabel(status: string): string {
    switch (status) {
      case CaptureStatus.ACTIVE: return '运行中';
      case CaptureStatus.STARTING: return '启动中';
      case CaptureStatus.STOPPING: return '停止中';
      case CaptureStatus.ERROR: return '异常';
      case StreamStatus.STREAMING: return '推流中';
      case StreamStatus.CONNECTING: return '连接中';
      case StreamStatus.RECONNECTING: return '重连中';
      case StreamStatus.STOPPED: return '已停止';
      case RecordStatus.RECORDING: return '录制中';
      default: return '空闲';
    }
  }

  @Builder
  statFps() {
    Column({ space: 2 }) {
      Text(this.vm.streamFps)
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
      Text('fps')
        .fontSize(11)
        .fontColor('#888888')
    }
  }

  @Builder
  statBitrate() {
    Column({ space: 2 }) {
      Text(`${this.vm.streamBitrate} kbps`)
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
      Text('码率')
        .fontSize(11)
        .fontColor('#888888')
    }
  }

  @Builder
  statSent() {
    Column({ space: 2 }) {
      Text(`${this.vm.sentBytes} KB`)
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
      Text('已发')
        .fontSize(11)
        .fontColor('#888888')
    }
  }

  @Builder
  statRecord() {
    Column({ space: 2 }) {
      Text(this.vm.recordDuration)
        .fontSize(15)
        .fontWeight(FontWeight.Medium)
      Text('录制')
        .fontSize(11)
        .fontColor('#888888')
    }
  }
}