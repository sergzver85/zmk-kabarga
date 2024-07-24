#include <zmk/events/activity_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zephyr/drivers/led.h>


#include <math.h>
#include <stdlib.h>

#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/workqueue.h>

#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// timeout
#define LED_BLINK_PROFILE_DELAY 200
#define LED_BLINK_CONN_DELAY 140
#define LED_FADE_DELAY 2
#define LED_BATTERY_SHOW_DELAY 700
#define LED_BATTERY_SLEEP_SHOW 1000
#define LED_BATTERY_BLINK_DELAY 200

#define LED_STATUS_ON 100
#define LED_STATUS_OFF 0

// animation options
#define disable_led_sleep_pc
// #define show_led_idle

// led array count
#define CHILD_COUNT(...) +1
#define DT_NUM_CHILD(node_id) (DT_FOREACH_CHILD(node_id, CHILD_COUNT))
#define BACKLIGHT_NUM_LEDS DT_NUM_CHILD(DT_CHOSEN(zmk_backlight))

bool check_conn_working = false;
static enum zmk_usb_conn_state usb_conn_state = ZMK_USB_CONN_NONE;
static bool indicator_busy = false;

struct led {
    const struct device *dev;
    uint32_t id;
};

enum {
    BAT_1,
    BAT_2,
    BAT_3,
    STATUS
};

struct led pwm_leds[] = {
    [BAT_1] = {
        .dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)),
        .id = 0,
    },
    [BAT_2] = {
        .dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)),
        .id = 1,
    },
    [BAT_3] = {
        .dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)),
        .id = 2,
    },
    [STATUS] = {
        .dev = DEVICE_DT_GET(DT_CHOSEN(zmk_backlight)),
        .id = 3,
    },
};

static void led_fade_ON(const struct led *led)
{
    for (int brightness = 0; brightness <= LED_STATUS_ON; brightness++)
    {
        led_set_brightness(led->dev, led->id, brightness);
        k_msleep(LED_FADE_DELAY);
    }
}

static void led_fade_OFF(const struct led *led)
{
    for (int brightness = LED_STATUS_ON; brightness >= LED_STATUS_OFF; brightness--)
    {
        led_set_brightness(led->dev, led->id, brightness);
        k_msleep(LED_FADE_DELAY);
    }
}

static void led_all_OFF() {
    for (int i = 0; i < BACKLIGHT_NUM_LEDS; i++) {
        const struct led *led = &pwm_leds[i];
        int ret = led_off(led->dev, led->id);
    }
}

void led_fade_blink(const struct led *led, uint32_t sleep_ms, const int count)
{
    for (int i = 0; i < count; i++)
    {
        led_fade_ON(led);
        k_msleep(sleep_ms);
        led_fade_OFF(led);
        k_msleep(sleep_ms);
    }
}


void wait_for_indicator_handler(struct k_work *work)
{
    while (indicator_busy)
    {
        k_msleep(200); // Ждать 0.5 секунды
    }
    return;
}

K_WORK_DEFINE(wait_for_indicator_work, wait_for_indicator_handler);

void usb_animation_work_handler(struct k_work *work)
{
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &wait_for_indicator_work);
    indicator_busy = true;
    #ifdef disable_led_sleep_pc
    if (usb_conn_state == USB_DC_SUSPEND)
    {
        led_all_OFF();
        indicator_busy = false;
        return;
    }
    #endif
    for (int i = 0; i < 4; i++)
    {
        led_fade_ON(&pwm_leds[i]);
        k_msleep(LED_BATTERY_BLINK_DELAY / 2);
    }

    k_msleep(LED_BATTERY_BLINK_DELAY);

    for (int i = 3; i >= 0; i--)
    {
        led_fade_OFF(&pwm_leds[i]);
        k_msleep(LED_BATTERY_BLINK_DELAY / 2);
    }
    indicator_busy = false;
}
// Define work for USB animation
K_WORK_DEFINE(usb_animation_work, usb_animation_work_handler);

struct k_work_delayable check_ble_conn_work;

void check_ble_conn_handler(struct k_work *work)
{
    if (!check_conn_working)
    {
        return;
    } 
    else
    {
        if (zmk_ble_active_profile_is_connected() || usb_conn_state != ZMK_USB_CONN_NONE )
        {
            check_conn_working = false;
            return;
        }
        else
        {
            k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &wait_for_indicator_work);
            indicator_busy = true;
            led_fade_blink(&pwm_leds[3], LED_BLINK_CONN_DELAY, 1);
            k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &check_ble_conn_work, K_SECONDS(4)); // Restart work for next status check
            indicator_busy = false;
        }
    }
}
K_WORK_DELAYABLE_DEFINE(check_ble_conn_work, check_ble_conn_handler);

