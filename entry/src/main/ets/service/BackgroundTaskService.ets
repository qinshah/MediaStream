// 长时任务服务：申请/取消后台运行（录屏需 MODE_AV_PLAYBACK_AND_RECORD + SUBMODE_SCREEN_RECORD）
// 长时任务与输出生命周期严格同步：开始任一输出时申请，全部结束后取消（US1/T014/T031）
import { backgroundTaskManager } from '@kit.BackgroundTasksKit';
import { common, wantAgent, WantAgent } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'BackgroundTaskService';

export class BackgroundTaskService {
  private static instance_: BackgroundTaskService | null = null;
  private running_ = false;

  static getInstance(): BackgroundTaskService {
    if (BackgroundTaskService.instance_ === null) {
      BackgroundTaskService.instance_ = new BackgroundTaskService();
    }
    return BackgroundTaskService.instance_;
  }

  private constructor() {
  }

  isRunning(): boolean {
    return this.running_;
  }

  // 申请长时任务（幂等：已运行时直接返回）
  async start(context: common.UIAbilityContext): Promise<void> {
    if (this.running_) {
      return;
    }
    try {
      // 长时任务通知点击后拉起宿主 Ability
      const wantAgentInfo: wantAgent.WantAgentInfo = {
        wants: [
          {
            bundleName: context.abilityInfo.bundleName,
            abilityName: context.abilityInfo.name,
          },
        ],
        operationType: wantAgent.OperationType.START_ABILITY,
        requestCode: 0,
        wantAgentFlags: [wantAgent.WantAgentFlags.UPDATE_PRESENT_FLAG],
      };
      const agent: WantAgent = await wantAgent.getWantAgent(wantAgentInfo);
      const request = new backgroundTaskManager.ContinuousTaskRequest();
      request.backgroundTaskModes = [backgroundTaskManager.BackgroundTaskMode.MODE_AV_PLAYBACK_AND_RECORD];
      request.backgroundTaskSubmodes =
        [backgroundTaskManager.BackgroundTaskSubmode.SUBMODE_SCREEN_RECORD_NORMAL_NOTIFICATION];
      request.wantAgent = agent;
      await backgroundTaskManager.startBackgroundRunning(context, request);
      this.running_ = true;
      hilog.info(DOMAIN, TAG, 'continuous task started');
    } catch (e) {
      this.running_ = false;
      hilog.error(DOMAIN, TAG, 'start continuous task err %{public}s', JSON.stringify(e));
      // 长时任务失败不阻塞采集/编码主流程，仅告警
    }
  }

  // 取消长时任务（幂等）
  async stop(context: common.UIAbilityContext): Promise<void> {
    if (!this.running_) {
      return;
    }
    try {
      await backgroundTaskManager.stopBackgroundRunning(context);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'stop continuous task err %{public}s', JSON.stringify(e));
    }
    this.running_ = false;
  }
}