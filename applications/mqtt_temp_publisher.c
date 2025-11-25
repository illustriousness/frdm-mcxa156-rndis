#include <rtthread.h>
#ifdef PKG_USING_PAHOMQTT
#include <rtdevice.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <rtdbg.h>

#include "paho_mqtt.h"
#include "p3t1755.h"
#include <webclient.h>
#include <cJSON.h>

#define MQTT_URI                    "tcp://192.168.1.232:1883"
#define MQTT_SUBTOPIC               "danmaku/test"
#define MQTT_PUBTOPIC               "danmaku/test"
#define MQTT_WILLMSG                "Goodbye!"
#define MQTT_TEMP_THREAD_STACK      (512*3)
#define MQTT_TEMP_THREAD_PRIO       20
#define MQTT_TEMP_INTERVAL_MS       10000

/* mqtt client context */
static MQTTClient mqtt_client;
static int mqtt_started = 0;

/* temp publish thread (static) */
static struct rt_thread temp_thread;
static rt_uint8_t temp_thread_stack[MQTT_TEMP_THREAD_STACK];
static rt_bool_t temp_thread_running = RT_FALSE;
static rt_bool_t p3t_inited = RT_FALSE;

/* message batching for model trigger */
static int msg_count = 0;

/* forward declarations */
static void mqtt_model_request_schedule(const char *prompt);

static rt_err_t ensure_p3t_ready(void)
{
    if (!p3t_inited)
    {
        if (p3t1755_init() != RT_EOK)
        {
            LOG_E("p3t1755 init failed");
            return -RT_ERROR;
        }
        p3t_inited = RT_TRUE;
    }
    return RT_EOK;
}

static void temp_publish_thread_entry(void *parameter)
{
    (void)parameter;
    while (temp_thread_running)
    {
        if (ensure_p3t_ready() == RT_EOK)
        {
            float temp = 0.0f;
            if (p3t1755_read_temp(&temp) == RT_EOK)
            {
                char json_payload[128];
                time_t now = time(RT_NULL);
                struct tm *tm_now = localtime(&now);
                if (tm_now)
                {
                    rt_snprintf(json_payload, sizeof(json_payload),
                                "{\"user\":\"无情的大佬A\",\"text\":\"%04d-%02d-%02d %02d:%02d:%02d temperature:%.2f\"}",
                                tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
                                tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec, temp);
                }
                else
                {
                    rt_snprintf(json_payload, sizeof(json_payload),
                                "{\"user\":\"无情的大佬A\",\"text\":\"temperature:%.2f\"}", temp);
                }
                if (mqtt_started && mqtt_client.isconnected)
                {
                    paho_mqtt_publish(&mqtt_client, QOS1, MQTT_PUBTOPIC, json_payload);
                    LOG_D("publish temp json: %s", json_payload);
                }
                else
                {
                    LOG_W("mqtt not connected, skip publish: %s", json_payload);
                }
            }
            else
            {
                LOG_E("read temperature failed");
            }
        }
        rt_thread_mdelay(MQTT_TEMP_INTERVAL_MS);
    }
    /* thread exits if temp_thread_running becomes false */
}

static void mqtt_handle_incoming_text(const char *text)
{
    /* trigger on /model prefix */
    if (rt_strncmp(text, "/model", 6) == 0)
    {
        const char *prompt = text + 6;
        while (*prompt == ' ') prompt++;
        mqtt_model_request_schedule(prompt);
        return;
    }

    /* every 10th message triggers model with current payload */
    msg_count++;
    if (msg_count >= 10)
    {
        mqtt_model_request_schedule(text);
        msg_count = 0;
    }
}

static void mqtt_sub_callback(MQTTClient *c, MessageData *msg_data)
{
    (void)c;
    *((char *)msg_data->message->payload + msg_data->message->payloadlen) = '\0';
    const char *payload = (const char *)msg_data->message->payload;
    // LOG_D("mqtt sub callback: %.*s %s",
    //       msg_data->topicName->lenstring.len,
    //       msg_data->topicName->lenstring.data,
    //       payload);
    mqtt_handle_incoming_text(payload);
}

static void mqtt_sub_default_callback(MQTTClient *c, MessageData *msg_data)
{
    mqtt_sub_callback(c, msg_data);
}

