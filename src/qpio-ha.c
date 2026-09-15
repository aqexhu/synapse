#define _DEFAULT_SOURCE

#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <mosquitto.h>
#include <cjson/cJSON.h>

#define CONSUMER "qPIO-driver"
#define MAX_INPUTS 8
#define MAX_OUTPUTS 4
#define DEBOUNCE_INTERVAL_MS 50.0

/* Pin Mapping Definitions according to AQEX Synapse model variants */
static const unsigned int INPUT_GPIOS_NEXUS[MAX_INPUTS] = {27, 17, 22, 5, 16, 13, 12, 6};
static const unsigned int OUTPUT_GPIOS_NEXUS[MAX_OUTPUTS] = {19, 26, 20, 21};
static const unsigned int INPUT_GPIOS_NEXUS_LITE[2] = {17, 27};
static const unsigned int OUTPUT_GPIOS_NEXUS_LITE[2] = {19, 26};
static const unsigned int OUTPUT_GPIOS_STEEL[MAX_OUTPUTS] = {19, 26, 20, 21};
static const unsigned int OUTPUT_GPIOS_FLEX[MAX_OUTPUTS] = {19, 26, 20, 21};

static char device_type[32] = "synapse_nexus";
static int device_input_count = 8;
static int device_output_count = 4;
static const unsigned int *input_gpios = INPUT_GPIOS_NEXUS;
static const unsigned int *output_gpios = OUTPUT_GPIOS_NEXUS;

/* Global Handles */
static struct gpiod_chip *chip = NULL;
static struct gpiod_line_request *in_request = NULL;
static struct gpiod_line_request *out_request = NULL;
static struct gpiod_edge_event_buffer *evbuf = NULL;

static struct mosquitto *g_mosq = NULL;
static pthread_t g_event_thread;

/* State Cache */
static uint8_t input_states[MAX_INPUTS] = {0};
static uint8_t output_states[MAX_OUTPUTS] = {0};
static struct timespec last_input_time[MAX_INPUTS];

/* Config Defaults */
static bool mqtt_enabled = true;
static char chip_path[64] = "/dev/gpiochip0";
static char mqtt_broker[128] = "127.0.0.1";
static int mqtt_port = 1883;
static char mqtt_user[64] = "";
static char mqtt_pass[64] = "";
static char node_id[64] = "qpio_nexus";
static char base_topic[128] = "qpio";
static char discovery_prefix[64] = "homeassistant";

/* Helper: Time Difference in milliseconds */
static double diff_ms(struct timespec start, struct timespec end)
{
    double s = (double)(end.tv_sec - start.tv_sec) * 1000.0;
    double ns = (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
    return s + ns;
}

static void copy_config_string(char *dest, size_t dest_size, const char *src)
{
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

/* MQTT State Publication */
static void publish_input_state(int index)
{
    if (!mqtt_enabled || !g_mosq) return;
    if (index < 0 || index >= device_input_count) return;

    char topic[256];
    char payload[32];

    snprintf(topic, sizeof(topic), "%s/input/%d/state", base_topic, index + 1);
    snprintf(payload, sizeof(payload), "%s", input_states[index] ? "ON" : "OFF");

    mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 1, true);
}

static void publish_output_state(int index)
{
    if (!mqtt_enabled || !g_mosq) return;
    if (index < 0 || index >= device_output_count) return;

    char topic[256];
    char payload[32];

    snprintf(topic, sizeof(topic), "%s/relay/%d/state", base_topic, index + 1);
    snprintf(payload, sizeof(payload), "%s", output_states[index] ? "ON" : "OFF");

    mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 1, true);
}

/* Home Assistant MQTT Auto-Discovery */
static void publish_ha_discovery(void)
{
    if (!mqtt_enabled || !g_mosq) return;

    char topic[256];
    char payload[1024];

    const char *device =
        "\"device\":{"
        "\"identifiers\":[\"aqex_qpio_nexus\"],"
        "\"name\":\"AQEX Synapse Nexus (qPIO)\","
        "\"manufacturer\":\"AQEX Electronics\","
        "\"model\":\"qPIO IO Module\"}";

    /* Inputs (Binary Sensors) */
    for (int i = 0; i < device_input_count; i++)
    {
        snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/input_%d/config",
                 discovery_prefix, node_id, i + 1);

        snprintf(payload, sizeof(payload),
                 "{\"name\":\"Input %d\","
                 "\"unique_id\":\"%s_input_%d\","
                 "\"state_topic\":\"%s/input/%d/state\","
                 "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
                 "%s}",
                 i + 1, node_id, i + 1, base_topic, i + 1, device);

        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 1, true);
    }

    /* Relay Outputs (Switches) */
    for (int i = 0; i < device_output_count; i++)
    {
        snprintf(topic, sizeof(topic), "%s/switch/%s/relay_%d/config",
                 discovery_prefix, node_id, i + 1);

        snprintf(payload, sizeof(payload),
                 "{\"name\":\"Relay %d\","
                 "\"unique_id\":\"%s_relay_%d\","
                 "\"state_topic\":\"%s/relay/%d/state\","
                 "\"command_topic\":\"%s/relay/%d/set\","
                 "\"payload_on\":\"ON\",\"payload_off\":\"OFF\","
                 "%s}",
                 i + 1, node_id, i + 1, base_topic, i + 1, base_topic, i + 1, device);

        mosquitto_publish(g_mosq, NULL, topic, strlen(payload), payload, 1, true);
    }
}

