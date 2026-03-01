// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_widget_catalog_overlay.h"

#include "ui_effects.h"
#include "ui_fonts.h"
#include "ui_nav_manager.h"

#include "panel_widget_config.h"
#include "panel_widget_registry.h"
#include "theme_manager.h"

#include <lvgl/lvgl.h>
#include <spdlog/spdlog.h>

#include <string>

namespace helix {

// ============================================================================
// State shared between the overlay and row click callbacks.
// Only one catalog overlay can be open at a time.
// ============================================================================

namespace {

struct CatalogState {
    lv_obj_t* overlay_root = nullptr;
    lv_obj_t* backdrop = nullptr; // Semi-transparent dark backdrop behind the catalog
    WidgetSelectedCallback on_select;
    CatalogClosedCallback on_close;
};

CatalogState g_catalog_state;

void close_catalog() {
    if (g_catalog_state.overlay_root) {
        auto on_close = std::move(g_catalog_state.on_close);
        // Unregister close callback to prevent double-firing via go_back
        NavigationManager::instance().unregister_overlay_close_callback(
            g_catalog_state.overlay_root);
        NavigationManager::instance().go_back();
        // Delete backdrop after nav pop (overlay is deleted by NavigationManager)
        if (g_catalog_state.backdrop) {
            lv_obj_delete(g_catalog_state.backdrop);
            g_catalog_state.backdrop = nullptr;
        }
        g_catalog_state.overlay_root = nullptr;
        g_catalog_state.on_select = nullptr;
        g_catalog_state.on_close = nullptr;
        if (on_close) {
            on_close();
        }
    }
}

} // namespace

// ============================================================================
// Row creation
// ============================================================================

lv_obj_t* WidgetCatalogOverlay::create_row(lv_obj_t* parent, const char* name, const char* icon,
                                           const char* description, int colspan, int rowspan,
                                           bool already_placed, bool hardware_gated) {
    // Row container: horizontal, fixed height
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_set_style_bg_color(row, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Icon
    if (icon && icon[0] != '\0') {
        const char* variant = (already_placed || hardware_gated) ? "muted" : "secondary";
        const char* icon_attrs[] = {"src", icon, "size", "sm", "variant", variant, nullptr};
        lv_xml_create(row, "icon", icon_attrs);
    }

    if (already_placed || hardware_gated) {
        lv_obj_set_style_opa(row, LV_OPA_40, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        // Pressed feedback
        lv_obj_set_style_bg_color(row, theme_get_accent_color(), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_PART_MAIN | LV_STATE_PRESSED);
    }

    // Left side: name + description column
    lv_obj_t* text_col = lv_obj_create(row);
    lv_obj_set_width(text_col, LV_SIZE_CONTENT);
    lv_obj_set_height(text_col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(text_col, 0, 0);
    lv_obj_set_style_bg_opa(text_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_col, 0, 0);
    lv_obj_set_layout(text_col, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(text_col, 1);
    lv_obj_remove_flag(text_col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* name_label = lv_label_create(text_col);
    lv_label_set_text(name_label, name);
    lv_obj_set_style_text_font(name_label, &noto_sans_16, 0);
    lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);

    if (description && description[0] != '\0') {
        lv_obj_t* desc_label = lv_label_create(text_col);
        lv_label_set_text(desc_label, description);
        lv_obj_set_style_text_font(desc_label, &noto_sans_12, 0);
        lv_obj_set_style_text_color(desc_label, theme_manager_get_color("text_muted"), 0);
    }

    // Right side: size badge + optional "Placed" label
    lv_obj_t* right_group = lv_obj_create(row);
    lv_obj_set_size(right_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(right_group, 0, 0);
    lv_obj_set_style_pad_gap(right_group, 6, 0);
    lv_obj_set_style_bg_opa(right_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_group, 0, 0);
    lv_obj_set_layout(right_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_cross_place(right_group, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_remove_flag(right_group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(right_group, LV_OBJ_FLAG_SCROLLABLE);

    if (already_placed) {
        lv_obj_t* placed_label = lv_label_create(right_group);
        lv_label_set_text(placed_label, "Placed");
        lv_obj_set_style_text_font(placed_label, &noto_sans_12, 0);
        lv_obj_set_style_text_color(placed_label, theme_manager_get_color("text_muted"), 0);
    }

    // Size badge (e.g. "2x1")
    lv_obj_t* badge = lv_obj_create(right_group);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(badge, 6, 0);
    lv_obj_set_style_pad_right(badge, 6, 0);
    lv_obj_set_style_pad_top(badge, 2, 0);
    lv_obj_set_style_pad_bottom(badge, 2, 0);
    lv_obj_set_style_bg_color(badge, theme_manager_get_color("secondary"), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 4, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    char size_text[16];
    snprintf(size_text, sizeof(size_text), "%dx%d", colspan, rowspan);
    lv_obj_t* badge_label = lv_label_create(badge);
    lv_label_set_text(badge_label, size_text);
    lv_obj_set_style_text_font(badge_label, &noto_sans_12, 0);
    lv_obj_set_style_text_color(badge_label, theme_manager_get_color("text_muted"), 0);

    return row;
}

// ============================================================================
// Populate rows
// ============================================================================

void WidgetCatalogOverlay::populate_rows(lv_obj_t* scroll, const PanelWidgetConfig& config,
                                         WidgetSelectedCallback /*on_select*/) {
    const auto& defs = get_all_widget_defs();

    for (const auto& def : defs) {
        // Determine if already placed (enabled in config)
        bool already_placed = config.is_enabled(def.id);

        const char* display_name = def.display_name ? def.display_name : def.id;

        // Check hardware gate
        bool hardware_gated = false;
        if (def.hardware_gate_subject) {
            lv_subject_t* gate = lv_xml_get_subject(nullptr, def.hardware_gate_subject);
            if (gate && lv_subject_get_int(gate) == 0) {
                hardware_gated = true;
            }
        }

        // Build display name with "(not detected)" suffix if hardware-gated
        std::string name_str(display_name);
        if (hardware_gated) {
            name_str += " (not detected)";
        }

        lv_obj_t* row = create_row(scroll, name_str.c_str(), def.icon, def.description,
                                   def.colspan, def.rowspan, already_placed, hardware_gated);

        if (!already_placed && !hardware_gated) {
            // Store widget ID in user data for the click handler.
            // The ID string comes from the static widget def table, so the pointer is stable.
            lv_obj_set_user_data(row, const_cast<char*>(def.id));

            // Widget pool recycling exception: dynamic row click handler
            lv_obj_add_event_cb(
                row,
                [](lv_event_t* ev) {
                    auto* widget_id = static_cast<const char*>(lv_event_get_user_data(ev));
                    if (!widget_id) {
                        return;
                    }
                    spdlog::info("[WidgetCatalog] Selected widget: {}", widget_id);
                    // Copy callback and ID before closing (close resets state)
                    auto cb = g_catalog_state.on_select;
                    std::string id_copy(widget_id);
                    close_catalog();
                    if (cb) {
                        cb(id_copy);
                    }
                },
                LV_EVENT_CLICKED, const_cast<char*>(def.id));
        }
    }
}

// ============================================================================
// Show
// ============================================================================

void WidgetCatalogOverlay::show(lv_obj_t* parent_screen, const PanelWidgetConfig& config,
                                WidgetSelectedCallback on_select, CatalogClosedCallback on_close) {
    if (g_catalog_state.overlay_root) {
        spdlog::warn("[WidgetCatalog] Already open, ignoring duplicate show()");
        return;
    }

    // Create a semi-transparent dark backdrop so the home panel shows through.
    // Uses the same modal_backdrop_opacity constant as Modal dialogs (DRY).
    lv_opa_t backdrop_opa = 100; // fallback
    const char* opa_str = lv_xml_get_const(nullptr, "modal_backdrop_opacity");
    if (opa_str) {
        int val = atoi(opa_str);
        if (val >= 0 && val <= 255)
            backdrop_opa = static_cast<lv_opa_t>(val);
    }
    auto* backdrop = helix::ui::create_fullscreen_backdrop(parent_screen, backdrop_opa);
    if (backdrop) {
        // Don't block clicks — let taps on the backdrop close the catalog
        lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    }
    g_catalog_state.backdrop = backdrop;

    // Create overlay from XML
    auto* overlay =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen, "widget_catalog_overlay", nullptr));
    if (!overlay) {
        spdlog::error("[WidgetCatalog] Failed to create widget_catalog_overlay from XML");
        return;
    }

    // Initially hidden (NavigationManager will unhide during push)
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    // Store state
    g_catalog_state.overlay_root = overlay;
    g_catalog_state.on_select = std::move(on_select);
    g_catalog_state.on_close = std::move(on_close);

    // DELETE cleanup exception: detect when NavigationManager pops the overlay
    // without going through close_catalog() (e.g., system back navigation)
    lv_obj_add_event_cb(
        overlay,
        [](lv_event_t* /*e*/) {
            // Clean up backdrop if still present
            if (g_catalog_state.backdrop) {
                lv_obj_delete(g_catalog_state.backdrop);
                g_catalog_state.backdrop = nullptr;
            }
            auto on_close_cb = std::move(g_catalog_state.on_close);
            g_catalog_state.overlay_root = nullptr;
            g_catalog_state.on_select = nullptr;
            g_catalog_state.on_close = nullptr;
            if (on_close_cb) {
                on_close_cb();
            }
        },
        LV_EVENT_DELETE, nullptr);

    // Find scroll container and populate
    lv_obj_t* scroll = lv_obj_find_by_name(overlay, "catalog_scroll");
    if (!scroll) {
        spdlog::error("[WidgetCatalog] catalog_scroll not found in XML");
        lv_obj_delete(overlay);
        g_catalog_state.overlay_root = nullptr;
        g_catalog_state.on_select = nullptr;
        return;
    }

    populate_rows(scroll, config, g_catalog_state.on_select);

    // Register with nullptr lifecycle — this overlay is function-based, not class-based
    NavigationManager::instance().register_overlay_instance(overlay, nullptr);

    // Push onto navigation stack — keep the home panel visible behind the catalog
    NavigationManager::instance().push_overlay(overlay, /*hide_previous=*/false);

    // Register close callback with NavigationManager so that go_back() (e.g., from
    // the header back button) properly cleans up catalog state. NavigationManager hides
    // overlays rather than deleting them, so LV_EVENT_DELETE alone is insufficient.
    NavigationManager::instance().register_overlay_close_callback(overlay, [overlay]() {
        if (g_catalog_state.overlay_root == overlay) {
            if (g_catalog_state.backdrop) {
                lv_obj_delete(g_catalog_state.backdrop);
                g_catalog_state.backdrop = nullptr;
            }
            auto on_close_cb = std::move(g_catalog_state.on_close);
            g_catalog_state.overlay_root = nullptr;
            g_catalog_state.on_select = nullptr;
            g_catalog_state.on_close = nullptr;
            if (on_close_cb) {
                on_close_cb();
            }
            spdlog::debug("[WidgetCatalog] Closed via navigation go_back");
        }
    });

    spdlog::info("[WidgetCatalog] Overlay shown with {} widget definitions",
                 get_all_widget_defs().size());
}

} // namespace helix