static void mqtt_connect_callback(MQTTClient *c)
{
    LOG_D("enter mqtt_connect_callback!");
}

static void mqtt_online_callback(MQTTClient *c)
{
    LOG_D("enter mqtt_online_callback!");
}

static void mqtt_offline_callback(MQTTClient *c)
{
    LOG_D("enter mqtt_offline_callback!");
}

/* ---------- bigmodel request via system workqueue ---------- */

struct model_work
{
    struct rt_work work;
    char prompt[64];
};

static void publish_model_reply(const char *reply)
{
    if (mqtt_started && mqtt_client.isconnected && reply)
    {
        paho_mqtt_publish(&mqtt_client, QOS1, MQTT_PUBTOPIC, reply);
        LOG_D("publish model reply");
    }
    else
    {
        LOG_W("skip publish model reply (not connected)");
    }
}

static void model_work_handler(struct rt_work *work, void *work_data)
{
    (void)work_data;
    struct model_work *mw = rt_container_of(work, struct model_work, work);

    /* build request JSON */
    cJSON *root = cJSON_CreateObject();
    char *payload = RT_NULL;
    char *resp = RT_NULL;
    char *reply_text = RT_NULL;
    size_t resp_len = 0;

    if (!root)
    {
        LOG_E("cJSON create root failed");
        goto __exit;
    }

    cJSON_AddStringToObject(root, "model", "glm-4.6");
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    if (!messages) goto __exit;
    cJSON *msg1 = cJSON_CreateObject();
    cJSON_AddStringToObject(msg1, "role", "user");
    cJSON_AddStringToObject(msg1, "content", "作为回复弹幕的有趣机器人，你需要快速回应用户的弹幕");
    cJSON_AddItemToArray(messages, msg1);
    cJSON *msg2 = cJSON_CreateObject();
    cJSON_AddStringToObject(msg2, "role", "user");
    cJSON_AddStringToObject(msg2, "content", mw->prompt);
    cJSON_AddItemToArray(messages, msg2);
    cJSON *thinking = cJSON_AddObjectToObject(root, "thinking");
    cJSON_AddStringToObject(thinking, "type", "disabled");
    cJSON_AddNumberToObject(root, "max_tokens", 128);
    cJSON_AddNumberToObject(root, "temperature", 1.0);

    payload = cJSON_PrintUnformatted(root);
    if (!payload)
    {
        LOG_E("cJSON print failed");
        goto __exit;
    }

    char *header = RT_NULL;
    const char *uri = "http://open.bigmodel.cn/api/paas/v4/chat/completions";
    webclient_request_header_add(&header, "Content-Type: application/json\r\n");
    webclient_request_header_add(&header, "Authorization: 179856f93aa84432abf016307178bef8.w4nixKrMJI2VmoEQ\r\n");

    if (webclient_request(uri, header, payload, rt_strlen(payload), (void **)&resp, &resp_len) < 0)
    {
        LOG_E("model request failed");
        if (header) web_free(header);
        goto __exit;
    }
    if (header) web_free(header);

    /* parse response */
    cJSON *resp_json = cJSON_ParseWithLength(resp, resp_len);
    if (resp_json)
    {
        cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
        if (choices && cJSON_GetArraySize(choices) > 0)
        {
            cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
            cJSON *message = cJSON_GetObjectItem(choice0, "message");
            cJSON *content = message ? cJSON_GetObjectItem(message, "content") : RT_NULL;
            if (cJSON_IsString(content))
            {
                reply_text = content->valuestring;
            }
        }
        cJSON_Delete(resp_json);
    }

    if (reply_text)
    {
        publish_model_reply(reply_text);
    }
    else
    {
        LOG_W("model reply parse failed");
    }

__exit:
    if (payload) cJSON_free(payload);
    if (resp) web_free(resp);
    if (root) cJSON_Delete(root);
    rt_free(mw);
}

