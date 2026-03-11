// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_probe_overlay.h"

#include "ui_callback_helpers.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_bed_mesh.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "probe_sensor_manager.h"
#include "probe_sensor_types.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include "hv/json.hpp"

using json = nlohmann::json;

using namespace helix;
using helix::sensors::probe_type_to_display_string;
using helix::sensors::ProbeSensorManager;
using helix::sensors::ProbeSensorType;

// ============================================================================
// GLOBAL INSTANCE AND ROW CLICK HANDLER
// ============================================================================

static std::unique_ptr<ProbeOverlay> g_probe_overlay;

// Forward declarations
static void on_probe_row_clicked(lv_event_t* e);
MoonrakerAPI* get_moonraker_api();
MoonrakerClient* get_moonraker_client();

ProbeOverlay& get_global_probe_overlay() {
    if (!g_probe_overlay) {
        g_probe_overlay = std::make_unique<ProbeOverlay>();
        StaticPanelRegistry::instance().register_destroy("ProbeOverlay",
                                                         []() { g_probe_overlay.reset(); });
    }
    return *g_probe_overlay;
}

ProbeOverlay::~ProbeOverlay() {
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    overlay_root_ = nullptr;
    parent_screen_ = nullptr;

    if (!StaticPanelRegistry::is_destroyed()) {
        spdlog::trace("[Probe] Destroyed");
    }
}

void init_probe_row_handler() {
    lv_xml_register_event_cb(nullptr, "on_probe_row_clicked", on_probe_row_clicked);
    spdlog::trace("[Probe] Row click callback registered");
}

static void on_probe_row_clicked(lv_event_t* e) {
    (void)e;
    spdlog::debug("[Probe] Probe row clicked");

    auto& overlay = get_global_probe_overlay();

    // Lazy-create the probe overlay
    if (!overlay.get_root()) {
        spdlog::debug("[Probe] Creating probe overlay...");

        MoonrakerAPI* api = get_moonraker_api();
        overlay.set_api(api);

        lv_obj_t* screen = lv_display_get_screen_active(nullptr);
        if (!overlay.create(screen)) {
            spdlog::error("[Probe] Failed to create probe_overlay");
            return;
        }
        spdlog::info("[Probe] Overlay created");
    }

    overlay.show();
}

// ============================================================================
// XML EVENT CALLBACK REGISTRATION
// ============================================================================

// Helper to send a GCode command via MoonrakerClient
static void send_probe_gcode(const char* gcode, const char* label) {
    MoonrakerClient* client = get_moonraker_client();
    if (!client) {
        spdlog::error("[Probe] No client for {} command", label);
        return;
    }
    spdlog::debug("[Probe] Sending {}: {}", label, gcode);
    client->gcode_script(gcode);
}