/* Set Relay Hardware State */
static void set_relay(int index, bool state)
{
    if (index < 0 || index >= device_output_count || !out_request) return;

    enum gpiod_line_value val = state ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    if (gpiod_line_request_set_value(out_request, output_gpios[index], val) == 0)
    {
        output_states[index] = state ? 1 : 0;
        syslog(LOG_INFO, "Relay %d (GPIO %u) set to %s", index + 1, output_gpios[index], state ? "ON" : "OFF");
        publish_output_state(index);
    }
    else
    {
        syslog(LOG_ERR, "Failed to set Relay %d state: %s", index + 1, strerror(errno));
    }
}

/* MQTT Command Callback */
static void on_mqtt_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)obj;

    if (!msg->payload || msg->payloadlen == 0) return;

    char command[16] = {0};
    size_t copy_len = (size_t)msg->payloadlen < sizeof(command) - 1 ? (size_t)msg->payloadlen : sizeof(command) - 1;
    memcpy(command, msg->payload, copy_len);

    for (int i = 0; i < device_output_count; i++)
    {
        char target_topic[256];
        snprintf(target_topic, sizeof(target_topic), "%s/relay/%d/set", base_topic, i + 1);

        if (strcmp(msg->topic, target_topic) == 0)
        {
            if (strcasecmp(command, "ON") == 0 || strcmp(command, "1") == 0)
            {
                set_relay(i, true);
            }
            else if (strcasecmp(command, "OFF") == 0 || strcmp(command, "0") == 0)
            {
                set_relay(i, false);
            }
            break;
        }
    }
}

static void on_mqtt_connect(struct mosquitto *mosq, void *obj, int result)
{
    (void)mosq;
    (void)obj;

    if (result == MOSQ_ERR_SUCCESS)
    {
        syslog(LOG_INFO, "MQTT connection successful: %s:%d", mqtt_broker, mqtt_port);

        char sub_topic[256];
        snprintf(sub_topic, sizeof(sub_topic), "%s/relay/+/set", base_topic);
        int subscribe_result = mosquitto_subscribe(mosq, NULL, sub_topic, 1);
        if (subscribe_result == MOSQ_ERR_SUCCESS)
        {
            syslog(LOG_INFO, "MQTT subscribed to %s", sub_topic);
        }
        else
        {
            syslog(LOG_ERR, "MQTT subscribe failed for %s: %s", sub_topic, mosquitto_strerror(subscribe_result));
        }

        publish_ha_discovery();
    }
    else
    {
        syslog(LOG_ERR, "MQTT connection failed to %s:%d: %s", mqtt_broker, mqtt_port, mosquitto_strerror(result));
    }
}

static void on_mqtt_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;

    if (rc == 0)
    {
        syslog(LOG_INFO, "MQTT disconnected cleanly from %s:%d", mqtt_broker, mqtt_port);
    }
    else
    {
        syslog(LOG_WARNING, "MQTT disconnected unexpectedly from %s:%d: %s", mqtt_broker, mqtt_port, mosquitto_strerror(rc));
    }
}