static void mqtt_model_request_schedule(const char *prompt)
{
    if (!prompt) return;
    struct model_work *mw = rt_calloc(1, sizeof(struct model_work));
    if (!mw)
    {
        LOG_E("no mem for model work");
        return;
    }
    rt_strncpy(mw->prompt, prompt, sizeof(mw->prompt) - 1);
    rt_work_init(&mw->work, model_work_handler, RT_NULL);
    if (rt_work_submit(&mw->work, 0) != RT_EOK)
    {
        LOG_E("submit model work failed");
        rt_free(mw);
    }
}

/* ---------- mqtt lifecycle ---------- */

static rt_err_t mqtt_client_start(void)
{
    /* init condata param by using MQTTPacket_connectData_initializer */
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;
    static char cid[20] = { 0 };

    if (mqtt_started)
    {
        return RT_EOK;
    }

    mqtt_client.isconnected = 0;
    mqtt_client.uri = MQTT_URI;

    /* generate the random client ID */
    rt_snprintf(cid, sizeof(cid), "rtthread%d", rt_tick_get());
    /* config connect param */
    rt_memcpy(&mqtt_client.condata, &condata, sizeof(condata));
    mqtt_client.condata.clientID.cstring = cid;
    mqtt_client.condata.keepAliveInterval = 30;
    mqtt_client.condata.cleansession = 1;

    /* config MQTT will param. */
    mqtt_client.condata.willFlag = 1;
    mqtt_client.condata.will.qos = 1;
    mqtt_client.condata.will.retained = 0;
    mqtt_client.condata.will.topicName.cstring = MQTT_PUBTOPIC;
    mqtt_client.condata.will.message.cstring = MQTT_WILLMSG;

    /* rt_malloc buffer. */
    mqtt_client.buf_size = mqtt_client.readbuf_size = 1024;
    mqtt_client.buf = rt_calloc(1, mqtt_client.buf_size);
    mqtt_client.readbuf = rt_calloc(1, mqtt_client.readbuf_size);
    if (!(mqtt_client.buf && mqtt_client.readbuf))
    {
        LOG_E("no memory for MQTT client buffer!");
        return -RT_ENOMEM;
    }

    /* set event callback function */
    mqtt_client.connect_callback = mqtt_connect_callback;
    mqtt_client.online_callback = mqtt_online_callback;
    mqtt_client.offline_callback = mqtt_offline_callback;

    /* set subscribe table and event callback */
    mqtt_client.messageHandlers[0].topicFilter = rt_strdup(MQTT_SUBTOPIC);
    mqtt_client.messageHandlers[0].callback = mqtt_sub_callback;
    mqtt_client.messageHandlers[0].qos = QOS1;

    /* set default subscribe event callback */
    mqtt_client.defaultMessageHandler = mqtt_sub_default_callback;

    /* run mqtt client */
    paho_mqtt_start(&mqtt_client);
    mqtt_started = 1;

    return RT_EOK;
}

static rt_err_t temp_publish_start(void)
{
    if (temp_thread_running)
    {
        return RT_EOK;
    }

    temp_thread_running = RT_TRUE;
    if (rt_thread_init(&temp_thread,
                       "mt_pub",
                       temp_publish_thread_entry,
                       RT_NULL,
                       temp_thread_stack,
                       sizeof(temp_thread_stack),
                       MQTT_TEMP_THREAD_PRIO,
                       20) != RT_EOK)
    {
        temp_thread_running = RT_FALSE;
        LOG_E("init temp publish thread failed");
        return -RT_ERROR;
    }

    rt_thread_startup(&temp_thread);
    LOG_I("start publish temperature every %d ms.", MQTT_TEMP_INTERVAL_MS);
    return RT_EOK;
}

static int mqtt_app_autostart(void)
{
    mqtt_client_start();
    temp_publish_start();
    return RT_EOK;
}
/* delay init via system workqueue to wait network ready */
static struct rt_work autostart_work;
static void autostart_work_handler(struct rt_work *work, void *work_data)
{
    (void)work;
    (void)work_data;
    mqtt_app_autostart();
}

static int mqtt_app_autostart_schedule(void)
{
    rt_work_init(&autostart_work, autostart_work_handler, RT_NULL);
    /* delay 10s */
    rt_work_submit(&autostart_work, RT_TICK_PER_SECOND * 10);
    return RT_EOK;
}
INIT_APP_EXPORT(mqtt_app_autostart_schedule);

#endif /* PKG_USING_PAHOMQTT */
