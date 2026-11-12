/* Offline checks for render-rule syntax and wildcard matching. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

int main(void) {
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
    puts("render modifier phases, scope, legacy syntax, and glob matching passed");
    return 0;
}