static void set_device_type_config(const char *type)
{
    if (!type || type[0] == '\0')
        return;

    if (strcasecmp(type, "synapse_nexus") == 0 || strcasecmp(type, "nexus") == 0)
    {
        copy_config_string(device_type, sizeof(device_type), "synapse_nexus");
        device_input_count = 8;
        device_output_count = 4;
        input_gpios = INPUT_GPIOS_NEXUS;
        output_gpios = OUTPUT_GPIOS_NEXUS;
        syslog(LOG_INFO, "Device type set to synapse_nexus (8 inputs, 4 relay outputs)");
    }
    else if (strcasecmp(type, "synapse_nexus_lite") == 0 || strcasecmp(type, "nexus_lite") == 0 || strcasecmp(type, "lite") == 0)
    {
        copy_config_string(device_type, sizeof(device_type), "synapse_nexus_lite");
        device_input_count = 2;
        device_output_count = 2;
        input_gpios = INPUT_GPIOS_NEXUS_LITE;
        output_gpios = OUTPUT_GPIOS_NEXUS_LITE;
        syslog(LOG_INFO, "Device type set to synapse_nexus_lite (2 inputs, 2 relay outputs)");
    }
    else if (strcasecmp(type, "synapse_steel") == 0 || strcasecmp(type, "steel") == 0)
    {
        copy_config_string(device_type, sizeof(device_type), "synapse_steel");
        device_input_count = 0;
        device_output_count = 4;
        input_gpios = NULL;
        output_gpios = OUTPUT_GPIOS_STEEL;
        syslog(LOG_INFO, "Device type set to synapse_steel (0 inputs, 4 relay outputs)");
    }
    else if (strcasecmp(type, "synapse_flex") == 0 || strcasecmp(type, "flex") == 0)
    {
        copy_config_string(device_type, sizeof(device_type), "synapse_flex");
        device_input_count = 0;
        device_output_count = 4;
        input_gpios = NULL;
        output_gpios = OUTPUT_GPIOS_FLEX;
        syslog(LOG_INFO, "Device type set to synapse_flex (0 inputs, 4 relay outputs)");
    }
    else
    {
        syslog(LOG_WARNING, "Unsupported device_type '%s'; using synapse_nexus defaults", type);
        copy_config_string(device_type, sizeof(device_type), "synapse_nexus");
        device_input_count = 8;
        device_output_count = 4;
        input_gpios = INPUT_GPIOS_NEXUS;
        output_gpios = OUTPUT_GPIOS_NEXUS;
    }
}

static void mqtt_init(void)
{
    if (!mqtt_enabled) return;

    mosquitto_lib_init();
    g_mosq = mosquitto_new("qups-guard2-ha", true, NULL);
    if (!g_mosq)
    {
        syslog(LOG_ERR, "Failed to create Mosquitto instance.");
        return;
    }

    mosquitto_message_callback_set(g_mosq, on_mqtt_message);
    mosquitto_connect_callback_set(g_mosq, on_mqtt_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_mqtt_disconnect);

    if (mqtt_user[0] != '\0')
        mosquitto_username_pw_set(g_mosq, mqtt_user, mqtt_pass);

    mosquitto_reconnect_delay_set(g_mosq, 2, 30, true);
    syslog(LOG_INFO, "Attempting MQTT connection to %s:%d", mqtt_broker, mqtt_port);

    int result = mosquitto_connect(g_mosq, mqtt_broker, mqtt_port, 60);
    if (result != MOSQ_ERR_SUCCESS)
    {
        syslog(LOG_ERR, "MQTT connection attempt failed for %s:%d: %s", mqtt_broker, mqtt_port, mosquitto_strerror(result));
    }

    if (mosquitto_loop_start(g_mosq) != MOSQ_ERR_SUCCESS)
    {
        syslog(LOG_ERR, "Failed to start MQTT loop.");
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
        mosquitto_lib_cleanup();
        return;
    }

    syslog(LOG_INFO, "MQTT loop started successfully");
}

/* GPIO Event Monitoring Thread */
static void *gpio_event_thread(void *args)
{
    (void)args;
    evbuf = gpiod_edge_event_buffer_new(64);
    if (!evbuf)
    {
        syslog(LOG_ERR, "Failed to allocate GPIO event buffer.");
        return NULL;
    }

    int64_t timeout_ns = 1000000000LL; /* 1 sec poll timeout */

    while (true)
    {
        int result = gpiod_line_request_wait_edge_events(in_request, timeout_ns);
        if (result == 1)
        {
            int read_events = gpiod_line_request_read_edge_events(in_request, evbuf, 64);
            if (read_events <= 0) continue;

            size_t count = gpiod_edge_event_buffer_get_num_events(evbuf);
            for (size_t i = 0; i < count; ++i)
            {
                struct gpiod_edge_event *event = gpiod_edge_event_buffer_get_event(evbuf, i);
                unsigned int offset = gpiod_edge_event_get_line_offset(event);

                /* Find Input index */
                int input_idx = -1;
                for (int j = 0; j < device_input_count; j++)
                {
                    if (input_gpios[j] == offset)
                    {
                        input_idx = j;
                        break;
                    }
                }

                if (input_idx >= 0)
                {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);

                    if (diff_ms(last_input_time[input_idx], now) < DEBOUNCE_INTERVAL_MS)
                        continue;

                    enum gpiod_line_value val = gpiod_line_request_get_value(in_request, offset);
                    uint8_t current = (val == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;

                    if (input_states[input_idx] != current)
                    {
                        input_states[input_idx] = current;
                        last_input_time[input_idx] = now;
                        syslog(LOG_INFO, "Input %d (GPIO %u) state changed -> %s", input_idx + 1, offset, current ? "ON" : "OFF");
                        publish_input_state(input_idx);
                    }
                }
            }
        }
    }
    return NULL;
}

