// 配置持久化仓库：Preferences 存储推流设置（rtmpUrl/streamKey/preset/fps/bitrate/audioMode）
// 取代旧的 service/SettingsStore.ets，属 data 数据层（US5/T013/T028）
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { StreamSettings } from '../service/MediaStreamService';

const DOMAIN = 0x0000;
const TAG = 'PreferencesRepository';

const KEY_URL = 'rtmpUrl';
const KEY_STREAMKEY = 'streamKey';
const KEY_PRESET = 'preset';
const KEY_FPS = 'fps';
const KEY_BITRATE = 'bitrate';
const KEY_AUDIO = 'audioMode';
const STORE_NAME = 'stream_settings';

export class PreferencesRepository {
  private static instance_: PreferencesRepository | null = null;
  private prefs_: preferences.Preferences | null = null;

  static getInstance(): PreferencesRepository {
    if (PreferencesRepository.instance_ === null) {
      PreferencesRepository.instance_ = new PreferencesRepository();
    }
    return PreferencesRepository.instance_;
  }

  private constructor() {
  }

  async init(context: common.UIAbilityContext): Promise<void> {
    if (this.prefs_) {
      return;
    }
    this.prefs_ = await preferences.getPreferences(context, STORE_NAME);
  }

  async load(): Promise<StreamSettings> {
    const s = new StreamSettings();
    if (!this.prefs_) {
      return s;
    }
    try {
      s.rtmpUrl = (await this.prefs_.get(KEY_URL, '')) as string;
      // 老版本没有独立的推流码键，缺省空串由上层做「整串拆分」迁移
      s.streamKey = (await this.prefs_.get(KEY_STREAMKEY, '')) as string;
      s.preset = (await this.prefs_.get(KEY_PRESET, '720p')) as string;
      s.fps = (await this.prefs_.get(KEY_FPS, 30)) as number;
      s.videoBitrateKbps = (await this.prefs_.get(KEY_BITRATE, 4000)) as number;
      s.audioMode = (await this.prefs_.get(KEY_AUDIO, 'inner')) as string;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'load settings err %{public}s', JSON.stringify(e));
    }
    return s;
  }

  async save(settings: StreamSettings): Promise<void> {
    if (!this.prefs_) {
      return;
    }
    try {
      await this.prefs_.put(KEY_URL, settings.rtmpUrl);
      await this.prefs_.put(KEY_STREAMKEY, settings.streamKey);
      await this.prefs_.put(KEY_PRESET, settings.preset);
      await this.prefs_.put(KEY_FPS, settings.fps);
      await this.prefs_.put(KEY_BITRATE, settings.videoBitrateKbps);
      await this.prefs_.put(KEY_AUDIO, settings.audioMode);
      await this.prefs_.flush();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'save settings err %{public}s', JSON.stringify(e));
    }
  }
}