// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "cjk_font_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix::system;

class CjkFontManagerFixture : public LVGLTestFixture {
  public:
    CjkFontManagerFixture() : LVGLTestFixture() {}

    ~CjkFontManagerFixture() override {
        CjkFontManager::instance().shutdown();
    }
};

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: not loaded by default", "[cjk_font]") {
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: English does not load CJK", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("en");
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: German does not load CJK", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("de");
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: Chinese loads CJK", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: Japanese loads CJK", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("ja");
    REQUIRE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: switching zh to en unloads", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CjkFontManager::instance().on_language_changed("en");
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: double load is idempotent", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: switching ja to zh stays loaded", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("ja");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: shutdown cleans up", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(CjkFontManager::instance().is_loaded());

    CjkFontManager::instance().shutdown();
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: shutdown when not loaded is safe", "[cjk_font]") {
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
    CjkFontManager::instance().shutdown();
    REQUIRE_FALSE(CjkFontManager::instance().is_loaded());
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: load sets fallback on compiled fonts", "[cjk_font]") {
    const lv_font_t* pre_fallback = noto_sans_14.fallback;

    CjkFontManager::instance().on_language_changed("zh");

    REQUIRE(noto_sans_14.fallback != nullptr);
    REQUIRE(noto_sans_14.fallback != pre_fallback);
}

TEST_CASE_METHOD(CjkFontManagerFixture,
                 "CjkFontManager: unload clears fallback on compiled fonts", "[cjk_font]") {
    CjkFontManager::instance().on_language_changed("zh");
    REQUIRE(noto_sans_14.fallback != nullptr);

    CjkFontManager::instance().on_language_changed("en");
    REQUIRE(noto_sans_14.fallback == nullptr);
}
