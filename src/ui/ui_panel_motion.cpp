// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_motion.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_jog_pad.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_panel_singleton_macros.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "config.h"
#include "format_utils.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "ui_panel_controls.h"
#include "subject_managed_panel.h"
#include "theme_manager.h"
#include "unit_conversions.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <memory>

using namespace helix;

// Forward declarations for XML event callbacks
static void on_motion_z_button(lv_event_t* e);
static void on_motion_qgl(lv_event_t* e);
static void on_motion_z_tilt(lv_event_t* e);
static void on_jog_mode_fine(lv_event_t* e);
static void on_jog_mode_coarse(lv_event_t* e);

// ============================================================================
// Global Instance (via DEFINE_GLOBAL_PANEL macro)
// ============================================================================

DEFINE_GLOBAL_PANEL(MotionPanel, motion)

// ============================================================================
// Constructor
// ============================================================================

MotionPanel::MotionPanel() {
    // Initialize buffer contents (axis labels are in XML, values only here)
    std::strcpy(pos_x_buf_, "— mm");
    std::strcpy(pos_y_buf_, "— mm");
    std::strcpy(pos_z_buf_, "— mm");
    std::strcpy(z_axis_label_buf_, "Z Axis"); // Default before kinematics detected
    std::strcpy(z_up_icon_buf_, "arrow_up");
    std::strcpy(z_down_icon_buf_, "arrow_down");
    std::strcpy(z_large_label_buf_, "10mm");
    std::strcpy(z_small_label_buf_, "1mm");

    // Load persisted jog mode (default: coarse)
    auto* cfg = Config::get_instance();
    if (cfg) {
        bool fine = cfg->get<bool>("/motion/jog_mode_fine", false);
        current_distance_ = fine ? JogDistance::Dist0_1mm : JogDistance::Dist1mm;
    }

    spdlog::trace("[MotionPanel] Instance created");
}

MotionPanel::~MotionPanel() {
    // SubjectManager (subjects_) handles deinit automatically via RAII
    // No need to call deinit_subjects() manually
}

// ============================================================================
// Subject Initialization
// ============================================================================

void MotionPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[{}] Subjects already initialized", get_name());
        return;
    }

    spdlog::debug("[{}] Initializing subjects", get_name());

    // Initialize position subjects with default placeholder values
    // Axis labels are in XML, subjects contain values only
    UI_MANAGED_SUBJECT_STRING(pos_x_subject_, pos_x_buf_, "— mm", "motion_pos_x", subjects_);
    UI_MANAGED_SUBJECT_STRING(pos_y_subject_, pos_y_buf_, "— mm", "motion_pos_y", subjects_);
    UI_MANAGED_SUBJECT_STRING(pos_z_subject_, pos_z_buf_, "— mm", "motion_pos_z", subjects_);

    // Z-axis label: "Bed" (corexy/corexz) or "Print Head" (cartesian/delta)
    UI_MANAGED_SUBJECT_STRING(z_axis_label_subject_, z_axis_label_buf_, "Z Axis",
                              "motion_z_axis_label", subjects_);

    // Z button icons: expand variants for bed-moves, regular for head-moves
    UI_MANAGED_SUBJECT_STRING(z_up_icon_subject_, z_up_icon_buf_, "arrow_up", "motion_z_up_icon",
                              subjects_);
    UI_MANAGED_SUBJECT_STRING(z_down_icon_subject_, z_down_icon_buf_, "arrow_down",
                              "motion_z_down_icon", subjects_);

    // Z button labels — dynamic based on Fine/Coarse mode
    bool is_fine = (current_distance_ == JogDistance::Dist0_1mm);
    std::strcpy(z_large_label_buf_, is_fine ? "1mm" : "10mm");
    std::strcpy(z_small_label_buf_, is_fine ? "0.1mm" : "1mm");
    UI_MANAGED_SUBJECT_STRING(z_large_label_subject_, z_large_label_buf_, z_large_label_buf_,
                              "motion_z_large_label", subjects_);
    UI_MANAGED_SUBJECT_STRING(z_small_label_subject_, z_small_label_buf_, z_small_label_buf_,
                              "motion_z_small_label", subjects_);

    // Jog mode toggle subjects for button styling
    UI_MANAGED_SUBJECT_INT(jog_mode_fine_active_, is_fine ? 1 : 0, "motion_jog_mode_fine_active",
                           subjects_);
    UI_MANAGED_SUBJECT_INT(jog_mode_coarse_active_, is_fine ? 0 : 1, "motion_jog_mode_coarse_active",
                           subjects_);

    // Homing status subjects for declarative bind_style indicators (0=unhomed, 1=homed)
    // Prefixed with motion_ to avoid collision with ControlsPanel's x_homed/y_homed/z_homed
    UI_MANAGED_SUBJECT_INT(motion_x_homed_, 0, "motion_x_homed", subjects_);
    UI_MANAGED_SUBJECT_INT(motion_y_homed_, 0, "motion_y_homed", subjects_);
    UI_MANAGED_SUBJECT_INT(motion_z_homed_, 0, "motion_z_homed", subjects_);

    // Register PrinterState observers (RAII - auto-removed on destruction)
    register_position_observers();

    subjects_initialized_ = true;

    // Sync initial position values (observers only fire on change, not on subscribe)
    // Without this, panel shows dashes until next position update even if printer is homed
    int x_centimm = lv_subject_get_int(get_printer_state().get_gcode_position_x_subject());
    int y_centimm = lv_subject_get_int(get_printer_state().get_gcode_position_y_subject());
    gcode_z_centimm_ = lv_subject_get_int(get_printer_state().get_gcode_position_z_subject());
    actual_z_centimm_ = lv_subject_get_int(get_printer_state().get_position_z_subject());
    int bed_moves = lv_subject_get_int(get_printer_state().get_printer_bed_moves_subject());

    // Update X position display
    float x = static_cast<float>(helix::units::from_centimm(x_centimm));
    current_x_ = x;
    helix::format::format_distance_mm(x, 2, pos_x_buf_, sizeof(pos_x_buf_));
    lv_subject_copy_string(&pos_x_subject_, pos_x_buf_);

    // Update Y position display
    float y = static_cast<float>(helix::units::from_centimm(y_centimm));
    current_y_ = y;
    helix::format::format_distance_mm(y, 2, pos_y_buf_, sizeof(pos_y_buf_));
    lv_subject_copy_string(&pos_y_subject_, pos_y_buf_);

    // Update Z position display (uses gcode_z_centimm_ and actual_z_centimm_ we just set)
    current_z_ = static_cast<float>(helix::units::from_centimm(gcode_z_centimm_));
    update_z_display();

    // Update Z axis label
    update_z_axis_label(bed_moves != 0);

    spdlog::debug("[{}] Subjects initialized: X/Y/Z position + Z-axis label + observers ({} "
                  "subjects managed)",
                  get_name(), subjects_.count());
}