static void load_config_file(const char *filepath)
{
    FILE *file = fopen(filepath, "rb");
    if (!file)
    {
        syslog(LOG_WARNING, "Failed to open config file '%s': %s", filepath, strerror(errno));
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        syslog(LOG_WARNING, "Failed to seek config file '%s': %s", filepath, strerror(errno));
        fclose(file);
        return;
    }

    long length = ftell(file);
    if (length < 0)
    {
        syslog(LOG_WARNING, "Failed to determine config file size for '%s': %s", filepath, strerror(errno));
        fclose(file);
        return;
    }
    fseek(file, 0, SEEK_SET);

    char *data = malloc((size_t)length + 1);
    if (!data)
    {
        syslog(LOG_ERR, "Out of memory while reading config file '%s'", filepath);
        fclose(file);
        return;
    }

    size_t read_bytes = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (read_bytes != (size_t)length)
    {
        syslog(LOG_WARNING, "Config file '%s' was read partially; expected %ld bytes but got %zu", filepath, length, read_bytes);
    }
    data[read_bytes] = '\0';

    cJSON *json = cJSON_Parse(data);
    free(data);
    if (!json)
    {
        syslog(LOG_WARNING, "Invalid JSON in config file '%s'", filepath);
        return;
    }

    cJSON *device = cJSON_GetObjectItemCaseSensitive(json, "device");
    if (device)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(device, "type");
        if (cJSON_IsString(type) && type->valuestring)
            set_device_type_config(type->valuestring);
    }

    cJSON *device_type_item = cJSON_GetObjectItemCaseSensitive(json, "device_type");
    if (cJSON_IsString(device_type_item) && device_type_item->valuestring)
        set_device_type_config(device_type_item->valuestring);

    cJSON *gpio = cJSON_GetObjectItemCaseSensitive(json, "gpio");
    if (gpio)
    {
        cJSON *path = cJSON_GetObjectItemCaseSensitive(gpio, "chip_path");
        if (cJSON_IsString(path) && path->valuestring)
            copy_config_string(chip_path, sizeof(chip_path), path->valuestring);

        cJSON *device_type_gpio = cJSON_GetObjectItemCaseSensitive(gpio, "device_type");
        if (cJSON_IsString(device_type_gpio) && device_type_gpio->valuestring)
            set_device_type_config(device_type_gpio->valuestring);
    }

    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(json, "mqtt");
    if (mqtt)
    {
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(mqtt, "enabled");
        if (cJSON_IsBool(enabled)) mqtt_enabled = cJSON_IsTrue(enabled);

        cJSON *broker = cJSON_GetObjectItemCaseSensitive(mqtt, "broker");
        if (cJSON_IsString(broker) && broker->valuestring)
            copy_config_string(mqtt_broker, sizeof(mqtt_broker), broker->valuestring);

        cJSON *port = cJSON_GetObjectItemCaseSensitive(mqtt, "port");
        if (cJSON_IsNumber(port) && port->valueint > 0) mqtt_port = port->valueint;

        cJSON *username = cJSON_GetObjectItemCaseSensitive(mqtt, "username");
        if (cJSON_IsString(username) && username->valuestring)
            copy_config_string(mqtt_user, sizeof(mqtt_user), username->valuestring);

        cJSON *password = cJSON_GetObjectItemCaseSensitive(mqtt, "password");
        if (cJSON_IsString(password) && password->valuestring)
            copy_config_string(mqtt_pass, sizeof(mqtt_pass), password->valuestring);

        cJSON *node = cJSON_GetObjectItemCaseSensitive(mqtt, "node_id");
        if (cJSON_IsString(node) && node->valuestring)
            copy_config_string(node_id, sizeof(node_id), node->valuestring);

        cJSON *topic = cJSON_GetObjectItemCaseSensitive(mqtt, "base_topic");
        if (cJSON_IsString(topic) && topic->valuestring)
            copy_config_string(base_topic, sizeof(base_topic), topic->valuestring);
    }

    cJSON_Delete(json);
}

