/* The language tables, built from ui_strings.def by including it once per
 * language with UI_STR() defined to pick out that language's column.
 */

#include "ui_i18n.h"

#include "esp_log.h"

static const char *TAG = "ui_i18n";

/* ---- string tables ----------------------------------------------------- */

#define UI_STR(name, en, ja) en,
static const char *const k_en[UI_STR_COUNT] = {
#include "ui_strings.def"
};
#undef UI_STR

#define UI_STR(name, en, ja) ja,
static const char *const k_ja[UI_STR_COUNT] = {
#include "ui_strings.def"
};
#undef UI_STR

/* Indexed by app_language_t. */
static const char *const *const k_tables[APP_LANG_MAX] = {
    [APP_LANG_EN] = k_en,
    [APP_LANG_JA] = k_ja,
};

/* ---- fonts ------------------------------------------------------------- */

LV_FONT_DECLARE(ui_font_ja_12);
LV_FONT_DECLARE(ui_font_ja_14);
LV_FONT_DECLARE(ui_font_ja_16);
LV_FONT_DECLARE(ui_font_ja_20);
LV_FONT_DECLARE(ui_font_ja_24);
LV_FONT_DECLARE(ui_font_ja_32);

static const lv_font_t *const k_font_en[UI_FONT_SLOT_COUNT] = {
    &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
    &lv_font_montserrat_20, &lv_font_montserrat_24, &lv_font_montserrat_32,
    &lv_font_montserrat_48,
};

/* No 48 px Japanese face: the one 48 px label in the UI is a symbol glyph. */
static const lv_font_t *const k_font_ja[UI_FONT_SLOT_COUNT] = {
    &ui_font_ja_12, &ui_font_ja_14, &ui_font_ja_16,
    &ui_font_ja_20, &ui_font_ja_24, &ui_font_ja_32,
    &lv_font_montserrat_48,
};

/* Latin until ui_i18n_init() says otherwise, so a widget built before init
 * -- there should be none -- still has a usable face. */
const lv_font_t *ui_fonts[UI_FONT_SLOT_COUNT] = {
    &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
    &lv_font_montserrat_20, &lv_font_montserrat_24, &lv_font_montserrat_32,
    &lv_font_montserrat_48,
};

/* ---- state ------------------------------------------------------------- */

static app_language_t s_lang = APP_LANG_EN;

static void apply(app_language_t lang)
{
    const lv_font_t *const *fonts = (lang == APP_LANG_JA) ? k_font_ja : k_font_en;

    for (int i = 0; i < UI_FONT_SLOT_COUNT; i++) {
        ui_fonts[i] = fonts[i];
    }
    s_lang = lang;
}

void ui_i18n_init(void)
{
    app_language_t lang = app_settings()->language;

    /* A settings blob from an older build can carry a language that no
     * longer exists. */
    if (lang >= APP_LANG_MAX) {
        ESP_LOGW(TAG, "unknown language %d, falling back to English", (int)lang);
        lang = APP_LANG_EN;
    }

    apply(lang);
    ESP_LOGI(TAG, "language %s", (lang == APP_LANG_JA) ? "ja" : "en");
}

const char *ui_tr(ui_str_t id)
{
    if (id >= UI_STR_COUNT) {
        return "";
    }

    const char *s = k_tables[s_lang][id];

    /* An empty column means "not translated yet" -- show the English rather
     * than a blank label. */
    return (s && s[0]) ? s : k_en[id];
}

const char *ui_tr_access_result(access_result_t result)
{
    switch (result) {
    case ACCESS_GRANTED:        return UI_T(ACC_RES_GRANTED);
    case ACCESS_GRANTED_MANUAL: return UI_T(ACC_RES_MANUAL);
    case ACCESS_ENROLLED:       return UI_T(ACC_RES_ENROLLED);
    case ACCESS_DENIED_DISABLED: return UI_T(ACC_RES_DISABLED);
    case ACCESS_DENIED_LOCKOUT:  return UI_T(ACC_RES_LOCKOUT);
    default:                    return UI_T(ACC_RES_UNKNOWN);
    }
}

const char *ui_tr_detect_class(detect_class_t cls)
{
    switch (cls) {
    case DETECT_PERSON:  return UI_T(DET_CLASS_PERSON);
    case DETECT_VEHICLE: return UI_T(DET_CLASS_VEHICLE);
    case DETECT_OBJECT:  return UI_T(DET_CLASS_OBJECT);
    default:             return UI_T(DET_CLASS_NONE);
    }
}

app_language_t ui_i18n_language(void)
{
    return s_lang;
}

const lv_font_t *ui_i18n_font(app_language_t lang, ui_font_slot_t slot)
{
    if (slot >= UI_FONT_SLOT_COUNT) {
        return NULL;
    }
    return (lang == APP_LANG_JA) ? k_font_ja[slot] : k_font_en[slot];
}

const lv_font_t *ui_i18n_base_font(void)
{
    return ui_fonts[UI_FONT_SLOT_14];
}

bool ui_i18n_set_language(app_language_t lang)
{
    if (lang >= APP_LANG_MAX || lang == s_lang) {
        return false;
    }

    app_settings()->language = lang;
    app_settings_commit();

    /* Not applied here: the caller restarts, and ui_i18n_init() will read
     * what we just wrote. Swapping the tables under the live screens would
     * only leave them half-translated. */
    return true;
}