void MotionPanel::deinit_subjects() {
    // NOTE: This method exists for API symmetry with init_subjects() and to support
    // explicit cleanup if needed. However, it's NOT called in the destructor because
    // SubjectManager handles cleanup via RAII. This is intentional - RAII is preferred.
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[{}] Deinitializing subjects", get_name());

    // SubjectManager handles deinitialization of all registered subjects
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

void MotionPanel::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    // Register unified Z-axis button callback (user_data from XML distinguishes buttons)
    lv_xml_register_event_cb(nullptr, "on_motion_z_button", on_motion_z_button);

    // Register leveling button callbacks (delegate to ControlsPanel singleton)
    lv_xml_register_event_cb(nullptr, "on_motion_qgl", on_motion_qgl);
    lv_xml_register_event_cb(nullptr, "on_motion_z_tilt", on_motion_z_tilt);

    // Register jog mode toggle callbacks
    lv_xml_register_event_cb(nullptr, "on_jog_mode_fine", on_jog_mode_fine);
    lv_xml_register_event_cb(nullptr, "on_jog_mode_coarse", on_jog_mode_coarse);

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* MotionPanel::create(lv_obj_t* parent) {
    overlay_root_ = create_overlay_from_xml(parent, "motion_panel");
    if (!overlay_root_)
        return nullptr;

    setup_jog_pad();

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void MotionPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[{}] on_activate()", get_name());

    // Recalculate jog pad size — the wrapper dimensions may differ after re-layout
    if (jog_pad_ && overlay_root_) {
        lv_obj_t* jog_wrapper = lv_obj_get_parent(jog_pad_);
        if (jog_wrapper) {
            lv_obj_update_layout(overlay_root_);
            lv_coord_t w = lv_obj_get_width(jog_wrapper);
            lv_coord_t h = lv_obj_get_height(jog_wrapper);
            lv_coord_t size = LV_MIN(w, h);
            lv_obj_set_width(jog_pad_, size);
            lv_obj_set_height(jog_pad_, size);
        }
    }
}

void MotionPanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// Jog Pad Setup
// ============================================================================

void MotionPanel::setup_jog_pad() {
    // Find overlay_content to access motion panel widgets
    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (!overlay_content) {
        spdlog::error("[{}] overlay_content not found!", get_name());
        return;
    }

    // Find jog pad container from XML and replace it with the widget
    lv_obj_t* jog_pad_container = lv_obj_find_by_name(overlay_content, "jog_pad_container");
    if (!jog_pad_container) {
        spdlog::warn("[{}] jog_pad_container NOT FOUND in XML!", get_name());
        return;
    }

    // Get parent container (jog_pad_wrapper, which flex_grows inside left_column)
    lv_obj_t* jog_wrapper = lv_obj_get_parent(jog_pad_container);

    // Force flex layout resolution so dimensions are available
    lv_obj_update_layout(overlay_root_);

    // Jog pad is square: fit within the wrapper's resolved dimensions.
    // The jog pad draws axis labels (Y+/Y-) that extend beyond the circle edge,
    // so we size the widget to fill the wrapper and let the draw code handle overflow.
    lv_coord_t wrapper_w = lv_obj_get_width(jog_wrapper);
    lv_coord_t wrapper_h = lv_obj_get_height(jog_wrapper);
    lv_coord_t jog_size = LV_MIN(wrapper_w, wrapper_h);

    // Delete placeholder container
    helix::ui::safe_delete(jog_pad_container);

    // Create jog pad widget
    jog_pad_ = ui_jog_pad_create(jog_wrapper);
    if (jog_pad_) {
        lv_obj_set_name(jog_pad_, "jog_pad");
        lv_obj_set_width(jog_pad_, jog_size);
        lv_obj_set_height(jog_pad_, jog_size);
        lv_obj_set_align(jog_pad_, LV_ALIGN_CENTER);

        // Set callbacks - pass 'this' as user_data
        ui_jog_pad_set_jog_callback(jog_pad_, jog_pad_jog_cb, this);
        ui_jog_pad_set_home_callback(jog_pad_, jog_pad_home_cb, this);

        // Set initial distance
        ui_jog_pad_set_distance(jog_pad_, current_distance_);

        spdlog::debug("[{}] Jog pad widget created (size: {}px)", get_name(), jog_size);
    } else {
        spdlog::error("[{}] Failed to create jog pad widget!", get_name());
    }
}

// ============================================================================
// Position Observers
// ============================================================================

void MotionPanel::register_position_observers() {
    // Subscribe to PrinterState position updates so UI reflects real printer position
    // Using observer factory for type-safe lambda-based observers with RAII cleanup

    using helix::ui::observe_int_sync;
    using helix::ui::observe_string;

    // Use gcode position (commanded) for X/Y display and jog calculations
    position_x_observer_ = observe_int_sync<MotionPanel>(
        get_printer_state().get_gcode_position_x_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            float x = static_cast<float>(helix::units::from_centimm(centimm));
            self->current_x_ = x;
            helix::format::format_distance_mm(x, 2, self->pos_x_buf_, sizeof(self->pos_x_buf_));
            lv_subject_copy_string(&self->pos_x_subject_, self->pos_x_buf_);
        });

    position_y_observer_ = observe_int_sync<MotionPanel>(
        get_printer_state().get_gcode_position_y_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            float y = static_cast<float>(helix::units::from_centimm(centimm));
            self->current_y_ = y;
            helix::format::format_distance_mm(y, 2, self->pos_y_buf_, sizeof(self->pos_y_buf_));
            lv_subject_copy_string(&self->pos_y_subject_, self->pos_y_buf_);
        });

    // Z needs both gcode (commanded) and actual (with mesh compensation) positions
    // Display shows commanded with actual in brackets when they differ
    gcode_z_observer_ = observe_int_sync<MotionPanel>(
        get_printer_state().get_gcode_position_z_subject(), this,
        [](MotionPanel* self, int centimm) {
            if (!self->subjects_initialized_)
                return;
            self->gcode_z_centimm_ = centimm;
            self->current_z_ = static_cast<float>(helix::units::from_centimm(centimm));
            self->update_z_display();
        });

    actual_z_observer_ = observe_int_sync<MotionPanel>(get_printer_state().get_position_z_subject(),
                                                       this, [](MotionPanel* self, int centimm) {
                                                           if (!self->subjects_initialized_)
                                                               return;
                                                           self->actual_z_centimm_ = centimm;
                                                           self->update_z_display();
                                                       });

    // Watch for kinematics changes to update Z-axis label ("Bed" vs "Print Head")
    bed_moves_observer_ =
        observe_int_sync<MotionPanel>(get_printer_state().get_printer_bed_moves_subject(), this,
                                      [](MotionPanel* self, int bed_moves) {
                                          if (!self->subjects_initialized_)
                                              return;
                                          self->update_z_axis_label(bed_moves != 0);
                                      });

    // Observe homed_axes from PrinterState to update homing indicator subjects
    // Same pattern as ControlsPanel - parse "xyz" string into individual integer subjects
    homed_axes_observer_ =
        observe_string<MotionPanel>(get_printer_state().get_homed_axes_subject(), this,
                                    [](MotionPanel* self, const char* axes) {
                                        if (!self->subjects_initialized_)
                                            return;
                                        int x = (strchr(axes, 'x') != nullptr) ? 1 : 0;
                                        int y = (strchr(axes, 'y') != nullptr) ? 1 : 0;
                                        int z = (strchr(axes, 'z') != nullptr) ? 1 : 0;

                                        if (lv_subject_get_int(&self->motion_x_homed_) != x)
                                            lv_subject_set_int(&self->motion_x_homed_, x);
                                        if (lv_subject_get_int(&self->motion_y_homed_) != y)
                                            lv_subject_set_int(&self->motion_y_homed_, y);
                                        if (lv_subject_get_int(&self->motion_z_homed_) != z)
                                            lv_subject_set_int(&self->motion_z_homed_, z);
                                    });

    spdlog::debug("[{}] Position + kinematics + homing observers registered (observer factory)",
                  get_name());
}

