// 宿主页面：Navigation + NavPathStack 承载控制台视图，并注册录制列表路由（T015/T027）
import { ConsoleView } from '../views/ConsoleView';
import { RecordingsView } from '../views/RecordingsView';
import { ROUTE_RECORDINGS } from '../model/Types';

@Entry
@ComponentV2
struct Index {
  pathStack: NavPathStack = new NavPathStack();

  build() {
    Navigation(this.pathStack) {
      ConsoleView({ pathStack: this.pathStack })
    }
    .mode(NavigationMode.Stack)
    .hideTitleBar(true)
    .navDestination(this.pageMap)
  }

  @Builder
  pageMap(name: string) {
    if (name === ROUTE_RECORDINGS) {
      NavDestination() {
        RecordingsView()
      }
      .title('录制列表')
    }
  }
}