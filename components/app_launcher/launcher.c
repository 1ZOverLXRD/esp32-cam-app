#include "launcher.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "LAUNCHER";

#define MAX_APPS 8
static app_t *s_apps[MAX_APPS] = {NULL};
static int s_app_count = 0;
static int s_current_app = -1;
static bool s_is_open = false;

esp_err_t launcher_init(void)
{
    s_app_count = 0;
    s_current_app = -1;
    s_is_open = false;
    memset(s_apps, 0, sizeof(s_apps));
    ESP_LOGI(TAG, "Launcher initialized");
    return ESP_OK;
}

esp_err_t launcher_register_app(app_t *app)
{
    if (!app || s_app_count >= MAX_APPS) return ESP_ERR_NO_MEM;
    s_apps[s_app_count++] = app;
    ESP_LOGI(TAG, "Registered app: %s", app->name);
    return ESP_OK;
}

int launcher_get_app_count(void)
{
    return s_app_count;
}

app_t *launcher_get_app(int index)
{
    if (index < 0 || index >= s_app_count) return NULL;
    return s_apps[index];
}

void launcher_open_app(int index)
{
    if (s_is_open || index < 0 || index >= s_app_count) return;
    s_current_app = index;
    s_is_open = true;

    app_t *app = s_apps[index];
    ESP_LOGI(TAG, "Opening app: %s", app->name);

    if (app->on_create) app->on_create(lv_scr_act());
}

void launcher_close_app(void)
{
    if (!s_is_open || s_current_app < 0) return;

    app_t *app = s_apps[s_current_app];
    if (app->on_destroy) app->on_destroy();
    if (app->page) {
        lv_obj_del(app->page);
        app->page = NULL;
    }

    s_is_open = false;
    s_current_app = -1;
}

bool launcher_is_app_open(void)
{
    return s_is_open;
}

void launcher_handle_joystick(joystick_evt_t evt)
{
    if (s_is_open && s_current_app >= 0) {
        app_t *app = s_apps[s_current_app];
        if (app->on_joystick) app->on_joystick(evt);
    }
}