void ui_probe_overlay_register_callbacks() {
    register_xml_callbacks({
        // Universal probe actions
        {"on_probe_accuracy",
         [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_probe_accuracy(); }},
        {"on_zoffset_cal",
         [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_zoffset_cal(); }},
        {"on_bed_mesh", [](lv_event_t* /*e*/) { get_global_probe_overlay().handle_bed_mesh(); }},
        // BLTouch controls
        {"on_bltouch_deploy",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BLTOUCH_DEBUG COMMAND=pin_down", "BLTouch Deploy");
         }},
        {"on_bltouch_stow",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BLTOUCH_DEBUG COMMAND=pin_up", "BLTouch Stow");
         }},
        {"on_bltouch_reset",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BLTOUCH_DEBUG COMMAND=reset", "BLTouch Reset");
         }},
        {"on_bltouch_selftest",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("BLTOUCH_DEBUG COMMAND=self_test", "BLTouch Self-Test");
         }},
        {"on_bltouch_output_5v",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("SET_BLTOUCH OUTPUT_MODE=5V", "BLTouch Output 5V");
         }},
        {"on_bltouch_output_od",
         [](lv_event_t* /*e*/) {
             send_probe_gcode("SET_BLTOUCH OUTPUT_MODE=OD", "BLTouch Output OD");
         }},
    });

    // Cartographer controls
    lv_xml_register_event_cb(nullptr, "on_carto_calibrate", [](lv_event_t* /*e*/) {
        send_probe_gcode("CARTOGRAPHER_CALIBRATE", "Cartographer Calibrate");
    });
    lv_xml_register_event_cb(nullptr, "on_carto_touch_cal", [](lv_event_t* /*e*/) {
        send_probe_gcode("CARTOGRAPHER_TOUCH_CALIBRATE", "Cartographer Touch Calibrate");
    });

    // Beacon controls
    lv_xml_register_event_cb(nullptr, "on_beacon_calibrate", [](lv_event_t* /*e*/) {
        send_probe_gcode("BEACON_CALIBRATE", "Beacon Calibrate");
    });
    lv_xml_register_event_cb(nullptr, "on_beacon_auto_cal", [](lv_event_t* /*e*/) {
        send_probe_gcode("BEACON_AUTO_CALIBRATE", "Beacon Auto-Calibrate");
    });

    // Klicky controls
    lv_xml_register_event_cb(nullptr, "on_klicky_deploy", [](lv_event_t* /*e*/) {
        send_probe_gcode("ATTACH_PROBE", "Klicky Deploy");
    });
    lv_xml_register_event_cb(nullptr, "on_klicky_dock", [](lv_event_t* /*e*/) {
        send_probe_gcode("DOCK_PROBE", "Klicky Dock");
    });

    // Config edit callbacks
    lv_xml_register_event_cb(nullptr, "on_probe_cfg_x_offset", [](lv_event_t* /*e*/) {
        get_global_probe_overlay().handle_config_edit("x_offset", "X Offset",
                                                      "Horizontal offset from nozzle to probe");
    });
    lv_xml_register_event_cb(nullptr, "on_probe_cfg_y_offset", [](lv_event_t* /*e*/) {
        get_global_probe_overlay().handle_config_edit("y_offset", "Y Offset",
                                                      "Vertical offset from nozzle to probe");
    });
    lv_xml_register_event_cb(nullptr, "on_probe_cfg_samples", [](lv_event_t* /*e*/) {
        get_global_probe_overlay().handle_config_edit("samples", "Samples",
                                                      "Number of probe samples per point");
    });
    lv_xml_register_event_cb(nullptr, "on_probe_cfg_speed", [](lv_event_t* /*e*/) {
        get_global_probe_overlay().handle_config_edit("speed", "Probe Speed",
                                                      "Speed (mm/s) during probing moves");
    });
    lv_xml_register_event_cb(nullptr, "on_probe_cfg_retract", [](lv_event_t* /*e*/) {
        get_global_probe_overlay().handle_config_edit("sample_retract_dist", "Retract Distance",
                                                      "Distance (mm) to retract between samples");
    });
    lv_xml_register_event_cb(nullptr, "on_probe_cfg_tolerance", [](lv_event_t* /*e*/) {
        get_global_probe_overlay().handle_config_edit(
            "samples_tolerance", "Samples Tolerance",
            "Maximum allowed deviation between samples (mm)");
    });

    // Config edit modal buttons
    lv_xml_register_event_cb(nullptr, "on_probe_config_save", [](lv_event_t* /*e*/) {
        get_global_probe_overlay().handle_config_save();
    });
    lv_xml_register_event_cb(nullptr, "on_probe_config_cancel", [](lv_event_t* /*e*/) {
        get_global_probe_overlay().handle_config_cancel();
    });

    spdlog::trace("[Probe] Event callbacks registered");
}

// ============================================================================
// OVERLAY LIFECYCLE
// ============================================================================

void ProbeOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Display subjects
    UI_MANAGED_SUBJECT_STRING(probe_display_name_, probe_display_name_buf_, "",
                              "probe_display_name", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_type_label_, probe_type_label_buf_, "", "probe_type_label",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_z_offset_display_, probe_z_offset_display_buf_, "--",
                              "probe_z_offset_display", subjects_);

    // Overlay state (0=normal)
    UI_MANAGED_SUBJECT_INT(probe_overlay_state_, 0, "probe_overlay_state", subjects_);

    // Cartographer subjects
    UI_MANAGED_SUBJECT_STRING(probe_carto_coil_temp_, probe_carto_coil_temp_buf_, "--",
                              "probe_carto_coil_temp", subjects_);

    // Beacon subjects
    UI_MANAGED_SUBJECT_STRING(probe_beacon_temp_, probe_beacon_temp_buf_, "--", "probe_beacon_temp",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_beacon_temp_comp_status_, probe_beacon_temp_comp_status_buf_,
                              "--", "probe_beacon_temp_comp_status", subjects_);

    // Klicky detection subject
    UI_MANAGED_SUBJECT_INT(probe_is_klicky_, 0, "probe_is_klicky", subjects_);

    // Config display subjects
    UI_MANAGED_SUBJECT_INT(probe_config_loaded_, 0, "probe_config_loaded", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_x_offset_, probe_cfg_x_offset_buf_, "--",
                              "probe_cfg_x_offset", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_y_offset_, probe_cfg_y_offset_buf_, "--",
                              "probe_cfg_y_offset", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_samples_, probe_cfg_samples_buf_, "--", "probe_cfg_samples",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_speed_, probe_cfg_speed_buf_, "--", "probe_cfg_speed",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_retract_dist_, probe_cfg_retract_dist_buf_, "--",
                              "probe_cfg_retract_dist", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_cfg_tolerance_, probe_cfg_tolerance_buf_, "--",
                              "probe_cfg_tolerance", subjects_);

    // Config edit modal subjects
    UI_MANAGED_SUBJECT_STRING(probe_config_edit_title_, probe_config_edit_title_buf_, "",
                              "probe_config_edit_title", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_config_edit_desc_, probe_config_edit_desc_buf_, "",
                              "probe_config_edit_desc", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_config_edit_current_, probe_config_edit_current_buf_, "",
                              "probe_config_edit_current", subjects_);
    UI_MANAGED_SUBJECT_STRING(probe_config_edit_value_, probe_config_edit_value_buf_, "",
                              "probe_config_edit_value", subjects_);

    subjects_initialized_ = true;
    spdlog::trace("[Probe] Subjects initialized");
}

lv_obj_t* ProbeOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::debug("[Probe] Overlay already created");
        return overlay_root_;
    }

    parent_screen_ = parent;

    // Ensure subjects are initialized before XML creation
    if (!subjects_initialized_) {
        init_subjects();
    }

    spdlog::debug("[Probe] Creating overlay from XML");
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "probe_overlay", nullptr));

    if (!overlay_root_) {
        spdlog::error("[Probe] Failed to create overlay from XML");
        return nullptr;
    }

    // Start hidden (push_overlay will show it)
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Cache type panel container for later swapping
    type_panel_container_ = lv_obj_find_by_name(overlay_root_, "probe_type_panel");

    spdlog::info("[Probe] Overlay created successfully");
    return overlay_root_;
}

void ProbeOverlay::show() {
    if (!overlay_root_) {
        spdlog::error("[Probe] Cannot show: overlay not created");
        return;
    }

    spdlog::debug("[Probe] Showing overlay");

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);

    spdlog::info("[Probe] Overlay shown");
}

void ProbeOverlay::on_activate() {
    spdlog::debug("[Probe] Activated");

    // Update display subjects from current probe state
    update_display_subjects();

    // Load type-specific panel
    load_type_panel();

    // Load config values from Klipper
    load_config_values();
}

void ProbeOverlay::on_deactivate() {
    spdlog::debug("[Probe] Deactivated");
}

void ProbeOverlay::cleanup() {
    spdlog::trace("[Probe] Cleanup");
}

void ProbeOverlay::set_api(MoonrakerAPI* api) {
    api_ = api;
}

// ============================================================================
// DISPLAY SUBJECTS
// ============================================================================

