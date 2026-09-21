// 麦克风权限服务：应用级授权弹窗 → 设置级（权限设置弹窗 / 系统应用设置页）两级申请，
// 返回最终结果供「仅内录降级」决策（US1/T014）
import { abilityAccessCtrl, common, Permissions, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'PermissionService';

const MIC_PERMISSION: Permissions = 'ohos.permission.MICROPHONE';

// 系统设置应用（拉起「应用详情 → 权限」页的兜底入口）
const SETTINGS_BUNDLE = 'com.huawei.hmos.settings';
const SETTINGS_ABILITY = 'com.huawei.hmos.settings.MainAbility';
const SETTINGS_URI_APP_INFO = 'application_info_entry';

// 申请结果：区分「已授权」「设置级也拒绝」「已引导到系统设置」三种，供调用方决定下一步
export enum MicAuthResult {
  // 应用级或设置级任一通过
  GRANTED = 'granted',
  // 应用级被拒后，设置级（权限设置页）仍未打开开关
  DENIED_SETTING_LEVEL = 'denied_setting_level',
  // 权限设置弹窗在本机型/版本不可用，已把用户送到系统「应用设置页」：
  // 此时不能立刻判定失败（用户还没操作），由调用方提示「开启后重试」，不弹降级确认
  GUIDED_TO_SETTINGS = 'guided_to_settings',
}

export class PermissionService {
  private static instance_: PermissionService | null = null;

  static getInstance(): PermissionService {
    if (PermissionService.instance_ === null) {
      PermissionService.instance_ = new PermissionService();
    }
    return PermissionService.instance_;
  }

  private constructor() {
  }

  // 检查当前是否已授予麦克风权限
  hasMicPermission(context: common.UIAbilityContext): boolean {
    try {
      const atManager = abilityAccessCtrl.createAtManager();
      const tokenId = context.applicationInfo.accessTokenId;
      const state = atManager.checkAccessTokenSync(tokenId, MIC_PERMISSION);
      return state === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'check mic permission err %{public}s', JSON.stringify(e));
      return false;
    }
  }

  // 第一级（应用级）：系统授权弹窗。仅在「尚未被拒绝」时才会真正弹出；
  // 用户拒绝过一次后系统不再弹窗，本调用会直接返回拒绝态 —— 此时靠下面的设置级兜底。
  async requestMicPermission(context: common.UIAbilityContext): Promise<boolean> {
    if (this.hasMicPermission(context)) {
      return true;
    }
    try {
      await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(context, [MIC_PERMISSION]);
      // 授予成功后以系统状态为准，更可靠
      return this.hasMicPermission(context);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'request mic permission err %{public}s', JSON.stringify(e));
      return false;
    }
  }

  // 第二级（设置级）：优先用 requestPermissionOnSetting 拉起「权限设置」弹窗（API 12+，
  // 专为「首次弹窗已被拒、需要二次申请」设计；系统要求此前调用过 requestPermissionsFromUser）。
  // 若该接口在本机型/版本不可用（例如报 12100014「该权限不支持弹窗式设置页申请」），
  // 退化为直接拉起系统「应用详情」设置页，保证用户始终有入口把开关打开。
  async requestMicPermissionOnSetting(context: common.UIAbilityContext): Promise<MicAuthResult> {
    try {
      const atManager = abilityAccessCtrl.createAtManager();
      await atManager.requestPermissionOnSetting(context, [MIC_PERMISSION]);
      return this.hasMicPermission(context) ? MicAuthResult.GRANTED : MicAuthResult.DENIED_SETTING_LEVEL;
    } catch (e) {
      // 12100011/12100012：已授权或无需再弹窗 → 以系统状态为准；
      // 其余（含 12100014）：本机型不支持弹窗式设置页申请 → 走系统设置页兜底。
      hilog.warn(DOMAIN, TAG, 'requestPermissionOnSetting unavailable: %{public}s', JSON.stringify(e));
      if (this.hasMicPermission(context)) {
        return MicAuthResult.GRANTED;
      }
      return await this.openAppSettings(context);
    }
  }

  // 兜底：拉起「设置 → 应用 → 本应用」详情页（uri=application_info_entry）。
  // startAbility 只保证页面被拉起，此刻并不能判定用户是否已开启开关（用户还没操作），
  // 因此返回 GUIDED_TO_SETTINGS，由调用方提示「开启后重试」。
  async openAppSettings(context: common.UIAbilityContext): Promise<MicAuthResult> {
    try {
      const want: Want = {
        bundleName: SETTINGS_BUNDLE,
        abilityName: SETTINGS_ABILITY,
        uri: SETTINGS_URI_APP_INFO,
        parameters: {
          // 传递本应用包名，让设置页直接落到本应用的权限分组
          pushParams: context.applicationInfo.name,
        },
      };
      await context.startAbility(want);
      return MicAuthResult.GUIDED_TO_SETTINGS;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'open app settings err %{public}s', JSON.stringify(e));
      return MicAuthResult.DENIED_SETTING_LEVEL;
    }
  }

  // 完整授权链路：应用级 → 仍不通过则设置级。任一级通过即 GRANTED。
  async ensureMicPermission(context: common.UIAbilityContext): Promise<MicAuthResult> {
    if (this.hasMicPermission(context)) {
      return MicAuthResult.GRANTED;
    }
    if (await this.requestMicPermission(context)) {
      return MicAuthResult.GRANTED;
    }
    return await this.requestMicPermissionOnSetting(context);
  }
}
