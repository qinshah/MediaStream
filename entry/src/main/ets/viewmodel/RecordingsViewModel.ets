// 录制列表视图模型：加载/删除/分享/清理操作编排（US4/T027）
import { common, Want, wantConstant } from '@kit.AbilityKit';
import { fileUri } from '@kit.CoreFileKit';
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

  // 系统分享（ACTION_SEND_DATA + FLAG_GRANT_READ_URI_PERMISSION；失败返回 false 由视图降级 toast）
  async share(path: string): Promise<boolean> {
    const ctx = this.context;
    if (!ctx) {
      return false;
    }
    try {
      const want: Want = {
        action: 'ohos.want.action.sendData',
        type: 'video/mp4',
        uri: fileUri.getUriFromPath(path),
        flags: wantConstant.Flags.FLAG_AUTH_READ_URI_PERMISSION,
        parameters: {
          'ohos.userFile.manager.BundleName': ctx.abilityInfo.bundleName,
        },
      };
      await ctx.startAbility(want);
      return true;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'share %{public}s err %{public}s', path, JSON.stringify(e));
      return false;
    }
  }
}