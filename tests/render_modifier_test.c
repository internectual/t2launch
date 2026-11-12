/* Offline checks for render-rule syntax and wildcard matching. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

static unsigned char fake_class_rep[0x40];
static unsigned char fake_object[0x60];
static unsigned char fake_fields[3 * 24];
static unsigned char fake_dictionary[19 * sizeof(uint32_t)];
static unsigned char fake_dynamic_entry[12];
static const char *fake_class_name = "Sky";
static const char *fake_material_field = "materialList";
static const char *fake_position_field = "position";
static const char *fake_extent_field = "extent";
static const char *fake_dynamic_field_name = "customPosition";
static unsigned int fake_sky_pref_reads;
static unsigned int fake_sky_material_sets;
static char fake_sky_material_script[512];

static const char *fake_sky_pref_evaluate(const char *code, unsigned char echo,
                                          const char *file_name, unsigned char no_calls) {
    (void)echo; (void)file_name; (void)no_calls;
    if (strcmp(code, "t2launch_skyRotationSpeed();") == 0) {
        fake_sky_pref_reads++;
        return "0.2";
    }
    if (strncmp(code, "t2launch_renderModifierObjectName(", 34) == 0) return "Sky";
    if (strncmp(code, "t2launch_applySkyMaterialList(", 30) == 0) {
        snprintf(fake_sky_material_script, sizeof fake_sky_material_script, "%s", code);
        fake_sky_material_sets++;
        return "1";
    }
    assert(!"unexpected Torque evaluation in render-modifier fixture");
    return "";
}

static void *fake_get_class_rep(void *object) {
    assert(object == fake_object);
    return fake_class_rep;
}

static void fake_put_word(void *object, size_t offset, uint32_t value) {
    memcpy((unsigned char *)object + offset, &value, sizeof value);
}

static void fake_put_pointer(void *object, size_t offset, const void *value) {
    fake_put_word(object, offset, (uint32_t)(uintptr_t)value);
}

static void setup_fake_object(void) {
    static void *fake_vtable[3];
    fake_vtable[2] = (void *)fake_get_class_rep;
    fake_put_pointer(fake_object, 0, fake_vtable);
    fake_put_word(fake_object, 0x20, 12345);
    fake_put_pointer(fake_object, SHAZBOT_SIM_FIELD_DICTIONARY_OFFSET, fake_dictionary);
    fake_put_pointer(fake_class_rep, 0x18, fake_class_name);
    fake_put_pointer(fake_class_rep, 0x20, fake_fields);
    fake_put_word(fake_class_rep, 0x24, 3);

    const char *field_names[] = {fake_material_field, fake_position_field, fake_extent_field};
    for (size_t i = 0; i < 3; i++) {
        fake_put_pointer(fake_fields, i * 24, field_names[i]);
        fake_put_word(fake_fields, i * 24 + 8, (uint32_t)(0x40 + i * 4));
    }
    fake_put_pointer(fake_dictionary, 0, fake_dynamic_entry);
    fake_put_pointer(fake_dynamic_entry, 0, fake_dynamic_field_name);
    fake_put_pointer(fake_dynamic_entry, 4, "old");
}

int main(void) {
    render_modifier_connect_retry_until_ns = 10000000000ULL;
    render_modifier_connect_last_apply_ns = 1000000000ULL;
    assert(!render_modifier_connect_retry_due(1499999999ULL));
    assert(render_modifier_connect_retry_due(1500000000ULL));
    assert(!render_modifier_connect_retry_due(10000000000ULL));
    render_modifier_connect_retry_until_ns = 0;
    render_modifier_connect_last_apply_ns = 0;
    assert(sky_rotation_speed_value("0") == 0.0f);
    assert(sky_rotation_speed_value("1.25") > 1.249f && sky_rotation_speed_value("1.25") < 1.251f);
    assert(sky_rotation_speed_value("21") == 0.0f);
    assert(sky_rotation_speed_value("not-a-speed") == 0.0f);
    Con__evaluate = fake_sky_pref_evaluate;
    core_runtime_installed = 1;
    store_hook_option("render.sky_rotation_speed", "0.0");
    assert(sky_rotation_speed() == 0.0f && fake_sky_pref_reads == 0);
    store_hook_option("render.sky_rotation_speed", "1.5");
    assert(sky_rotation_speed() > 1.499f && sky_rotation_speed() < 1.501f);
    assert(fake_sky_pref_reads == 0);
    store_hook_option("render.sky_rotation_speed", "");
    float preference_speed = sky_rotation_speed();
    assert(preference_speed > 0.199f && preference_speed < 0.201f);
    assert(fake_sky_pref_reads == 1);
    assert(strstr(render_modifier_scan_script,
                  "%class $= \"SimGroup\" || %class $= \"SimSet\"") != NULL);
    assert(strstr(render_modifier_scan_script,
                  "t2launch_scanNamedModifierGroup(MissionGroup)") != NULL);
    assert(strstr(render_modifier_scan_script, "getSubStr(%class, 0, 3) $= \"Gui\"") == NULL);
    assert(render_glob_match("Hud*", "HudDamage"));
    assert(render_glob_match("Gui?itmapCtrl", "GuiBitmapCtrl"));
    assert(render_glob_match("*osition", "position"));
    assert(!render_glob_match("Hud?", "HudDamage"));
    assert(!render_glob_match("class:Sky*", "Sun"));

    char sky_line[] = "client connect MissionSky.materialList = \"textures/sky_Rollercoaster.dml\"\n";
    struct render_rule sky;
    assert(render_parse_rule(sky_line, RENDER_SCOPE_BOTH, RENDER_PHASE_FRAME, &sky) == 1);
    assert(sky.scope == RENDER_SCOPE_CLIENT && sky.phase == RENDER_PHASE_CONNECT);
    assert(strcmp(sky.object_pattern, "MissionSky") == 0);
    assert(strcmp(sky.field_pattern, "materialList") == 0);
    assert(strcmp(sky.value, "\"textures/sky_Rollercoaster.dml\"") == 0);

    char wildcard_line[] = "client frame class:Gui*.*position* = 20 30\n";
    struct render_rule wildcard;
    assert(render_parse_rule(wildcard_line, RENDER_SCOPE_BOTH, RENDER_PHASE_CONNECT,
                             &wildcard) == 1);
    assert(wildcard.phase == RENDER_PHASE_FRAME);
    assert(render_rule_applies(&wildcard, "HudLayout", "GuiBitmapCtrl", "position"));
    assert(!render_rule_applies(&wildcard, "HudLayout", "GuiBitmapCtrl", "extent"));
    assert(render_rule_applies(&wildcard, "Other", "GuiBitmapCtrl", "position"));
    assert(!render_rule_applies(&wildcard, "HudLayout", "Sun", "position"));
    assert(strcmp(wildcard.value, "20 30") == 0);

    char legacy_line[] = "MissionSky.materialList=textures/sky_Rollercoaster.dml\n";
    struct render_rule legacy;
    assert(render_parse_rule(legacy_line, RENDER_SCOPE_BOTH, RENDER_PHASE_FRAME, &legacy) == 1);
    assert(legacy.scope == RENDER_SCOPE_BOTH && legacy.phase == RENDER_PHASE_FRAME);
    assert(strcmp(legacy.value, "\"textures/sky_Rollercoaster.dml\"") == 0);

    char invalid_line[] = "client now MissionSky.materialList = foo\n";
    assert(render_parse_rule(invalid_line, RENDER_SCOPE_BOTH, RENDER_PHASE_FRAME, &legacy) < 0);

    setup_fake_object();
    assert(rv_memory_snapshot(&render_modifier_memory));
    target = &game_targets[0];
    render_rule_count = 3;
    memset(render_rules, 0, sizeof render_rules);
    snprintf(render_rules[0].object_pattern, sizeof render_rules[0].object_pattern, "class:Sky");
    snprintf(render_rules[0].field_pattern, sizeof render_rules[0].field_pattern, "material*");
    snprintf(render_rules[0].value, sizeof render_rules[0].value, "\"textures/grid.dml\"");
    render_rules[0].scope = RENDER_SCOPE_CLIENT;
    render_rules[0].phase = RENDER_PHASE_FRAME;
    snprintf(render_rules[1].object_pattern, sizeof render_rules[1].object_pattern, "Mission*");
    snprintf(render_rules[1].field_pattern, sizeof render_rules[1].field_pattern, "*position*");
    snprintf(render_rules[1].value, sizeof render_rules[1].value, "\"20 30\"");
    render_rules[1].scope = RENDER_SCOPE_CLIENT;
    render_rules[1].phase = RENDER_PHASE_FRAME;
    snprintf(render_rules[2].object_pattern, sizeof render_rules[2].object_pattern, "PlayGui");
    snprintf(render_rules[2].field_pattern, sizeof render_rules[2].field_pattern, "position");
    snprintf(render_rules[2].value, sizeof render_rules[2].value, "\"4 4\"");
    render_rules[2].scope = RENDER_SCOPE_CLIENT;
    render_rules[2].phase = RENDER_PHASE_FRAME;
    render_build_direct_object_script();
    assert(render_modifier_direct_script_used == 0);
    assert(render_modifier_direct_assign_count[RENDER_PHASE_FRAME] == 1);
    assert(strstr(render_modifier_direct_assign_scripts[RENDER_PHASE_FRAME],
                  "nameToID(\"PlayGui\")") != NULL);
    assert(strstr(render_modifier_direct_assign_scripts[RENDER_PHASE_FRAME],
                  "%object.position = \"4 4\"") != NULL);
    assert(strstr(render_modifier_direct_script, "Mission*") == NULL);
    assert(strstr(render_modifier_direct_script, "class:Sky") == NULL);
    render_field_cache_count = 0;
    memset(render_field_caches, 0, sizeof render_field_caches);
    render_modifier_active_phase = RENDER_PHASE_FRAME;
    render_modifier_collecting = 1;
    render_modifier_output = render_modifier_output_storage;
    render_modifier_output_storage[0] = 0;
    render_modifier_output_used = render_modifier_matches = 0;
    const char *method_argv[] = {
        "SimObject::t2launch_collectRenderModifiers", "object-id", "MissionSky", "Sky"
    };
    assert(strcmp(render_modifier_object_cb(fake_object, 4, method_argv), "") == 0);
    render_modifier_collecting = 0;
    render_modifier_output = NULL;
    assert(strstr(render_modifier_output_storage,
                  "MissionSky.materialList = \"textures/grid.dml\"") != NULL);
    assert(strstr(render_modifier_output_storage, "MissionSky.position = \"20 30\"") != NULL);
    assert(strstr(render_modifier_output_storage,
                  "MissionSky.customPosition = \"20 30\"") != NULL);
    assert(render_modifier_matches == 3);
    size_t unique_output_length = render_modifier_output_used;
    assert(strcmp(render_modifier_object_cb(fake_object, 4, method_argv), "") == 0);
    assert(render_modifier_output_used == unique_output_length);

    render_modifier_output_storage[0] = 0;
    render_modifier_output_used = render_modifier_matches = 0;
    render_modifier_seen_count = 0;
    const char *play_gui_argv[] = {
        "SimObject::t2launch_collectRenderModifiers", "object-id", "PlayGui", "GuiCanvas"
    };
    render_modifier_collecting = 1;
    render_modifier_output = render_modifier_output_storage;
    assert(strcmp(render_modifier_object_cb(fake_object, 4, play_gui_argv), "") == 0);
    render_modifier_collecting = 0;
    render_modifier_output = NULL;
    assert(render_modifier_output_storage[0] == 0);
    assert(render_modifier_matches == 0);

    snprintf(render_rules[3].object_pattern, sizeof render_rules[3].object_pattern, "Sky");
    snprintf(render_rules[3].field_pattern, sizeof render_rules[3].field_pattern, "materialList");
    snprintf(render_rules[3].value, sizeof render_rules[3].value, "\"grid.dml\"");
    render_rules[3].scope = RENDER_SCOPE_CLIENT;
    render_rules[3].phase = RENDER_PHASE_CONNECT;
    render_rule_count = 4;
    render_modifier_method_registered = 1;
    render_sky_modifier_cached_object = NULL;
    render_sky_modifier_cached_id = 0;
    render_sky_modifier_cache_valid = 0;
    render_sky_modifier_context[0] = 0;
    render_sky_connect_retry_until_ns = 0;
    render_sky_connect_last_apply_ns = 0;
    fake_sky_material_sets = 0;
    assert(render_apply_sky_material_modifiers(fake_object) == 2);
    assert(fake_sky_material_sets == 2);
    assert(strstr(fake_sky_material_script, "t2launch_applySkyMaterialList(12345, \"grid.dml\")") != NULL);
    assert(render_apply_sky_material_modifiers(fake_object) == 1);
    assert(fake_sky_material_sets == 3);

    render_rule_count = RENDER_RULE_LIMIT;
    for (size_t i = 0; i < render_rule_count; i++) {
        char name[RENDER_RULE_TEXT];
        snprintf(name, sizeof name, "Object%03u", (unsigned)i);
        size_t length = strlen(name);
        memset(name + length, 'x', sizeof name - length - 1);
        name[sizeof name - 1] = 0;
        snprintf(render_rules[i].object_pattern, sizeof render_rules[i].object_pattern, "%s", name);
        snprintf(render_rules[i].field_pattern, sizeof render_rules[i].field_pattern, "*");
    }
    render_build_direct_object_script();
    assert(render_modifier_direct_script_used > 30000);
    assert(strstr(render_modifier_direct_script, "Object127") != NULL);
    puts("render modifier syntax, direct HUD names, glob, and persistent/dynamic field expansion passed");
    return 0;
}
