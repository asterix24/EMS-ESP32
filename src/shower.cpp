/*
 * EMS-ESP - https://github.com/emsesp/EMS-ESP
 * Copyright 2020  Paul Derbyshire
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "shower.h"

namespace emsesp {

uuid::log::Logger Shower::logger_{F_(shower), uuid::log::Facility::CONSOLE};

void Shower::start() {
    EMSESP::webSettingsService.read([&](WebSettings & settings) {
        shower_timer_ = settings.shower_timer;
        shower_alert_ = settings.shower_alert;
    });

    set_shower_state(false, true); // turns shower to off and creates HA topic if not already done
}

void Shower::loop() {
    if (!shower_timer_) {
        return;
    }

    uint32_t time_now = uuid::get_uptime();

    // if already in cold mode, ignore all this logic until we're out of the cold blast
    if (!doing_cold_shot_) {
        // is the hot water running?
        if (EMSESP::tap_water_active()) {
            // if heater was previously off, start the timer
            if (timer_start_ == 0) {
                // hot water just started...
                timer_start_     = time_now;
                timer_pause_     = 0; // remove any last pauses
                doing_cold_shot_ = false;
                duration_        = 0;
                shower_state_    = false;
            } else {
                // hot water has been  on for a while
                // first check to see if hot water has been on long enough to be recognized as a Shower/Bath
                if (!shower_state_ && (time_now - timer_start_) > SHOWER_MIN_DURATION) {
                    set_shower_state(true);
                    publish_shower_data();
                    LOG_DEBUG(F("[Shower] hot water still running, starting shower timer"));
                }
                // check if the shower has been on too long
                else if ((time_now - timer_start_) > SHOWER_MAX_DURATION) {
                    shower_alert_start();
                }
            }
        } else { // hot water is off
            // if it just turned off, record the time as it could be a short pause
            if ((timer_start_) && (timer_pause_ == 0)) {
                timer_pause_ = time_now;
            }

            // if shower has been off for longer than the wait time
            if ((timer_pause_) && ((time_now - timer_pause_) > SHOWER_PAUSE_TIME)) {
                // it is over the wait period, so assume that the shower has finished and calculate the total time and publish
                // because its unsigned long, can't have negative so check if length is less than OFFSET_TIME
                if ((timer_pause_ - timer_start_) > SHOWER_OFFSET_TIME) {
                    duration_ = (timer_pause_ - timer_start_ - SHOWER_OFFSET_TIME);
                    if (duration_ > SHOWER_MIN_DURATION) {
                        publish_shower_data();
                        LOG_DEBUG(F("[Shower] finished with duration %d"), duration_);
                    }
                }

                // reset everything
                timer_start_       = 0;
                timer_pause_       = 0;
                doing_cold_shot_   = false;
                alert_timer_start_ = 0;

                set_shower_state(false);
            }
        }
        return;
    }

    // at this point we're in the shower cold shot (doing_cold_shot_ == true)
    // keep repeating until the time is up
    if ((time_now - alert_timer_start_) > SHOWER_COLDSHOT_DURATION) {
        shower_alert_stop();
    }
}

// turn back on the hot water for the shower
void Shower::shower_alert_stop() {
    if (doing_cold_shot_) {
        LOG_DEBUG(F("Shower Alert stopped"));
        (void)Command::call(EMSdevice::DeviceType::BOILER, "wwtapactivated", "true");
        doing_cold_shot_ = false;
    }
}
// turn off hot water to send a shot of cold
void Shower::shower_alert_start() {
    if (shower_alert_) {
        LOG_DEBUG(F("Shower Alert started"));
        (void)Command::call(EMSdevice::DeviceType::BOILER, "wwtapactivated", "false");
        doing_cold_shot_   = true;
        alert_timer_start_ = uuid::get_uptime(); // timer starts now
    }
}

// Publish to the shower_data topic
// showing whether the shower timer and alert are enabled or disabled
// and the duration of the last shower
void Shower::publish_shower_data() {
    StaticJsonDocument<EMSESP_JSON_SIZE_SMALL> doc;

    if (EMSESP::system_.bool_format() == BOOL_FORMAT_TRUEFALSE) {
        doc["shower_timer"] = shower_timer_;
        doc["shower_alert"] = shower_alert_;
    } else if (EMSESP::system_.bool_format() == BOOL_FORMAT_10) {
        doc["shower_timer"] = shower_timer_ ? 1 : 0;
        doc["shower_alert"] = shower_alert_ ? 1 : 0;
    } else {
        char result[10];
        doc["shower_timer"] = Helpers::render_boolean(result, shower_timer_);
        doc["shower_alert"] = Helpers::render_boolean(result, shower_alert_);
    }

    // only publish shower duration if there is a value
    if (duration_ > SHOWER_MIN_DURATION) {
        char s[50];
        snprintf(s, 50, "%d minutes and %d seconds", (uint8_t)(duration_ / 60000), (uint8_t)((duration_ / 1000) % 60));
        doc["duration"] = s;
    }

    Mqtt::publish(F("shower_data"), doc.as<JsonObject>());
}

// send status of shower to MQTT topic called shower_active - which is determined by the state parameter
// and creates the HA config topic if HA enabled
// force is used by EMSESP::publish_all_loop()
void Shower::set_shower_state(bool state, bool force) {
    if (!shower_timer_ && !shower_alert_) {
        return;
    }

    // sets the state
    shower_state_ = state;

    // only publish if that state has changed
    static bool old_shower_state_;
    if ((shower_state_ == old_shower_state_) && !force) {
        return;
    }
    old_shower_state_ = shower_state_; // copy current state

    // always publish as a string
    char s[7];
    Mqtt::publish(F("shower_active"), Helpers::render_boolean(s, shower_state_)); // https://github.com/emsesp/EMS-ESP/issues/369

    // send out HA MQTT Discovery config topic
    if ((Mqtt::ha_enabled()) && (!ha_configdone_ || force)) {
        ha_configdone_ = true;

        StaticJsonDocument<EMSESP_JSON_SIZE_HA_CONFIG> doc;
        doc["name"]    = FJSON("Shower Active");
        doc["uniq_id"] = FJSON("shower_active");
        doc["~"]       = Mqtt::base();
        doc["stat_t"]  = FJSON("~/shower_active");

        // always render boolean as strings for HA
        char result[10];
        doc[F("payload_on")]  = Helpers::render_boolean(result, true);
        doc[F("payload_off")] = Helpers::render_boolean(result, false);

        JsonObject dev = doc.createNestedObject("dev");
        JsonArray  ids = dev.createNestedArray("ids");
        ids.add("ems-esp");

        char topic[Mqtt::MQTT_TOPIC_MAX_SIZE];
        snprintf(topic, sizeof(topic), "binary_sensor/%s/shower_active/config", Mqtt::base().c_str());
        Mqtt::publish_ha(topic, doc.as<JsonObject>()); // publish the config payload with retain flag
    }
}

} // namespace emsesp
