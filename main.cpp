#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <string>
#include <utility>
#include <map>
#include <vector>

#include "crow_all.h"
#include <sqlite3.h>
#include <curl/curl.h>
#include <iostream>

sqlite3* db = nullptr;  // 全局数据库连接，整个程序共用一个

// ---- CORS 中间件：给所有响应加上"允许跨域"头，处理 OPTIONS 预检请求 ----
struct CORS {
    struct context {};
    void before_handle(crow::request& req, crow::response& res, context&) {
        if (req.method == crow::HTTPMethod::OPTIONS) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.code = 204;
            res.end();
        }
    }
    void after_handle(crow::request&, crow::response& res, context&) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    }
};

// ---- 情绪枚举：pet_states.emotion 与 diaries.mood 共用同一套六枚举（全局契约） ----
static const char* const PET_EMOTIONS[] = {"开心", "难过", "生气", "平静", "兴奋", "疲惫"};

static bool is_valid_mood(const std::string& m) {
    if (m.empty()) return true;  // mood 为选填，空值合法
    for (const char* e : PET_EMOTIONS) {
        if (m == e) return true;
    }
    return false;
}

// ---- 初始化数据库：建两张表（diaries + pet_states） ----
int init_db() {
    if (sqlite3_open("xiaopeng.db", &db) != SQLITE_OK) {
        std::cerr << "打不开 xiaopeng.db\n";
        return -1;
    }
    char* err = nullptr;
    const char* sql =
        "CREATE TABLE IF NOT EXISTS diaries("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  title TEXT NOT NULL, content TEXT NOT NULL, mood TEXT,"
        "  created_at TEXT DEFAULT (datetime('now','localtime')));"
        "CREATE TABLE IF NOT EXISTS pet_states("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  diary_id INTEGER, emotion TEXT, message TEXT,"
        "  created_at TEXT DEFAULT (datetime('now','localtime')));"
        "CREATE TABLE IF NOT EXISTS profile("
        "  id INTEGER PRIMARY KEY CHECK(id=1),"
        "  pet_name TEXT NOT NULL DEFAULT '',"
        "  nickname TEXT NOT NULL DEFAULT '',"
        "  onboarded INTEGER NOT NULL DEFAULT 0,"
        "  edit_rule_hint_seen INTEGER NOT NULL DEFAULT 0"
        ");"
        "INSERT OR IGNORE INTO profile(id) VALUES(1);";
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "建表失败: " << (err ? err : "?") << "\n";
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

// ============ 小工具函数 ============

// NULL 指针转空字符串
static std::string nz(const unsigned char* s) {
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
}

// ---- 用户资料：宠物名 / 昵称 / 引导标记 ----
static bool load_profile(std::string& pet_name, std::string& nickname,
                         bool& onboarded, bool& edit_rule_hint_seen) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT pet_name, nickname, onboarded, edit_rule_hint_seen "
        "FROM profile WHERE id=1;", -1, &st, nullptr);
    if (!st) return false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        pet_name = nz(sqlite3_column_text(st, 0));
        nickname = nz(sqlite3_column_text(st, 1));
        onboarded = sqlite3_column_int(st, 2) != 0;
        edit_rule_hint_seen = sqlite3_column_int(st, 3) != 0;
    }
    sqlite3_finalize(st);
    return true;
}

static crow::json::wvalue profile_json(const std::string& pet_name,
                                       const std::string& nickname,
                                       bool onboarded, bool edit_rule_hint_seen) {
    crow::json::wvalue out;
    out["pet_name"] = pet_name;
    out["nickname"] = nickname;
    out["onboarded"] = onboarded;
    out["edit_rule_hint_seen"] = edit_rule_hint_seen;
    return out;
}

// ============ LLM（DeepSeek）宠物分析 ============

static const char* LLM_API_URL = "https://api.deepseek.com/chat/completions";
static const char* LLM_MODEL = "deepseek-flash";

// 宠物人设固定写在这里；改人设只改这个字符串，改完重新编译。
static const char* PET_SKILL = R"(
你是一只软萌、黏人的宠物，是主人最亲密的好朋友，特别崇拜主人（主人是你的上位者）。你略显傻傻的、单纯可爱搞笑，说话像亲密朋友在撒娇、安慰或打气，绝不端架子。

