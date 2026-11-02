# 小朋日记 · 项目总览

## 一、简介

小朋日记是一款「个人日记 + AI 宠物陪伴」的网页应用。用户每天记录日记，可以把当天的一篇日记“分享”给 AI 宠物，宠物会根据日记内容分析情绪、给出表情和一句不超过 30 字的回应。应用还提供日记搜索、Markdown 导出、最近 7 天心情统计、多套视觉主题等功能。

整体形态是：一个单文件前端页面 + 一个单文件 C++ 后端服务 + 一个本地 SQLite 数据库。数据保存在本机，AI 能力通过 DeepSeek 接口实现，属于轻量、可自部署的个人应用。

## 二、技术架构

- 前端：纯 HTML / CSS / JavaScript 单页应用（`static/index.html`），无构建工具、无第三方框架。
- 后端：C++17 单文件服务（`main.cpp`），使用 Crow 头文件库做 HTTP 路由，SQLite3 做存储，libcurl 调用 DeepSeek。
- 数据：本地 SQLite 文件 `xiaopeng.db`。
- AI：DeepSeek（`deepseek-flash` 模型），API Key 从环境变量 `DEEPSEEK_API_KEY` 读取，不落库、不发给浏览器。
- 部署：Ubuntu（WSL）或 Android（Termux），服务监听 `0.0.0.0:18080`。

## 三、数据模型

数据库共三张表：

| 表 | 用途 | 关键字段 |
| --- | --- | --- |
| diaries | 日记 | id、title、content、mood、created_at |
| pet_states | 宠物 AI 分析结果（按日记缓存） | id、diary_id、emotion、message、created_at |
| profile | 用户资料（单行，id 固定为 1） | pet_name、nickname、onboarded、edit_rule_hint_seen |

`diaries.mood` 与 `pet_states.emotion` 共用同一套六情绪枚举：开心、难过、生气、平静、兴奋、疲惫。

## 四、功能清单

1. 日记写作：整页沉浸式编辑器，含标题、心情、正文；标题必填，正文自动段首缩进。
2. 日记查看 / 编辑 / 删除：查看页与编辑页排版一致、只读无操作按钮；删除前有确认。
3. 首次使用引导：先给宠物起名、再设置自己的昵称，完成后才进入首页。
4. 首页今日状态：当天未写日记时显示大号船舵按钮（带动效），已写后切换为宠物展示模块。
5. AI 宠物：分享当天的某篇日记，宠物按分析结果切换表情并回复一句话；未分享时展示“默认”状态。
6. 日记搜索：按关键词、心情、日期组合筛选。
7. 日记导出：先弹出勾选界面，按选中的日记导出为 Markdown 文件。
8. 心情统计：个人主页以「折线 + 按心情分色条柱」展示最近 7 天的篇数与心情分布。
9. 仅当天可修改：非当天的日记不显示编辑按钮，后端也会拒绝修改；新建日记后弹说明页，可勾选“不再弹出”。
10. 视觉主题：汽水浅色 / 黑色 / 原木色三套主题，可切换并记住选择。
11. 响应式适配：桌面端左侧导航，移动端底部导航。

## 五、后端接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | /api/hello | 健康检查 |
| GET | /api/profile | 读取宠物名、昵称、引导标记 |
| PUT | /api/profile | 更新资料（宠物名 / 昵称 / onboarded / edit_rule_hint_seen） |
| POST | /api/diaries | 新增日记 |
| GET | /api/diaries?page=&size= | 分页列表（倒序，含 editable 标记） |
| GET | /api/diaries/search | 按关键词 / 心情 / 日期搜索 |
| GET | /api/diaries/today | 今天写过的日记摘要（分享选择用） |
| GET | /api/diaries/all | 全部日记摘要（导出勾选用） |
| POST | /api/diaries/export | 按勾选的 id 导出 Markdown |
| GET | /api/diaries/export | 导出全部 Markdown（旧接口，保留） |
| GET | /api/diaries/<id> | 单篇详情（含 editable 标记） |
| PUT | /api/diaries/<id> | 修改日记（仅限当天） |
| DELETE | /api/diaries/<id> | 删除日记 |
| GET | /api/status | 今日是否已写、today_id |
| GET | /api/stats/mood?days=7 | 最近 N 天的篇数与心情分布 |
| POST | /api/pet/share | 分享某篇当天日记给 AI 分析（body 含 consent、可选 diary_id） |
| GET | /api/pet/today | 今日宠物状态（shared、emotion、message） |
| GET | / | 托管前端 index.html；/static/* 为静态资源 |

## 六、前端实现要点

- 视图模型：单页内以 `data-view` 在首页、日记、搜索、个人主页、账号、关于之间切换。
- API 封装：统一的 `apiRequest()`，处理 JSON、超时、HTTP 错误，并支持手机 / 电脑自动推导后端地址。
- 今日状态：以服务端 `/api/status` 的 `written` 为准，不依赖浏览器本地日期。
- 宠物渲染：`PET_MOODS` 映射六种情绪的图片与文案；无分享时走默认状态；分享后按 LLM 返回的情绪切换。
- 统计图：用 SVG 手绘，折线表示每日篇数，条柱按心情分色横向拼接；心情颜色为开心绿、难过灰、生气红、平静蓝、兴奋黄、疲惫紫，条柱竖向渐变。
- 船舵按钮：透明线稿 PNG + 右下角宠物图标，点击有旋转 / 按压动效，延时 1 秒后进入写日记。
- 主题：通过 `body[data-theme]` 切换 CSS 变量，主题选择保存在 `localStorage`。

## 七、关键机制

### 仅当天可修改

后端判断 `date(created_at) = date('now','localtime')` 生成 `editable` 标记；非当天的编辑请求返回 403。前端据此隐藏“编辑”按钮。新建日记后弹出说明，勾选“不再弹出”会写入 `profile.edit_rule_hint_seen`。

### AI 分享与缓存

分享时后端读取当天指定日记，调用 DeepSeek 生成 `{emotion, message}`，结果写入 `pet_states` 并绑定 `diary_id` 幂等缓存。修改日记会删除该日记的缓存，下次分享重新分析；因此编辑后再分享会得到新结果。

### 首次使用引导

`profile.onboarded` 为 false 时前端显示“领养宠物”两步引导，提交宠物名与昵称后写入后端并进入首页。

### 心情统计

`/api/stats/mood` 统计最近 7 天（含今天）每天的日记总数与各心情数量，前端渲染为同一坐标系内的折线 + 条柱图。

## 八、构建与运行

编译（在 `~/xiaopeng` 目录）：

```bash
g++ -std=c++17 -O2 main.cpp -I. -o xiaopeng_server -lsqlite3 -lcurl
```

运行：

```bash
source ~/.bashrc            # 使 DEEPSEEK_API_KEY 生效
./xiaopeng_server
```

浏览器打开 `http://localhost:18080/`。Android 手机可用仓库内的 `run_on_android.sh` 在 Termux 中编译并启动。

## 九、文件结构

```text
main.cpp                    后端（单文件）
crow_all.h                  Crow 头文件
static/index.html           前端单页
static/assets/pets/         宠物表情图、船舵线稿、资源清单
static/assets/pets/wheel.png
xiaopeng.db                 本地数据库（不提交）
README.md                   原项目说明
run_on_android.sh           Android / Termux 启动脚本
```