void ProbeOverlay::update_display_subjects() {
    auto& mgr = ProbeSensorManager::instance();
    auto sensors = mgr.get_sensors();

    if (sensors.empty()) {
        snprintf(probe_display_name_buf_, sizeof(probe_display_name_buf_), "No Probe Detected");
        lv_subject_copy_string(&probe_display_name_, probe_display_name_buf_);
        lv_subject_copy_string(&probe_type_label_, "");
        snprintf(probe_z_offset_display_buf_, sizeof(probe_z_offset_display_buf_), "--");
        lv_subject_copy_string(&probe_z_offset_display_, probe_z_offset_display_buf_);
        return;
    }

    // Use first sensor (primary probe)
    const auto& sensor = sensors[0];
    std::string display_name = probe_type_to_display_string(sensor.type);
    snprintf(probe_display_name_buf_, sizeof(probe_display_name_buf_), "%s", display_name.c_str());
    lv_subject_copy_string(&probe_display_name_, probe_display_name_buf_);

    // Type description label
    const char* type_label = "Standard Probe";
    switch (sensor.type) {
    case ProbeSensorType::CARTOGRAPHER:
        type_label = "Eddy Current Scanning Probe";
        break;
    case ProbeSensorType::BEACON:
        type_label = "Eddy Current Probe";
        break;
    case ProbeSensorType::BLTOUCH:
        type_label = "Servo-Actuated Touch Probe";
        break;
    case ProbeSensorType::TAP:
        type_label = "Nozzle Contact Probe";
        break;
    case ProbeSensorType::KLICKY:
        type_label = "Magnetic Dock Probe";
        break;
    case ProbeSensorType::EDDY_CURRENT:
        type_label = "Eddy Current Probe";
        break;
    case ProbeSensorType::SMART_EFFECTOR:
        type_label = "Piezo Contact Probe";
        break;
    case ProbeSensorType::STANDARD:
    default:
        break;
    }
    snprintf(probe_type_label_buf_, sizeof(probe_type_label_buf_), "%s", type_label);
    lv_subject_copy_string(&probe_type_label_, probe_type_label_buf_);

    // Z offset display
    float z_offset = mgr.get_z_offset();
    snprintf(probe_z_offset_display_buf_, sizeof(probe_z_offset_display_buf_), "%.3fmm", z_offset);
    lv_subject_copy_string(&probe_z_offset_display_, probe_z_offset_display_buf_);

    // Set Klicky detection flag for generic panel visibility binding
    lv_subject_set_int(&probe_is_klicky_, sensor.type == ProbeSensorType::KLICKY ? 1 : 0);

    // Cartographer coil temperature (placeholder until live query)
    if (sensor.type == ProbeSensorType::CARTOGRAPHER) {
        snprintf(probe_carto_coil_temp_buf_, sizeof(probe_carto_coil_temp_buf_), "--");
        lv_subject_copy_string(&probe_carto_coil_temp_, probe_carto_coil_temp_buf_);
    }

    // Beacon sensor temperature (placeholder until live query)
    if (sensor.type == ProbeSensorType::BEACON) {
        snprintf(probe_beacon_temp_buf_, sizeof(probe_beacon_temp_buf_), "--");
        lv_subject_copy_string(&probe_beacon_temp_, probe_beacon_temp_buf_);
        snprintf(probe_beacon_temp_comp_status_buf_, sizeof(probe_beacon_temp_comp_status_buf_),
                 "Unknown");
        lv_subject_copy_string(&probe_beacon_temp_comp_status_, probe_beacon_temp_comp_status_buf_);
    }
}

// ============================================================================
// TYPE-SPECIFIC PANEL LOADING
// ============================================================================

