#ifndef ADC_BATTERY_MONITOR_H
#define ADC_BATTERY_MONITOR_H

#include <driver/gpio.h>
#include <adc_battery_estimation.h>

class AdcBatteryMonitor {
public:
    AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel, float upper_resistor, float lower_resistor, gpio_num_t charging_pin = GPIO_NUM_NC);
    ~AdcBatteryMonitor();

    bool IsCharging();
    bool IsDischarging();
    uint8_t GetBatteryLevel();

private:
    gpio_num_t charging_pin_;
    adc_battery_estimation_handle_t adc_battery_estimation_handle_ = nullptr;
};

#endif // ADC_BATTERY_MONITOR_H