// Observer callbacks migrated to lambda-based observer factory pattern
// See register_position_observers() for inline observers

void MotionPanel::update_z_axis_label(bool bed_moves) {
    bed_moves_ = bed_moves; // Store for Z button direction inversion
    const char* label = bed_moves ? "Bed" : "Print Head";
    std::strncpy(z_axis_label_buf_, label, sizeof(z_axis_label_buf_) - 1);
    z_axis_label_buf_[sizeof(z_axis_label_buf_) - 1] = '\0';
    lv_subject_copy_string(&z_axis_label_subject_, z_axis_label_buf_);

    // Update Z button icons: expand variants (with platform line) for bed-moves
    const char* up_icon = bed_moves ? "arrow_expand_up" : "arrow_up";
    const char* down_icon = bed_moves ? "arrow_expand_down" : "arrow_down";
    std::strncpy(z_up_icon_buf_, up_icon, sizeof(z_up_icon_buf_) - 1);
    z_up_icon_buf_[sizeof(z_up_icon_buf_) - 1] = '\0';
    lv_subject_copy_string(&z_up_icon_subject_, z_up_icon_buf_);
    std::strncpy(z_down_icon_buf_, down_icon, sizeof(z_down_icon_buf_) - 1);
    z_down_icon_buf_[sizeof(z_down_icon_buf_) - 1] = '\0';
    lv_subject_copy_string(&z_down_icon_subject_, z_down_icon_buf_);

    spdlog::debug("[{}] Z-axis updated: label={}, icons={}/{} (bed_moves={})", get_name(), label,
                  up_icon, down_icon, bed_moves);
}