// struct k_work_delayable bat_animation_work;
// void bat_animation_work_handler(struct k_work *work);

void bat_animation_work_handler(struct k_work *work)
{
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &wait_for_indicator_work);
    indicator_busy = true;
    uint8_t level = zmk_battery_state_of_charge();
    if (level != 0)
    {
        if (level <= 15)
        {
            led_fade_blink(&pwm_leds[0], LED_BATTERY_BLINK_DELAY, 3);
        }
        else
        {
            led_fade_ON(&pwm_leds[0]);
        }
        if (level > 30 && level <= 50)
        {
            led_fade_blink(&pwm_leds[1], LED_BATTERY_BLINK_DELAY, 3);
        }
        else if (level > 50)
        {
            led_fade_ON(&pwm_leds[1]);
        }
        if (level > 70 && level <= 90)
        {
            led_fade_blink(&pwm_leds[2], LED_BATTERY_BLINK_DELAY, 3);
        }
        else if (level > 90)
        {
            led_fade_ON(&pwm_leds[2]);
        }
        if (level == 100)
        {
            led_fade_OFF(&pwm_leds[0]);
            led_fade_OFF(&pwm_leds[1]);
            led_fade_OFF(&pwm_leds[2]);
            k_msleep(LED_BATTERY_BLINK_DELAY);
            led_fade_ON(&pwm_leds[0]);
            led_fade_ON(&pwm_leds[1]);
            led_fade_ON(&pwm_leds[2]);
            k_msleep(LED_BATTERY_BLINK_DELAY);
        }
        k_msleep(LED_BATTERY_SHOW_DELAY);
        led_all_OFF();
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &check_ble_conn_work, K_SECONDS(4));
    }
    indicator_busy = false;
}
K_WORK_DELAYABLE_DEFINE(bat_animation_work, bat_animation_work_handler);

static int led_init(const struct device *dev)
{
    led_all_OFF();
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &bat_animation_work, K_SECONDS(1));
    return 0;
}

SYS_INIT(led_init, APPLICATION, 32);

// Show leds on profile changing
int ble_profile_listener(const zmk_event_t *eh)
{
    const struct zmk_ble_active_profile_changed *profile_ev = NULL;
    if ((profile_ev = as_zmk_ble_active_profile_changed(eh)) == NULL)
    {
        return ZMK_EV_EVENT_BUBBLE;
    }
    // For profiles 1-3 led_fade_blink appropriate leds.
    if (profile_ev->index <= 2)
    {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &wait_for_indicator_work);
        indicator_busy = true;
        led_fade_blink(&pwm_leds[profile_ev->index], LED_BLINK_PROFILE_DELAY, 1);
        indicator_busy = false;
    }

    if (!check_conn_working)
    {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &wait_for_indicator_work);
        check_conn_working = true;
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &check_ble_conn_work, K_SECONDS(4));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ble_profile_status, ble_profile_listener)
ZMK_SUBSCRIPTION(ble_profile_status, zmk_ble_active_profile_changed);

// Restore activity after return to active state
int led_state_listener(const zmk_event_t *eh)
{
    enum zmk_activity_state state = zmk_activity_get_state();
    if (state == ZMK_ACTIVITY_ACTIVE && !check_conn_working)
    {
        check_conn_working = true;
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &check_ble_conn_work, K_SECONDS(4));
    }
    // CONFIG_ZMK_IDLE_TIMEOUT Default 30sec
#ifdef show_led_idle
    if (state != ZMK_ACTIVITY_ACTIVE)
    {
        led_bat_animation();
    }
    else
    {
        led_all_OFF();
    }
#else
    // led_bat_animation();
#endif
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_activity_state, led_state_listener)
ZMK_SUBSCRIPTION(led_activity_state, zmk_activity_state_changed);
int usb_conn_listener(const zmk_event_t *eh)
{
    const struct zmk_usb_conn_state_changed *usb_ev = NULL;
    if ((usb_ev = as_zmk_usb_conn_state_changed(eh)) == NULL)
    {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    usb_conn_state = usb_ev->conn_state;

    if (usb_conn_state == ZMK_USB_CONN_POWERED) //ZMK_USB_CONN_HID
    {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &usb_animation_work);
    }
    else
    {
        check_conn_working = true;
        k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &check_ble_conn_work, K_SECONDS(4));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(usb_conn_state_listener, usb_conn_listener)
ZMK_SUBSCRIPTION(usb_conn_state_listener, zmk_usb_conn_state_changed);

void show_battery()
{
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &bat_animation_work);
}

void hide_battery()
{
    led_all_OFF();
}
