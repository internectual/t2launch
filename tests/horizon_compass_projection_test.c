/* Camera-projection checks for the native horizon compass. The marker points
 * are world-space bearings at eye height; no game process is launched. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>

static void view_projection(const float right[3], const float up[3],
                            const float forward[3], const float position[3],
                            float fov, float width, float height,
                            float matrix[16], float eye[3]) {
    float view[16] = {0}, projection[16] = {0};
    float sine, cosine;
    sky_sincos(fov * (3.14159265358979323846f / 360.0f), &sine, &cosine);
    float tan_h = sine / cosine;
    float tan_v = tan_h * height / width;
    for (unsigned int i = 0; i < 3; i++) {
        view[i] = right[i];
        view[4 + i] = forward[i];
        view[8 + i] = up[i];
    }
    view[3] = -(right[0] * position[0] + right[1] * position[1] + right[2] * position[2]);
    view[7] = -(forward[0] * position[0] + forward[1] * position[1] + forward[2] * position[2]);
    view[11] = -(up[0] * position[0] + up[1] * position[1] + up[2] * position[2]);
    view[15] = 1.0f;
    projection[0] = 1.0f / tan_h;
    projection[6] = 1.0f / tan_v;
    projection[13] = 1.0f;  // clip-w is camera-forward (T2 local +Y)
    float gl_view[16], gl_projection[16];
    for (unsigned int row = 0; row < 4; row++)
        for (unsigned int column = 0; column < 4; column++) {
            gl_view[column * 4 + row] = view[row * 4 + column];
            gl_projection[column * 4 + row] = projection[row * 4 + column];
        }
    assert(hud_demo_compass_compose_view_projection(gl_view, gl_projection, matrix, eye));
}

int main(void) {
    const int viewport[4] = {0, 0, 640, 480};
    const float origin[3] = {17.0f, -23.0f, 42.0f};
    const float north_right[3] = {1, 0, 0};
    const float north_up[3] = {0, 0, 1};
    const float north_forward[3] = {0, 1, 0};
    float matrix[16], eye[3], x, y;
    view_projection(north_right, north_up, north_forward, origin, 90.0f, 640, 480,
                    matrix, eye);
    assert(hud_demo_compass_project(matrix, eye, viewport, 640.0f, 480.0f, 0.0f, &x, &y));
    assert(x > 319.9f && x < 320.1f && y > 239.9f && y < 240.1f);
    /* Due east is 90 degrees off-axis at a 90-degree FOV. */
    assert(!hud_demo_compass_project(matrix, eye, viewport, 640.0f, 480.0f, 90.0f, &x, &y));

    /* Turning counter-clockwise from north to west makes the west bearing the
       forward direction (center), while east is behind the camera. */
    const float west_right[3] = {0, -1, 0};
    const float west_up[3] = {0, 0, 1};
    const float west_forward[3] = {-1, 0, 0};
    view_projection(west_right, west_up, west_forward, origin, 90.0f, 640, 480,
                    matrix, eye);
    assert(hud_demo_compass_project(matrix, eye, viewport, 640.0f, 480.0f, 270.0f, &x, &y));
    assert(x > 319.0f && x < 321.0f);
    assert(!hud_demo_compass_project(matrix, eye, viewport, 640.0f, 480.0f, 90.0f, &x, &y));

    /* Looking up moves the same-height north marker down from the viewport
       midpoint, onto the projected horizon rather than a fixed UI row. */
    float sine, cosine;
    sky_sincos(0.5235987756f, &sine, &cosine);
    const float pitch_right[3] = {1, 0, 0};
    const float pitch_forward[3] = {0, cosine, sine};
    const float pitch_up[3] = {0, -sine, cosine};
    view_projection(pitch_right, pitch_up, pitch_forward, origin, 90.0f, 640, 480,
                    matrix, eye);
    assert(hud_demo_compass_project(matrix, eye, viewport, 640.0f, 480.0f, 0.0f, &x, &y));
    assert(y > 240.0f);

    /* FOV controls the projected marker spread. */
    float narrow_x, wide_x, unused_y;
    view_projection(north_right, north_up, north_forward, origin, 60.0f, 640, 480,
                    matrix, eye);
    assert(hud_demo_compass_project(matrix, eye, viewport, 640.0f, 480.0f, 30.0f,
                                    &narrow_x, &unused_y));
    view_projection(north_right, north_up, north_forward, origin, 120.0f, 640, 480,
                    matrix, eye);
    assert(hud_demo_compass_project(matrix, eye, viewport, 640.0f, 480.0f, 30.0f,
                                    &wide_x, &unused_y));
    assert(narrow_x > wide_x);

    puts("horizon compass world-bearing, counter-clockwise west, pitch, and FOV projection passed");
    return 0;
}