void MotionPanel::update_z_display() {
    float gcode_z = static_cast<float>(helix::units::from_centimm(gcode_z_centimm_));
    float actual_z = static_cast<float>(helix::units::from_centimm(actual_z_centimm_));

    // Show actual in brackets only when it differs from commanded (e.g., mesh compensation)
    // Use 1 centimm (0.01mm) threshold to avoid floating point noise
    if (std::abs(gcode_z_centimm_ - actual_z_centimm_) > 1) {
        // Special case: compound format not covered by formatter
        snprintf(pos_z_buf_, sizeof(pos_z_buf_), "%.2f [%.2f] mm", gcode_z, actual_z);
    } else {
        helix::format::format_distance_mm(gcode_z, 2, pos_z_buf_, sizeof(pos_z_buf_));
    }
    lv_subject_copy_string(&pos_z_subject_, pos_z_buf_);
}

// ============================================================================
// Z Button Handler
// ============================================================================

void MotionPanel::handle_z_button(const char* name) {
    spdlog::debug("[{}] Z button callback fired! Button name: '{}'", get_name(),
                  name ? name : "(null)");

    if (!name) {
        spdlog::error("[{}] Button has no name!", get_name());
        return;
    }

    // Determine Z distance from current jog mode and button size (large/small)
    // Fine:   large=1mm,  small=0.1mm
    // Coarse: large=10mm, small=1mm
    bool is_fine = (current_distance_ == JogDistance::Dist0_1mm);
    double large_dist = is_fine ? 1.0 : 10.0;
    double small_dist = is_fine ? 0.1 : 1.0;

    double distance = 0.0;
    if (strcmp(name, "z_up_large") == 0) {
        distance = large_dist;
    } else if (strcmp(name, "z_up_small") == 0) {
        distance = small_dist;
    } else if (strcmp(name, "z_down_small") == 0) {
        distance = -small_dist;
    } else if (strcmp(name, "z_down_large") == 0) {
        distance = -large_dist;
    } else {
        spdlog::error("[{}] Unknown button name: '{}'", get_name(), name);
        return;
    }

    // For bed-moves printers (CoreXY etc), invert direction so arrows match physical motion:
    // - Up arrow = bed moves UP toward nozzle = G-code Z- (bed rises, gap decreases)
    // - Down arrow = bed moves DOWN away from nozzle = G-code Z+ (bed lowers, gap increases)
    if (bed_moves_) {
        distance = -distance;
        spdlog::debug("[{}] Bed-moves printer: inverted Z direction for bed movement", get_name());
    }

    spdlog::debug("[{}] Z jog: {:+.0f}mm (bed_moves={})", get_name(), distance, bed_moves_);

    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        // Z feedrate: 600 mm/min (10 mm/s) - slower for safety
        constexpr double Z_FEEDRATE = 600.0;

        api->motion().move_axis(
            'Z', distance, Z_FEEDRATE, []() { spdlog::debug("[MotionPanel] Z jog complete"); },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR("Z jog failed: {}", err.user_message());
            });
    }
}

