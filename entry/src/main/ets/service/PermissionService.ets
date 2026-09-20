// 麦克风权限服务：检查/申请，返回授权结果供"仅内录降级"决策（US1/T014）
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'PermissionService';

const MIC_PERMISSION: Permissions = 'ohos.permission.MICROPHONE';

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
    const atManager = abilityAccessCtrl.createAtManager();
    const tokenId = context.applicationInfo.accessTokenId;
    const state = atManager.checkAccessTokenSync(tokenId, MIC_PERMISSION);
    return state === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED;
  }

  // 申请麦克风权限，返回是否最终授予（拒绝→false，由调用方决定降级为仅内录）
  async requestMicPermission(context: common.UIAbilityContext): Promise<boolean> {
    if (this.hasMicPermission(context)) {
      return true;
    }
    try {
      const result = await abilityAccessCtrl.createAtManager()
        .requestPermissionsFromUser(context, [MIC_PERMISSION]);
      // 授予成功后以系统状态为准，更可靠
      return this.hasMicPermission(context);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'request mic permission err %{public}s', JSON.stringify(e));
      return false;
    }
  }
}