int init_hardware(void)
{
    chip = gpiod_chip_open(chip_path);
    if (!chip)
    {
        chip = gpiod_chip_open("/dev/gpiochip4");
        if (!chip)
        {
            syslog(LOG_ERR, "Failed to open GPIO chip at %s or /dev/gpiochip4", chip_path);
            return -1;
        }
    }

    syslog(LOG_INFO, "GPIO chip opened successfully: %s", chip_path);

    /* Config Output Lines */
    struct gpiod_request_config *out_req_cfg = gpiod_request_config_new();
    struct gpiod_line_settings *out_settings = gpiod_line_settings_new();
    struct gpiod_line_config *out_line_cfg = gpiod_line_config_new();

    gpiod_request_config_set_consumer(out_req_cfg, CONSUMER);
    gpiod_line_settings_set_direction(out_settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(out_settings, GPIOD_LINE_VALUE_INACTIVE);

    gpiod_line_config_add_line_settings(out_line_cfg, output_gpios, device_output_count, out_settings);
    out_request = gpiod_chip_request_lines(chip, out_req_cfg, out_line_cfg);

    gpiod_line_settings_free(out_settings);
    gpiod_line_config_free(out_line_cfg);
    gpiod_request_config_free(out_req_cfg);

    if (!out_request)
    {
        syslog(LOG_ERR, "Failed requesting GPIO output lines");
        return -1;
    }

    /* Config Input Lines */
    if (device_input_count > 0)
    {
        struct gpiod_request_config *in_req_cfg = gpiod_request_config_new();
        struct gpiod_line_settings *in_settings = gpiod_line_settings_new();
        struct gpiod_line_config *in_line_cfg = gpiod_line_config_new();

        gpiod_request_config_set_consumer(in_req_cfg, CONSUMER);
        gpiod_line_settings_set_direction(in_settings, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_edge_detection(in_settings, GPIOD_LINE_EDGE_BOTH);
        gpiod_line_settings_set_bias(in_settings, GPIOD_LINE_BIAS_DISABLED);

        gpiod_line_config_add_line_settings(in_line_cfg, input_gpios, device_input_count, in_settings);
        in_request = gpiod_chip_request_lines(chip, in_req_cfg, in_line_cfg);

        gpiod_line_settings_free(in_settings);
        gpiod_line_config_free(in_line_cfg);
        gpiod_request_config_free(in_req_cfg);

        if (!in_request)
        {
            syslog(LOG_ERR, "Failed requesting GPIO input lines");
            return -1;
        }
    }

    /* Initial Reading & Publish */
    for (int i = 0; i < device_input_count; i++)
    {
        enum gpiod_line_value val = gpiod_line_request_get_value(in_request, input_gpios[i]);
        input_states[i] = (val == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
        publish_input_state(i);
    }

    for (int i = 0; i < device_output_count; i++)
    {
        output_states[i] = 0;
        publish_output_state(i);
    }

    syslog(LOG_INFO, "GPIO hardware initialized successfully for %s (%d inputs, %d outputs)", device_type, device_input_count, device_output_count);
    return 0;
}

void cleanup(void)
{
    if (in_request) gpiod_line_request_release(in_request);
    if (out_request) gpiod_line_request_release(out_request);
    if (evbuf) gpiod_edge_event_buffer_free(evbuf);
    if (chip) gpiod_chip_close(chip);

    if (mqtt_enabled && g_mosq)
    {
        mosquitto_loop_stop(g_mosq, true);
        mosquitto_destroy(g_mosq);
        mosquitto_lib_cleanup();
    }
}

int main(int argc, char **argv)
{
    if (argc == 1)
    {
        fprintf(stderr, "Error: no arguments provided - use: --config <config file>\n");
        return EXIT_FAILURE;
    }

    openlog(CONSUMER, LOG_PID | LOG_NDELAY, LOG_USER);

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            load_config_file(argv[++i]);
        else if (strcmp(argv[i], "--mqtt-broker") == 0 && i + 1 < argc)
            copy_config_string(mqtt_broker, sizeof(mqtt_broker), argv[++i]);
        else if (strcmp(argv[i], "--mqtt-port") == 0 && i + 1 < argc)
            mqtt_port = atoi(argv[++i]);
    }

    mqtt_init();

    if (init_hardware() != 0)
    {
        cleanup();
        closelog();
        return EXIT_FAILURE;
    }

    if (device_input_count > 0 && pthread_create(&g_event_thread, NULL, gpio_event_thread, NULL) != 0)
    {
        syslog(LOG_ERR, "Failed to start event thread");
        cleanup();
        closelog();
        return EXIT_FAILURE;
    }

    if (device_input_count > 0)
        pthread_join(g_event_thread, NULL);
    cleanup();
    closelog();
    return 0;
}