// ============================================================================
// Jog Pad Callbacks
// ============================================================================

void MotionPanel::jog_pad_jog_cb(JogDirection direction, float distance_mm, void* user_data) {
    auto* self = static_cast<MotionPanel*>(user_data);
    if (self) {
        self->jog(direction, distance_mm);
    }
}

void MotionPanel::jog_pad_home_cb(void* user_data) {
    auto* self = static_cast<MotionPanel*>(user_data);
    if (self) {
        self->home('A'); // Home XY
    }
}

// ============================================================================
// Public API
// ============================================================================

void MotionPanel::set_position(float x, float y, float z) {
    current_x_ = x;
    current_y_ = y;
    current_z_ = z;

    // When set directly via API, gcode and actual are the same
    int z_centimm = helix::units::to_centimm(static_cast<double>(z));
    gcode_z_centimm_ = z_centimm;
    actual_z_centimm_ = z_centimm;

    if (!subjects_initialized_)
        return;

    // Update subjects (will automatically update bound UI elements)
    helix::format::format_distance_mm(x, 2, pos_x_buf_, sizeof(pos_x_buf_));
    helix::format::format_distance_mm(y, 2, pos_y_buf_, sizeof(pos_y_buf_));

    lv_subject_copy_string(&pos_x_subject_, pos_x_buf_);
    lv_subject_copy_string(&pos_y_subject_, pos_y_buf_);
    update_z_display(); // Also copies to pos_z_subject_
}

void MotionPanel::jog(JogDirection direction, float distance_mm) {
    const char* dir_names[] = {"N(+Y)",    "S(-Y)",    "E(+X)",    "W(-X)",
                               "NE(+X+Y)", "NW(-X+Y)", "SE(+X-Y)", "SW(-X-Y)"};

    spdlog::debug("[{}] Jog command: {} {:.1f}mm", get_name(),
                  dir_names[static_cast<int>(direction)], distance_mm);

    // Calculate dx/dy from direction
    float dx = 0.0f, dy = 0.0f;

    switch (direction) {
    case JogDirection::N:
        dy = distance_mm;
        break;
    case JogDirection::S:
        dy = -distance_mm;
        break;
    case JogDirection::E:
        dx = distance_mm;
        break;
    case JogDirection::W:
        dx = -distance_mm;
        break;
    case JogDirection::NE:
        dx = distance_mm;
        dy = distance_mm;
        break;
    case JogDirection::NW:
        dx = -distance_mm;
        dy = distance_mm;
        break;
    case JogDirection::SE:
        dx = distance_mm;
        dy = -distance_mm;
        break;
    case JogDirection::SW:
        dx = -distance_mm;
        dy = -distance_mm;
        break;
    }

    // Send jog commands via Moonraker API
    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        // Default feedrate: 6000 mm/min (100 mm/s) for XY jog moves
        constexpr double JOG_FEEDRATE = 6000.0;

        if (dx != 0.0f) {
            api->motion().move_axis(
                'X', static_cast<double>(dx), JOG_FEEDRATE,
                []() { spdlog::debug("[MotionPanel] X jog complete"); },
                [](const MoonrakerError& err) {
                    NOTIFY_ERROR("X jog failed: {}", err.user_message());
                });
        }
        if (dy != 0.0f) {
            api->motion().move_axis(
                'Y', static_cast<double>(dy), JOG_FEEDRATE,
                []() { spdlog::debug("[MotionPanel] Y jog complete"); },
                [](const MoonrakerError& err) {
                    NOTIFY_ERROR("Y jog failed: {}", err.user_message());
                });
        }
    }
}