void ProbeOverlay::load_type_panel() {
    if (!type_panel_container_) {
        spdlog::warn("[Probe] Type panel container not found");
        return;
    }

    // Clear existing type panel children
    lv_obj_clean(type_panel_container_);

    auto& mgr = ProbeSensorManager::instance();
    auto sensors = mgr.get_sensors();

    if (sensors.empty()) {
        spdlog::debug("[Probe] No sensors, skipping type panel load");
        return;
    }

    const auto& sensor = sensors[0];
    const char* component = nullptr;

    switch (sensor.type) {
    case ProbeSensorType::BLTOUCH:
        component = "probe_bltouch_panel";
        break;
    case ProbeSensorType::CARTOGRAPHER:
        component = "probe_cartographer_panel";
        break;
    case ProbeSensorType::BEACON:
        component = "probe_beacon_panel";
        break;
    default:
        component = "probe_generic_panel";
        break;
    }

    spdlog::debug("[Probe] Loading type panel: {}", component);
    auto* panel = static_cast<lv_obj_t*>(lv_xml_create(type_panel_container_, component, nullptr));
    if (!panel) {
        spdlog::warn("[Probe] Failed to create type panel: {}", component);
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void ProbeOverlay::handle_probe_accuracy() {
    spdlog::debug("[Probe] Probe accuracy test requested");

    MoonrakerClient* client = get_moonraker_client();
    if (!client) {
        ToastManager::instance().show(ToastSeverity::ERROR, "No printer connection");
        return;
    }

    // Send PROBE_ACCURACY command (async via gcode_script)
    int result = client->gcode_script("PROBE_ACCURACY");
    if (result != 0) {
        spdlog::error("[Probe] PROBE_ACCURACY command failed: {}", result);
        ToastManager::instance().show(ToastSeverity::ERROR, "Probe accuracy test failed");
    } else {
        ToastManager::instance().show(ToastSeverity::INFO,
                                      "Probe accuracy test started — results in console");
    }
}

void ProbeOverlay::handle_zoffset_cal() {
    spdlog::debug("[Probe] Z-Offset calibration requested");

    auto& overlay = get_global_zoffset_cal_panel();

    // Lazy-create z-offset overlay
    if (!overlay.get_root()) {
        overlay.init_subjects();
        overlay.set_api(get_moonraker_api());
        overlay.create(lv_display_get_screen_active(nullptr));
    }

    overlay.show();
}

void ProbeOverlay::handle_bed_mesh() {
    spdlog::debug("[Probe] Bed mesh requested");

    auto& panel = get_global_bed_mesh_panel();

    // Lazy-create bed mesh overlay
    if (!panel.get_root()) {
        if (!panel.are_subjects_initialized()) {
            panel.init_subjects();
        }
        panel.register_callbacks();
        auto* root = panel.create(lv_display_get_screen_active(nullptr));
        if (root) {
            NavigationManager::instance().register_overlay_instance(root, &panel);
        }
    }

    if (panel.get_root()) {
        NavigationManager::instance().push_overlay(panel.get_root());
    }
}

// ============================================================================
// CONFIG VALUE LOADING
// ============================================================================

std::string ProbeOverlay::get_probe_config_section() const {
    auto& mgr = ProbeSensorManager::instance();
    auto sensors = mgr.get_sensors();
    if (sensors.empty())
        return "probe";

    switch (sensors[0].type) {
    case ProbeSensorType::BLTOUCH:
        return "bltouch";
    case ProbeSensorType::SMART_EFFECTOR:
        return "smart_effector";
    default:
        return "probe";
    }
}

void ProbeOverlay::load_config_values() {
    if (!api_) {
        spdlog::debug("[Probe] No API, skipping config load");
        return;
    }

    probe_section_ = get_probe_config_section();
    spdlog::debug("[Probe] Loading config values for [{}]", probe_section_);

    api_->query_configfile(
        [this](const json& config) {
            // query_configfile returns the full config object
            // Section names in the config JSON are lowercased
            if (!config.contains(probe_section_)) {
                spdlog::debug("[Probe] Section [{}] not found in config", probe_section_);
                return;
            }

            const auto& section = config[probe_section_];

            // Helper to extract string value and copy to subject buffer
            auto set_cfg = [](const json& sec, const char* key, char* buf, size_t buf_size,
                              lv_subject_t* subject) {
                if (sec.contains(key)) {
                    std::string val = sec[key].get<std::string>();
                    snprintf(buf, buf_size, "%s", val.c_str());
                } else {
                    snprintf(buf, buf_size, "default");
                }
                lv_subject_copy_string(subject, buf);
            };

            helix::ui::queue_update([this, section, set_cfg]() {
                set_cfg(section, "x_offset", probe_cfg_x_offset_buf_,
                        sizeof(probe_cfg_x_offset_buf_), &probe_cfg_x_offset_);
                set_cfg(section, "y_offset", probe_cfg_y_offset_buf_,
                        sizeof(probe_cfg_y_offset_buf_), &probe_cfg_y_offset_);
                set_cfg(section, "samples", probe_cfg_samples_buf_, sizeof(probe_cfg_samples_buf_),
                        &probe_cfg_samples_);
                set_cfg(section, "speed", probe_cfg_speed_buf_, sizeof(probe_cfg_speed_buf_),
                        &probe_cfg_speed_);
                set_cfg(section, "sample_retract_dist", probe_cfg_retract_dist_buf_,
                        sizeof(probe_cfg_retract_dist_buf_), &probe_cfg_retract_dist_);
                set_cfg(section, "samples_tolerance", probe_cfg_tolerance_buf_,
                        sizeof(probe_cfg_tolerance_buf_), &probe_cfg_tolerance_);

                lv_subject_set_int(&probe_config_loaded_, 1);
                spdlog::debug("[Probe] Config values loaded for [{}]", probe_section_);
            });
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[Probe] Failed to query configfile: {}", err.message);
        });
}

// ============================================================================
// CONFIG EDITING
// ============================================================================

void ProbeOverlay::handle_config_edit(const std::string& field_key, const std::string& title,
                                      const std::string& description) {
    spdlog::debug("[Probe] Config edit requested: {}", field_key);

    editing_field_key_ = field_key;

    // Set modal subjects
    snprintf(probe_config_edit_title_buf_, sizeof(probe_config_edit_title_buf_), "Edit %s",
             title.c_str());
    lv_subject_copy_string(&probe_config_edit_title_, probe_config_edit_title_buf_);

    snprintf(probe_config_edit_desc_buf_, sizeof(probe_config_edit_desc_buf_), "%s",
             description.c_str());
    lv_subject_copy_string(&probe_config_edit_desc_, probe_config_edit_desc_buf_);

    // Get current value from the corresponding display subject
    const char* current_val = "--";
    if (field_key == "x_offset")
        current_val = probe_cfg_x_offset_buf_;
    else if (field_key == "y_offset")
        current_val = probe_cfg_y_offset_buf_;
    else if (field_key == "samples")
        current_val = probe_cfg_samples_buf_;
    else if (field_key == "speed")
        current_val = probe_cfg_speed_buf_;
    else if (field_key == "sample_retract_dist")
        current_val = probe_cfg_retract_dist_buf_;
    else if (field_key == "samples_tolerance")
        current_val = probe_cfg_tolerance_buf_;

    snprintf(probe_config_edit_current_buf_, sizeof(probe_config_edit_current_buf_), "%s",
             current_val);
    lv_subject_copy_string(&probe_config_edit_current_, probe_config_edit_current_buf_);

    // Pre-fill edit value with current
    snprintf(probe_config_edit_value_buf_, sizeof(probe_config_edit_value_buf_), "%s", current_val);
    lv_subject_copy_string(&probe_config_edit_value_, probe_config_edit_value_buf_);

    // Show the modal
    edit_modal_ = Modal::show("probe_config_edit_modal");
    if (!edit_modal_) {
        spdlog::error("[Probe] Failed to show config edit modal");
    }
}

void ProbeOverlay::handle_config_save() {
    if (editing_field_key_.empty()) {
        spdlog::warn("[Probe] No field being edited");
        return;
    }

    // Read the input value from the modal
    if (edit_modal_) {
        auto* input = lv_obj_find_by_name(edit_modal_, "probe_config_input");
        if (input) {
            const char* text = lv_textarea_get_text(input);
            if (text && text[0] != '\0') {
                snprintf(probe_config_edit_value_buf_, sizeof(probe_config_edit_value_buf_), "%s",
                         text);
            }
        }
    }

    std::string new_value = probe_config_edit_value_buf_;
    std::string field = editing_field_key_;
    std::string section = probe_section_;

    spdlog::info("[Probe] Saving config: [{}] {} = {}", section, field, new_value);

    // Close the edit modal
    if (edit_modal_) {
        Modal::hide(edit_modal_);
        edit_modal_ = nullptr;
    }

    if (!api_) {
        spdlog::error("[Probe] No API for config edit");
        return;
    }

    // Use the safe edit flow: backup -> edit -> firmware restart -> monitor -> revert on failure
    config_editor_.load_config_files(
        *api_,
        [this, section, field,
         new_value](std::map<std::string, helix::system::SectionLocation> /*section_map*/) {
            config_editor_.safe_edit_value(
                *api_, section, field, new_value,
                [this]() {
                    spdlog::info("[Probe] Config edit saved successfully");
                    helix::ui::queue_update([this]() {
                        // Reload config values to reflect the change
                        load_config_values();
                    });
                },
                [](const std::string& err) {
                    spdlog::error("[Probe] Config edit failed: {}", err);
                    // TODO: Show error to user via modal
                });
        },
        [](const std::string& err) {
            spdlog::error("[Probe] Failed to load config files for edit: {}", err);
        });
}

void ProbeOverlay::handle_config_cancel() {
    spdlog::debug("[Probe] Config edit cancelled");
    editing_field_key_.clear();

    if (edit_modal_) {
        Modal::hide(edit_modal_);
        edit_modal_ = nullptr;
    }
}
