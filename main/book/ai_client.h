/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：多厂商 AI 客户端（OpenAI 兼容协议），内置豆包/DeepSeek/千问预设，可自定义。
 * English: Multi-vendor AI client over the OpenAI-compatible chat/completions protocol.
 *
 * 配置存 SD 卡文件 /sdcard/.weread/ai.json，长字符串不进 NVS。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AI_URL_MAX 192
#define AI_KEY_MAX 256
#define AI_MODEL_MAX 64
#define AI_SYS_MAX 400
#define AI_ANSWER_MAX 48000

typedef enum {
    AI_PRESET_CUSTOM = 0,
    AI_PRESET_DOUBAO = 1,   // 火山引擎方舟
    AI_PRESET_DEEPSEEK = 2,
    AI_PRESET_QWEN = 3,     // 阿里云 DashScope 兼容模式
} ai_preset_t;

typedef struct {
    uint8_t preset;
    char base_url[AI_URL_MAX];
    char api_key[AI_KEY_MAX];
    char model[AI_MODEL_MAX];
    int temperature_x10;    // 0.1 步进，7 = 0.7
    int max_tokens;
    char system_prompt[AI_SYS_MAX];
    uint8_t answer_mode;    // 0=日常问答（简洁），1=深度问答（专业分点）
} ai_config_t;

typedef enum {
    AI_SUMMARY = 0, // 本章总结
    AI_EXPLAIN = 1, // 选区解释
    AI_TRANSLATE = 2,
    AI_POLISH = 3,
    AI_QUESTION = 4,
} ai_action_t;

void ai_config_defaults(ai_config_t *cfg);
void ai_config_apply_preset(ai_config_t *cfg, ai_preset_t preset);
bool ai_config_load(ai_config_t *cfg);
bool ai_config_save(const ai_config_t *cfg);

/* 一次 chat completion；成功返回 true，*answer 为 PSRAM 缓冲，调用方 free。 */
bool ai_chat(const ai_config_t *cfg, const char *system, const char *user, char **answer);

/* 同 ai_chat，但可用 max_tokens_override 覆盖本次 token 上限（>0 生效，<=0 用配置值），归纳等场景用。 */
bool ai_chat_limit(const ai_config_t *cfg, const char *system, const char *user,
                   char **answer, int max_tokens_override);

/* 最近一次失败的可读原因（服务端错误消息或网络原因），供界面展示。 */
const char *ai_last_error(void);

/* 月读式便捷封装：按动作构造提示词，上下文为书名/章名/选区/自由问题。 */
bool ai_ask(const ai_config_t *cfg, ai_action_t action,
            const char *book_title, const char *chapter_title,
            const char *selection, const char *question, char **answer);