void MotionPanel::home(char axis) {
    spdlog::debug("[{}] Home command: {} axis", get_name(), axis);

    MoonrakerAPI* api = get_moonraker_api();
    if (api) {
        // Convert axis char to string for API ("" for all, "X", "Y", "Z", or "XY")
        std::string axes_str;
        if (axis == 'A') {
            axes_str = ""; // Empty string = home all
        } else {
            axes_str = std::string(1, axis);
        }

        api->motion().home_axes(
            axes_str,
            [axis]() {
                if (axis == 'A') {
                    NOTIFY_SUCCESS("All axes homed");
                } else {
                    NOTIFY_SUCCESS("{} axis homed", axis);
                }
            },
            [](const MoonrakerError& err) {
                NOTIFY_ERROR("Homing failed: {}", err.user_message());
            });
    }
}

// ============================================================================
// Static Callback for XML event_cb (Z-axis buttons)
// ============================================================================

static void on_motion_z_button(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionPanel] on_motion_z_button");
    const char* button_id = static_cast<const char*>(lv_event_get_user_data(e));
    if (button_id) {
        get_global_motion_panel().handle_z_button(button_id);
    }
    LVGL_SAFE_EVENT_CB_END();
}

static void on_motion_qgl(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionPanel] on_motion_qgl");
    (void)e;
    get_global_controls_panel().handle_qgl();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_motion_z_tilt(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionPanel] on_motion_z_tilt");
    (void)e;
    get_global_controls_panel().handle_z_tilt();
    LVGL_SAFE_EVENT_CB_END();
}

static void on_jog_mode_fine(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionPanel] on_jog_mode_fine");
    (void)e;
    get_global_motion_panel().set_jog_mode_fine(true);
    LVGL_SAFE_EVENT_CB_END();
}

static void on_jog_mode_coarse(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MotionPanel] on_jog_mode_coarse");
    (void)e;
    get_global_motion_panel().set_jog_mode_fine(false);
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// Jog Mode (Fine/Coarse)
// ============================================================================

void MotionPanel::set_jog_mode_fine(bool fine) {
    JogDistance new_dist = fine ? JogDistance::Dist0_1mm : JogDistance::Dist1mm;
    if (current_distance_ == new_dist)
        return;

    current_distance_ = new_dist;

    // Update jog pad
    if (jog_pad_) {
        ui_jog_pad_set_distance(jog_pad_, current_distance_);
        lv_obj_invalidate(jog_pad_); // Redraw ring labels
    }

    // Update toggle button styles
    if (subjects_initialized_) {
        lv_subject_set_int(&jog_mode_fine_active_, fine ? 1 : 0);
        lv_subject_set_int(&jog_mode_coarse_active_, fine ? 0 : 1);
    }

    // Update Z button labels
    update_z_button_labels();

    // Persist setting
    auto* cfg = Config::get_instance();
    if (cfg) {
        cfg->set("/motion/jog_mode_fine", fine);
        cfg->save();
    }

    spdlog::info("[MotionPanel] Jog mode: {}", fine ? "Fine" : "Coarse");
}

void MotionPanel::update_z_button_labels() {
    if (!subjects_initialized_)
        return;

    bool is_fine = (current_distance_ == JogDistance::Dist0_1mm);
    const char* large = is_fine ? "1mm" : "10mm";
    const char* small = is_fine ? "0.1mm" : "1mm";

    std::strncpy(z_large_label_buf_, large, sizeof(z_large_label_buf_) - 1);
    z_large_label_buf_[sizeof(z_large_label_buf_) - 1] = '\0';
    lv_subject_copy_string(&z_large_label_subject_, z_large_label_buf_);

    std::strncpy(z_small_label_buf_, small, sizeof(z_small_label_buf_) - 1);
    z_small_label_buf_[sizeof(z_small_label_buf_) - 1] = '\0';
    lv_subject_copy_string(&z_small_label_subject_, z_small_label_buf_);
}