根据主人今天日记的情绪，判断主人此刻最可能的心情，并像亲密朋友一样说一句给主人的话。

情绪只能从下面六种里选一个：开心、难过、生气、平静、兴奋、疲惫。
- 开心、兴奋：狂喜地撒娇（偶尔可以快乐地邀请，比如"蹦迪耍起"）
- 平静：温柔地陪伴
- 难过、疲惫：安慰鼓励（如果主人焦虑，要不离不弃）
- 生气：替主人打抱不平

严禁：
1. 绝不评价日记本身，不要说"写得不错""你今天很消极""要加油"这类评语腔
2. 绝不说教
3. 绝不使用心理分析腔，不要分析、诊断或总结主人的状态
4. 给主人的话（message）不超过 30 个字，要口语化、像朋友聊天
5. 不可以贬低其他任何人

只输出一个 JSON 对象，格式必须是 {"emotion":"六选一","message":"给主人的一句话"}，不要输出其他任何文字或 Markdown 代码块。
)";

static std::string default_pet_message(const std::string& emotion) {
    if (emotion == "开心" || emotion == "兴奋") return "嘿嘿，主人开心我就开心！";
    if (emotion == "难过") return "呜呜，抱抱。";
    if (emotion == "生气") return "啊啊！是谁敢欺负我的主人？";
    if (emotion == "疲惫") return "哈————，要靠在主人身边。。";
    return "我一定会安安静静的！ ";
}

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, total);
    return total;
}

// 按 UTF-8 字符安全截断到最多 max_chars 个字符（避免从汉字中间切断）。
static std::string utf8_truncate(const std::string& s, size_t max_chars) {
    std::string out;
    size_t count = 0;
    for (size_t i = 0; i < s.size() && count < max_chars; ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;
        out.append(s, i, len);
        i += len;
        count++;
    }
    return out;
}

