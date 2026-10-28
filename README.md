# 小朋日记（手机端网页版日记应用 + AI 宠物）

前后端已按交接指南对接完成。前端为「鲸鱼娘版 v3」，后端为 C++17 + Crow + SQLite3 + libcurl 单文件服务。

## 目录结构

```text
main.cpp         后端（单文件）
crow_all.h       Crow 头文件
static/index.html   前端页面
static/assets/pets/ 鲸鱼娘表情图（开心/平静/难过/生气/兴奋/疲惫 + 平静眨眼）
```

## 编译与运行

```bash
g++ -std=c++17 -O2 main.cpp -I. -o xiaopeng_server -lsqlite3 -lcurl
DEEPSEEK_API_KEY=sk-xxx ./xiaopeng_server
```

服务监听 0.0.0.0:18080。打开 `http://localhost:18080/` 即可使用；`/static/*` 由 Crow 内置静态服务提供。

## 接口契约

- `GET /api/hello` 健康检查
- `POST /api/diaries` 新增日记（title 必填，content/mood 选填）
- `GET /api/diaries?page=&size=` 分页列表（倒序）
- `GET /api/diaries/search?keyword=&mood=&date=` 三参数可组合
- `GET /api/diaries/<id>` / `PUT` / `DELETE`
- `GET /api/diaries/export` Markdown 导出
- `GET /api/status` 今日状态 `{"written":bool,"today_id":id|null}`
- `POST /api/pet/share` body `{"consent":true}`，幂等，当天只调一次 LLM
- `GET /api/pet/today` 今日宠物状态 `{"shared":bool,"emotion","message"}`

情绪六枚举：开心、难过、生气、平静、兴奋、疲惫（diaries.mood 与 pet_states.emotion 共用，POST/PUT 已做白名单校验）。

## 前端 AI 流程

政策弹窗 → 勾选同意 → 点「同意并启用」→ 写完今天的日记后点「分享今天给鲸鱼娘」→ 后端读取当天一篇日记调用 DeepSeek → 宠物切换表情并气泡回复。刷新页面时由 `/api/status` 与 `/api/pet/today` 恢复状态。

## 宠物人设与 LLM 配置

人设写死在 `main.cpp` 的 `PET_SKILL` 字符串中，改动后需重新编译。API 地址与模型通过 `LLM_API_URL` / `LLM_MODEL` 两个常量切换；Key 只从环境变量 `DEEPSEEK_API_KEY` 读取。

## 清当天宠物缓存（测试不同情绪触发时）

```bash
sqlite3 ~/xiaopeng/xiaopeng.db "DELETE FROM pet_states WHERE date(created_at)=date('now','localtime');"
```
