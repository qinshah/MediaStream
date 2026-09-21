// RTMP 服务器可达性预检：开始推流前先确认「该地址确实有服务在监听」。
// 动机：原生 RtmpClient::Start() 是异步连接的，若地址不存在，App 会先拉起录屏采集、
// 把状态切到「连接中」，若干秒后才报错 —— 用户看到的是「对着不存在的服务器也能开始推流」。
import { socket } from '@kit.NetworkKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'RtmpProbe';

// 预检超时：局域网内「无监听」通常毫秒级返回连接拒绝；跨网段黑洞地址由此兜底。
const PROBE_TIMEOUT_MS = 3000;

const DEFAULT_RTMP_PORT = 1935;

// 推流目标（host + port）
export class RtmpTarget {
  host: string;
  port: number;

  constructor(host: string, port: number) {
    this.host = host;
    this.port = port;
  }
}

// 解析 rtmp://host[:port]/app[/stream]，并要求至少含 app 段（与原生 ParseUrl 的校验保持一致）。
// 解析不出合法目标时返回 null，调用方按「地址格式不正确」提示。
export function parseRtmpTarget(url: string): RtmpTarget | null {
  const prefix = 'rtmp://';
  const u = url.trim();
  if (!u.startsWith(prefix)) {
    return null;
  }
  const rest = u.substring(prefix.length);
  const slash = rest.indexOf('/');
  if (slash <= 0) {
    return null; // 缺 host 或缺 app 段
  }
  const hostPort = rest.substring(0, slash);
  if (rest.substring(slash + 1).length === 0) {
    return null; // 只有 rtmp://host/ 没有 app/stream
  }
  let host = hostPort;
  let port = DEFAULT_RTMP_PORT;
  const colon = hostPort.lastIndexOf(':');
  if (colon > 0) {
    host = hostPort.substring(0, colon);
    const parsed = parseInt(hostPort.substring(colon + 1), 10);
    if (!(parsed > 0 && parsed <= 65535)) {
      return null;
    }
    port = parsed;
  }
  if (host.length === 0) {
    return null;
  }
  return new RtmpTarget(host, port);
}

// TCP 连通性探测：只验证「地址可达且端口有服务」，不做 RTMP 握手（握手交给原生客户端）。
// 返回 true 表示可以尝试推流；false 表示服务器不存在/端口未开放/网络不通。
export async function probeRtmpServer(target: RtmpTarget): Promise<boolean> {
  const tcp = socket.constructTCPSocketInstance();
  const address: socket.NetAddress = { address: target.host, port: target.port };
  const options: socket.TCPConnectOptions = { address, timeout: PROBE_TIMEOUT_MS };
  try {
    await tcp.connect(options);
    return true;
  } catch (e) {
    hilog.warn(DOMAIN, TAG, 'probe %{public}s:%{public}d unreachable: %{public}s',
      target.host, target.port, JSON.stringify(e));
    return false;
  } finally {
    try {
      await tcp.close();
    } catch (e) {
      // 连接未建立时 close 报错属预期，忽略
    }
  }
}
