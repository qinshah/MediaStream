// 录制列表视图：按创建时间倒序 List，含播放 Sheet / 删除确认 / 系统分享 / 无效标记与清理（US4/T026）
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RecordingsViewModel } from '../viewmodel/RecordingsViewModel';
import { RecordingItem } from '../data/RecordingsRepository';

const DOMAIN = 0x0000;
const TAG = 'RecordingsView';

@ComponentV2
export struct RecordingsView {
  @Local vm: RecordingsViewModel = new RecordingsViewModel();

  aboutToAppear(): void {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    this.vm.loadAsync(ctx).then(() => {
      hilog.info(DOMAIN, TAG, 'recordings loaded %{public}d', this.vm.recordings.length);
    }).catch((e: Error) => {
      hilog.error(DOMAIN, TAG, 'recordings load err %{public}s', JSON.stringify(e));
    });
  }

  // 删除确认
  presentDelete(item: RecordingItem): void {
    this.getUIContext().showAlertDialog({
      title: '删除录制',
      message: `确定删除"${item.fileName}"吗？该操作同时删除文件与列表索引。`,
      primaryButton: {
        value: '删除',
        action: () => {
          this.vm.remove(item);
        },
      },
      secondaryButton: {
        value: '取消',
        action: () => {
        },
      },
    });
  }

  onShare(path: string): void {
    this.vm.share(path).then((ok: boolean) => {
      if (!ok) {
        this.getUIContext().getPromptAction().showToast({ message: '分享失败，请重试' });
      }
    }).catch((e: Error) => {
      hilog.error(DOMAIN, TAG, 'share err %{public}s', JSON.stringify(e));
    });
  }

  formatSize(bytes: number): string {
    if (bytes >= 1048576) {
      return `${(bytes / 1048576).toFixed(1)} MB`;
    }
    if (bytes >= 1024) {
      return `${(bytes / 1024).toFixed(0)} KB`;
    }
    return `${bytes} B`;
  }

  formatTime(ms: number): string {
    const d = new Date(ms);
    const pad = (n: number): string => (n < 10 ? `0${n}` : `${n}`);
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }

  build() {
    Column() {
      if (this.vm.errorText.length > 0) {
        Text(this.vm.errorText)
          .fontSize(13)
          .fontColor('#D93026')
          .width('100%')
          .padding(12)
      }

      // 播放 Sheet（沙箱路径）
      if (this.vm.playingPath.length > 0) {
        Column({ space: 8 }) {
          Video({ src: `file://${this.vm.playingPath}` })
            .width('100%')
            .height(220)
            .controls(true)
            .autoPlay(true)
          Button('收起')
            .fontSize(13)
            .height(30)
            .onClick(() => {
              this.vm.playingPath = '';
            })
        }
        .width('100%')
        .padding({ left: 16, right: 16, bottom: 8 })
      }

      // 无效残留文件清理入口
      if (this.vm.recordings.some((it: RecordingItem) => it.invalid)) {
        Button('清理无效残留')
          .fontSize(13)
          .height(32)
          .backgroundColor('#F5A623')
          .onClick(() => {
            this.vm.cleanup();
            this.getUIContext().getPromptAction().showToast({ message: '已清理无效残留文件' });
          })
          .margin({ left: 16, top: 4, bottom: 4 })
      }

      if (this.vm.recordings.length === 0) {
        Blank()
        Text('暂无录制文件')
          .fontSize(14)
          .fontColor('#999999')
        Blank()
      } else {
        List({ space: 10 }) {
          ForEach(this.vm.recordings, (item: RecordingItem) => {
            ListItem() {
              Column({ space: 6 }) {
                Row() {
                  Column({ space: 4 }) {
                    Row({ space: 6 }) {
                      Text(item.fileName)
                        .fontSize(15)
                        .fontWeight(FontWeight.Medium)
                        .maxLines(1)
                        .textOverflow({ overflow: TextOverflow.Ellipsis })
                      if (item.invalid) {
                        Text('无效')
                          .fontSize(10)
                          .fontColor('#FFFFFF')
                          .backgroundColor('#CCCCCC')
                          .borderRadius(4)
                          .padding({ left: 4, right: 4 })
                      }
                    }
                    Row({ space: 8 }) {
                      Text(this.formatSize(item.sizeBytes))
                        .fontSize(12)
                        .fontColor('#888888')
                      Text(this.formatTime(item.createdAtMs))
                        .fontSize(12)
                        .fontColor('#888888')
                    }
                  }
                  .layoutWeight(1)
                  .alignItems(HorizontalAlign.Start)
                }
                .width('100%')

                Row({ space: 8 }) {
                  Button('播放')
                    .fontSize(12)
                    .height(30)
                    .backgroundColor('#0A59F7')
                    .onClick(() => this.vm.togglePlay(item))
                  Button('分享')
                    .fontSize(12)
                    .height(30)
                    .backgroundColor('#18A058')
                    .onClick(() => this.onShare(item.filePath))
                  Button('删除')
                    .fontSize(12)
                    .height(30)
                    .backgroundColor('#D93026')
                    .onClick(() => this.presentDelete(item))
                }
              }
              .width('100%')
              .padding(12)
              .backgroundColor('#F7F7F7')
              .borderRadius(10)
            }
          }, (item: RecordingItem) => item.filePath)
        }
        .width('100%')
        .layoutWeight(1)
        .padding({ left: 16, right: 16 })
      }
    }
    .width('100%')
    .height('100%')
  }
}