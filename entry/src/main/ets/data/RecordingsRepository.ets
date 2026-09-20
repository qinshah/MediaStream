// 录制文件仓库：维护录制索引（{filesDir}/recordings.idx）、增删、孤儿文件检测与清理
// 取代旧的 service/RecordingsStore.ets 与 model/RecordingItem.ets，属 data 数据层（US2/US4/T023/T026）
import { fileIo } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { RecordingMeta } from '../service/MediaStreamService';

const DOMAIN = 0x0000;
const TAG = 'RecordingsRepository';

const IDX_NAME = 'recordings.idx';
const SEP = '|';

// 录制列表项模型：一张 mp4 录制文件的展示元数据
export class RecordingItem {
  fileName: string;
  filePath: string;
  durationMs: number;
  sizeBytes: number;
  createdAtMs: number;
  invalid: boolean = false;

  constructor(fileName: string, filePath: string, durationMs: number, sizeBytes: number, createdAtMs: number, invalid: boolean = false) {
    this.fileName = fileName;
    this.filePath = filePath;
    this.durationMs = durationMs;
    this.sizeBytes = sizeBytes;
    this.createdAtMs = createdAtMs;
    this.invalid = invalid;
  }
}

export class RecordingsRepository {
  private static instance_: RecordingsRepository | null = null;
  private items_: Array<RecordingItem> = [];
  private filesDir_: string = '';
  private idxPath_: string = '';

  static getInstance(): RecordingsRepository {
    if (RecordingsRepository.instance_ === null) {
      RecordingsRepository.instance_ = new RecordingsRepository();
    }
    return RecordingsRepository.instance_;
  }

  private constructor() {
  }

  current(): Array<RecordingItem> {
    return this.items_;
  }

  // 录制完成事件入列（去重按 filePath）并持久化索引
  add(meta: RecordingMeta): void {
    const exists = this.items_.findIndex((it) => it.filePath === meta.filePath);
    const item = new RecordingItem(meta.fileName, meta.filePath, meta.durationMs, meta.sizeBytes, meta.createdAtMs);
    if (exists >= 0) {
      this.items_.splice(exists, 1);
    }
    this.items_.unshift(item);
    this.persist();
    hilog.info(DOMAIN, TAG, 'record indexed %{public}s', meta.fileName);
  }

  // 刷新索引：读持久化索引 → 扫描 videos 目录 → 孤儿/缺失文件标记无效 → 倒序返回
  async refresh(filesDir: string): Promise<Array<RecordingItem>> {
    this.filesDir_ = filesDir;
    this.idxPath_ = `${filesDir}/${IDX_NAME}`;
    const byPath = this.loadIndex();
    const videosDir = `${filesDir}/videos`;
    const list: Array<RecordingItem> = [];
    try {
      const names = fileIo.listFileSync(videosDir);
      for (const n of names) {
        if (!n.endsWith('.mp4')) {
          continue;
        }
        const p = `${videosDir}/${n}`;
        const f = fileIo.openSync(p, fileIo.OpenMode.READ_ONLY);
        const fs = fileIo.statSync(f.fd);
        const ct = fs.ctime * 1000;
        const known = byPath.get(p);
        // 磁盘存在但不在已建索引中 → 孤儿（上次会话崩溃/异常退出残留）→ 标记无效待清理
        const invalid = known === undefined;
        list.push(new RecordingItem(n, p, known ? known.durationMs : 0, fs.size, ct, invalid));
        fileIo.closeSync(f);
      }
    } catch (e) {
      hilog.info(DOMAIN, TAG, 'refresh list (dir may missing) %{public}s', JSON.stringify(e));
    }
    // 索引中记录但磁盘文件已丢失 → 标记无效（列表可统一点击清理）
    byPath.forEach((item: RecordingItem, p: string) => {
      const onDisk = list.findIndex((it) => it.filePath === p) >= 0;
      if (!onDisk) {
        list.push(new RecordingItem(item.fileName, p, item.durationMs, 0, item.createdAtMs, true));
      }
    });
    list.sort((a, b) => b.createdAtMs - a.createdAtMs);
    this.items_ = list;
    this.persist();
    return list;
  }

  // 删除单个录制文件（文件 + 索引项）
  remove(filePath: string): void {
    try {
      fileIo.unlinkSync(filePath);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'unlink %{public}s err %{public}s', filePath, JSON.stringify(e));
    }
    const idx = this.items_.findIndex((it) => it.filePath === filePath);
    if (idx >= 0) {
      this.items_.splice(idx, 1);
    }
    this.persist();
  }

  // 清理所有无效（孤儿/缺失）文件及其索引项
  cleanup(): void {
    const keep: Array<RecordingItem> = [];
    for (const it of this.items_) {
      if (it.invalid) {
        try {
          fileIo.unlinkSync(it.filePath);
        } catch (e) {
          hilog.info(DOMAIN, TAG, 'cleanup unlink %{public}s err %{public}s', it.filePath, JSON.stringify(e));
        }
      } else {
        keep.push(it);
      }
    }
    this.items_ = keep;
    this.persist();
  }

  // —— 索引持久化（纯文本行式，避免 JSON 解析类型问题）——
  private loadIndex(): Map<string, RecordingItem> {
    const map: Map<string, RecordingItem> = new Map<string, RecordingItem>();
    if (!this.idxPath_ || this.filesDir_.length === 0) {
      return map;
    }
    try {
      const text = fileIo.readTextSync(this.idxPath_);
      const lines = text.split('\n');
      for (const line of lines) {
        if (line.length === 0) {
          continue;
        }
        const parts = line.split(SEP);
        if (parts.length < 6) {
          continue;
        }
        const fileName = parts[0];
        const filePath = parts[1];
        const durationMs = Number(parts[2]);
        const sizeBytes = Number(parts[3]);
        const createdAtMs = Number(parts[4]);
        const invalid = parts[5] === '1';
        map.set(filePath, new RecordingItem(fileName, filePath, durationMs, sizeBytes, createdAtMs, invalid));
      }
    } catch (e) {
      hilog.info(DOMAIN, TAG, 'no index yet %{public}s', JSON.stringify(e));
    }
    return map;
  }

  private persist(): void {
    if (!this.idxPath_ || this.filesDir_.length === 0) {
      return;
    }
    try {
      const lines: Array<string> = [];
      for (const it of this.items_) {
        lines.push(`${it.fileName}${SEP}${it.filePath}${SEP}${it.durationMs}${SEP}${it.sizeBytes}${SEP}${it.createdAtMs}${SEP}${it.invalid ? 1 : 0}`);
      }
      const text = lines.join('\n');
      const f = fileIo.openSync(this.idxPath_, fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC);
      fileIo.writeSync(f.fd, text);
      fileIo.closeSync(f);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'persist index err %{public}s', JSON.stringify(e));
    }
  }
}