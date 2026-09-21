// 录制列表视图模型：加载/删除/分享/清理操作编排（US4/T027）
import { common } from '@kit.AbilityKit';
import { fileUri } from '@kit.CoreFileKit';
import { systemShare } from '@kit.ShareKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RecordingsRepository, RecordingItem } from '../data/RecordingsRepository';

const DOMAIN = 0x0000;
const TAG = 'RecordingsViewModel';

@ObservedV2
export class RecordingsViewModel {
  @Trace recordings: Array<RecordingItem> = [];
  @Trace loading: boolean = false;
  @Trace errorText: string = '';
  @Trace playingPath: string = '';

  context: common.UIAbilityContext | null = null;

  constructor() {
  }

  // 从沙箱 videos 目录重建列表（含孤儿文件检测标记）
  async loadAsync(context: common.UIAbilityContext): Promise<void> {
    this.context = context;
    this.loading = true;
    try {
      this.recordings = await RecordingsRepository.getInstance().refresh(context.filesDir);
      this.errorText = '';
    } catch (e) {
      this.errorText = '读取录制目录失败';
      hilog.error(DOMAIN, TAG, 'load recordings err %{public}s', JSON.stringify(e));
    }
    this.loading = false;
  }

  refresh(): void {
    if (this.context) {
      this.loadAsync(this.context).catch((e: Error) => {
        hilog.error(DOMAIN, TAG, 'refresh err %{public}s', JSON.stringify(e));
      });
    }
  }

  // 播放 Sheet：点击条目展开/收起沙箱路径 Video
  togglePlay(item: RecordingItem): void {
    this.playingPath = this.playingPath === item.filePath ? '' : item.filePath;
  }

  remove(item: RecordingItem): void {
    RecordingsRepository.getInstance().remove(item.filePath);
    this.recordings = RecordingsRepository.getInstance().current();
  }

  // 清理所有无效（孤儿/缺失）文件
  cleanup(): void {
    RecordingsRepository.getInstance().cleanup();
    this.recordings = RecordingsRepository.getInstance().current();
  }

  // 系统分享（Share Kit：SharedData + ShareController，DETAIL 预览；接收方获得临时读权限，
  // 可把沙箱里的 MP4 真正导出到文件管理/其他应用做分析）
  async share(path: string): Promise<boolean> {
    const ctx = this.context;
    if (!ctx) {
      return false;
    }
    try {
      const record: systemShare.SharedRecord = {
        utd: 'general.video', // UDMF 统一数据类型：视频
        uri: fileUri.getUriFromPath(path),
        title: '录制视频',
        label: 'VIDEO',
      };
      const data = new systemShare.SharedData(record);
      const controller = new systemShare.ShareController(data);
      await controller.show(ctx, { previewMode: systemShare.SharePreviewMode.DETAIL });
      return true;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'share %{public}s err %{public}s', path, JSON.stringify(e));
      return false;
    }
  }
}