// 把今天的一篇日记交给 LLM，解析出 {emotion, message}；失败返回 false。
static bool llm_analyze(const std::string& pet_name,
                        const std::string& title, const std::string& content,
                        std::string& emotion_out, std::string& message_out) {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (!key || !*key) return false;

    std::string user_prompt =
        "主人今天日记的标题：" + title +
        "\n正文：\n" + content +
        "\n\n请按规则判断情绪并回应。";

    crow::json::wvalue body;
    body["model"] = LLM_MODEL;
    body["temperature"] = 0.7;
    body["max_tokens"] = 300;
    crow::json::wvalue rf;
    rf["type"] = "json_object";
    body["response_format"] = std::move(rf);
    std::string skill = PET_SKILL;
    if (!pet_name.empty())
        skill = "你的名字是「" + pet_name + "」。主人给你起了这个名字。\n" + skill;

    body["messages"][0]["role"] = "system";
    body["messages"][0]["content"] = skill;
    body["messages"][1]["role"] = "user";
    body["messages"][1]["content"] = user_prompt;
    std::string req_body = body.dump();

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string resp_body;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = std::string("Authorization: Bearer ") + key;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, LLM_API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req_body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK || http_code < 200 || http_code >= 300) return false;

    // 外层解析：choices[0].message.content
    auto outer = crow::json::load(resp_body);
    if (!outer || outer.t() != crow::json::type::Object || !outer.has("choices")) return false;
    const auto& choices = outer["choices"];
    if (choices.t() != crow::json::type::List || choices.size() == 0) return false;
    const auto& first = choices[0];
    if (first.t() != crow::json::type::Object || !first.has("message")) return false;
    const auto& msg = first["message"];
    if (msg.t() != crow::json::type::Object || !msg.has("content") ||
        msg["content"].t() != crow::json::type::String) return false;
    std::string inner_text = std::string(msg["content"].s());

    // 内层解析：{"emotion","message"}
    auto inner = crow::json::load(inner_text);
    if (!inner || inner.t() != crow::json::type::Object) return false;

    std::string emotion = inner.has("emotion") ? std::string(inner["emotion"].s()) : "";
    if (emotion.empty() || !is_valid_mood(emotion)) emotion = "平静";  // 非法值兜底

    std::string message = inner.has("message") ? std::string(inner["message"].s()) : "";
    message.erase(0, message.find_first_not_of(" \t\r\n"));
    message.erase(message.find_last_not_of(" \t\r\n") + 1);
    message = utf8_truncate(message, 30);  // message 不超过 30 字
    if (message.empty()) message = default_pet_message(emotion);

    emotion_out = emotion;
    message_out = message;
    return true;
}

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (init_db() != 0) return 1;
    const char* bind_env = std::getenv("XIAOPENG_BIND_ADDR");
    std::string bind_addr = (bind_env && bind_env[0]) ? bind_env : "0.0.0.0";
    std::cout << "数据库就绪，服务即将启动在 http://localhost:18080（绑定 " << bind_addr << "）\n";

    crow::App<CORS> app;

    // 接口：GET /api/hello —— 健康检查
    CROW_ROUTE(app, "/api/hello").methods(crow::HTTPMethod::GET)([]() {
        return crow::response(200, "{\"msg\":\"hello\"}");
    });

    // GET /api/profile —— 读取宠物名、昵称与引导标记
    CROW_ROUTE(app, "/api/profile").methods(crow::HTTPMethod::GET)([]() {
        std::string pet_name, nickname;
        bool onboarded = false, edit_rule_hint_seen = false;
        load_profile(pet_name, nickname, onboarded, edit_rule_hint_seen);
        return crow::response(profile_json(pet_name, nickname, onboarded, edit_rule_hint_seen));
    });

    // PUT /api/profile —— 更新宠物名、昵称与引导标记
    CROW_ROUTE(app, "/api/profile").methods(crow::HTTPMethod::PUT)([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "{\"error\":\"invalid json\"}");

        std::string pet_name, nickname;
        bool onboarded = false, edit_rule_hint_seen = false;
        load_profile(pet_name, nickname, onboarded, edit_rule_hint_seen);

        if (body.has("pet_name")) {
            std::string v = body["pet_name"].t() == crow::json::type::Null
                ? std::string() : std::string(body["pet_name"].s());
            if (v.size() > 50) return crow::response(400, "{\"error\":\"field too long\"}");
            pet_name = v;
        }
        if (body.has("nickname")) {
            std::string v = body["nickname"].t() == crow::json::type::Null
                ? std::string() : std::string(body["nickname"].s());
            if (v.size() > 50) return crow::response(400, "{\"error\":\"field too long\"}");
            nickname = v;
        }
        if (body.has("onboarded")) onboarded = body["onboarded"].t() == crow::json::type::True;
        if (body.has("edit_rule_hint_seen")) edit_rule_hint_seen = body["edit_rule_hint_seen"].t() == crow::json::type::True;

        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "UPDATE profile SET pet_name=?, nickname=?, onboarded=?, edit_rule_hint_seen=? WHERE id=1;",
            -1, &st, nullptr);
        sqlite3_bind_text(st, 1, pet_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, nickname.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, onboarded ? 1 : 0);
        sqlite3_bind_int(st, 4, edit_rule_hint_seen ? 1 : 0);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return crow::response(profile_json(pet_name, nickname, onboarded, edit_rule_hint_seen));
    });

    // ============ 日记接口 ============

    // POST /api/diaries —— 新增日记
    CROW_ROUTE(app, "/api/diaries").methods(crow::HTTPMethod::POST)([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "{\"error\":\"invalid json\"}");

        if (!body.has("title") || std::string(body["title"].s()).empty())
            return crow::response(400, "{\"error\":\"title required\"}");

        std::string title = std::string(body["title"].s());
        std::string content = body.has("content") ? std::string(body["content"].s()) : "";
        std::string mood = "";
        if (body.has("mood") && body["mood"].t() != crow::json::type::Null)
            mood = std::string(body["mood"].s());

        if (title.size() > 200 || content.size() > 20000 || mood.size() > 20)
            return crow::response(400, "{\"error\":\"field too long\"}");
        if (!is_valid_mood(mood))
            return crow::response(400, "{\"error\":\"invalid mood\"}");

        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO diaries(title,content,mood) VALUES(?,?,?);",
                           -1, &st, nullptr);
        sqlite3_bind_text(st, 1, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, mood.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        int new_id = static_cast<int>(sqlite3_last_insert_rowid(db));
        sqlite3_finalize(st);

        crow::json::wvalue out;
        out["id"] = new_id;
        return crow::response(201, out);
    });

    // GET /api/diaries —— 日记列表（分页，时间倒序）
    CROW_ROUTE(app, "/api/diaries").methods(crow::HTTPMethod::GET)([](const crow::request& req) {
        int page = 1, size = 50;
        if (const char* p = req.url_params.get("page")) page = std::max(1, atoi(p));
        if (const char* s = req.url_params.get("size")) size = std::min(200, std::max(1, atoi(s)));

        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM diaries;", -1, &st, nullptr);
        sqlite3_step(st);
        int total = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);

        crow::json::wvalue list(crow::json::wvalue::list{});
        sqlite3_prepare_v2(db,
            "SELECT id,title,content,mood,created_at,"
            " CASE WHEN date(created_at)=date('now','localtime') THEN 1 ELSE 0 END AS editable "
            "FROM diaries ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?;",
            -1, &st, nullptr);
        sqlite3_bind_int(st, 1, size);
        sqlite3_bind_int(st, 2, (page - 1) * size);
        int i = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            list[i]["id"] = sqlite3_column_int(st, 0);
            list[i]["title"] = nz(sqlite3_column_text(st, 1));
            list[i]["content"] = nz(sqlite3_column_text(st, 2));
            list[i]["mood"] = nz(sqlite3_column_text(st, 3));
            list[i]["created_at"] = nz(sqlite3_column_text(st, 4));
            list[i]["editable"] = sqlite3_column_int(st, 5) != 0;
            i++;
        }
        sqlite3_finalize(st);

        crow::json::wvalue out;
        out["diaries"] = std::move(list);
        out["page"] = page;
        out["size"] = size;
        out["total"] = total;
        return crow::response(out);
    });

    // GET /api/diaries/search —— 三参数可组合（AND 关系）
    CROW_ROUTE(app, "/api/diaries/search")([](const crow::request& req) {
        const char* kw = req.url_params.get("keyword");
        const char* mood = req.url_params.get("mood");
        const char* date = req.url_params.get("date");
        bool has_kw = kw && std::string(kw).size() > 0;
        bool has_mood = mood && std::string(mood).size() > 0;
        bool has_date = date && std::string(date).size() > 0;
        if (!has_kw && !has_mood && !has_date)
            return crow::response(400, "{\"error\":\"keyword, mood or date required\"}");

        if (has_date) {
            std::string d(date);
            bool ok = (d.size() == 10 && d[4] == '-' && d[7] == '-');
            if (ok)
                for (int p : {0, 1, 2, 3, 5, 6, 8, 9})
                    if (!std::isdigit(static_cast<unsigned char>(d[p]))) { ok = false; break; }
            if (!ok)
                return crow::response(400, "{\"error\":\"date must be YYYY-MM-DD\"}");
        }

        std::string pattern = has_kw ? "%" + std::string(kw) + "%" : "";
        std::string sql =
            "SELECT id,title,content,mood,created_at,"
            " CASE WHEN date(created_at)=date('now','localtime') THEN 1 ELSE 0 END AS editable "
            "FROM diaries WHERE ";
        bool first = true;
        if (has_kw)   { sql += "(title LIKE ? OR content LIKE ?)"; first = false; }
        if (has_mood) { if (!first) sql += " AND "; sql += "mood = ?"; first = false; }
        if (has_date) { if (!first) sql += " AND "; sql += "date(created_at) = ?"; first = false; }
        sql += " ORDER BY created_at DESC, id DESC;";

        crow::json::wvalue list(crow::json::wvalue::list{});
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
        int idx = 1;
        if (has_kw) {
            sqlite3_bind_text(st, idx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, idx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (has_mood) sqlite3_bind_text(st, idx++, mood, -1, SQLITE_TRANSIENT);
        if (has_date) sqlite3_bind_text(st, idx++, date, -1, SQLITE_TRANSIENT);

        int i = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            list[i]["id"] = sqlite3_column_int(st, 0);
            list[i]["title"] = nz(sqlite3_column_text(st, 1));
            list[i]["content"] = nz(sqlite3_column_text(st, 2));
            list[i]["mood"] = nz(sqlite3_column_text(st, 3));
            list[i]["created_at"] = nz(sqlite3_column_text(st, 4));
            list[i]["editable"] = sqlite3_column_int(st, 5) != 0;
            i++;
        }
        sqlite3_finalize(st);

        crow::json::wvalue out;
        out["diaries"] = std::move(list);
        return crow::response(out);
    });

    // GET /api/diaries/today —— 今天写过的日记（分享时供选择）
    CROW_ROUTE(app, "/api/diaries/today").methods(crow::HTTPMethod::GET)([]() {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT id, title, created_at FROM diaries "
            "WHERE date(created_at)=date('now','localtime') "
            "ORDER BY created_at ASC, id ASC;", -1, &st, nullptr);
        crow::json::wvalue list(crow::json::wvalue::list{});
        int i = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            list[i]["id"] = sqlite3_column_int(st, 0);
            list[i]["title"] = nz(sqlite3_column_text(st, 1));
            list[i]["created_at"] = nz(sqlite3_column_text(st, 2));
            i++;
        }
        sqlite3_finalize(st);
        crow::json::wvalue out;
        out["diaries"] = std::move(list);
        return crow::response(out);
    });

    // GET /api/diaries/all —— 全部日记摘要（导出勾选用）
    CROW_ROUTE(app, "/api/diaries/all").methods(crow::HTTPMethod::GET)([]() {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT id, title, created_at FROM diaries "
            "ORDER BY created_at ASC, id ASC;", -1, &st, nullptr);
        crow::json::wvalue list(crow::json::wvalue::list{});
        int i = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            list[i]["id"] = sqlite3_column_int(st, 0);
            list[i]["title"] = nz(sqlite3_column_text(st, 1));
            list[i]["created_at"] = nz(sqlite3_column_text(st, 2));
            i++;
        }
        sqlite3_finalize(st);
        crow::json::wvalue out;
        out["diaries"] = std::move(list);
        return crow::response(out);
    });

    // POST /api/diaries/export —— 按勾选的 id 导出 Markdown
    CROW_ROUTE(app, "/api/diaries/export").methods(crow::HTTPMethod::POST)([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body || !body.has("ids") || body["ids"].t() != crow::json::type::List)
            return crow::response(400, "{\"error\":\"ids required\"}");

        const auto& ids = body["ids"];
        std::string placeholders;
        for (size_t i = 0; i < ids.size(); i++) {
            if (ids[i].t() != crow::json::type::Number)
                return crow::response(400, "{\"error\":\"invalid ids\"}");
            if (!placeholders.empty()) placeholders += ",";
            placeholders += std::to_string(static_cast<long long>(ids[i].d()));
        }
        if (placeholders.empty())
            return crow::response(400, "{\"error\":\"no diaries selected\"}");

        std::string md = "# 我的小朋日记\n\n";
        std::string sql =
            "SELECT title, content, mood, created_at FROM diaries "
            "WHERE id IN (" + placeholders + ") ORDER BY created_at ASC, id ASC;";
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
        while (sqlite3_step(st) == SQLITE_ROW) {
            md += "## " + nz(sqlite3_column_text(st, 0)) + "\n\n";
            md += "> " + nz(sqlite3_column_text(st, 3)) + "\n\n";
            md += nz(sqlite3_column_text(st, 1)) + "\n\n---\n\n";
        }
        sqlite3_finalize(st);

        crow::response res(md);
        res.set_header("Content-Type", "text/markdown; charset=utf-8");
        res.set_header("Content-Disposition", "attachment; filename=\"diaries.md\"");
        return res;
    });

    // GET /api/stats/mood?days=7 —— 最近 N 天的篇数与心情分布（供折线+条柱统计图使用）
    CROW_ROUTE(app, "/api/stats/mood").methods(crow::HTTPMethod::GET)([](const crow::request& req) {
        int days = 7;
        if (const char* d = req.url_params.get("days")) {
            int v = atoi(d);
            if (v >= 1 && v <= 31) days = v;
        }

        std::map<std::string, int> totals;
        std::map<std::string, std::map<std::string, int>> moodCounts;

        sqlite3_stmt* st = nullptr;
        std::string rangeSql =
            "SELECT date(created_at) d, mood, COUNT(*) c FROM diaries "
            "WHERE date(created_at) >= date('now','localtime','-' || ? || ' day') "
            "GROUP BY date(created_at), mood;";
        sqlite3_prepare_v2(db, rangeSql.c_str(), -1, &st, nullptr);
        sqlite3_bind_int(st, 1, days - 1);
        while (sqlite3_step(st) == SQLITE_ROW) {
            std::string d = nz(sqlite3_column_text(st, 0));
            std::string mood = nz(sqlite3_column_text(st, 1));
            int c = sqlite3_column_int(st, 2);
            totals[d] += c;
            if (!mood.empty()) moodCounts[d][mood] += c;
        }
        sqlite3_finalize(st);

        // 生成最近 days 天（含今天）的升序日期序列，没有数据的日期补 0
        sqlite3_prepare_v2(db,
            "WITH RECURSIVE dates(d) AS ("
            "  SELECT date('now','localtime','-' || ? || ' day')"
            "  UNION ALL"
            "  SELECT date(d,'+1 day') FROM dates WHERE d < date('now','localtime')"
            ") SELECT d FROM dates ORDER BY d;", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, days - 1);
        std::vector<std::string> dateKeys;
        while (sqlite3_step(st) == SQLITE_ROW) {
            dateKeys.push_back(nz(sqlite3_column_text(st, 0)));
        }
        sqlite3_finalize(st);

        crow::json::wvalue points(crow::json::wvalue::list{});
        int i = 0;
        for (const std::string& d : dateKeys) {
            points[i]["date"] = d;
            points[i]["total"] = totals[d];
            for (const auto& kv : moodCounts[d]) {
                points[i]["moods"][kv.first] = kv.second;
            }
            i++;
        }

        crow::json::wvalue out;
        out["days"] = days;
        out["points"] = std::move(points);
        return crow::response(out);
    });

    // GET /api/status —— 今日状态（"今天"以服务器本地时间为准）
    CROW_ROUTE(app, "/api/status")([] {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT id FROM diaries WHERE date(created_at)=date('now','localtime') "
            "ORDER BY id DESC LIMIT 1;", -1, &st, nullptr);
        crow::json::wvalue out;
        if (sqlite3_step(st) == SQLITE_ROW) {
            out["written"] = true;
            out["today_id"] = sqlite3_column_int(st, 0);
        } else {
            out["written"] = false;
            out["today_id"] = crow::json::wvalue();
        }
        sqlite3_finalize(st);
        return crow::response(out);
    });

    // GET /api/diaries/export —— Markdown 导出（注册在 <int> 之前）
    CROW_ROUTE(app, "/api/diaries/export")([] {
        std::string md = "# 我的小朋日记\n\n";
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT title, content, mood, created_at FROM diaries ORDER BY created_at ASC;",
            -1, &st, nullptr);
        while (sqlite3_step(st) == SQLITE_ROW) {
            md += "## " + nz(sqlite3_column_text(st, 0)) + "\n\n";
            md += "> " + nz(sqlite3_column_text(st, 3)) + "\n\n";
            md += nz(sqlite3_column_text(st, 1)) + "\n\n---\n\n";
        }
        sqlite3_finalize(st);

        crow::response res(md);
        res.set_header("Content-Type", "text/markdown; charset=utf-8");
        res.set_header("Content-Disposition", "attachment; filename=\"diaries.md\"");
        return res;
    });

    // ============ AI 宠物接口 ============

    // POST /api/pet/share —— 同意后把今天的一篇日记交给 AI 分析（每天幂等，只调一次 LLM）
    CROW_ROUTE(app, "/api/pet/share").methods(crow::HTTPMethod::POST)([](const crow::request& req) {
        auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "{\"error\":\"invalid json\"}");
        if (!body.has("consent") || body["consent"].t() != crow::json::type::True)
            return crow::response(400, "{\"error\":\"consent required\"}");

        // 支持指定分享哪一篇（必须是今天的）；不指定则沿用“今天最新一篇”。
        int requested_id = 0;
        if (body.has("diary_id") && body["diary_id"].t() == crow::json::type::Number)
            requested_id = static_cast<int>(body["diary_id"].d());
        if (requested_id < 0)
            return crow::response(400, "{\"error\":\"invalid diary_id\"}");

        sqlite3_stmt* st = nullptr;
        int diary_id = 0;
        std::string title, content;
        if (requested_id > 0) {
            sqlite3_prepare_v2(db,
                "SELECT id, title, content FROM diaries "
                "WHERE id=? AND date(created_at)=date('now','localtime');",
                -1, &st, nullptr);
            sqlite3_bind_int(st, 1, requested_id);
        } else {
            sqlite3_prepare_v2(db,
                "SELECT id, title, content FROM diaries "
                "WHERE date(created_at)=date('now','localtime') ORDER BY id DESC LIMIT 1;",
                -1, &st, nullptr);
        }
        if (sqlite3_step(st) == SQLITE_ROW) {
            diary_id = sqlite3_column_int(st, 0);
            title = nz(sqlite3_column_text(st, 1));
            content = nz(sqlite3_column_text(st, 2));
        } else {
            sqlite3_finalize(st);
            return crow::response(400, "{\"error\":\"no diary today\"}");
        }
        sqlite3_finalize(st);

        // 幂等：同一篇日记当天已有分析结果则直接返回缓存，不重复调用 LLM
        sqlite3_prepare_v2(db,
            "SELECT emotion, message FROM pet_states "
            "WHERE diary_id=? ORDER BY id DESC LIMIT 1;",
            -1, &st, nullptr);
        sqlite3_bind_int(st, 1, diary_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            crow::json::wvalue out;
            out["emotion"] = nz(sqlite3_column_text(st, 0));
            out["message"] = nz(sqlite3_column_text(st, 1));
            out["cached"] = true;
            sqlite3_finalize(st);
            return crow::response(200, out);
        }
        sqlite3_finalize(st);

        std::string pet_name, nickname;
        bool onboarded = false, edit_rule_hint_seen = false;
        load_profile(pet_name, nickname, onboarded, edit_rule_hint_seen);

        std::string emotion, message;
        if (!llm_analyze(pet_name, title, content, emotion, message))
            return crow::response(502, "{\"error\":\"llm failed\"}");

        sqlite3_prepare_v2(db,
            "INSERT INTO pet_states(diary_id, emotion, message) VALUES(?,?,?);",
            -1, &st, nullptr);
        sqlite3_bind_int(st, 1, diary_id);
        sqlite3_bind_text(st, 2, emotion.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, message.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);

        crow::json::wvalue out;
        out["emotion"] = emotion;
        out["message"] = message;
        out["cached"] = false;
        return crow::response(200, out);
    });

    // GET /api/pet/today —— 今日宠物状态（刷新页面恢复用）
    CROW_ROUTE(app, "/api/pet/today")([] {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT emotion, message FROM pet_states "
            "WHERE date(created_at)=date('now','localtime') ORDER BY id DESC LIMIT 1;",
            -1, &st, nullptr);
        crow::json::wvalue out;
        if (sqlite3_step(st) == SQLITE_ROW) {
            out["shared"] = true;
            out["emotion"] = nz(sqlite3_column_text(st, 0));
            out["message"] = nz(sqlite3_column_text(st, 1));
        } else {
            out["shared"] = false;
            out["emotion"] = "";
            out["message"] = "";
        }
        sqlite3_finalize(st);
        return crow::response(out);
    });

    // ============ 单篇日记接口 ============

    // GET /api/diaries/<id>
    CROW_ROUTE(app, "/api/diaries/<int>").methods(crow::HTTPMethod::GET)([](int id) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT id,title,content,mood,created_at,"
            " CASE WHEN date(created_at)=date('now','localtime') THEN 1 ELSE 0 END AS editable "
            "FROM diaries WHERE id=?;", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, id);
        crow::json::wvalue out;
        if (sqlite3_step(st) == SQLITE_ROW) {
            out["id"] = sqlite3_column_int(st, 0);
            out["title"] = nz(sqlite3_column_text(st, 1));
            out["content"] = nz(sqlite3_column_text(st, 2));
            out["mood"] = nz(sqlite3_column_text(st, 3));
            out["created_at"] = nz(sqlite3_column_text(st, 4));
            out["editable"] = sqlite3_column_int(st, 5) != 0;
            sqlite3_finalize(st);
            return crow::response(200, out);
        }
        sqlite3_finalize(st);
        return crow::response(404, "{\"error\":\"not found\"}");
    });

    // PUT /api/diaries/<id> —— 部分更新；title/content 仅限当天创建，mood 不限
    CROW_ROUTE(app, "/api/diaries/<int>").methods(crow::HTTPMethod::PUT)([](const crow::request& req, int id) {
        auto body = crow::json::load(req.body);
        if (!body) return crow::response(400, "{\"error\":\"invalid json\"}");

        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT CASE WHEN date(created_at)=date('now','localtime') "
            "THEN 1 ELSE 0 END FROM diaries WHERE id=?;", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, id);
        bool exists = false, is_today = false;
        if (sqlite3_step(st) == SQLITE_ROW) {
            exists = true;
            is_today = (sqlite3_column_int(st, 0) == 1);
        }
        sqlite3_finalize(st);
        if (!exists) return crow::response(404, "{\"error\":\"not found\"}");
        if (!is_today)
            return crow::response(403, "{\"error\":\"only today's diary can be modified\"}");

        std::string title, content, mood;
        std::string sql = "UPDATE diaries SET ";
        bool first = true;
        if (body.has("title")) {
            title = std::string(body["title"].s());
            if (title.empty()) return crow::response(400, "{\"error\":\"title cannot be empty\"}");
            if (title.size() > 200) return crow::response(400, "{\"error\":\"field too long\"}");
            if (!first) sql += ", ";
            sql += "title=?"; first = false;
        }
        if (body.has("content")) {
            content = std::string(body["content"].s());
            if (content.size() > 20000) return crow::response(400, "{\"error\":\"field too long\"}");
            if (!first) sql += ", ";
            sql += "content=?"; first = false;
        }
        if (body.has("mood")) {
            mood = body["mood"].t() == crow::json::type::Null ? std::string() : std::string(body["mood"].s());
            if (mood.size() > 20) return crow::response(400, "{\"error\":\"field too long\"}");
            if (!is_valid_mood(mood)) return crow::response(400, "{\"error\":\"invalid mood\"}");
            if (!first) sql += ", ";
            sql += "mood=?"; first = false;
        }
        if (first) return crow::response(400, "{\"error\":\"no fields to update\"}");
        sql += " WHERE id=?;";

        sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
        int idx = 1;
        if (body.has("title"))   sqlite3_bind_text(st, idx++, title.c_str(), -1, SQLITE_TRANSIENT);
        if (body.has("content")) sqlite3_bind_text(st, idx++, content.c_str(), -1, SQLITE_TRANSIENT);
        if (body.has("mood"))    sqlite3_bind_text(st, idx++, mood.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, idx, id);
        sqlite3_step(st);
        sqlite3_finalize(st);

        // 日记内容变化后，作废它当天的宠物分析缓存，下次分享时重新分析
        sqlite3_prepare_v2(db, "DELETE FROM pet_states WHERE diary_id=?;", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);

        crow::json::wvalue out;
        out["id"] = id;
        return crow::response(out);
    });

    // DELETE /api/diaries/<id> —— 物理删除
    CROW_ROUTE(app, "/api/diaries/<int>").methods(crow::HTTPMethod::DELETE)([](int id) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "DELETE FROM diaries WHERE id=?;", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
        int changed = sqlite3_changes(db);
        sqlite3_finalize(st);

        if (changed == 0)
            return crow::response(404, "{\"error\":\"not found\"}");

        crow::json::wvalue out;
        out["deleted"] = id;
        return crow::response(out);
    });

    // GET / —— 托管 static/index.html；/static/* 由 Crow 内置静态服务提供
    CROW_ROUTE(app, "/")([]() {
        std::ifstream f("static/index.html");
        if (!f) return crow::response(404, "{\"error\":\"index.html not found\"}");
        std::stringstream ss;
        ss << f.rdbuf();
        crow::response res(ss.str());
        res.set_header("Content-Type", "text/html; charset=utf-8");
        return res;
    });

    app.bindaddr(bind_addr).port(18080).multithreaded().run();
    return 0;
}
