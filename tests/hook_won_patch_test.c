/* Test the actual patch code with read-only pages at the game's addresses. */
#define T2_HOOK_TEST
#include "../t2_hook.c"
#include <assert.h>
#include <sys/stat.h>

/* A BitStream stand-in for the native handshake: bits in order, LSB first. */
struct fake_stream { unsigned char bits[16384]; int length, position; };
static char fake_write_flag(void *stream, int flag) {
    struct fake_stream *fake = stream;
    fake->bits[fake->length++] = (flag & 0xff) != 0;
    return (flag & 0xff) != 0;
}
static void fake_write_int(void *stream, int value, int bits) {
    struct fake_stream *fake = stream;
    for (int i = 0; i < bits; i++) fake->bits[fake->length++] = (value >> i) & 1;
}
static int fake_read_int(void *stream, int bits) {
    struct fake_stream *fake = stream;
    int value = 0;
    for (int i = 0; i < bits; i++)
        if (fake->position < fake->length && fake->bits[fake->position++]) value |= 1 << i;
    return value;
}
static int32_t call_target_offset(uintptr_t site) {
    int32_t offset;
    memcpy(&offset, (const void *)(site + 1), sizeof(offset));
    return offset;
}
/* A HUD bar's value function stand-in (vtable +0x118). */
static float fake_value;
static float fake_bar_value(void *control) { (void)control; return fake_value; }

/* A stand-in for the real glEnable, to see which caps the no-fog wrapper drops. */
static int gl_enable_calls;
static unsigned gl_enable_last_cap;
static void fake_gl_enable(unsigned cap) { gl_enable_calls++; gl_enable_last_cap = cap; }

/* A stand-in for the engine's original alx* int callback, to see pass-through. */
static int soundpack_test_calls;
static int fake_alx_original(void *obj, int argc, const char **argv) {
    (void)obj; (void)argc; (void)argv;
    soundpack_test_calls++;
    return 1060;
}

static int shazbot_command_eval_calls;
static char shazbot_command_eval_script[1024];
static const char *shazbot_fake_command_evaluate(const char *code, unsigned char echo,
                                                 const char *file_name, unsigned char no_calls) {
    (void)echo; (void)file_name; (void)no_calls;
    shazbot_command_eval_calls++;
    snprintf(shazbot_command_eval_script, sizeof shazbot_command_eval_script, "%s", code);
    return "";
}

static void *shazbot_class_rep_watched_object;
static unsigned int shazbot_class_rep_watched_calls;
static unsigned int shazbot_test_velocity_calls;
static float shazbot_test_velocity_vector[3];

/* Small synthetic GameConnection/Player pair for the read-only Shazbot
   telemetry identity and bounds checks. */
struct shazbot_test_fixture {
    unsigned char connection[0x8440];
    unsigned char player[0xA50];
    unsigned char enemies[4][0xA50];
    unsigned char energy_mount[0xA50];
    unsigned char energy_mount_two[0xA50];
    unsigned char client_targets[4][0x48];
    unsigned char beacon[0x30];
    unsigned char image_data[0xcdc];
    unsigned char pack_image_data[0xcdc];
    unsigned char image_shape[0x10];
    unsigned char projectile_data[0x20];
    uint32_t dictionary[4096];
    unsigned char target_manager[SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET +
                                SHAZBOT_TARGET_INFO_COUNT * SHAZBOT_TARGET_INFO_STRIDE];
    unsigned char visible_set[0x3c + 64 * sizeof(uint32_t)];
    uint32_t visible_entries[64];
    unsigned char client_target_list[0x214];
    unsigned char sensor_data[0x70];
    unsigned char player_data[0x384];
    unsigned char energy_mount_data[0x330];
    unsigned char energy_mount_two_data[0x330];
    unsigned char connection_class[0x1c];
    unsigned char player_class[0x1c];
    unsigned char player_data_class[0x1c];
    unsigned char energy_mount_class[0x1c];
    unsigned char energy_mount_data_class[0x1c];
    unsigned char energy_mount_two_class[0x1c];
    unsigned char energy_mount_two_data_class[0x1c];
    unsigned char enemy_classes[4][0x1c];
    unsigned char client_target_classes[4][0x1c];
    unsigned char sensor_class[0x1c];
    unsigned char beacon_class[0x1c];
    unsigned char image_class[0x1c];
    unsigned char pack_image_class[0x24];
    unsigned char terrain_block[0x1000];
    unsigned char terrain_class[0x80];
    char connection_name[32];
    char player_name[16];
    char player_data_name[16];
    char energy_mount_name[24];
    char energy_mount_data_name[24];
    char energy_mount_two_name[24];
    char energy_mount_two_data_name[24];
    char enemy_names[4][16];
    char client_target_names[4][24];
    char sensor_name[16];
    char beacon_name[24];
    char image_name[32];
    char shape_file_field_name[16];
    char image_shape_file_name[40];
    char pack_shape_file_name[40];
    char terrain_name[24];
    char terrain_file_field[24];
    char terrain_square_field[24];
    struct { uint32_t name, type, offset, element_count, table, flags; }
        image_persist_fields[1], pack_image_persist_fields[1], terrain_fields[2];
    uint16_t terrain_heightmap[256 * 256];
    unsigned char terrain_materialmap[256 * 256 * 2];
    uint32_t player_field_buckets[SHAZBOT_SIM_FIELD_BUCKET_COUNT];
    uint32_t image_field_buckets[SHAZBOT_SIM_FIELD_BUCKET_COUNT];
    uint32_t pack_image_field_buckets[SHAZBOT_SIM_FIELD_BUCKET_COUNT];
    struct { uint32_t name, value, next; } field_entries[32];
    char field_names[32][32];
    char field_values[32][32];
    size_t field_count;
    float camera_fov;
    float eye_matrix[16];
    float native_aim_point[3];
    unsigned char native_render_frame[0x28];
    void *vtable[SHAZBOT_CAMERA_FOV_VTABLE_OFFSET / sizeof(void *) + 1];
};
static struct shazbot_test_fixture shazbot_fixture;

static void shazbot_test_put_word(void *object, size_t offset, uint32_t value) {
    memcpy((unsigned char *)object + offset, &value, sizeof value);
}

static void shazbot_test_put_pointer(void *object, size_t offset, const void *pointer) {
    shazbot_test_put_word(object, offset, (uint32_t)(uintptr_t)pointer);
}

static void shazbot_test_add_dynamic_field(void *object, const char *name, const char *value) {
    assert(shazbot_fixture.field_count < sizeof shazbot_fixture.field_entries /
                                          sizeof shazbot_fixture.field_entries[0]);
    size_t index = shazbot_fixture.field_count++;
    snprintf(shazbot_fixture.field_names[index], sizeof shazbot_fixture.field_names[index],
             "%s", name);
    snprintf(shazbot_fixture.field_values[index], sizeof shazbot_fixture.field_values[index],
             "%s", value);
    uint32_t *buckets = object == shazbot_fixture.player
        ? shazbot_fixture.player_field_buckets
        : object == shazbot_fixture.pack_image_data
            ? shazbot_fixture.pack_image_field_buckets
            : shazbot_fixture.image_field_buckets;
    void *entry = &shazbot_fixture.field_entries[index];
    shazbot_test_put_pointer(entry, 0, shazbot_fixture.field_names[index]);
    shazbot_test_put_pointer(entry, 4, shazbot_fixture.field_values[index]);
    shazbot_test_put_pointer(entry, 8, (void *)(uintptr_t)buckets[0]);
    shazbot_test_put_pointer(buckets, 0, entry);
    if (!rv_pointer(object, SHAZBOT_SIM_FIELD_DICTIONARY_OFFSET))
        shazbot_test_put_pointer(object, SHAZBOT_SIM_FIELD_DICTIONARY_OFFSET, buckets);
}

static void *shazbot_test_get_class_rep(void *object) {
    if (object == shazbot_class_rep_watched_object) shazbot_class_rep_watched_calls++;
    return (void *)(uintptr_t)rv_word(object, 0x24);
}

static void shazbot_test_get_velocity(float *out, void *object) {
    (void)object;
    shazbot_test_velocity_calls++;
    memcpy(out, shazbot_test_velocity_vector, sizeof shazbot_test_velocity_vector);
}

static void shazbot_test_get_eye_transform(void *object, float *matrix) {
    (void)object;
    memcpy(matrix, shazbot_fixture.eye_matrix, sizeof shazbot_fixture.eye_matrix);
}

static float shazbot_test_get_camera_fov(void *object) {
    (void)object;
    return shazbot_fixture.camera_fov;
}

static void shazbot_test_set_class(unsigned char *object, unsigned char *class_rep,
                                   const char *name) {
    shazbot_test_put_pointer(object, 0, shazbot_fixture.vtable);
    shazbot_test_put_pointer(object, 0x24, class_rep);
    shazbot_test_put_pointer(class_rep, 0x18, name);
}

static void shazbot_test_set_shape_file_field(unsigned char *object,
                                               unsigned char *class_rep,
                                               void *persist_fields,
                                               const char *shape_file,
                                               char *shape_name_storage,
                                               size_t shape_name_capacity,
                                               const char *shape_name,
                                               size_t array_slot,
                                               size_t count_slot) {
    snprintf(shazbot_fixture.shape_file_field_name,
             sizeof shazbot_fixture.shape_file_field_name, "%s", shape_file);
    shazbot_test_put_pointer(persist_fields, 0,
                             shazbot_fixture.shape_file_field_name);
    shazbot_test_put_word(persist_fields, 8, 0x100);
    shazbot_test_put_word(persist_fields, 12, 1);
    shazbot_test_put_word(class_rep, count_slot, 1);
    shazbot_test_put_pointer(class_rep, array_slot, persist_fields);
    snprintf(shape_name_storage, shape_name_capacity, "%s", shape_name);
    shazbot_test_put_pointer(object, 0x100, shape_name_storage);
}

static void shazbot_test_memory(struct rv_memory *memory) {
    uintptr_t data = (uintptr_t)&shazbot_fixture;
    struct rv_map readable = {data, data + sizeof shazbot_fixture, PROT_READ | PROT_WRITE};
    memory->maps[0] = readable;
    memory->count = 1;
    const uintptr_t code[] = {
        (uintptr_t)shazbot_test_get_class_rep,
        (uintptr_t)shazbot_test_get_velocity,
        (uintptr_t)shazbot_test_get_eye_transform,
        (uintptr_t)shazbot_test_get_camera_fov,
    };
    for (size_t i = 0; i < sizeof code / sizeof code[0]; i++) {
        memory->maps[memory->count++] = (struct rv_map){code[i], code[i] + 1, PROT_READ | PROT_EXEC};
    }
    for (size_t i = 1; i < memory->count; i++) {
        struct rv_map item = memory->maps[i];
        size_t j = i;
        while (j && memory->maps[j - 1].start > item.start) {
            memory->maps[j] = memory->maps[j - 1];
            j--;
        }
        memory->maps[j] = item;
    }
}

static void shazbot_test_prepare_player(uint32_t id, const char *class_name) {
    memset(&shazbot_fixture, 0, sizeof shazbot_fixture);
    snprintf(shazbot_fixture.connection_name, sizeof shazbot_fixture.connection_name,
             "GameConnection");
    snprintf(shazbot_fixture.player_name, sizeof shazbot_fixture.player_name,
             "%s", class_name);
    snprintf(shazbot_fixture.sensor_name, sizeof shazbot_fixture.sensor_name, "SensorData");
    snprintf(shazbot_fixture.beacon_name, sizeof shazbot_fixture.beacon_name, "BeaconObject");
    snprintf(shazbot_fixture.image_name, sizeof shazbot_fixture.image_name, "ShapeBaseImageData");
    shazbot_fixture.vtable[2] = (void *)shazbot_test_get_class_rep;
    shazbot_fixture.vtable[0xEC / sizeof(void *)] = (void *)shazbot_test_get_velocity;
    shazbot_fixture.vtable[SHAZBOT_EYE_TRANSFORM_VTABLE_OFFSET / sizeof(void *)] =
        (void *)shazbot_test_get_eye_transform;
    shazbot_fixture.vtable[SHAZBOT_CAMERA_FOV_VTABLE_OFFSET / sizeof(void *)] =
        (void *)shazbot_test_get_camera_fov;
    shazbot_fixture.camera_fov = 90.0f;
    shazbot_fixture.eye_matrix[0] = 1.0f;
    shazbot_fixture.eye_matrix[5] = 1.0f;
    shazbot_fixture.eye_matrix[10] = 1.0f;
    shazbot_fixture.eye_matrix[15] = 1.0f;
    shazbot_fixture.eye_matrix[3] = 11.0f;
    shazbot_fixture.eye_matrix[7] = 22.0f;
    shazbot_fixture.eye_matrix[11] = 33.0f;
    shazbot_test_set_class(shazbot_fixture.connection, shazbot_fixture.connection_class,
                           shazbot_fixture.connection_name);
    shazbot_test_set_class(shazbot_fixture.player, shazbot_fixture.player_class,
                           shazbot_fixture.player_name);
    shazbot_test_set_class(shazbot_fixture.sensor_data, shazbot_fixture.sensor_class,
                           shazbot_fixture.sensor_name);
    shazbot_test_set_class(shazbot_fixture.beacon, shazbot_fixture.beacon_class,
                           shazbot_fixture.beacon_name);
    shazbot_test_set_class(shazbot_fixture.image_data, shazbot_fixture.image_class,
                           shazbot_fixture.image_name);
    shazbot_test_set_class(shazbot_fixture.pack_image_data, shazbot_fixture.pack_image_class,
                           shazbot_fixture.image_name);
    shazbot_test_set_shape_file_field(shazbot_fixture.image_data,
        shazbot_fixture.image_class, shazbot_fixture.image_persist_fields,
        "shapeFile", shazbot_fixture.image_shape_file_name,
        sizeof shazbot_fixture.image_shape_file_name, "weapon_energy.dts", 12, 4);
    shazbot_test_set_shape_file_field(shazbot_fixture.pack_image_data,
        shazbot_fixture.pack_image_class, shazbot_fixture.pack_image_persist_fields,
        "shapeFile", shazbot_fixture.pack_shape_file_name,
        sizeof shazbot_fixture.pack_shape_file_name, "pack_upgrade_energy.dts", 8, 4);
    shazbot_test_put_pointer(shazbot_fixture.connection, SHAZBOT_LOCAL_CONTROL_OFFSET,
                             shazbot_fixture.player);
    shazbot_test_put_word(shazbot_fixture.player, 0x20, id);
    if (id) shazbot_fixture.dictionary[id & 0xfff] = (uint32_t)(uintptr_t)shazbot_fixture.player;
}

static void shazbot_test_prepare_terrain(uint32_t id, int square_size,
                                         float origin_x, float origin_y) {
    assert(id && id < 4096 && square_size > 0);
    snprintf(shazbot_fixture.terrain_name, sizeof shazbot_fixture.terrain_name,
             "TerrainBlock");
    snprintf(shazbot_fixture.terrain_file_field, sizeof shazbot_fixture.terrain_file_field,
             "terrainFile");
    snprintf(shazbot_fixture.terrain_square_field, sizeof shazbot_fixture.terrain_square_field,
             "squareSize");
    shazbot_test_set_class(shazbot_fixture.terrain_block, shazbot_fixture.terrain_class,
                           shazbot_fixture.terrain_name);
    shazbot_test_put_pointer(shazbot_fixture.terrain_class, 4,
                             shazbot_fixture.terrain_fields);
    shazbot_test_put_word(shazbot_fixture.terrain_class, 8, 2);
    shazbot_test_put_pointer(&shazbot_fixture.terrain_fields[0], 0,
                             shazbot_fixture.terrain_file_field);
    shazbot_test_put_word(&shazbot_fixture.terrain_fields[0], 8, 0x300);
    shazbot_test_put_pointer(&shazbot_fixture.terrain_fields[1], 0,
                             shazbot_fixture.terrain_square_field);
    shazbot_test_put_word(&shazbot_fixture.terrain_fields[1], 8, 0x400);
    shazbot_test_put_pointer(shazbot_fixture.terrain_block, 0x2f0,
                             shazbot_fixture.terrain_heightmap);
    shazbot_test_put_pointer(shazbot_fixture.terrain_block, 0x2ec,
                             shazbot_fixture.terrain_materialmap);
    shazbot_test_put_word(shazbot_fixture.terrain_block, 0x300, 0x0042a000);
    shazbot_test_put_word(shazbot_fixture.terrain_block, 0x400, (uint32_t)square_size);
    memcpy(shazbot_fixture.terrain_block + SHAZBOT_POSITION_X_OFFSET,
           &origin_x, sizeof origin_x);
    memcpy(shazbot_fixture.terrain_block + SHAZBOT_POSITION_Y_OFFSET,
           &origin_y, sizeof origin_y);
    float zero = 0.0f;
    memcpy(shazbot_fixture.terrain_block + SHAZBOT_POSITION_Z_OFFSET, &zero, sizeof zero);
    for (size_t y = 0; y < 256; y++) {
        for (size_t x = 0; x < 256; x++) {
            size_t index = y * 256 + x;
            shazbot_fixture.terrain_heightmap[index] = (uint16_t)(x + y);
            shazbot_fixture.terrain_materialmap[index * 2] = 0;
            shazbot_fixture.terrain_materialmap[index * 2 + 1] = (uint8_t)((x / 32 + y / 32) & 7);
        }
    }
    shazbot_fixture.dictionary[id & 0xfff] =
        (uint32_t)(uintptr_t)shazbot_fixture.terrain_block;
}

static void shazbot_test_prepare_view(uint32_t object_id, uint32_t target_id, float radius) {
    shazbot_test_prepare_player(object_id, "Player");
    unsigned char *target_info = shazbot_fixture.target_manager + SHAZBOT_TARGET_ARRAY_OFFSET +
                                 (size_t)target_id * SHAZBOT_TARGET_STRIDE;
    shazbot_test_put_word(shazbot_fixture.player, SHAZBOT_TARGET_ID_OFFSET, target_id);
    shazbot_test_put_pointer(shazbot_fixture.player, SHAZBOT_TARGET_INFO_OFFSET, target_info);
    shazbot_test_put_pointer(target_info, SHAZBOT_TARGET_SENSOR_DATA_OFFSET,
                             shazbot_fixture.sensor_data);
    memcpy(shazbot_fixture.sensor_data + SHAZBOT_SENSOR_DETECT_RADIUS_OFFSET,
           &radius, sizeof radius);
}

static void shazbot_test_prepare_observation(uint32_t player_id, int local_group,
                                             const float position[3],
                                             const float velocity[3]) {
    shazbot_test_prepare_view(player_id, 77, 250.0f);
    snprintf(shazbot_fixture.player_data_name, sizeof shazbot_fixture.player_data_name,
             "PlayerData");
    shazbot_test_set_class(shazbot_fixture.player_data, shazbot_fixture.player_data_class,
                           shazbot_fixture.player_data_name);
    shazbot_test_put_pointer(shazbot_fixture.player, SHAZBOT_PLAYER_DATABLOCK_OFFSET,
                             shazbot_fixture.player_data);
    shazbot_test_put_word(shazbot_fixture.player_data,
                          SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET, 0);
    const float max_energy = 100.0f;
    const float current_energy = 75.0f;
    const float max_damage = 100.0f;
    const float current_damage = 25.0f;
    const float max_forward_speed = 15.0f;
    memcpy(shazbot_fixture.player_data + SHAZBOT_DATABLOCK_MAX_ENERGY_OFFSET,
           &max_energy, sizeof max_energy);
    memcpy(shazbot_fixture.player_data + SHAZBOT_DATABLOCK_MAX_DAMAGE_OFFSET,
           &max_damage, sizeof max_damage);
    memcpy(shazbot_fixture.player_data + SHAZBOT_PLAYER_MAX_FORWARD_SPEED_OFFSET,
           &max_forward_speed, sizeof max_forward_speed);
    memcpy(shazbot_fixture.player + SHAZBOT_PLAYER_DAMAGE_OFFSET,
           &current_damage, sizeof current_damage);
    memcpy(shazbot_fixture.player + SHAZBOT_PLAYER_ENERGY_OFFSET,
           &current_energy, sizeof current_energy);
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "weaponSlotCount", "4");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "weaponSlot[0]", "Disc");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "weaponSlot[1]", "Plasma");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "weaponSlot[2]", "Blaster");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "weaponSlot[3]", "Mortar");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "inv[Disc]", "1");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "inv[Plasma]", "0");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "inv[Blaster]", "1");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "inv[Mortar]", "1");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "inv[EnergyPack]", "1");
    shazbot_test_add_dynamic_field(shazbot_fixture.player, "inv[RepairPack]", "0");
    shazbot_test_add_dynamic_field(shazbot_fixture.image_data, "item", "Blaster");
    shazbot_test_add_dynamic_field(shazbot_fixture.pack_image_data, "item", "EnergyPack");
    shazbot_test_put_word(shazbot_fixture.image_data, 0x20, 900);
    shazbot_fixture.dictionary[900 & 0xfff] = (uint32_t)(uintptr_t)shazbot_fixture.image_data;
    shazbot_test_put_word(shazbot_fixture.pack_image_data, 0x20, 901);
    shazbot_fixture.dictionary[901 & 0xfff] = (uint32_t)(uintptr_t)shazbot_fixture.pack_image_data;
    shazbot_test_put_pointer(shazbot_fixture.player, SHAZBOT_MOUNTED_IMAGES_OFFSET,
                             shazbot_fixture.image_data);
    shazbot_test_put_pointer(shazbot_fixture.player,
                             SHAZBOT_MOUNTED_IMAGES_OFFSET + 2 * SHAZBOT_MOUNTED_IMAGE_STRIDE,
                             shazbot_fixture.pack_image_data);
    shazbot_test_put_word(shazbot_fixture.connection, SHAZBOT_LOCAL_SENSOR_GROUP_OFFSET,
                          (uint32_t)local_group);
    for (int axis = 0; axis < 3; axis++) {
        memcpy(shazbot_fixture.player + SHAZBOT_POSITION_X_OFFSET + (size_t)axis * 16,
               &position[axis], sizeof(float));
        memcpy(shazbot_fixture.player + SHAZBOT_PLAYER_VELOCITY_X_OFFSET + (size_t)axis * 4,
               &velocity[axis], sizeof(float));
    }
}

static void shazbot_test_prepare_energy_mount(uint32_t id, float max_energy,
                                              float energy, int inherit_energy) {
    snprintf(shazbot_fixture.energy_mount_name, sizeof shazbot_fixture.energy_mount_name,
             "Vehicle");
    snprintf(shazbot_fixture.energy_mount_data_name,
             sizeof shazbot_fixture.energy_mount_data_name, "VehicleData");
    shazbot_test_set_class(shazbot_fixture.energy_mount, shazbot_fixture.energy_mount_class,
                           shazbot_fixture.energy_mount_name);
    shazbot_test_set_class(shazbot_fixture.energy_mount_data,
                           shazbot_fixture.energy_mount_data_class,
                           shazbot_fixture.energy_mount_data_name);
    shazbot_test_put_word(shazbot_fixture.energy_mount, 0x20, id);
    shazbot_test_put_pointer(shazbot_fixture.energy_mount, SHAZBOT_PLAYER_DATABLOCK_OFFSET,
                             shazbot_fixture.energy_mount_data);
    shazbot_test_put_word(shazbot_fixture.energy_mount_data,
                          SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET,
                          (uint32_t)inherit_energy);
    memcpy(shazbot_fixture.energy_mount_data + SHAZBOT_DATABLOCK_MAX_ENERGY_OFFSET,
           &max_energy, sizeof max_energy);
    memcpy(shazbot_fixture.energy_mount + SHAZBOT_PLAYER_ENERGY_OFFSET,
           &energy, sizeof energy);
    shazbot_fixture.dictionary[id & 0xfff] = (uint32_t)(uintptr_t)shazbot_fixture.energy_mount;
}

static void shazbot_test_prepare_energy_mount_two(uint32_t id, float max_energy,
                                                  float energy, int inherit_energy) {
    snprintf(shazbot_fixture.energy_mount_two_name,
             sizeof shazbot_fixture.energy_mount_two_name, "Vehicle");
    snprintf(shazbot_fixture.energy_mount_two_data_name,
             sizeof shazbot_fixture.energy_mount_two_data_name, "VehicleData");
    shazbot_test_set_class(shazbot_fixture.energy_mount_two,
                           shazbot_fixture.energy_mount_two_class,
                           shazbot_fixture.energy_mount_two_name);
    shazbot_test_set_class(shazbot_fixture.energy_mount_two_data,
                           shazbot_fixture.energy_mount_two_data_class,
                           shazbot_fixture.energy_mount_two_data_name);
    shazbot_test_put_word(shazbot_fixture.energy_mount_two, 0x20, id);
    shazbot_test_put_pointer(shazbot_fixture.energy_mount_two, SHAZBOT_PLAYER_DATABLOCK_OFFSET,
                             shazbot_fixture.energy_mount_two_data);
    shazbot_test_put_word(shazbot_fixture.energy_mount_two_data,
                          SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET,
                          (uint32_t)inherit_energy);
    memcpy(shazbot_fixture.energy_mount_two_data + SHAZBOT_DATABLOCK_MAX_ENERGY_OFFSET,
           &max_energy, sizeof max_energy);
    memcpy(shazbot_fixture.energy_mount_two + SHAZBOT_PLAYER_ENERGY_OFFSET,
           &energy, sizeof energy);
    shazbot_fixture.dictionary[id & 0xfff] =
        (uint32_t)(uintptr_t)shazbot_fixture.energy_mount_two;
}

static void shazbot_test_add_visible_enemy(size_t slot, uint32_t id, int sensor_group,
                                           const float position[3], const float velocity[3]) {
    assert(slot < 4 && id && id < 4096);
    unsigned char *enemy = shazbot_fixture.enemies[slot];
    snprintf(shazbot_fixture.enemy_names[slot], sizeof shazbot_fixture.enemy_names[slot], "Player");
    shazbot_test_set_class(enemy, shazbot_fixture.enemy_classes[slot],
                           shazbot_fixture.enemy_names[slot]);
    shazbot_test_put_word(enemy, 0x20, id);
    shazbot_test_put_word(enemy, 0x40, 2); /* current ghosted ShapeBase */
    uint32_t target_id = (uint32_t)(100 + slot);
    unsigned char *target_info = shazbot_fixture.target_manager + SHAZBOT_TARGET_ARRAY_OFFSET +
                                 (size_t)target_id * SHAZBOT_TARGET_STRIDE;
    shazbot_test_put_word(enemy, SHAZBOT_TARGET_ID_OFFSET, target_id);
    shazbot_test_put_pointer(enemy, SHAZBOT_TARGET_INFO_OFFSET, target_info);
    shazbot_test_put_word(target_info, 0x18, (uint32_t)sensor_group);
    for (int axis = 0; axis < 3; axis++) {
        memcpy(enemy + SHAZBOT_POSITION_X_OFFSET + (size_t)axis * 16,
               &position[axis], sizeof(float));
        memcpy(enemy + SHAZBOT_PLAYER_VELOCITY_X_OFFSET + (size_t)axis * 4,
               &velocity[axis], sizeof(float));
    }
    shazbot_fixture.dictionary[id & 0xfff] = (uint32_t)(uintptr_t)enemy;
    uint32_t visible_count = rv_word(shazbot_fixture.visible_set, 0x30);
    assert(visible_count < 64);
    shazbot_fixture.visible_entries[visible_count] = (uint32_t)(uintptr_t)enemy;
    shazbot_test_put_pointer(shazbot_fixture.visible_set, 0x38,
                             shazbot_fixture.visible_entries);
    shazbot_test_put_word(shazbot_fixture.visible_set, 0x30, visible_count + 1);
}

static void shazbot_test_set_current_target(uint32_t target_id, void *object) {
    unsigned char *target_info = shazbot_fixture.target_manager +
                                 SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET +
                                 (size_t)target_id * SHAZBOT_TARGET_INFO_STRIDE;
    shazbot_test_put_pointer(target_info, 0x24, object);
    shazbot_test_put_word(target_info, 0x3c, 1); /* live client target association */
}

static void shazbot_test_add_client_target(size_t slot, uint32_t type, uint32_t target_id,
                                           const float position[3]) {
    assert(slot < 4);
    unsigned char *client_target = shazbot_fixture.client_targets[slot];
    snprintf(shazbot_fixture.client_target_names[slot],
             sizeof shazbot_fixture.client_target_names[slot], "ClientTarget");
    shazbot_test_set_class(client_target, shazbot_fixture.client_target_classes[slot],
                           shazbot_fixture.client_target_names[slot]);
    shazbot_test_put_word(client_target, SHAZBOT_CLIENT_TARGET_TYPE_OFFSET, type);
    shazbot_test_put_word(client_target, 0x34, target_id);
    for (int axis = 0; axis < 3; axis++)
        memcpy(client_target + SHAZBOT_CLIENT_TARGET_POSITION_OFFSET + (size_t)axis * 4,
               &position[axis], sizeof(float));
    uint32_t count = rv_word(shazbot_fixture.client_target_list,
                             SHAZBOT_CLIENT_TARGET_LIST_COUNT_OFFSET);
    assert(count < SHAZBOT_CLIENT_TARGET_LIST_CAPACITY);
    shazbot_test_put_pointer(shazbot_fixture.client_target_list,
                             SHAZBOT_CLIENT_TARGET_LIST_ENTRIES_OFFSET +
                             (size_t)count * SHAZBOT_CLIENT_TARGET_ENTRY_STRIDE,
                             client_target);
    shazbot_test_put_word(shazbot_fixture.client_target_list,
                          SHAZBOT_CLIENT_TARGET_LIST_COUNT_OFFSET, count + 1);
}

static void shazbot_test_prepare_native_aim(uint32_t player_id, uint32_t beacon_id,
                                            uint32_t image_slot, float radius,
                                            unsigned char parent_frame[0x28]) {
    shazbot_test_prepare_view(player_id, 77, radius);
    unsigned char *image_state = shazbot_fixture.player + SHAZBOT_MOUNTED_IMAGES_OFFSET +
                                 (size_t)image_slot * SHAZBOT_MOUNTED_IMAGE_STRIDE;
    shazbot_test_put_pointer(image_state, 0, shazbot_fixture.image_data);
    shazbot_test_put_pointer(image_state, 0x2c, shazbot_fixture.image_shape);
    shazbot_test_put_pointer(shazbot_fixture.image_data, SHAZBOT_IMAGE_PROJECTILE_OFFSET,
                             shazbot_fixture.projectile_data);
    shazbot_test_put_word(shazbot_fixture.beacon, 0x20, beacon_id);
    shazbot_fixture.dictionary[beacon_id & 0xfff] = (uint32_t)(uintptr_t)shazbot_fixture.beacon;
    shazbot_test_put_pointer(parent_frame, 0x0c, shazbot_fixture.beacon);
    shazbot_test_put_pointer(parent_frame, 0x14, shazbot_fixture.player + SHAZBOT_POSITION_X_OFFSET);
    shazbot_test_put_pointer(parent_frame, 0x1c, shazbot_fixture.projectile_data);
    shazbot_spatial_context_valid = 1;
    shazbot_spatial_player = shazbot_fixture.player;
    shazbot_spatial_player_id = player_id;
    shazbot_spatial_detection_radius = radius;
}

static int shazbot_fake_send_calls;
static int shazbot_fake_send_flags;
static ssize_t shazbot_fake_send_result;
static int shazbot_fake_send_errno;
static ssize_t shazbot_fake_sendto(int fd, const void *packet, size_t length, int flags,
                                   const struct sockaddr *address, socklen_t address_length) {
    (void)fd; (void)packet; (void)length; (void)address; (void)address_length;
    shazbot_fake_send_calls++;
    shazbot_fake_send_flags = flags;
    errno = shazbot_fake_send_errno;
    return shazbot_fake_send_result;
}

#define TEST_RSA_MODULUS "8302c77f71653bd46d53472094c45171bbb1b2dc8408bab25ea4b016be4da0869737ea99ca887a4869a09176c43e73eab974ccdbf9a25ab8ccbb86fa343720f600c6cd6be3a4599a2796868790a2051e15607d06b470ad281b57e66fde832e576a760fb757c8ea3f6c82ed0381caeddfcbb6a0e467fc253b1e86d3f50341ea95"
#define TEST_RSA_PRIVATE "346782ffc6f54b21c55482d9d51b53c717e0adf1ce69e447590eacd5e5b8a69c3c7cc43d843697502a403a2f81b294c44a2eb857fd742449eb7e35fdae7c739506d703ccf3688366bd209480bc8e0a517d37ec89912132399cf3003298fef461bee2565b46084f4983a1f5accf6ead869d3d4c187b6ffa2f04c95250a26c130d"

int main(void) {
    /* AuthInfo comes from the emulator's Auth2 certificate; the game's own
       account check and warning must run unmodified. */
    const char *scripts[] = {core_runtime_script, tribesnext_runtime_script,
                             player_pane_button_click_override, depot_downloads_script,
                             discord_presence_script};
    for (size_t i = 0; i < sizeof(scripts) / sizeof(scripts[0]); ++i) {
        assert(strstr(scripts[i], "function checkNamesAndAliases") == NULL);
        assert(strstr(scripts[i], "function LaunchGui::onWake") == NULL);
        assert(strstr(scripts[i], "function MessageBoxOK") == NULL);
        assert(strstr(scripts[i], "CachedAuthInfo") == NULL);
    }

    /* Hook evaluations must be statements; a bare expression is a syntax error. */
    assert(strstr(core_runtime_script, "function t2launch_audioState()") != NULL);
    assert(strstr(core_runtime_script, "function t2launch_launchGuiReady()") != NULL);
    assert(strstr(core_runtime_script, "function t2launch_hudDemoRadarEnabled() {") != NULL);
    assert(strstr(core_runtime_script, "!isObject(PlayGui) || !PlayGui.isAwake()") != NULL);
    assert(strstr(core_runtime_script, "!isObject(navHud) || !navHud.isAwake() || !navHud.visible") != NULL);
    assert(strstr(core_runtime_script,
                  "function t2launch_skyRotationSpeed() { return $pref::skyRotationSpeed; }") != NULL);
    assert(strstr(shazbot_hud_telemetry_script, "clientCmdSetWeaponsHudItem") != NULL);
    assert(strstr(shazbot_hud_telemetry_script, "shazbot_hud_weapon_item") != NULL);
    assert(strstr(shazbot_hud_telemetry_script,
                  "shazbot_hud_weapon_item(%slot, %item, %addItem)") != NULL);
    assert(strstr(shazbot_hud_telemetry_script, "%item = $WeaponNames[%slot]") != NULL);
    assert(strstr(shazbot_hud_telemetry_script,
                  "$WeaponsHudData[%slot, itemDataName]") != NULL);
    assert(strstr(shazbot_hud_telemetry_script, "Parent::clientCmdSetWeaponsHudActive") != NULL);
    assert(strstr(shazbot_hud_telemetry_script,
                  "activatePackage(T2LaunchShazbotHudTelemetry)") != NULL &&
           strstr(shazbot_hud_telemetry_script, "isFunction(") == NULL);
    assert(strstr(autoplay_demo_exit_package,
                  "function demoPlaybackComplete()") != NULL &&
           strstr(autoplay_demo_exit_package, "Parent::demoPlaybackComplete()") != NULL &&
           strstr(autoplay_demo_exit_package, "schedule(1, 0, \"quit\")") != NULL);
    /* The hook evaluates these helpers even when the TribesNext script is off. */
    assert(strstr(tribesnext_runtime_script, "function t2launch_audioState()") == NULL);
    assert(strstr(tribesnext_runtime_script, "function t2launch_launchGuiReady()") == NULL);

    /* Options: the config file wins, then a legacy environment variable, then
       the default; unknown keys are kept for hook_option(). */
    char config_path[] = "/tmp/t2_hook_options_XXXXXX";
    int config_fd = mkstemp(config_path);
    assert(config_fd >= 0);
    const char config[] = "# comment\n video.full_redraw = 0 \n"
                          "telemetry.shazbot_command_labels=1\n"
                          "future.datablock=1.5\nnot a pair\n";
    assert(write(config_fd, config, sizeof(config) - 1) == (ssize_t)(sizeof(config) - 1));
    close(config_fd);
    setenv("T2LAUNCH_HOOK_CONFIG", config_path, 1);
    setenv("T2LAUNCH_FULL_REDRAW", "1", 1);
    setenv("T2LAUNCH_DISABLE_HOOK_COMMANDS", "1", 1);
    assert(!hook_option_bool("video.full_redraw"));
    assert(!hook_option_bool("hook.console_commands"));
    assert(hook_option_bool("telemetry.shazbot_command_labels"));
    assert(hook_option_bool("hook.tribesnext"));
    assert(strcmp(hook_option_str("net.build_identifier"), "25034") == 0);
    store_hook_option("security.safe_soundpacks", "");
    assert(hook_option_bool("security.safe_soundpacks"));  /* blank restores the safe default */
    store_hook_option("security.safe_soundpacks", "0");
    assert(!hook_option_bool("security.safe_soundpacks")); /* explicit off stays off */
    store_hook_option("security.safe_soundpacks", "1");
    store_hook_option("telemetry.shazbot_command_labels", "0");
    assert(strcmp(hook_option_str("future.datablock"), "1.5") == 0);
    assert(hook_option_str("missing.key") == NULL);
    assert(hook_long_or_default("security.max_soundpack_kbytes", 8192, 0,
                                LONG_MAX / 1024L) == 8192);
    store_hook_option("security.max_soundpack_kbytes", "");
    assert(hook_long_or_default("security.max_soundpack_kbytes", 8192, 0,
                                LONG_MAX / 1024L) == 8192);
    store_hook_option("security.max_soundpack_kbytes", "0");
    assert(hook_long_or_default("security.max_soundpack_kbytes", 8192, 0,
                                LONG_MAX / 1024L) == 0);  /* explicit zero means no cap */
    store_hook_option("security.max_soundpack_kbytes", "not-a-number");
    assert(hook_long_or_default("security.max_soundpack_kbytes", 8192, 0,
                                LONG_MAX / 1024L) == 8192);
    store_hook_option("security.max_soundpack_kbytes", "8192");
    const char *option_args[] = {"hook_option", "future.datablock"};
    assert(strcmp(hook_option_cb(NULL, 2, option_args), "1.5") == 0);
    unlink(config_path);

    sdl_rect_t mode_16_9 = {0, 0, 1920, 1080};
    sdl_rect_t mode_16_10 = {0, 0, 1920, 1200};
    sdl_rect_t *host_modes[] = {&mode_16_9, &mode_16_10, NULL};
    sdl_rect_t **windowed_any = (sdl_rect_t **)(intptr_t)-1;
    sdl_rect_t *windowed_modes[] = {&mode_16_9, NULL};
    assert(prefer_concrete_host_modes(windowed_any, host_modes) == host_modes);
    assert(prefer_concrete_host_modes(windowed_modes, host_modes) == windowed_modes);
    assert(prefer_concrete_host_modes(windowed_any, NULL) == windowed_any);
    assert(sdl_mode_count(host_modes) == 2);

    char browser_ip[64] = "IP:185.66.108.39:28000";
    strcpy(browser_ip, browser_ip + 3);
    assert(strcmp(browser_ip, "185.66.108.39:28000") == 0);

    long size = sysconf(_SC_PAGESIZE);
    uintptr_t page = 0x084846B1 & ~((uintptr_t)size - 1);
    char *mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    char *a = (char *)0x084846B1;
    char *b = (char *)0x084846CD;
    char *c = (char *)0x084846E9;
    strcpy(a, "tribes2.m1.sierra.com:15104");
    strcpy(b, "unsupported");
    strcpy(c, "tribes2.m3.sierra.com:15104");
    assert(mprotect(mapping, size, PROT_READ) == 0);
    assert(!redirect_won_bootstrap());
    assert(strcmp(a, "tribes2.m1.sierra.com:15104") == 0);
    assert(mprotect(mapping, size, PROT_READ | PROT_WRITE) == 0);
    strcpy(b, "tribes2.m2.sierra.com:15104");
    assert(mprotect(mapping, size, PROT_READ) == 0);
    assert(redirect_won_bootstrap());
    assert(strcmp(a, "127.0.0.1:15104") == 0);
    assert(strcmp(b, "127.0.0.1:15104") == 0);
    assert(strcmp(c, "127.0.0.1:15104") == 0);
    int protection;
    assert(readable_range((uintptr_t)a, 15, &protection));
    assert(protection == PROT_READ);
    munmap(mapping, size);
    /* The game clock jumps to the CLOCK_MONOTONIC replacement only over the
       expected getRealMilliseconds prologue, and restores the protection. */
    page = REAL_MILLISECONDS_ADDRESS & ~((uintptr_t)size - 1);
    mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    unsigned char *timer = (unsigned char *)REAL_MILLISECONDS_ADDRESS;
    memcpy(timer, real_milliseconds_original, sizeof(real_milliseconds_original));
    timer[7] = 0x10;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(!install_monotonic_clock());
    assert(timer[0] == 0x0f);
    assert(mprotect(mapping, size, PROT_READ | PROT_WRITE) == 0);
    timer[7] = 0x14;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(install_monotonic_clock());
    int32_t offset;
    memcpy(&offset, timer + 1, sizeof(offset));
    assert(timer[0] == 0xe9);
    assert(REAL_MILLISECONDS_ADDRESS + 5 + (uintptr_t)offset == (uintptr_t)monotonic_real_milliseconds);
    assert(memcmp(timer + 5, real_milliseconds_original + 5, 3) == 0);
    assert(readable_range(REAL_MILLISECONDS_ADDRESS, 5, &protection));
    assert(protection == (PROT_READ | PROT_EXEC));
    uint32_t (*patched_clock)(void) = (uint32_t (*)(void))REAL_MILLISECONDS_ADDRESS;
    uint32_t before = patched_clock();
    struct timespec pause = {0, 20 * 1000000};
    nanosleep(&pause, NULL);
    uint32_t after = patched_clock();
    assert(after >= before + 19 && after < before + 1000);
    munmap(mapping, size);

    /* The browser's default bot limit changes only over the expected code. */
    page = BROWSER_BOT_LIMIT_ADDRESS & ~((uintptr_t)size - 1);
    mapping = mmap((void *)page, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(mapping == (void *)page);
    unsigned char *bot_limit = (unsigned char *)BROWSER_BOT_LIMIT_ADDRESS;
    memcpy(bot_limit, browser_bot_limit_original, sizeof(browser_bot_limit_original));
    bot_limit[9] = 0x07;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(!install_browser_bot_limit());
    assert(bot_limit[1] == 0x10);
    assert(mprotect(mapping, size, PROT_READ | PROT_WRITE) == 0);
    bot_limit[9] = 0x06;
    assert(mprotect(mapping, size, PROT_READ | PROT_EXEC) == 0);
    assert(install_browser_bot_limit());
    assert(bot_limit[0] == 0xb8 && bot_limit[1] == 0xff);
    assert(memcmp(bot_limit + 2, browser_bot_limit_original + 2, sizeof(browser_bot_limit_original) - 2) == 0);
    assert(readable_range(BROWSER_BOT_LIMIT_ADDRESS, 2, &protection));
    assert(protection == (PROT_READ | PROT_EXEC));
    munmap(mapping, size);
    assert(strstr(filter_max_bots_override, "function FilterEditDlg::setMaxBots(%this)") != NULL);
    assert(strstr(filter_max_bots_override, "> 255") != NULL);

    /* The client and dedicated helpers must use a profile's build ID; the
       client master-list request clears only the version-match flag so local
       filtering can select the configured ID from the complete result set. */
    {
        target = &game_targets[0];
        uintptr_t helper_page = target->build_version_helper & ~((uintptr_t)size - 1);
        uintptr_t flag_page = target->master_list_version_flag & ~((uintptr_t)size - 1);
        assert(helper_page != flag_page);
        unsigned char *helper = mmap((void *)helper_page, size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(helper == (void *)helper_page);
        unsigned char *flags = mmap((void *)flag_page, size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(flags == (void *)flag_page);
        memcpy((void *)target->build_version_helper, build_version_helper_original,
               sizeof(build_version_helper_original));
        memcpy((void *)target->master_list_version_flag, master_list_version_flag_original,
               sizeof(master_list_version_flag_original));
        assert(mprotect(helper, size, PROT_READ | PROT_EXEC) == 0);
        assert(mprotect(flags, size, PROT_READ | PROT_EXEC) == 0);
        store_hook_option("net.build_identifier", "0");
        assert(!install_build_identifier());
        uint32_t version;
        memcpy(&version, (void *)(target->build_version_helper + 4), sizeof(version));
        assert(version == 25034);
        store_hook_option("net.build_identifier", "987654");
        assert(install_build_identifier());
        memcpy(&version, (void *)(target->build_version_helper + 4), sizeof(version));
        assert(version == 987654);
        assert(memcmp((void *)target->master_list_version_flag, master_list_version_flag_any, 3) == 0);
        assert(mprotect(helper, size, PROT_READ | PROT_WRITE) == 0);
        assert(mprotect(flags, size, PROT_READ | PROT_WRITE) == 0);
        memcpy((void *)target->build_version_helper, build_version_helper_original,
               sizeof(build_version_helper_original));
        memcpy((void *)target->master_list_version_flag, master_list_version_flag_original,
               sizeof(master_list_version_flag_original));
        assert(mprotect(helper, size, PROT_READ | PROT_EXEC) == 0);
        assert(mprotect(flags, size, PROT_READ | PROT_EXEC) == 0);
        store_hook_option("net.build_identifier", "");
        assert(install_build_identifier());
        memcpy(&version, (void *)(target->build_version_helper + 4), sizeof(version));
        assert(version == 25034);
        assert(memcmp((void *)target->master_list_version_flag, master_list_version_flag_any, 3) == 0);
        assert(munmap(helper, size) == 0);
        assert(munmap(flags, size) == 0);

        target = &game_targets[1];
        helper_page = target->build_version_helper & ~((uintptr_t)size - 1);
        helper = mmap((void *)helper_page, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(helper == (void *)helper_page);
        memcpy((void *)target->build_version_helper, build_version_helper_original,
               sizeof(build_version_helper_original));
        assert(mprotect(helper, size, PROT_READ | PROT_EXEC) == 0);
        store_hook_option("net.build_identifier", "24834");
        assert(install_build_identifier());
        memcpy(&version, (void *)(target->build_version_helper + 4), sizeof(version));
        assert(version == 24834);
        assert(munmap(helper, size) == 0);
        target = &game_targets[0];
    }

    /* A profile's demo header tag patches the recorder, strict playback gate,
       and getDemoVersion() report atomically, without touching packet parsing. */
    {
        target = &game_targets[0];
        uintptr_t writer_page = target->demo_tag_writer & ~((uintptr_t)size - 1);
        uintptr_t helper_page = target->demo_tag_helper & ~((uintptr_t)size - 1);
        assert(writer_page != helper_page);
        unsigned char *writer_map = mmap((void *)writer_page, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(writer_map == (void *)writer_page);
        unsigned char *helper_map = mmap((void *)helper_page, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(helper_map == (void *)helper_page);
        memcpy((void *)target->demo_tag_writer, demo_tag_writer_original,
               sizeof(demo_tag_writer_original));
        memcpy((void *)target->demo_tag_compare, demo_tag_compare_original,
               sizeof(demo_tag_compare_original));
        memcpy((void *)target->demo_tag_helper, demo_tag_helper_original,
               sizeof(demo_tag_helper_original));
        ((unsigned char *)target->demo_tag_compare)[2] ^= 1;
        assert(mprotect(writer_map, size, PROT_READ | PROT_EXEC) == 0);
        assert(mprotect(helper_map, size, PROT_READ | PROT_EXEC) == 0);
        store_hook_option("demo.protocol_tag", "3145732");
        assert(!install_demo_protocol_tag());  /* One bad site must refuse all writes. */
        assert(memcmp((void *)target->demo_tag_writer, demo_tag_writer_original,
                      sizeof(demo_tag_writer_original)) == 0);
        assert(memcmp((void *)target->demo_tag_helper, demo_tag_helper_original,
                      sizeof(demo_tag_helper_original)) == 0);
        assert(mprotect(writer_map, size, PROT_READ | PROT_WRITE) == 0);
        memcpy((void *)target->demo_tag_compare, demo_tag_compare_original,
               sizeof(demo_tag_compare_original));
        assert(mprotect(writer_map, size, PROT_READ | PROT_EXEC) == 0);
        store_hook_option("demo.protocol_tag", "0");
        assert(!install_demo_protocol_tag());
        uint32_t tag;
        memcpy(&tag, (void *)(target->demo_tag_writer + 3), sizeof(tag));
        assert(tag == 0x00330004);
        store_hook_option("demo.protocol_tag", "");  /* blank means retail default, not zero */
        assert(install_demo_protocol_tag());
        memcpy(&tag, (void *)(target->demo_tag_writer + 3), sizeof(tag));
        assert(tag == 0x00330004);
        store_hook_option("demo.protocol_tag", "3145732");
        assert(install_demo_protocol_tag());
        memcpy(&tag, (void *)(target->demo_tag_writer + 3), sizeof(tag));
        assert(tag == 0x00300004);
        memcpy(&tag, (void *)(target->demo_tag_compare + 1), sizeof(tag));
        assert(tag == 0x00300004);
        memcpy(&tag, (void *)target->demo_tag_helper, sizeof(tag));
        assert(tag == 0x00300004);
        assert(readable_range(target->demo_tag_writer, sizeof(demo_tag_writer_original),
                              &protection));
        assert(protection == (PROT_READ | PROT_EXEC));
        assert(readable_range(target->demo_tag_helper, sizeof(demo_tag_helper_original),
                              &protection));
        assert(protection == (PROT_READ | PROT_EXEC));
        assert(munmap(writer_map, size) == 0);
        assert(munmap(helper_map, size) == 0);
    }

    /* Shazbot telemetry is opt-in, validates the local Player identity, uses
       the IPC v1 byte layout, and drops unavailable/full nonblocking sends. */
    {
        target = &game_targets[0];
        char action_socket_directory[] = "/tmp/t2_hook_shazbot_action_XXXXXX";
        assert(mkdtemp(action_socket_directory));
        char action_socket_path[128];
        snprintf(action_socket_path, sizeof action_socket_path, "%s/action.sock",
                 action_socket_directory);
        store_hook_option("telemetry.shazbot", "1");
        store_hook_option("telemetry.shazbot_actions", "1");
        store_hook_option("telemetry.shazbot_action_socket", action_socket_path);
        shazbot_action_socket_fd = -1;
        shazbot_action_socket_path[0] = '\0';
        shazbot_action_trigger_known = 0;
        shazbot_action_command_known = 0;
        shazbot_local_player_valid = 0;
        shazbot_demo_state = 0;
        shazbot_poll_actions();
        assert(shazbot_action_socket_fd == -1 && access(action_socket_path, F_OK) != 0);
        shazbot_local_player_valid = 1;
        shazbot_poll_actions();
        assert(shazbot_action_socket_fd >= 0 && access(action_socket_path, F_OK) == 0);
        close(shazbot_action_socket_fd);
        shazbot_action_socket_fd = -1;
        unlink(action_socket_path);
        rmdir(action_socket_directory);
        shazbot_action_socket_path[0] = '\0';
        shazbot_local_player_valid = 0;
        store_hook_option("telemetry.shazbot_actions", "0");

        shazbot_socket_fd = -1;
        shazbot_sequence = 0;
        shazbot_last_attempt_ns = 0;
        shazbot_socket_error_reported = 0;
        store_hook_option("telemetry.shazbot", "0");
        poll_shazbot_telemetry();
        assert(shazbot_socket_fd == -1 && shazbot_sequence == 0 && shazbot_last_attempt_ns == 0);

        const float position[3] = {1.0f, -2.5f, 3.25f};
        unsigned char packet[SHAZBOT_PACKET_SIZE];
        static const unsigned char expected_packet[SHAZBOT_PACKET_SIZE] = {
            0x53,0x48,0x5a,0x42, 0x01,0x00, 0x03,0x00, 0x10,0x00,0x00,0x00,
            0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01,
            0x18,0x17,0x16,0x15,0x14,0x13,0x12,0x11,
            0x00,0x00,0x80,0x3f, 0x00,0x00,0x20,0xc0,
            0x00,0x00,0x50,0x40, 0x00,0x00,0x80,0x3f
        };
        assert(shazbot_encode_telemetry(packet, UINT64_C(0x0102030405060708),
                                        UINT64_C(0x1112131415161718), position, 1.0f) ==
               sizeof expected_packet);
        assert(memcmp(packet, expected_packet, sizeof expected_packet) == 0);
        unsigned char view_packet[SHAZBOT_VIEW_PACKET_SIZE];
        const float origin[3] = {1.0f, -2.5f, 3.25f};
        const float forward[3] = {0.0f, 1.0f, 0.0f};
        static const unsigned char expected_view_packet[SHAZBOT_VIEW_PACKET_SIZE] = {
            0x53,0x48,0x5a,0x42, 0x01,0x00, 0x04,0x00, 0x24,0x00,0x00,0x00,
            0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x34,0x12,0x00,0x00,
            0x00,0x00,0x80,0x3f, 0x00,0x00,0x20,0xc0, 0x00,0x00,0x50,0x40,
            0x00,0x00,0x00,0x00, 0x00,0x00,0x80,0x3f, 0x00,0x00,0x00,0x00,
            0x00,0x00,0xb4,0x42, 0x00,0x00,0x7a,0x43
        };
        assert(shazbot_encode_view_cone(view_packet, 1, 2, 0x1234, origin, forward,
                                        90.0f, 250.0f) == sizeof expected_view_packet);
        assert(memcmp(view_packet, expected_view_packet, sizeof expected_view_packet) == 0);
        assert(shazbot_encode_view_cone(view_packet, 1, 2, 0x1234, origin, forward,
                                        0.0f, 250.0f) == 0);
        assert(shazbot_encode_view_cone(view_packet, 1, 2, 0x1234, origin, forward,
                                        90.0f, 0.0f) == 0);
        float muzzle[3] = {1.0f, -2.5f, 3.25f};
        float aim[3] = {4.0f, 5.0f, 6.0f};
        unsigned char aim_packet[SHAZBOT_MUZZLE_AIM_PACKET_SIZE];
        static const unsigned char expected_aim_packet[SHAZBOT_MUZZLE_AIM_PACKET_SIZE] = {
            0x53,0x48,0x5a,0x42, 0x01,0x00, 0x05,0x00, 0x28,0x00,0x00,0x00,
            0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
            0x34,0x12,0x00,0x00, 0x02,0x00,0x00,0x00, 0x78,0x56,0x00,0x00,
            0x00,0x00,0x80,0x3f, 0x00,0x00,0x20,0xc0, 0x00,0x00,0x50,0x40,
            0x00,0x00,0x80,0x40, 0x00,0x00,0xa0,0x40, 0x00,0x00,0xc0,0x40,
            0x00,0x00,0x7a,0x43
        };
        assert(shazbot_encode_muzzle_aim(aim_packet, 1, 2, 0x1234, 2, 0x5678,
                                         muzzle, aim, 250.0f) == sizeof expected_aim_packet);
        assert(memcmp(aim_packet, expected_aim_packet, sizeof expected_aim_packet) == 0);
        assert(shazbot_encode_muzzle_aim(aim_packet, 1, 2, 0x1234,
                                         SHAZBOT_MOUNTED_IMAGE_COUNT, 0x5678,
                                         muzzle, aim, 250.0f) == 0);

        /* The native aim-marker draw hook is optional and atomically relocates
           only the researched #25034 CALL in a synthetic executable mapping. */
        long aim_page_size = sysconf(_SC_PAGESIZE);
        assert(aim_page_size > 0);
        uintptr_t aim_call_page = SHAZBOT_AIM_BILLBOARD_CALL & ~((uintptr_t)aim_page_size - 1);
        uintptr_t aim_native_page = SHAZBOT_NATIVE_BILLBOARD & ~((uintptr_t)aim_page_size - 1);
        void *aim_call_map = mmap((void *)aim_call_page, (size_t)aim_page_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        void *aim_native_map = mmap((void *)aim_native_page, (size_t)aim_page_size,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(aim_call_map == (void *)aim_call_page && aim_native_map == (void *)aim_native_page);
        memcpy((void *)SHAZBOT_AIM_BILLBOARD_CALL, shazbot_aim_billboard_call_original,
               sizeof shazbot_aim_billboard_call_original);
        memcpy((void *)SHAZBOT_NATIVE_BILLBOARD, shazbot_native_billboard_prologue,
               sizeof shazbot_native_billboard_prologue);
        assert(mprotect(aim_call_map, (size_t)aim_page_size, PROT_READ | PROT_EXEC) == 0);
        assert(mprotect(aim_native_map, (size_t)aim_page_size, PROT_READ | PROT_EXEC) == 0);
        target = &game_targets[0];
        shazbot_aim_hook_attempted = 0;
        store_hook_option("telemetry.shazbot", "0");
        assert(!shazbot_install_aim_billboard_hook());
        assert(memcmp((void *)SHAZBOT_AIM_BILLBOARD_CALL, shazbot_aim_billboard_call_original,
                      sizeof shazbot_aim_billboard_call_original) == 0);
        shazbot_aim_hook_attempted = 0;
        store_hook_option("telemetry.shazbot", "1");
        assert(shazbot_install_aim_billboard_hook());
        int32_t aim_relative;
        memcpy(&aim_relative, (void *)(SHAZBOT_AIM_BILLBOARD_CALL + 1), sizeof aim_relative);
        assert((uintptr_t)(SHAZBOT_AIM_BILLBOARD_CALL + 5 + aim_relative) ==
               (uintptr_t)shazbot_aim_billboard_wrapper);
        assert(mprotect(aim_call_map, (size_t)aim_page_size, PROT_READ | PROT_WRITE) == 0);
        memcpy((void *)SHAZBOT_AIM_BILLBOARD_CALL, shazbot_aim_billboard_call_original,
               sizeof shazbot_aim_billboard_call_original);
        assert(mprotect(aim_call_map, (size_t)aim_page_size, PROT_READ | PROT_EXEC) == 0);
        assert(munmap(aim_call_map, (size_t)aim_page_size) == 0);
        assert(munmap(aim_native_map, (size_t)aim_page_size) == 0);
        shazbot_aim_hook_installed = 0;
        shazbot_aim_hook_attempted = 0;

        float invalid_position[3] = {0.0f, NAN, 1.0f};
        assert(shazbot_encode_telemetry(packet, 1, 2, invalid_position, 1.0f) == 0);

        shazbot_test_prepare_player(1234, "Player");
        float x = 123.25f, y = -456.5f, z = 7.75f, read_position[3];
        memcpy(shazbot_fixture.player + SHAZBOT_POSITION_X_OFFSET, &x, sizeof x);
        memcpy(shazbot_fixture.player + SHAZBOT_POSITION_Y_OFFSET, &y, sizeof y);
        memcpy(shazbot_fixture.player + SHAZBOT_POSITION_Z_OFFSET, &z, sizeof z);
        struct rv_memory memory;
        shazbot_test_memory(&memory);
        uint32_t player_id = 0;
        assert(shazbot_local_player_position(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, read_position, &player_id, NULL));
        assert(player_id == 1234 && read_position[0] == x && read_position[1] == y &&
               read_position[2] == z);

        /* Radar enemies must come exclusively from the validated current-
           visibility set; a hidden dictionary player is not a radar contact. */
        const float radar_self[3] = {100.0f, 200.0f, 0.0f};
        const float radar_visible[3] = {110.0f, 220.0f, 0.0f};
        const float radar_hidden[3] = {130.0f, 240.0f, 0.0f};
        const float radar_velocity[3] = {0.0f, 0.0f, 0.0f};
        shazbot_test_prepare_observation(123, 1, radar_self, radar_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, radar_visible, radar_velocity);
        shazbot_test_add_visible_enemy(1, 202, 2, radar_hidden, radar_velocity);
        shazbot_test_put_pointer(shazbot_fixture.enemies[0], 0x26c,
                                 shazbot_fixture.client_targets[0]);
        shazbot_test_put_word(shazbot_fixture.client_targets[0], 0x18, 2);
        shazbot_test_put_pointer(shazbot_fixture.enemies[1], 0x26c,
                                 shazbot_fixture.client_targets[1]);
        shazbot_test_put_word(shazbot_fixture.client_targets[1], 0x18, 2);
        shazbot_test_put_word(shazbot_fixture.connection, 0x8434, 1);
        shazbot_test_put_word(shazbot_fixture.visible_set, 0x30, 1);
        shazbot_test_memory(&memory);
        struct radar_marker radar_test_markers[4];
        size_t radar_test_count = radar_collect_visible_enemies(&memory,
            shazbot_fixture.visible_set, shazbot_fixture.dictionary, 123, 1,
            radar_self, radar_test_markers, 4);
        assert(radar_test_count == 1 && radar_test_markers[0].kind == 1);
        assert(radar_test_markers[0].dx == 10.0f && radar_test_markers[0].dy == 20.0f);
        struct radar_point radar_test_point;
        assert(radar_project(600.0f, 0.0f, 0.0f, 1.0f, 600.0f, 50.0f, 60.0f, 20.0f,
                             &radar_test_point));
        assert(radar_test_point.x == 70.0f && radar_test_point.y == 60.0f);
        assert(radar_project(600.0f, 0.0f, 1.0f, 0.0f, 600.0f, 50.0f, 60.0f, 20.0f,
                             &radar_test_point));
        assert(radar_test_point.x == 50.0f && radar_test_point.y == 40.0f);
        assert(!radar_project(600.1f, 0.0f, 0.0f, 1.0f, 600.0f, 50.0f, 60.0f, 20.0f,
                              &radar_test_point));
        assert(!radar_project(NAN, 0.0f, 0.0f, 1.0f, 600.0f, 50.0f, 60.0f, 20.0f,
                              &radar_test_point));

        /* The observer Camera control object uses the same camera-eye virtual
           as a Player; team may be unknown without disabling the view itself. */
        shazbot_test_prepare_player(123, "Camera");
        const float camera_position[3] = {-80.0f, 240.0f, 100.0f};
        memcpy(shazbot_fixture.player + SHAZBOT_POSITION_X_OFFSET,
               &camera_position[0], sizeof(float));
        memcpy(shazbot_fixture.player + SHAZBOT_POSITION_Y_OFFSET,
               &camera_position[1], sizeof(float));
        memcpy(shazbot_fixture.player + SHAZBOT_POSITION_Z_OFFSET,
               &camera_position[2], sizeof(float));
        shazbot_fixture.eye_matrix[1] = 1.0f;
        shazbot_fixture.eye_matrix[5] = 0.0f;
        shazbot_fixture.eye_matrix[9] = 0.0f;
        shazbot_test_memory(&memory);
        float camera_origin[3], camera_forward[2];
        uint32_t camera_id = 0;
        int camera_team = 0;
        assert(radar_control_view(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, camera_origin, camera_forward,
               &camera_id, &camera_team));
        assert(camera_id == 123 && camera_team == RV_OTHER_TEAM);
        assert(camera_origin[0] == 11.0f && camera_origin[1] == 22.0f &&
               camera_origin[2] == 33.0f);
        assert(camera_forward[0] > 0.995f && camera_forward[1] < 0.001f &&
               camera_forward[1] > -0.001f);

        /* TerrainBlock's registered offsets and source layout resolve its
           height/material maps; cached sample cells cover the real map bounds. */
        shazbot_test_prepare_player(123, "Camera");
        shazbot_test_prepare_terrain(600, 8, -1024.0f, -1024.0f);
        shazbot_test_memory(&memory);
        assert(radar_capture_terrain(&memory, shazbot_fixture.dictionary));
        assert(radar_terrain_map.valid && radar_terrain_map.square_size == 32.0f);
        assert(radar_terrain_map.origin_x == -1008.0f &&
               radar_terrain_map.origin_y == -1008.0f);
        assert(radar_terrain_map.minimum == 0 && radar_terrain_map.maximum == 510);
        assert(radar_terrain_map.height[0] == 4 && radar_terrain_map.material[0] == 0);
        assert(radar_terrain_map.height[RADAR_TERRAIN_SIDE * RADAR_TERRAIN_SIDE - 1] == 508 &&
               radar_terrain_map.material[RADAR_TERRAIN_SIDE * RADAR_TERRAIN_SIDE - 1] == 6);

        shazbot_test_prepare_view(123, 77, 250.0f);
        shazbot_test_memory(&memory);
        float eye_origin[3], eye_forward[3], object_fov = 0.0f, detect_radius = 0.0f;
        void *local_player = shazbot_fixture.player;
        assert(shazbot_local_view_cone(&memory, local_player, 123,
               shazbot_fixture.target_manager, eye_origin, eye_forward,
               &object_fov, &detect_radius));
        assert(eye_origin[0] == 11.0f && eye_origin[1] == 22.0f && eye_origin[2] == 33.0f);
        assert(eye_forward[0] == 0.0f && eye_forward[1] == 1.0f && eye_forward[2] == 0.0f);
        assert(object_fov == 90.0f && detect_radius == 250.0f);

        /* Build the 32-float observation from the #25034 visible ShapeBase set,
           current object dictionary, TargetInfo team group and ClientTargetList. */
        const float self_position[3] = {100.0f, 200.0f, 300.0f};
        const float self_velocity[3] = {1.0f, 2.0f, 3.0f};
        const float enemy_a_position[3] = {110.0f, 200.0f, 300.0f};
        const float enemy_a_velocity[3] = {4.0f, 4.0f, 4.0f};
        const float enemy_b_position[3] = {90.0f, 200.0f, 300.0f};
        const float enemy_b_velocity[3] = {-1.0f, 3.0f, 2.0f};
        const float friendly_position[3] = {101.0f, 200.0f, 300.0f};
        const float objective_position[3] = {110.0f, 200.0f, 300.0f};
        const float ignored_waypoint[3] = {100.0f, 201.0f, 300.0f};
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        shazbot_test_add_visible_enemy(1, 199, 2, enemy_b_position, enemy_b_velocity);
        shazbot_test_add_visible_enemy(2, 101, 1, friendly_position, enemy_a_velocity);
        shazbot_test_set_current_target(100, shazbot_fixture.enemies[0]);
        shazbot_test_add_client_target(0, 0, 100, objective_position);
        shazbot_test_add_client_target(1, 2, 11, ignored_waypoint);
        shazbot_test_memory(&memory);
        float observation[SHAZBOT_OBSERVATION_DIMENSIONS];
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               1, forward, 90.0f, 250.0f, observation));
        static const float expected_observation[SHAZBOT_OBSERVATION_DIMENSIONS] = {
            100.0f,200.0f,300.0f, 1.0f,2.0f,3.0f,
            0.0f,0.0f,0.0f,0.0f,
            -10.0f,0.0f,0.0f, -2.0f,1.0f,-1.0f, 1.0f,1.0f,
            10.0f,0.0f,0.0f, 3.0f,2.0f,1.0f, 1.0f,1.0f,
             0.0f,1.0f,0.0f, 15.0f,0.75f,1.0f
        };
        assert(memcmp(observation, expected_observation, sizeof observation) == 0);

        shazbot_test_memory(&memory);
        float command_observation[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS] = {0};
        memcpy(command_observation, observation, sizeof observation);
        assert(shazbot_player_command_context(&memory, shazbot_fixture.dictionary,
                                              shazbot_fixture.player,
                                              command_observation));
        assert(command_observation[32] == 0.75f && command_observation[33] == 1.0f);
        assert(command_observation[34 + 2] == 1.0f && command_observation[40] == 1.0f);
        assert(command_observation[41] == 1.0f && command_observation[43] == 1.0f &&
               command_observation[47] == 1.0f);
        assert(command_observation[48] == 1.0f && command_observation[49] == 0.0f &&
               command_observation[50] == 0.0f && command_observation[60] == 1.0f);
        assert(command_observation[SHAZBOT_V5_MOUNTED_WEAPON_ID_BITS_BASE + 2] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_WEAPON_ID_BITS_BASE + 7] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_WEAPON_ID_BITS_BASE + 8] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_WEAPON_ID_BITS_BASE + 9] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_WEAPON_ID_VALID] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_PACK_ID_BITS_BASE] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_PACK_ID_BITS_BASE + 2] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_PACK_ID_BITS_BASE + 7] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_PACK_ID_BITS_BASE + 8] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_PACK_ID_BITS_BASE + 9] == 1.0f &&
               command_observation[SHAZBOT_V5_MOUNTED_PACK_ID_VALID] == 1.0f);
        unsigned char command_packet[SHAZBOT_COMMAND_OBSERVATION_V5_PACKET_SIZE];
        assert(shazbot_encode_command_observation_v5(command_packet, 9, 10,
                                                     command_observation) ==
               SHAZBOT_COMMAND_OBSERVATION_V5_PACKET_SIZE);
        assert(shazbot_get_u16le(command_packet + 6) == SHAZBOT_COMMAND_OBSERVATION_V5_KIND &&
               shazbot_get_u32le(command_packet + 8) == SHAZBOT_COMMAND_OBSERVATION_V5_PAYLOAD_SIZE);

        float semantic_observation[SHAZBOT_COMMAND_OBSERVATION_V6_DIMENSIONS] = {0};
        memcpy(semantic_observation, command_observation,
               SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_BITS_BASE * sizeof(float));
        shazbot_player_mounted_name_hashes(&memory, shazbot_fixture.player,
                                           semantic_observation);
        const uint16_t weapon_name_hash = shazbot_stable_name_hash("weapon_energy.dts");
        const uint16_t pack_name_hash = shazbot_stable_name_hash("pack_upgrade_energy.dts");
        assert(weapon_name_hash == 0x75c7 && pack_name_hash == 0x913b);
        for (size_t bit = 0; bit < 16; ++bit) {
            assert(semantic_observation[SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_BITS_BASE + bit] ==
                   ((weapon_name_hash & (1U << bit)) ? 1.0f : 0.0f));
            assert(semantic_observation[SHAZBOT_V6_MOUNTED_PACK_NAME_HASH_BITS_BASE + bit] ==
                   ((pack_name_hash & (1U << bit)) ? 1.0f : 0.0f));
        }
        assert(semantic_observation[SHAZBOT_V6_MOUNTED_WEAPON_NAME_HASH_VALID] == 1.0f &&
               semantic_observation[SHAZBOT_V6_MOUNTED_PACK_NAME_HASH_VALID] == 1.0f);
        float pack_context[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS] = {0};
        shazbot_apply_mounted_pack_availability(&memory, shazbot_fixture.player,
                                                 pack_context);
        assert(pack_context[48] == 1.0f && pack_context[60] == 1.0f);
        assert(shazbot_pack_command_index("pack_upgrade_repair.dts") == 1 &&
               shazbot_pack_command_index("pack_deploy_sensor_pulse.dts") == 11 &&
               shazbot_pack_command_index("turret_muzzlepoint.dts") == -1);
        static const char *const stock_hud_weapons[18] = {
            "Blaster", "Plasma", "Chaingun", "Disc", "GrenadeLauncher",
            "SniperRifle", "ELFGun", "Mortar", "MissileLauncher",
            "TargetingLaser", "ShockLance", "TR2Disc", "TR2GrenadeLauncher",
            "TR2Chaingun", "TR2GoldTargetingLaser", "TR2SilverTargetingLaser",
            "TR2ShockLance", "TR2Mortar",
        };
        for (size_t item = 0; item < sizeof stock_hud_weapons / sizeof stock_hud_weapons[0]; ++item)
            assert(shazbot_weapon_item_code(stock_hud_weapons[item]) > 0 &&
                   shazbot_weapon_item_code(stock_hud_weapons[item]) < (1U <<
                       SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_PER_SLOT));
        for (size_t item = 0; item < sizeof stock_hud_weapons / sizeof stock_hud_weapons[0]; ++item)
            assert(strcmp(shazbot_stock_hud_weapon_name((int)item), stock_hud_weapons[item]) == 0);
        assert(shazbot_weapon_item_code("UnmappedModWeapon") == 0);

        shazbot_hud_weapon_clear_state();
        shazbot_hud_weapon_update_state(3, "", 1);
        assert(shazbot_hud_weapon_count == 1 &&
               strcmp(shazbot_hud_weapon_order[0], "Disc") == 0);
        shazbot_hud_weapon_clear_state();
        shazbot_hud_weapon_update_state(3, "Disc", 1);
        shazbot_hud_weapon_update_state(1, "Plasma", 1);
        shazbot_hud_weapon_update_state(0, "Blaster", 1);
        shazbot_hud_weapon_active_state("Blaster");
        float hud_weapon_context[SHAZBOT_COMMAND_OBSERVATION_V5_DIMENSIONS] = {0};
        shazbot_apply_hud_weapon_context(hud_weapon_context);
        assert(hud_weapon_context[34] == 0.0f && hud_weapon_context[35] == 0.0f &&
               hud_weapon_context[36] == 1.0f && hud_weapon_context[40] == 1.0f &&
               hud_weapon_context[41] == 1.0f && hud_weapon_context[42] == 1.0f &&
               hud_weapon_context[43] == 1.0f && hud_weapon_context[47] == 1.0f);
        shazbot_hud_weapon_update_state(1, "Plasma", 0);
        shazbot_hud_weapon_active_state("Blaster");
        memset(hud_weapon_context, 0, sizeof hud_weapon_context);
        shazbot_apply_hud_weapon_context(hud_weapon_context);
        assert(hud_weapon_context[35] == 1.0f && hud_weapon_context[36] == 0.0f &&
               hud_weapon_context[41] == 1.0f && hud_weapon_context[42] == 1.0f &&
               hud_weapon_context[43] == 0.0f && hud_weapon_context[47] == 1.0f);
        float weapon_slot_observation[SHAZBOT_COMMAND_OBSERVATION_V7_DIMENSIONS] = {0};
        shazbot_store_hud_weapon_slot_codes(weapon_slot_observation);
        assert(weapon_slot_observation[SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_BASE + 2] == 1.0f &&
               weapon_slot_observation[SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_BASE +
                                       SHAZBOT_V7_WEAPON_SLOT_ITEM_CODE_BITS_PER_SLOT] == 1.0f);
        unsigned char weapon_slot_packet[SHAZBOT_COMMAND_OBSERVATION_V7_PACKET_SIZE];
        assert(shazbot_encode_command_observation_v7(weapon_slot_packet, 11, 12,
                                                     weapon_slot_observation) ==
               SHAZBOT_COMMAND_OBSERVATION_V7_PACKET_SIZE);
        assert(shazbot_get_u16le(weapon_slot_packet + 6) == SHAZBOT_COMMAND_OBSERVATION_V7_KIND &&
               shazbot_get_u32le(weapon_slot_packet + 8) ==
                   SHAZBOT_COMMAND_OBSERVATION_V7_PAYLOAD_SIZE);
        const char *hud_item_arguments[] = {
            "shazbot_hud_weapon_item", "5", "SniperRifle", "1"
        };
        assert(strcmp(shazbot_hud_weapon_item_cb(NULL, 4, hud_item_arguments), "ok") == 0);
        const char *invalid_hud_slot_arguments[] = {
            "shazbot_hud_weapon_item", "256", "SniperRifle", "1"
        };
        assert(strcmp(shazbot_hud_weapon_item_cb(NULL, 4, invalid_hud_slot_arguments),
                      "invalid HUD slot") == 0);
        unsigned char semantic_packet[SHAZBOT_COMMAND_OBSERVATION_V6_PACKET_SIZE];
        assert(shazbot_encode_command_observation_v6(semantic_packet, 10, 11,
                                                     semantic_observation) ==
               SHAZBOT_COMMAND_OBSERVATION_V6_PACKET_SIZE);
        assert(shazbot_get_u16le(semantic_packet + 6) == SHAZBOT_COMMAND_OBSERVATION_V6_KIND &&
               shazbot_get_u32le(semantic_packet + 8) ==
                   SHAZBOT_COMMAND_OBSERVATION_V6_PAYLOAD_SIZE);

        unsigned char notification_packet[SHAZBOT_NOTIFICATION_PACKET_SIZE] = {0};
        memcpy(notification_packet, "SHZB", 4);
        shazbot_put_u16le(notification_packet + 4, 1);
        shazbot_put_u16le(notification_packet + 6, SHAZBOT_NOTIFICATION_KIND);
        shazbot_put_u32le(notification_packet + 8, SHAZBOT_NOTIFICATION_PAYLOAD_SIZE);
        shazbot_put_u64le(notification_packet + 12, 123);
        shazbot_put_u64le(notification_packet + 20, 456);
        shazbot_put_u32le(notification_packet + SHAZBOT_HEADER_SIZE,
                          SHAZBOT_NOTIFICATION_CAPTURE_SAVED);
        uint64_t notification_sequence = 0;
        uint32_t notification_code = 0;
        assert(shazbot_decode_notification(notification_packet, sizeof notification_packet,
                                           &notification_sequence, &notification_code));
        assert(notification_sequence == 123 &&
               notification_code == SHAZBOT_NOTIFICATION_CAPTURE_SAVED);
        con_evaluate_t saved_notification_evaluate = Con__evaluate;
        Con__evaluate = shazbot_fake_command_evaluate;
        shazbot_command_eval_calls = 0;
        shazbot_handle_notification(notification_code);
        Con__evaluate = saved_notification_evaluate;
        assert(shazbot_command_eval_calls == 1 &&
               strstr(shazbot_command_eval_script, "clientCmdCenterPrint") != NULL &&
               strstr(shazbot_command_eval_script, "Shazbot v5 trace saved") != NULL);
        Con__evaluate = shazbot_fake_command_evaluate;
        shazbot_autoplay_demo_active = 1;
        shazbot_command_eval_calls = 0;
        shazbot_handle_notification(SHAZBOT_NOTIFICATION_CAPTURE_SAVED);
        assert(!shazbot_autoplay_demo_active && shazbot_command_eval_calls == 1 &&
               strstr(shazbot_command_eval_script, "schedule(1, 0, \"quit\")") != NULL);
        Con__evaluate = saved_notification_evaluate;

        /* An opt-in collector request starts labels without changing key
           bindings; the Sentry script can send slot commands through the hook. */
        store_hook_option("telemetry.shazbot_command_labels", "1");
        store_hook_option("telemetry.shazbot_auto_weapon_slot_test", "1");
        store_hook_option("telemetry.shazbot_weapon_slot_test_script",
                          "prefs/ShazbotSentryWeaponSlotTest_test.cs");
        shazbot_local_player_valid = 1;
        shazbot_auto_command_capture_active = 0;
        shazbot_auto_command_capture_reported = 0;
        shazbot_command_label_capture_active = 0;
        const char *label_arguments[] = {"shazbot_command_label", "0", "1"};
        assert(strcmp(shazbot_command_label_cb(NULL, 3, label_arguments), "disabled") == 0);
        shazbot_put_u32le(notification_packet + SHAZBOT_HEADER_SIZE,
                          SHAZBOT_NOTIFICATION_CAPTURE_START);
        assert(shazbot_decode_notification(notification_packet, sizeof notification_packet,
                                           &notification_sequence, &notification_code));
        assert(notification_code == SHAZBOT_NOTIFICATION_CAPTURE_START);
        Con__evaluate = shazbot_fake_command_evaluate;
        shazbot_command_eval_calls = 0;
        shazbot_handle_notification(notification_code);
        assert(shazbot_auto_command_capture_active && shazbot_command_label_capture_active &&
               shazbot_command_eval_calls == 2 &&
               strstr(shazbot_command_eval_script, "exec(\"prefs/ShazbotSentryWeaponSlotTest_test.cs\")") != NULL &&
               strstr(shazbot_command_eval_script, "ShazbotCalibration.cs") == NULL);
        const char *test_slot_arguments[] = {"shazbot_test_weapon_slot", "0"};
        shazbot_command_eval_calls = 0;
        assert(strcmp(shazbot_test_weapon_slot_cb(NULL, 2, test_slot_arguments), "ok") == 0);
        assert(shazbot_command_eval_calls == 1 &&
               strstr(shazbot_command_eval_script, "useFirstWeaponSlot(1)") != NULL);
        const char *invalid_slot_arguments[] = {"shazbot_test_weapon_slot", "6"};
        assert(strcmp(shazbot_test_weapon_slot_cb(NULL, 2, invalid_slot_arguments),
                      "slot must be in 0..5") == 0);
        assert(strcmp(shazbot_command_label_cb(NULL, 3, label_arguments), "ok") == 0);
        shazbot_command_eval_calls = 0;
        shazbot_handle_notification(SHAZBOT_NOTIFICATION_CAPTURE_SAVED);
        assert(!shazbot_auto_command_capture_active && !shazbot_command_label_capture_active &&
               shazbot_command_eval_calls == 1 &&
               strstr(shazbot_command_eval_script, "shazbot_command_capture_stop()") != NULL &&
               strstr(shazbot_command_eval_script, "stopRecordingDemo") == NULL &&
               strstr(shazbot_command_eval_script, "disconnect") == NULL);
        assert(strcmp(shazbot_command_label_cb(NULL, 3, label_arguments), "disabled") == 0);
        Con__evaluate = saved_notification_evaluate;

        /* A ghosted Player can point at either TargetManager TargetInfo array. */
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        unsigned char *client_enemy_info = shazbot_fixture.target_manager +
            SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET + 100 * SHAZBOT_TARGET_INFO_STRIDE;
        shazbot_test_put_word(client_enemy_info, 0x18, 2);
        shazbot_test_put_word(client_enemy_info, 0x20, 1);
        shazbot_test_put_pointer(client_enemy_info, 0x24, shazbot_fixture.enemies[0]);
        shazbot_test_put_pointer(shazbot_fixture.enemies[0], SHAZBOT_TARGET_INFO_OFFSET,
                                 client_enemy_info);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[16] == 1.0f && observation[10] == 10.0f);

        unsigned char observation_packet[SHAZBOT_OBSERVATION_PACKET_SIZE];
        assert(shazbot_encode_observation(observation_packet, 0x0102030405060708ULL,
                                          0x1112131415161718ULL, observation) ==
               sizeof observation_packet);
        static const unsigned char expected_observation_header[SHAZBOT_HEADER_SIZE] = {
            0x53,0x48,0x5a,0x42, 0x01,0x00, 0x01,0x00, 0x80,0x00,0x00,0x00,
            0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01,
            0x18,0x17,0x16,0x15,0x14,0x13,0x12,0x11
        };
        assert(memcmp(observation_packet, expected_observation_header,
                      sizeof expected_observation_header) == 0);
        assert(memcmp(observation_packet + SHAZBOT_HEADER_SIZE, observation,
                      sizeof observation) == 0);
        observation[10] = NAN;
        assert(shazbot_encode_observation(observation_packet, 1, 2, observation) == 0);

        /* Missing/stale targets and a position-only AssignedTask keep optional
           slots masked; contact normals require fresh ContactInfo flags. */
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        for (int slot = 6; slot <= 9; slot++) assert(observation[slot] == 0.0f);
         assert(observation[16] == 0.0f && observation[17] == 0.0f &&
                observation[24] == 0.0f && observation[25] == 0.0f &&
                 observation[26] == 0.0f && observation[29] == 15.0f &&
                observation[30] == 0.75f && observation[31] == 1.0f);
         shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
         shazbot_test_put_word(shazbot_fixture.player_data,
                               SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET, 1);
         shazbot_test_memory(&memory);
         assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, shazbot_fixture.target_manager,
                shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
                0, NULL, 0.0f, 0.0f, observation));
        assert(observation[30] == 0.0f && observation[31] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_word(shazbot_fixture.player_data,
                              SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET, 1);
        shazbot_test_prepare_energy_mount(456, 50.0f, 25.0f, 0);
        shazbot_test_put_pointer(shazbot_fixture.player, 0x7e4,
                                 shazbot_fixture.energy_mount);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[30] == 0.5f && observation[31] == 1.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_word(shazbot_fixture.player_data,
                              SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET, 1);
        shazbot_test_prepare_energy_mount(456, 50.0f, 0.0f, 1);
        shazbot_test_prepare_energy_mount_two(457, 100.0f, 25.0f, 0);
        shazbot_test_put_pointer(shazbot_fixture.energy_mount, 0x7e4,
                                 shazbot_fixture.energy_mount_two);
        shazbot_test_put_pointer(shazbot_fixture.player, 0x7e4,
                                 shazbot_fixture.energy_mount);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[30] == 0.5f && observation[31] == 1.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_word(shazbot_fixture.player_data,
                              SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET, 1);
        shazbot_test_prepare_energy_mount(456, 50.0f, 0.0f, 1);
        shazbot_test_prepare_energy_mount_two(457, 100.0f, 0.0f, 1);
        shazbot_test_put_pointer(shazbot_fixture.energy_mount, 0x7e4,
                                 shazbot_fixture.energy_mount_two);
        shazbot_test_put_pointer(shazbot_fixture.energy_mount_two, 0x7e4,
                                 shazbot_fixture.energy_mount);
        shazbot_test_put_pointer(shazbot_fixture.player, 0x7e4,
                                 shazbot_fixture.energy_mount);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[30] == 0.0f && observation[31] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_word(shazbot_fixture.player_data,
                              SHAZBOT_DATABLOCK_INHERIT_ENERGY_OFFSET, 1);
        shazbot_test_prepare_energy_mount(456, 50.0f, 25.0f, 0);
        shazbot_fixture.dictionary[456] = (uint32_t)(uintptr_t)shazbot_fixture.player;
        shazbot_test_put_pointer(shazbot_fixture.player, 0x7e4,
                                 shazbot_fixture.energy_mount);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[30] == 0.0f && observation[31] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        float invalid_max_energy = 0.0f;
        memcpy(shazbot_fixture.player_data + SHAZBOT_DATABLOCK_MAX_ENERGY_OFFSET,
               &invalid_max_energy, sizeof invalid_max_energy);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, shazbot_fixture.target_manager,
                shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
                0, NULL, 0.0f, 0.0f, observation));
        assert(observation[30] == 0.0f && observation[31] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        float invalid_speed_reference = NAN;
        memcpy(shazbot_fixture.player_data + SHAZBOT_PLAYER_MAX_FORWARD_SPEED_OFFSET,
               &invalid_speed_reference, sizeof invalid_speed_reference);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, shazbot_fixture.target_manager,
                shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
                0, NULL, 0.0f, 0.0f, observation));
        assert(observation[29] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        snprintf(shazbot_fixture.player_data_name, sizeof shazbot_fixture.player_data_name,
                 "ShapeBaseData");
        shazbot_test_set_class(shazbot_fixture.player_data, shazbot_fixture.player_data_class,
                               shazbot_fixture.player_data_name);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[30] == 0.0f && observation[31] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_word(shazbot_fixture.player, 0xA3C, 1);
        shazbot_test_put_word(shazbot_fixture.player, 0xA3D, 1);
        float normal_x = 0.0f, normal_y = 0.0f, normal_z = 2.0f;
        memcpy(shazbot_fixture.player + 0x990, &normal_x, sizeof normal_x);
        memcpy(shazbot_fixture.player + 0x994, &normal_y, sizeof normal_y);
        memcpy(shazbot_fixture.player + 0x998, &normal_z, sizeof normal_z);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[6] == 0.0f && observation[7] == 0.0f &&
               observation[8] == 1.0f && observation[9] == 1.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_word(shazbot_fixture.player, 0xA3C, 1);
        shazbot_test_put_word(shazbot_fixture.player, 0xA3D, 1);
        shazbot_test_put_pointer(shazbot_fixture.player, 0x7E4, shazbot_fixture.beacon);
        memcpy(shazbot_fixture.player + 0x990, &normal_x, sizeof normal_x);
        memcpy(shazbot_fixture.player + 0x994, &normal_y, sizeof normal_y);
        memcpy(shazbot_fixture.player + 0x998, &normal_z, sizeof normal_z);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[9] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_word(shazbot_fixture.player, 0xA3C, 1);
        shazbot_test_put_word(shazbot_fixture.player, 0xA3D, 0); /* contact but not a current jump surface */
        normal_x = 1.0f; normal_y = 0.0f; normal_z = 0.0f;
        memcpy(shazbot_fixture.player + 0x990, &normal_x, sizeof normal_x);
        memcpy(shazbot_fixture.player + 0x994, &normal_y, sizeof normal_y);
        memcpy(shazbot_fixture.player + 0x998, &normal_z, sizeof normal_z);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[6] == 0.0f && observation[7] == 0.0f &&
               observation[8] == 0.0f && observation[9] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_client_target(0, 0, UINT32_MAX, objective_position);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[24] == 1.0f && observation[25] == 0.0f &&
               observation[21] == 0.0f && observation[22] == 0.0f &&
               observation[23] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        shazbot_test_set_current_target(100, shazbot_fixture.enemies[0]);
        shazbot_test_put_word(shazbot_fixture.target_manager +
                              SHAZBOT_CLIENT_TARGET_ARRAY_OFFSET +
                              100 * SHAZBOT_TARGET_INFO_STRIDE, 0x3c, 0);
        shazbot_test_add_client_target(0, 0, 100, objective_position);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[24] == 1.0f && observation[25] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        shazbot_test_set_current_target(100, shazbot_fixture.enemies[0]);
        shazbot_fixture.dictionary[201] = 0; /* stale TargetInfo object, no dictionary ownership */
        shazbot_test_put_word(shazbot_fixture.visible_set, 0x30, 0);
        shazbot_test_add_client_target(0, 0, 100, objective_position);
        shazbot_class_rep_watched_object = shazbot_fixture.enemies[0];
        shazbot_class_rep_watched_calls = 0;
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[24] == 1.0f && observation[25] == 0.0f);
        assert(shazbot_class_rep_watched_calls == 0);
        shazbot_class_rep_watched_object = NULL;
        static const char *const supported_target_classes[] = {
            "Item", "Vehicle", "WheeledVehicle", "FlyingVehicle", "HoverVehicle"
        };
        shazbot_test_velocity_vector[0] = 4.0f;
        shazbot_test_velocity_vector[1] = 5.0f;
        shazbot_test_velocity_vector[2] = 6.0f;
        for (size_t class_index = 0;
             class_index < sizeof supported_target_classes / sizeof supported_target_classes[0];
             class_index++) {
            shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
            shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
            snprintf(shazbot_fixture.enemy_names[0], sizeof shazbot_fixture.enemy_names[0], "%s",
                     supported_target_classes[class_index]);
            shazbot_test_set_class(shazbot_fixture.enemies[0], shazbot_fixture.enemy_classes[0],
                                   shazbot_fixture.enemy_names[0]);
            shazbot_test_set_current_target(100, shazbot_fixture.enemies[0]);
            shazbot_test_put_word(shazbot_fixture.visible_set, 0x30, 0);
            shazbot_test_add_client_target(0, 0, 100, objective_position);
            shazbot_test_velocity_calls = 0;
            shazbot_test_memory(&memory);
            assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
                   shazbot_fixture.dictionary, shazbot_fixture.target_manager,
                   shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
                   0, NULL, 0.0f, 0.0f, observation));
            assert(observation[24] == 1.0f && observation[25] == 1.0f);
            assert(observation[21] == 3.0f && observation[22] == 3.0f &&
                   observation[23] == 3.0f && shazbot_test_velocity_calls == 1);
        }
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        snprintf(shazbot_fixture.enemy_names[0], sizeof shazbot_fixture.enemy_names[0], "Vehicle");
        shazbot_test_set_class(shazbot_fixture.enemies[0], shazbot_fixture.enemy_classes[0],
                               shazbot_fixture.enemy_names[0]);
        shazbot_test_set_current_target(100, shazbot_fixture.enemies[0]);
        shazbot_test_put_word(shazbot_fixture.visible_set, 0x30, 0);
        shazbot_test_add_client_target(0, 0, 100, objective_position);
        shazbot_test_velocity_vector[0] = NAN;
        shazbot_test_velocity_vector[1] = 5.0f;
        shazbot_test_velocity_vector[2] = 6.0f;
        shazbot_test_velocity_calls = 0;
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[24] == 1.0f && observation[25] == 0.0f &&
               shazbot_test_velocity_calls == 1);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        snprintf(shazbot_fixture.enemy_names[0], sizeof shazbot_fixture.enemy_names[0],
                 "ShapeBase");
        shazbot_test_set_class(shazbot_fixture.enemies[0], shazbot_fixture.enemy_classes[0],
                               shazbot_fixture.enemy_names[0]);
        shazbot_test_set_current_target(100, shazbot_fixture.enemies[0]);
        shazbot_test_put_word(shazbot_fixture.visible_set, 0x30, 0);
        shazbot_test_add_client_target(0, 0, 100, objective_position);
        shazbot_test_velocity_calls = 0;
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[24] == 1.0f && observation[25] == 0.0f &&
               shazbot_test_velocity_calls == 0);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        snprintf(shazbot_fixture.enemy_names[0], sizeof shazbot_fixture.enemy_names[0], "Vehicle");
        shazbot_test_set_class(shazbot_fixture.enemies[0], shazbot_fixture.enemy_classes[0],
                               shazbot_fixture.enemy_names[0]);
        shazbot_test_set_current_target(100, shazbot_fixture.enemies[0]);
        shazbot_test_put_word(shazbot_fixture.visible_set, 0x30, 0);
        shazbot_test_add_client_target(0, 0, 100, objective_position);
        shazbot_fixture.vtable[0xEC / sizeof(void *)] = (void *)(uintptr_t)0x1234;
        shazbot_test_velocity_calls = 0;
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[24] == 1.0f && observation[25] == 0.0f &&
               shazbot_test_velocity_calls == 0);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        shazbot_test_put_pointer(shazbot_fixture.visible_entries, 0, (void *)0x1234);
        shazbot_test_add_client_target(0, 1, 10, objective_position);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[16] == 0.0f && observation[17] == 0.0f &&
               observation[24] == 0.0f && observation[25] == 0.0f);

        /* A live enemy in the dictionary is not an opponent unless it is in
           the target's current-visible ShapeBase set. */
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        shazbot_test_put_word(shazbot_fixture.visible_set, 0x30, 0);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[16] == 0.0f && observation[17] == 0.0f);

        /* Stale dictionary identity and stale TargetInfo association invalidate
           an otherwise readable visible Player. */
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        shazbot_fixture.dictionary[201] = (uint32_t)(uintptr_t)shazbot_fixture.player;
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[16] == 0.0f && observation[17] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_add_visible_enemy(0, 201, 2, enemy_a_position, enemy_a_velocity);
        shazbot_test_put_pointer(shazbot_fixture.enemies[0], SHAZBOT_TARGET_INFO_OFFSET,
                                 shazbot_fixture.target_manager + SHAZBOT_TARGET_ARRAY_OFFSET);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[16] == 0.0f && observation[17] == 0.0f);

        /* A malformed list count or non-finite local velocity refuses unsafe
           source reads instead of converting unavailable state into valid zero. */
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_word(shazbot_fixture.client_target_list,
                              SHAZBOT_CLIENT_TARGET_LIST_COUNT_OFFSET,
                              SHAZBOT_CLIENT_TARGET_LIST_CAPACITY + 1);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[24] == 0.0f && observation[25] == 0.0f);
        shazbot_test_prepare_observation(123, 1, self_position, self_velocity);
        shazbot_test_put_pointer(shazbot_fixture.client_target_list,
                                 SHAZBOT_CLIENT_TARGET_LIST_ENTRIES_OFFSET, (void *)0x1234);
        shazbot_test_put_word(shazbot_fixture.client_target_list,
                              SHAZBOT_CLIENT_TARGET_LIST_COUNT_OFFSET, 1);
        shazbot_test_memory(&memory);
        assert(shazbot_collect_observation(&memory, shazbot_fixture.connection,
               shazbot_fixture.dictionary, shazbot_fixture.target_manager,
               shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
               0, NULL, 0.0f, 0.0f, observation));
        assert(observation[24] == 0.0f && observation[25] == 0.0f);
        float bad_velocity = NAN;
        memcpy(shazbot_fixture.player + SHAZBOT_PLAYER_VELOCITY_Y_OFFSET,
               &bad_velocity, sizeof bad_velocity);
        shazbot_test_memory(&memory);
        assert(!shazbot_collect_observation(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, shazbot_fixture.target_manager,
                shazbot_fixture.visible_set, shazbot_fixture.client_target_list,
                0, NULL, 0.0f, 0.0f, observation));

        memset(shazbot_fixture.native_render_frame, 0, sizeof shazbot_fixture.native_render_frame);
        shazbot_test_prepare_native_aim(123, 456, 2, 250.0f,
                                        shazbot_fixture.native_render_frame);
        shazbot_test_memory(&memory);
        shazbot_fixture.native_aim_point[0] = 100.0f;
        shazbot_fixture.native_aim_point[1] = 200.0f;
        shazbot_fixture.native_aim_point[2] = 50.0f;
        struct shazbot_native_aim_sample aim_sample;
        assert(shazbot_spatial_context_valid);
        assert(shazbot_spatial_player_id == 123 && shazbot_spatial_detection_radius == 250.0f);
        assert(rv_find_object(&memory, shazbot_fixture.dictionary, 123) == shazbot_fixture.player);
        assert(rv_find_object(&memory, shazbot_fixture.dictionary, 456) == shazbot_fixture.beacon);
        assert(rv_memory_range(&memory, shazbot_fixture.native_render_frame, 0x20, PROT_READ));
        assert(rv_memory_range(&memory, shazbot_fixture.native_aim_point, 12, PROT_READ));
        assert(rv_memory_range(&memory, shazbot_fixture.player + SHAZBOT_POSITION_X_OFFSET, 12, PROT_READ));
        assert(rv_memory_range(&memory, shazbot_fixture.projectile_data, 0x20, PROT_READ));
        assert(strcmp(rv_class_name(&memory, shazbot_fixture.beacon), "BeaconObject") == 0);
        uint32_t found_slot = UINT32_MAX;
        assert(shazbot_image_slot_for_projectile(&memory, shazbot_fixture.player,
               shazbot_fixture.projectile_data, &found_slot) && found_slot == 2);
        assert(shazbot_capture_native_aim(&memory, shazbot_fixture.dictionary,
                                          shazbot_fixture.native_render_frame,
                                          shazbot_fixture.native_aim_point, &aim_sample));
        assert(aim_sample.player_id == 123 && aim_sample.image_slot == 2 && aim_sample.target_id == 456);
        assert(aim_sample.muzzle[0] == 0.0f && aim_sample.muzzle[1] == 0.0f && aim_sample.muzzle[2] == 0.0f);
        assert(aim_sample.aim[0] == shazbot_fixture.native_aim_point[0] &&
               aim_sample.aim[1] == shazbot_fixture.native_aim_point[1] &&
               aim_sample.aim[2] == shazbot_fixture.native_aim_point[2] &&
               aim_sample.detection_radius == 250.0f);
        assert(shazbot_aim_sample_due(123, 2, 456, 99));
        assert(!shazbot_aim_sample_due(123, 2, 456, 99));
        assert(shazbot_aim_sample_due(123, 2, 456, 100));
        shazbot_test_put_pointer(shazbot_fixture.image_data, SHAZBOT_IMAGE_PROJECTILE_OFFSET,
                                 (void *)0x1234);
        shazbot_test_memory(&memory);
        assert(!shazbot_capture_native_aim(&memory, shazbot_fixture.dictionary,
                                           shazbot_fixture.native_render_frame,
                                           shazbot_fixture.native_aim_point, &aim_sample));

        shazbot_test_prepare_view(123, 77, 0.0f); /* no guessed radius fallback */
        shazbot_test_memory(&memory);
        assert(!shazbot_local_view_cone(&memory, shazbot_fixture.player, 123,
                shazbot_fixture.target_manager, eye_origin, eye_forward,
                &object_fov, &detect_radius));
        shazbot_test_prepare_view(123, 77, 250.0f);
        snprintf(shazbot_fixture.sensor_name, sizeof shazbot_fixture.sensor_name, "OtherData");
        shazbot_test_set_class(shazbot_fixture.sensor_data, shazbot_fixture.sensor_class,
                               shazbot_fixture.sensor_name);
        shazbot_test_memory(&memory);
        assert(!shazbot_local_view_cone(&memory, shazbot_fixture.player, 123,
                shazbot_fixture.target_manager, eye_origin, eye_forward,
                &object_fov, &detect_radius));

        shazbot_test_prepare_player(1234, "BeaconObject");
        shazbot_test_memory(&memory);
        assert(!shazbot_local_player_position(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, read_position, &player_id, NULL));
        shazbot_test_prepare_player(1234, "Player");
        snprintf(shazbot_fixture.connection_name, sizeof shazbot_fixture.connection_name,
                 "NetConnection");
        shazbot_test_set_class(shazbot_fixture.connection, shazbot_fixture.connection_class,
                               shazbot_fixture.connection_name);
        shazbot_test_memory(&memory);
        /* The live connection class metadata is not a reliable gate. The
           dictionary-backed exact Player validation below is authoritative. */
        assert(shazbot_local_player_position(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, read_position, &player_id, NULL));
        assert(player_id == 1234);
        shazbot_test_prepare_player(1234, "Player");
        shazbot_test_put_pointer(shazbot_fixture.connection, SHAZBOT_LOCAL_CONTROL_OFFSET,
                                 (void *)0x1234);
        shazbot_test_memory(&memory);
        assert(!shazbot_local_player_position(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, read_position, &player_id, NULL));
        assert(!shazbot_local_player_position(&memory, (void *)0x1234,
                shazbot_fixture.dictionary, read_position, &player_id, NULL));
        shazbot_test_prepare_player(1234, "Player");
        shazbot_fixture.dictionary[1234] = 0;  /* stale ID no longer maps to this pointer */
        shazbot_test_memory(&memory);
        assert(!shazbot_local_player_position(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, read_position, &player_id, NULL));
        shazbot_test_prepare_player(1234, "Player");
        x = NAN;
        memcpy(shazbot_fixture.player + SHAZBOT_POSITION_X_OFFSET, &x, sizeof x);
        shazbot_test_memory(&memory);
        assert(!shazbot_local_player_position(&memory, shazbot_fixture.connection,
                shazbot_fixture.dictionary, read_position, &player_id, NULL));

        unsigned char action_frame[SHAZBOT_ACTION_PACKET_SIZE] = {0};
        memcpy(action_frame, "SHZB", 4);
        shazbot_put_u16le(action_frame + 4, 1);
        shazbot_put_u16le(action_frame + 6, 2);
        shazbot_put_u32le(action_frame + 8, SHAZBOT_ACTION_PAYLOAD_SIZE);
        shazbot_put_u64le(action_frame + 12, 99);
        shazbot_put_u64le(action_frame + 20, 123456789);
        const float action_values[SHAZBOT_ACTION_DIMENSIONS] = {
            -0.5f, 0.75f, -1.0f, 0.1f, -0.2f, 0.05f,
            1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
        };
        for (size_t i = 0; i < SHAZBOT_ACTION_DIMENSIONS; i++)
            shazbot_put_f32le(action_frame + SHAZBOT_HEADER_SIZE + i * 4, action_values[i]);
        shazbot_put_f32le(action_frame + SHAZBOT_HEADER_SIZE +
                          SHAZBOT_COMMAND_ACTION_BASE * 4, 1.0f);
        uint64_t action_sequence = 0;
        float decoded_action[SHAZBOT_ACTION_DIMENSIONS];
        assert(shazbot_decode_action_frame(action_frame, sizeof action_frame,
                                           &action_sequence, decoded_action));
        assert(action_sequence == 99 && fabsf(decoded_action[3] - 0.1f) < 1.0e-6f &&
               fabsf(decoded_action[4] + 0.2f) < 1.0e-6f && decoded_action[9] == 0.0f &&
               decoded_action[SHAZBOT_COMMAND_ACTION_BASE] == 1.0f);
        char action_script[512];
        int next_triggers[7];
        const int previous_triggers[7] = {0, 0, 0, 0, 0, 0, 0};
        assert(shazbot_format_action_script(decoded_action, previous_triggers, 0,
                                            action_script, sizeof action_script,
                                            next_triggers));
        assert(strstr(action_script, "moveleft(0.5)") != NULL);
        assert(strstr(action_script, "moveright(0)") != NULL);
        assert(strstr(action_script, "movedown(1)") != NULL);
        assert(strstr(action_script, "moveforward(0.75)") != NULL);
        assert(strstr(action_script, "$mvPitch += 0.1") != NULL);
        assert(strstr(action_script, "$mvYaw += -0.2") != NULL);
        assert(strstr(action_script, "$mvRoll += 0.05") != NULL);
        assert(strstr(action_script, "toggleFreeLook(1)") != NULL);
        assert(strstr(action_script, "mouseFire(1)") != NULL);
        assert(strstr(action_script, "altTrigger(0)") != NULL);
        assert(strstr(action_script, "jump(0)") != NULL);
        assert(strstr(action_script, "mouseJet(1)") != NULL);
        assert(strstr(action_script, "$mvTriggerCount4") != NULL);
        assert(strstr(action_script, "$mvTriggerCount5") != NULL);
        assert(next_triggers[0] == 1 && next_triggers[1] == 1 && next_triggers[2] == 0 &&
               next_triggers[3] == 0 && next_triggers[4] == 1 && next_triggers[5] == 1 &&
               next_triggers[6] == 0);
        con_evaluate_t saved_evaluate = Con__evaluate;
        Con__evaluate = shazbot_fake_command_evaluate;
        shazbot_command_eval_calls = 0;
        float command_action[SHAZBOT_ACTION_DIMENSIONS] = {0};
        shazbot_action_command_known = 0;
        shazbot_action_command_state = -1;
        command_action[SHAZBOT_COMMAND_ACTION_BASE] = 1.0f;
        shazbot_apply_command_action(command_action);
        assert(shazbot_command_eval_calls == 1 &&
               strstr(shazbot_command_eval_script, "useFirstWeaponSlot(1)") != NULL);
        memset(command_action, 0, sizeof command_action);
        command_action[SHAZBOT_COMMAND_ACTION_BASE + SHAZBOT_COMMAND_USE_BACKPACK] = 1.0f;
        shazbot_apply_command_action(command_action);
        assert(shazbot_command_eval_calls == 2 &&
               strstr(shazbot_command_eval_script, "useBackPack(1)") != NULL);
        memset(command_action, 0, sizeof command_action);
        shazbot_apply_command_action(command_action);
        assert(shazbot_command_eval_calls == 3 &&
               strstr(shazbot_command_eval_script, "useBackPack(0)") != NULL);
        memset(command_action, 0, sizeof command_action);
        command_action[SHAZBOT_COMMAND_ACTION_BASE + 11] = 1.0f;
        shazbot_apply_command_action(command_action);
        assert(shazbot_command_eval_calls == 4 &&
               strstr(shazbot_command_eval_script, "quickPackEnergyPack(1)") != NULL);
        memset(command_action, 0, sizeof command_action);
        command_action[SHAZBOT_COMMAND_ACTION_BASE + 7] = 1.0f;
        shazbot_apply_command_action(command_action);
        assert(shazbot_command_eval_calls == 5 &&
               strstr(shazbot_command_eval_script, "prevWeapon(1)") != NULL);
        memset(command_action, 0, sizeof command_action);
        command_action[SHAZBOT_COMMAND_ACTION_BASE + 25] = 1.0f;
        shazbot_apply_command_action(command_action);
        assert(shazbot_command_eval_calls == 6 &&
               strstr(shazbot_command_eval_script, "throwFlag(1)") != NULL);
        shazbot_action_command_known = 0;
        shazbot_action_command_state = -1;
        Con__evaluate = saved_evaluate;
        assert(!shazbot_decode_action_frame(action_frame, sizeof action_frame - 1,
                                            &action_sequence, decoded_action));
        shazbot_put_f32le(action_frame + SHAZBOT_HEADER_SIZE + 2 * 4, 1.5f);
        assert(!shazbot_decode_action_frame(action_frame, sizeof action_frame,
                                            &action_sequence, decoded_action));
        shazbot_put_f32le(action_frame + SHAZBOT_HEADER_SIZE + 2 * 4, action_values[2]);
        shazbot_put_f32le(action_frame + SHAZBOT_HEADER_SIZE + 3 * 4, 7.0f);
        assert(!shazbot_decode_action_frame(action_frame, sizeof action_frame,
                                            &action_sequence, decoded_action));

        struct sockaddr_un address;
        memset(&address, 0, sizeof address);
        address.sun_family = AF_UNIX;
        snprintf(address.sun_path, sizeof address.sun_path, "/tmp/shazbot-test.sock");
        shazbot_fake_send_calls = 0;
        shazbot_fake_send_flags = 0;
        shazbot_fake_send_result = (ssize_t)sizeof packet;
        shazbot_fake_send_errno = 0;
        assert(!shazbot_send_datagram(0, 7, &address, sizeof address, packet,
                                      sizeof packet, shazbot_fake_sendto));
        assert(shazbot_fake_send_calls == 0);  /* disabled means no datagram */
        assert(!shazbot_send_datagram(1, -1, &address, sizeof address, packet,
                                      sizeof packet, shazbot_fake_sendto));
        assert(shazbot_fake_send_calls == 0);  /* no socket means drop */
        shazbot_fake_send_result = -1;
        shazbot_fake_send_errno = ENOENT;       /* receiver absent */
        assert(!shazbot_send_datagram(1, 7, &address, sizeof address, packet,
                                      sizeof packet, shazbot_fake_sendto));
        assert((shazbot_fake_send_flags & (MSG_DONTWAIT | MSG_NOSIGNAL)) ==
               (MSG_DONTWAIT | MSG_NOSIGNAL));
        shazbot_fake_send_errno = EAGAIN;       /* full/busy socket */
        assert(!shazbot_send_datagram(1, 7, &address, sizeof address, packet,
                                      sizeof packet, shazbot_fake_sendto));
        shazbot_fake_send_result = (ssize_t)sizeof packet;
        shazbot_fake_send_errno = 0;
        assert(shazbot_send_datagram(1, 7, &address, sizeof address, packet,
                                     sizeof packet, shazbot_fake_sendto));
        assert(shazbot_fake_send_calls == 3);

        store_hook_option("telemetry.shazbot_socket", "");  /* blank restores receiver default */
        assert(shazbot_open_socket());
        assert(strcmp(shazbot_address.sun_path, "/tmp/shazbot.sock") == 0);
        close(shazbot_socket_fd);
        shazbot_socket_fd = -1;
        shazbot_socket_path[0] = 0;

        /* Real AF_UNIX socket creation is nonblocking; an absent receiver drops. */
        char absent_socket[sizeof shazbot_address.sun_path];
        snprintf(absent_socket, sizeof absent_socket, "/tmp/t2-shazbot-no-receiver-%d.sock",
                 (int)getpid());
        store_hook_option("telemetry.shazbot_socket", absent_socket);
        assert(shazbot_open_socket());
        assert(fcntl(shazbot_socket_fd, F_GETFL) & O_NONBLOCK);
        assert(fcntl(shazbot_socket_fd, F_GETFD) & FD_CLOEXEC);
        assert(!shazbot_send_datagram(1, shazbot_socket_fd, &shazbot_address,
                                      shazbot_address_length, packet, sizeof packet,
                                      shazbot_real_sendto));
        close(shazbot_socket_fd);
        shazbot_socket_fd = -1;
        shazbot_socket_path[0] = 0;

        /* Received Tribes 2 data packets are mirrored to the same bounded
           SHZB datagram channel when packet capture is explicitly enabled. */
        char packet_socket_path[sizeof shazbot_address.sun_path];
        snprintf(packet_socket_path, sizeof packet_socket_path,
                 "/tmp/t2-shazbot-packet-%d.sock", (int)getpid());
        int packet_sink = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        assert(packet_sink >= 0);
        struct sockaddr_un packet_sink_address;
        memset(&packet_sink_address, 0, sizeof packet_sink_address);
        packet_sink_address.sun_family = AF_UNIX;
        snprintf(packet_sink_address.sun_path, sizeof packet_sink_address.sun_path,
                 "%s", packet_socket_path);
        unlink(packet_socket_path);
        assert(bind(packet_sink, (struct sockaddr *)&packet_sink_address,
                    sizeof packet_sink_address) == 0);
        store_hook_option("telemetry.shazbot_socket", packet_socket_path);
        store_hook_option("telemetry.shazbot_packet_capture", "1");

        int game_receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        int game_sender = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        assert(game_receiver >= 0 && game_sender >= 0);
        struct sockaddr_in game_address;
        memset(&game_address, 0, sizeof game_address);
        game_address.sin_family = AF_INET;
        game_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(game_receiver, (struct sockaddr *)&game_address,
                    sizeof game_address) == 0);
        socklen_t game_address_length = sizeof game_address;
        assert(getsockname(game_receiver, (struct sockaddr *)&game_address,
                           &game_address_length) == 0);
        const unsigned char game_packet[] = {0x01, 0x00, 0x00, 0xA5};
        assert(sendto(game_sender, game_packet, sizeof game_packet, 0,
                      (struct sockaddr *)&game_address, sizeof game_address) ==
               (ssize_t)sizeof game_packet);
        unsigned char received_packet[16];
        struct sockaddr_storage game_peer;
        socklen_t game_peer_length = sizeof game_peer;
        assert(recvfrom(game_receiver, received_packet, sizeof received_packet, 0,
                        (struct sockaddr *)&game_peer, &game_peer_length) ==
               (ssize_t)sizeof game_packet);
        unsigned char captured_frame[SHAZBOT_HOOK_SERVER_PACKET_FRAME_SIZE];
        const ssize_t captured_length = recvfrom(packet_sink, captured_frame,
                                                  sizeof captured_frame, 0, NULL, NULL);
        assert(captured_length == (ssize_t)(SHAZBOT_HEADER_SIZE + 2 + sizeof game_packet));
        assert(memcmp(captured_frame, "SHZB", 4) == 0 &&
               shazbot_get_u16le(captured_frame + 4) == 1 &&
               shazbot_get_u16le(captured_frame + 6) == SHAZBOT_HOOK_SERVER_PACKET_KIND &&
               shazbot_get_u32le(captured_frame + 8) == 2 + sizeof game_packet &&
               shazbot_get_u16le(captured_frame + SHAZBOT_HEADER_SIZE) == sizeof game_packet &&
               memcmp(captured_frame + SHAZBOT_HEADER_SIZE + 2, game_packet,
                      sizeof game_packet) == 0);
        close(game_sender);
        close(game_receiver);
        close(packet_sink);
        close(shazbot_socket_fd);
        shazbot_socket_fd = -1;
        shazbot_socket_path[0] = 0;
        unlink(packet_socket_path);
        store_hook_option("telemetry.shazbot_packet_capture", "0");
        puts("Shazbot telemetry: IPC bytes, Player validation and nonblocking drops passed");
    }

    /* HUD bars: the layout matches the stock fill, stores the value and fills
       vertically when asked; the patch applies only over the stock bytes. */
    {
        static unsigned char control[0x100];
        static void *vtable[0x120 / 4];
        vtable[0x118 / 4] = (void *)fake_bar_value;
        memset(control, 0, sizeof(control));
        *(void **)control = vtable;
        int32_t bounds[4] = {10, 20, 100, 8};
        memcpy(control + 0xC4, bounds, sizeof(bounds));
        int32_t offset[2] = {5, 7}, frame[6];
        fake_value = 0.5f;
        hud_bar_store_value = 0; hud_bar_vertical_fill = 1;
        hud_bar_layout(frame, control, offset);
        assert(frame[2] == 15 && frame[3] == 27 && frame[4] == 50 && frame[5] == 8);
        assert(*(float *)(control + HUD_BAR_VALUE_OFFSET) == 0.0f);
        hud_bar_store_value = 1;
        control[HUD_BAR_VERTICAL_OFFSET] = 1;
        int32_t tall[2] = {10, 40};
        memcpy(control + 0xCC, tall, sizeof(tall));
        hud_bar_layout(frame, control, offset);
        assert(frame[2] == 15 && frame[3] == 27 + 20 && frame[4] == 10 && frame[5] == 20);
        assert(*(float *)(control + HUD_BAR_VALUE_OFFSET) == 0.5f);
        assert(strcmp(hud_bar_get_value_cb(control, 2, NULL), "0.5") == 0);
        fake_value = 1.0f / 40.0f;  /* a one-pixel fill is drawn as two, as stock */
        hud_bar_layout(frame, control, offset);
        assert(frame[5] == 2 && frame[3] == 27 + 38);
        hud_bar_vertical_fill = 0;
        hud_bar_layout(frame, control, offset);
        assert(frame[4] == 0 && frame[5] == 40);

        /* Flips: along the fill reverses it, across it inverts the value. */
        fake_value = 0.25f;
        hud_bar_flip = 1;
        int32_t wide[2] = {100, 8};
        memcpy(control + 0xCC, wide, sizeof(wide));
        control[HUD_BAR_FLIP_HORIZONTAL_OFFSET] = 1;  /* horizontal, from the right */
        hud_bar_layout(frame, control, offset);
        assert(frame[2] == 15 + 75 && frame[3] == 27 && frame[4] == 25 && frame[5] == 8);
        control[HUD_BAR_FLIP_VERTICAL_OFFSET] = 1;    /* and inverted */
        hud_bar_layout(frame, control, offset);
        assert(frame[2] == 15 + 25 && frame[4] == 75);
        assert(*(float *)(control + HUD_BAR_VALUE_OFFSET) == 0.25f);  /* getValue() unchanged */
        hud_bar_vertical_fill = 1;
        memcpy(control + 0xCC, tall, sizeof(tall));   /* vertical: flipVertical fills from the top */
        control[HUD_BAR_FLIP_HORIZONTAL_OFFSET] = 0;
        hud_bar_layout(frame, control, offset);
        assert(frame[2] == 15 && frame[3] == 27 && frame[4] == 10 && frame[5] == 10);
        control[HUD_BAR_FLIP_HORIZONTAL_OFFSET] = 1;  /* inverted, from the top */
        hud_bar_layout(frame, control, offset);
        assert(frame[3] == 27 && frame[5] == 30);
        control[HUD_BAR_FLIP_VERTICAL_OFFSET] = 0;    /* inverted, from the bottom */
        hud_bar_layout(frame, control, offset);
        assert(frame[3] == 27 + 10 && frame[5] == 30);
        fake_value = 1.5f;                            /* inverted overfull is empty */
        hud_bar_layout(frame, control, offset);
        assert(frame[5] == 0);
        hud_bar_flip = 0;                             /* off: flips are ignored */
        fake_value = 0.25f;
        hud_bar_layout(frame, control, offset);
        assert(frame[3] == 27 + 30 && frame[5] == 10);
        control[HUD_BAR_FLIP_HORIZONTAL_OFFSET] = 0;

        uintptr_t hud_page = HUD_BAR_LAYOUT_ADDRESS & ~((uintptr_t)size - 1);
        assert(HUD_BAR_LAYOUT_ADDRESS + sizeof(hud_bar_layout_original) <= hud_page + (uintptr_t)size);
        unsigned char *hud = mmap((void *)hud_page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(hud == (void *)hud_page);
        unsigned char *site = (unsigned char *)HUD_BAR_LAYOUT_ADDRESS;
        memcpy(site, hud_bar_layout_original, sizeof(hud_bar_layout_original));
        site[10] = 0x02;
        assert(mprotect(hud, size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_hud_bars(1, 1, 1));
        assert(!hud_bar_store_value && !hud_bar_vertical_fill && !hud_bar_flip);  /* no getValue() either */
        assert(mprotect(hud, size, PROT_READ | PROT_WRITE) == 0);
        site[10] = 0x00;
        assert(mprotect(hud, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_hud_bars(1, 0, 1));
        assert(hud_bar_store_value == 1 && hud_bar_vertical_fill == 0);
        assert(memcmp(site, "\x53\x56\x8d\x45\xe8\x50\xe8", 7) == 0);
        assert(HUD_BAR_LAYOUT_ADDRESS + 11 + (uintptr_t)call_target_offset(HUD_BAR_LAYOUT_ADDRESS + 6) ==
               (uintptr_t)hud_bar_layout);
        assert(memcmp(site + 11, "\x83\xc4\x0c\xe9", 4) == 0);
        assert(HUD_BAR_LAYOUT_ADDRESS + 19 + (uintptr_t)call_target_offset(HUD_BAR_LAYOUT_ADDRESS + 14) ==
               HUD_BAR_LAYOUT_RESUME);
        assert(site[19] == 0x90 && site[sizeof(hud_bar_layout_original) - 1] == 0x90);
        munmap(hud, size);

        /* T2 Sky::renderSkyBox's prologue and mPoints layout are build-pinned;
           rotation is temporary and restores every source vertex afterward. */
        unsigned char sky_fixture[0x1000] = {0};
        float *sky_points = (float *)(sky_fixture + SKY_POINTS_OFFSET);
        sky_points[0] = 1.0f; sky_points[1] = 0.0f; sky_points[2] = 5.0f;
        sky_points[3] = 0.0f; sky_points[4] = 1.0f; sky_points[5] = -2.0f;
        sky_rotate_points(sky_fixture, sky_pi * 0.5f);
        assert(sky_points[0] < 0.001f && sky_points[0] > -0.001f &&
               sky_points[1] > 0.999f && sky_points[2] == 5.0f);
        sky_rotate_points(sky_fixture, -sky_pi * 0.5f);
        assert(sky_points[0] > 0.999f && sky_points[1] < 0.001f && sky_points[1] > -0.001f);
        sky_rotation_angle = 0.0f; sky_rotation_last_ns = 0;
        sky_rotation_update(1000000000ULL, 0.2f);
        sky_rotation_update(1250000000ULL, 0.2f);
        sky_rotation_update(1500000000ULL, 0.2f);
        sky_rotation_update(1750000000ULL, 0.2f);
        sky_rotation_update(2000000000ULL, 0.2f);
        assert(sky_rotation_angle > 0.199f && sky_rotation_angle < 0.201f);
        sky_rotation_update(4000000000ULL, 0.2f); /* a long pause is capped to 250 ms */
        assert(sky_rotation_angle > 0.249f && sky_rotation_angle < 0.251f);
        uintptr_t sky_page = SKY_RENDER_SLICES_ADDRESS & ~((uintptr_t)size - 1);
        unsigned char *sky_code = mmap((void *)sky_page, size, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(sky_code == (void *)sky_page);
        unsigned char *sky_site = (unsigned char *)SKY_RENDER_SLICES_ADDRESS;
        memcpy(sky_site, sky_render_slices_original, sizeof sky_render_slices_original);
        sky_site[9] = 0xc9; sky_site[10] = 0xc3; /* leave; ret after the copied prologue */
        assert(mprotect(sky_code, size, PROT_READ | PROT_EXEC) == 0);
        const struct game_target *saved_target = target;
        target = &game_targets[0];
        sky_rotation_hook_attempted = sky_rotation_hook_installed = 0;
        assert(install_sky_rotation_hook());
        int32_t sky_jump;
        memcpy(&sky_jump, sky_site + 1, sizeof sky_jump);
        assert(SKY_RENDER_SLICES_ADDRESS + 5 + sky_jump ==
               (uintptr_t)sky_render_slices_wrapper);
        assert(sky_site[5] == 0x90 && sky_site[8] == 0x90);
        assert(mprotect(sky_code, size, PROT_READ | PROT_WRITE) == 0);
        memcpy(sky_site, sky_render_slices_original, sizeof sky_render_slices_original);
        assert(mprotect(sky_code, size, PROT_READ | PROT_EXEC) == 0);
        assert(munmap(sky_code, size) == 0);
        assert(munmap(sky_rotation_trampoline, 32) == 0);
        sky_rotation_trampoline = NULL; sky_render_slices_original_fn = NULL;
        sky_rotation_hook_attempted = sky_rotation_hook_installed = 0;
        target = saved_target;

        uintptr_t iff_page = OBSERVER_IFF_ADDRESS & ~((uintptr_t)size - 1);
        unsigned char *iff = mmap((void *)iff_page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(iff == (void *)iff_page);
        memcpy((void *)OBSERVER_IFF_ADDRESS, observer_iff_original, sizeof(observer_iff_original));
        assert(mprotect(iff, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_observer_iff());
        assert(memcmp((void *)OBSERVER_IFF_ADDRESS, "\xeb\x4e\x8b\x88", 4) == 0);
        munmap(iff, size);

        /* LAN class-B join: the je becomes a jmp to the same target plus a nop;
           refused if the branch opcode is not the expected je. */
        uintptr_t lan_page = LAN_JOIN_CHECK_ADDRESS & ~((uintptr_t)size - 1);
        unsigned char *lan = mmap((void *)lan_page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(lan == (void *)lan_page);
        unsigned char *lan_site = (unsigned char *)LAN_JOIN_CHECK_ADDRESS;
        memcpy(lan_site, lan_join_check_original, sizeof(lan_join_check_original));
        lan_site[1] = 0x85;  /* jne instead of je -> refuse */
        assert(mprotect(lan, size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_lan_join_anywhere());
        assert(mprotect(lan, size, PROT_READ | PROT_WRITE) == 0);
        lan_site[1] = 0x84;
        assert(mprotect(lan, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_lan_join_anywhere());
        assert(memcmp(lan_site, "\xe9\xbd\x00\x00\x00\x90", 6) == 0);
        int32_t lan_rel; memcpy(&lan_rel, lan_site + 1, 4);  /* jmp keeps the je's target */
        assert(LAN_JOIN_CHECK_ADDRESS + 5 + lan_rel == LAN_JOIN_CHECK_ADDRESS + 6 + 0xbc);
        munmap(lan, size);

        /* Visible distance: the upper-clamp je becomes two nops; refused if the
           branch opcode is not the expected je. */
        uintptr_t vd_page = VISDIST_CLAMP_ADDRESS & ~((uintptr_t)size - 1);
        unsigned char *vd = mmap((void *)vd_page, size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(vd == (void *)vd_page);
        unsigned char *vd_site = (unsigned char *)VISDIST_CLAMP_ADDRESS;
        memcpy(vd_site, visdist_clamp_original, sizeof(visdist_clamp_original));
        vd_site[0] = 0x75;  /* jne instead of je -> refuse */
        assert(mprotect(vd, size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_visible_distance());
        assert(mprotect(vd, size, PROT_READ | PROT_WRITE) == 0);
        vd_site[0] = 0x74;
        assert(mprotect(vd, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_visible_distance());
        assert(memcmp(vd_site, "\x90\x90", 2) == 0);
        munmap(vd, size);

        /* No whiteout: the 1.5 scale load becomes fldz + nops. */
        uintptr_t wo_page = NO_WHITEOUT_ADDRESS & ~((uintptr_t)size - 1);
        unsigned char *wo = mmap((void *)wo_page, size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(wo == (void *)wo_page);
        unsigned char *wo_site = (unsigned char *)NO_WHITEOUT_ADDRESS;
        memcpy(wo_site, no_whiteout_original, sizeof(no_whiteout_original));
        wo_site[1] = 0x04;
        assert(mprotect(wo, size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_no_whiteout());
        assert(mprotect(wo, size, PROT_READ | PROT_WRITE) == 0);
        wo_site[1] = 0x05;
        assert(mprotect(wo, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_no_whiteout());
        assert(memcmp(wo_site, "\xd9\xee\x90\x90\x90\x90", 6) == 0);
        munmap(wo, size);

        /* Render scale: where the frame goes, and mouse positions back to the game. */
        struct render_rect fit = render_fit_rect(1920, 1080, 2560, 1600, 0);
        assert(fit.x == 0 && fit.y == 80 && fit.w == 2560 && fit.h == 1440);  /* width-limited */
        fit = render_fit_rect(1024, 768, 2560, 1600, 0);
        assert(fit.h == 1600 && fit.w == 2133 && fit.x == 213 && fit.y == 0);  /* height-limited */
        struct render_rect full = render_fit_rect(1920, 1080, 2560, 1600, 1);
        assert(full.x == 0 && full.y == 0 && full.w == 2560 && full.h == 1600);
        struct render_rect letterbox = render_fit_rect(1920, 1080, 2560, 1600, 0);
        int mx = 1280, my = 800;
        map_mouse_point(&mx, &my, letterbox, 1600, 1920, 1080);
        assert(mx == 960 && my == 540);                  /* centre stays centre */
        mx = 0; my = 10;                                 /* in the top bar: clamped */
        map_mouse_point(&mx, &my, letterbox, 1600, 1920, 1080);
        assert(mx == 0 && my == 0);
        mx = 2559; my = 1599;
        map_mouse_point(&mx, &my, letterbox, 1600, 1920, 1080);
        assert(mx == 1919 && my == 1079);
        assert(remap_draw_buffer(0x0405, 1) == GL_COLOR_ATTACHMENT0_);  /* GL_BACK */
        assert(remap_draw_buffer(0x0404, 1) == GL_COLOR_ATTACHMENT0_);  /* GL_FRONT */
        assert(remap_draw_buffer(0x0405, 0) == 0x0405);
        assert(remap_draw_buffer(0, 1) == 0);                          /* GL_NONE */
        unsigned char motion[20] = {4, 0, 0, 0};
        uint16_t ex = 1280, ey = 800;
        memcpy(motion + 4, &ex, 2); memcpy(motion + 6, &ey, 2);
        motion[8] = 7;                                   /* xrel untouched */
        scale_mouse_event(motion, letterbox, 1600, 1920, 1080);
        memcpy(&ex, motion + 4, 2); memcpy(&ey, motion + 6, 2);
        assert(ex == 960 && ey == 540 && motion[8] == 7);
        unsigned char key[20] = {2, 0, 0, 0, 0x34, 0x12};  /* SDL_KEYDOWN: not a mouse event */
        scale_mouse_event(key, letterbox, 1600, 1920, 1080);
        assert(key[4] == 0x34 && key[5] == 0x12);

        char pos_text[32];
        window_pos_text(0, 2160, pos_text, sizeof(pos_text));
        assert(strcmp(pos_text, "0,2160") == 0);
        window_pos_text(-5, 12, pos_text, sizeof(pos_text));
        assert(strcmp(pos_text, "-5,12") == 0);

        /* No fog: the glEnable wrapper drops GL_FOG and passes everything else. */
        real_gl_enable = fake_gl_enable;
        gl_enable_calls = 0;
        gl_enable_no_fog(GL_FOG_CAP);
        assert(gl_enable_calls == 0);                 /* fog enable dropped */
        gl_enable_no_fog(0x0BE2);                      /* GL_BLEND passes through */
        assert(gl_enable_calls == 1 && gl_enable_last_cap == 0x0BE2);
        real_gl_enable = NULL;

        /* setFov's limits: the max load points at 360.0, fld1 becomes fldz. */
        uintptr_t fov_page = FOV_MAX_LOAD & ~((uintptr_t)size - 1);
        unsigned char *fov = mmap((void *)fov_page, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(fov == (void *)fov_page);
        memcpy((void *)FOV_MAX_LOAD, fov_max_load_original, sizeof(fov_max_load_original));
        memcpy((void *)FOV_MIN_LOAD, fov_min_load_original, sizeof(fov_min_load_original));
        assert(mprotect(fov, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_fov_range());
        uint32_t max_address;
        memcpy(&max_address, (void *)(FOV_MAX_LOAD + 2), 4);
        assert(*(const float *)(uintptr_t)max_address == 360.0f);
        assert(memcmp((void *)FOV_MIN_LOAD, "\xd9\xee\xd8\xd1", 4) == 0);
        munmap(fov, size);

        /* Client datablocks: two je become jmp, the jne becomes nops. */
        uintptr_t data_page = CLIENT_SET_DATABLOCK_CHECK & ~((uintptr_t)size - 1);
        assert(mmap((void *)data_page, 2 * size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)data_page);
        assert(CLIENT_GET_DATABLOCK_CHECK + 5 <= data_page + 2 * (uintptr_t)size);
        memcpy((void *)CLIENT_GET_DATABLOCK_CHECK, client_get_datablock_original, 5);
        memcpy((void *)CLIENT_SET_DATABLOCK_CHECK, client_set_datablock_original, 5);
        memcpy((void *)CLIENT_SET_TARGET_CHECK, client_set_target_original, 6);
        assert(mprotect((void *)data_page, 2 * size, PROT_READ | PROT_EXEC) == 0);
        assert(install_client_datablocks());
        assert(((unsigned char *)CLIENT_GET_DATABLOCK_CHECK)[0] == 0xeb);
        assert(((unsigned char *)CLIENT_SET_DATABLOCK_CHECK)[0] == 0xeb);
        assert(memcmp((void *)CLIENT_SET_TARGET_CHECK, "\x90\x90\x90\x90\x90\x90", 6) == 0);
        munmap((void *)data_page, 2 * size);
    }

    /* Safe soundpacks: file_blocked_as_nonwav passes real WAVs and names that
       are not loose files, and blocks existing non-WAV files; the wrappers
       return "0" only when the file is blocked and otherwise call through. */
    {
        char tmpl[256];
        const char *tdir = getenv("TMPDIR");
        snprintf(tmpl, sizeof(tmpl), "%s/t2sp_XXXXXX", tdir && tdir[0] ? tdir : "/tmp");
        int fd = mkstemp(tmpl);
        assert(fd >= 0);
        unsigned char wav[44] = {
            'R','I','F','F', 36,0,0,0, 'W','A','V','E',
            'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0x44,0xac,0,0, 0x88,0x58,1,0, 2,0, 16,0,
            'd','a','t','a', 0,0,0,0};
        assert(write(fd, wav, sizeof(wav)) == (ssize_t)sizeof(wav));
        close(fd);
        soundpack_max_bytes = 0;
        assert(file_blocked_as_nonwav(tmpl) == 0);         /* a valid WAV passes */

        /* The byte ceiling blocks even a valid WAV that is too big, and only
           when a cap is set. */
        soundpack_max_bytes = 16;
        assert(file_blocked_as_nonwav(tmpl) == 1);         /* 44 bytes > 16 */
        soundpack_max_bytes = 44;
        assert(file_blocked_as_nonwav(tmpl) == 0);         /* exactly at the cap passes */
        soundpack_max_bytes = 0;

        fd = open(tmpl, O_WRONLY | O_TRUNC); assert(fd >= 0);
        wav[40] = 0xff; wav[41] = 0xff; wav[42] = 0xff; wav[43] = 0x7f;  /* data past EOF */
        assert(write(fd, wav, sizeof(wav)) == (ssize_t)sizeof(wav));
        close(fd);
        assert(file_blocked_as_nonwav(tmpl) == 1);

        fd = open(tmpl, O_WRONLY | O_TRUNC); assert(fd >= 0);
        const char *junk = "#!/bin/sh\necho definitely not a wave file, just text\n";
        assert(write(fd, junk, strlen(junk)) == (ssize_t)strlen(junk));  /* existing non-WAV */
        close(fd);
        assert(file_blocked_as_nonwav(tmpl) == 1);

        assert(file_blocked_as_nonwav("AudioChat") == 0);      /* an object name, not a file */
        assert(file_blocked_as_nonwav("no/such/file.wav") == 0);
        assert(file_blocked_as_nonwav("") == 0);

        soundpack_test_calls = 0;
        real_alx_get_wave_len = fake_alx_original;
        real_alx_create_source = fake_alx_original;
        const char *len_block[] = {"alxGetWaveLen", tmpl};
        assert(strcmp(alx_get_wave_len_safe_cb(NULL, 2, len_block), "0") == 0);
        assert(soundpack_test_calls == 0);                     /* original never reached */
        const char *len_pass[] = {"alxGetWaveLen", "AudioChat"};
        /* Pass-through returns the int result formatted as a string, not the
           raw int treated as a pointer (the bug that crashed the client). */
        assert(strcmp(alx_get_wave_len_safe_cb(NULL, 2, len_pass), "1060") == 0);
        assert(soundpack_test_calls == 1);
        const char *create_block[] = {"alxCreateSource", "AudioChat", tmpl};
        assert(strcmp(alx_create_source_safe_cb(NULL, 3, create_block), "0") == 0);
        const char *create_pass[] = {"alxCreateSource", "AudioChat", "AnAudioObject"};
        assert(strcmp(alx_create_source_safe_cb(NULL, 3, create_pass), "1060") == 0);
        assert(soundpack_test_calls == 2);
        real_alx_get_wave_len = real_alx_create_source = NULL;
        unlink(tmpl);
    }

    /* Network packet limits: twelve sites over two pages, all or nothing, at
       each executable's own sites and variables. */
    for (size_t t = 0; t < GAME_TARGET_COUNT; t++) {
        target = &game_targets[t];
        const uintptr_t *sites = target->net_rate_compare;
        uintptr_t pages[2] = {sites[0] & ~((uintptr_t)size - 1), sites[1] & ~((uintptr_t)size - 1)};
        assert(sites[2] + NET_SIZE_STORE_OFFSET + 10 <= pages[1] + (uintptr_t)size);
        for (int i = 0; i < 2; i++)
            assert(mmap((void *)pages[i], size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)pages[i]);
        uint32_t rate_variable = (uint32_t)target->net_rate_variable;
        uint32_t size_variable = (uint32_t)target->net_size_variable;
        for (int i = 0; i < 3; i++) {
            unsigned char *site = (unsigned char *)sites[i];
            memcpy(site, "\x83\x3d", 2); memcpy(site + 2, &rate_variable, 4); site[6] = 0x20;
            unsigned char *store = site + NET_RATE_STORE_OFFSET;
            memcpy(store, "\xc7\x05", 2); memcpy(store + 2, &rate_variable, 4);
            memcpy(store + 6, "\x20\x00\x00\x00", 4);
            unsigned char *size_compare = site + NET_SIZE_COMPARE_OFFSET;
            memcpy(size_compare, "\x81\x3d", 2); memcpy(size_compare + 2, &size_variable, 4);
            memcpy(size_compare + 6, "\xc2\x01\x00\x00", 4);
            unsigned char *size_store = site + NET_SIZE_STORE_OFFSET;
            memcpy(size_store, "\xc7\x05", 2); memcpy(size_store + 2, &size_variable, 4);
            memcpy(size_store + 6, "\xc2\x01\x00\x00", 4);
        }
        unsigned char *last = (unsigned char *)sites[2] + NET_SIZE_STORE_OFFSET;
        last[6] = 0xc3;
        for (int i = 0; i < 2; i++) assert(mprotect((void *)pages[i], size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_net_packet_limits());
        assert(((unsigned char *)sites[0])[6] == 0x20);
        assert(mprotect((void *)pages[1], size, PROT_READ | PROT_WRITE) == 0);
        last[6] = 0xc2;
        assert(mprotect((void *)pages[1], size, PROT_READ | PROT_EXEC) == 0);
        assert(install_net_packet_limits());
        for (int i = 0; i < 3; i++) {
            const unsigned char *site = (const unsigned char *)sites[i];
            uint32_t value;
            assert(site[6] == 64);
            memcpy(&value, site + NET_RATE_STORE_OFFSET + 6, 4);
            assert(value == 64);
            memcpy(&value, site + NET_SIZE_COMPARE_OFFSET + 6, 4);
            assert(value == 1000);
            memcpy(&value, site + NET_SIZE_STORE_OFFSET + 6, 4);
            assert(value == 1000);
            memcpy(&value, site + NET_SIZE_STORE_OFFSET + 2, 4);
            assert(value == size_variable);
        }
        assert(readable_range(sites[0], 7, &protection) && protection == (PROT_READ | PROT_EXEC));
        for (int i = 0; i < 2; i++) munmap((void *)pages[i], size);
    }
    target = &game_targets[0];
    assert(strstr(net_options_override, "OP_PacketRateSlider.range = \"2 64\"") != NULL);
    assert(strstr(net_options_override, "\\t64\\t32\\t1000") != NULL);

    /* The dedicated server's WON strings and clock live at its own addresses. */
    {
        target = &game_targets[1];
        assert(target->dedicated && strcmp(target->executable, "tribes2d.dynamic") == 0);
        uintptr_t won_page = target->won_bootstrap[0] & ~((uintptr_t)size - 1);
        char *won = mmap((void *)won_page, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(won == (void *)won_page);
        strcpy((char *)target->won_bootstrap[0], "tribes2.m1.sierra.com:15104");
        strcpy((char *)target->won_bootstrap[1], "tribes2.m2.sierra.com:15104");
        strcpy((char *)target->won_bootstrap[2], "tribes2.m3.sierra.com:15104");
        assert(mprotect(won, size, PROT_READ) == 0);
        assert(redirect_won_bootstrap());
        assert(strcmp((char *)target->won_bootstrap[2], "127.0.0.1:15104") == 0);
        munmap(won, size);
        uintptr_t clock_page = target->real_milliseconds & ~((uintptr_t)size - 1);
        unsigned char *clock = mmap((void *)clock_page, size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(clock == (void *)clock_page);
        memcpy((void *)target->real_milliseconds, real_milliseconds_original, sizeof(real_milliseconds_original));
        assert(mprotect(clock, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_monotonic_clock());
        assert(((unsigned char *)target->real_milliseconds)[0] == 0xe9);
        munmap(clock, size);

        /* Clients without a WON certificate: both server patches, all or nothing. */
        uintptr_t auth_page = SERVER_REPLY_CERT_FLAG & ~((uintptr_t)size - 1);
        assert((SERVER_LAN_CHECK & ~((uintptr_t)size - 1)) == auth_page);
        unsigned char *auth = mmap((void *)auth_page, size, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        assert(auth == (void *)auth_page);
        memcpy((void *)SERVER_REPLY_CERT_FLAG, server_reply_cert_flag_original,
               sizeof(server_reply_cert_flag_original));
        memcpy((void *)SERVER_LAN_CHECK, server_lan_check_original, sizeof(server_lan_check_original));
        ((unsigned char *)SERVER_LAN_CHECK)[9] = 0x43;
        /* The ping and info skips are on two adjacent pages. */
        uintptr_t query_page = SERVER_PING_QUERY_SKIP & ~((uintptr_t)size - 1);
        assert((SERVER_INFO_QUERY_SKIP & ~((uintptr_t)size - 1)) == query_page + (uintptr_t)size);
        assert(mmap((void *)query_page, 2 * size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)query_page);
        memcpy((void *)SERVER_PING_QUERY_SKIP, server_ping_query_skip_original, 6);
        memcpy((void *)SERVER_INFO_QUERY_SKIP, server_info_query_skip_original, 6);
        assert(mprotect((void *)query_page, 2 * size, PROT_READ | PROT_EXEC) == 0);
        assert(mprotect(auth, size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_server_tribesnext_auth());
        assert(memcmp((void *)SERVER_REPLY_CERT_FLAG, server_reply_cert_flag_original, 10) == 0);
        assert(mprotect(auth, size, PROT_READ | PROT_WRITE) == 0);
        ((unsigned char *)SERVER_LAN_CHECK)[9] = 0x42;
        assert(mprotect(auth, size, PROT_READ | PROT_EXEC) == 0);
        assert(install_server_tribesnext_auth());
        const unsigned char *flag = (const unsigned char *)SERVER_REPLY_CERT_FLAG;
        assert(memcmp(flag, "\x83\xbe\x54\x21\x00\x00\x00\x0f\x95\xc0", 10) == 0);
        const unsigned char *lan = (const unsigned char *)SERVER_LAN_CHECK;
        assert(lan[0] == 0xe9 && SERVER_LAN_CHECK + 5 + (uintptr_t)call_target_offset(SERVER_LAN_CHECK) ==
               0x0822BFD4);
        munmap(auth, size);
        /* Browser queries from clients without a certificate: both skips became nops. */
        assert(memcmp((void *)SERVER_PING_QUERY_SKIP, "\x90\x90\x90\x90\x90\x90", 6) == 0);
        assert(memcmp((void *)SERVER_INFO_QUERY_SKIP, "\x90\x90\x90\x90\x90\x90", 6) == 0);
        munmap((void *)query_page, 2 * size);
        target = &game_targets[0];
    }

    /* T2CSRI server helpers: hex addresses both ways, and certificates signed
       by the TribesNext root key (a real one from the game's public.store
       when there is one). */
    {
        const char *hex_args[] = {"hook_tn_hex_ipv4", "7f000001"};
        assert(strcmp(hook_tn_hex_ipv4_cb(NULL, 2, hex_args), "127.0.0.1") == 0);
        const char *bad_hex[] = {"hook_tn_hex_ipv4", "7f0000zz"};
        assert(strcmp(hook_tn_hex_ipv4_cb(NULL, 2, bad_hex), "") == 0);
        const char *dotted[] = {"hook_tn_ipv4_hex", "10.0.0.7"};
        assert(strcmp(hook_tn_ipv4_hex_cb(NULL, 2, dotted), "0a000007") == 0);
        const char *malformed[] = {"hook_tn_verify_cert", "name\t12\t3\tabc"};
        assert(strcmp(hook_tn_verify_cert_cb(NULL, 2, malformed), "0") == 0);
        const char *bad_guid[] = {"hook_tn_verify_cert", "name\t1x\t3\tabc\tdef"};
        assert(strcmp(hook_tn_verify_cert_cb(NULL, 2, bad_guid), "0") == 0);
        setenv("T2LAUNCH_RSA_HELPER", getenv("T2LAUNCH_TEST_RSA_HELPER") ?
               getenv("T2LAUNCH_TEST_RSA_HELPER") : "games/tribes2/rsa_helper", 1);
        char store_path[4096];
        snprintf(store_path, sizeof(store_path), "%s/t2-linux/base/public.store", getenv("HOME"));
        FILE *store = fopen(store_path, "r");
        static char line[20001];
        if (store && fgets(line, sizeof(line), store)) {
            line[strcspn(line, "\r\n")] = 0;
            const char *cert[] = {"hook_tn_verify_cert", line};
            assert(strcmp(hook_tn_verify_cert_cb(NULL, 2, cert), "1") == 0);
            line[0] ^= 0x20;  /* another name: the signature no longer matches */
            assert(strcmp(hook_tn_verify_cert_cb(NULL, 2, cert), "0") == 0);
        }
        if (store) fclose(store);
    }
    assert(strstr(server_tribesnext_script, "package T2LaunchTribesNextServer") != NULL);
    /* Without a public address the check falls back to the LAN, never to any address. */
    assert(strstr(server_tribesnext_script, " return t2launch_tnSameLan(%source, %destination);\n") != NULL);
    assert(strstr(server_tribesnext_script, "return %public $= \"\"") == NULL);
    assert(strstr(server_tribesnext_script, "function serverCmdt2csri_challengeResponse(%client, %serverChallenge)") != NULL);

    /* Native TribesNext handshake: blob layout, nonce search, the three
       BitStream hooks against a real RSA round trip, and the code patches. */
    {
        unsigned char bytes[8];
        assert(native_auth_hex_bytes("000a1b", 6, bytes, sizeof(bytes)) == 2 &&
               bytes[0] == 0x0a && bytes[1] == 0x1b);
        assert(native_auth_hex_bytes("5", 1, bytes, sizeof(bytes)) == 1 && bytes[0] == 5);
        assert(native_auth_hex_bytes("00", 2, bytes, sizeof(bytes)) == 1 && bytes[0] == 0);
        assert(native_auth_hex_bytes("xyz", 3, bytes, sizeof(bytes)) == -1);
        assert(native_auth_hex_bytes("112233445566778899", 18, bytes, sizeof(bytes)) == -1);

        static char certificate[2048], signature[1022];
        memset(signature, '0', sizeof(signature) - 1);
        signature[0] = '1';                    /* 1021 digits: 511 bytes, padded to 512 */
        signature[sizeof(signature) - 1] = 0;
        snprintf(certificate, sizeof(certificate), "Player\t0004321\t05\t%s\t%s", TEST_RSA_MODULUS, signature);
        unsigned char blob[NATIVE_AUTH_BLOB_MAX];
        int length = native_auth_blob(certificate, blob, sizeof(blob));
        const char *header = "Player\t4321\t1\t129\t";
        size_t header_length = strlen(header);
        assert(length == (int)(header_length + 1 + 1 + 128 + 512));
        assert(memcmp(blob, header, header_length) == 0);
        assert(blob[header_length] == 5 && blob[header_length + 1] == 0);
        assert(blob[header_length + 2] == 0x83 && blob[header_length + 129] == 0x95);
        assert(blob[header_length + 130] == 0 && blob[header_length + 131] == 0x01);
        assert(native_auth_blob("Player\t1\t05\tab", blob, sizeof(blob)) == -1);
        snprintf(certificate, sizeof(certificate), "Player\tguid\t05\t%s\t%s", TEST_RSA_MODULUS, signature);
        assert(native_auth_blob(certificate, blob, sizeof(blob)) == -1);
        snprintf(certificate, sizeof(certificate), "Player\t0004321\t05\t%s\t%s", TEST_RSA_MODULUS, signature);

        unsigned char plain[128] = {0}, nonce[8] = {1, 2, 3, 4, 5, 6, 7, 8}, block[64];
        memcpy(plain + 20, nonce, 8);
        plain[60] = 0xaa;
        assert(native_auth_find_block(plain, 128, nonce, block) && block[40] == 0xaa);
        memset(plain, 0, sizeof(plain));
        memcpy(plain + 70, nonce, 8);
        assert(!native_auth_find_block(plain, 128, nonce, block));

        /* The pending connection's address (an IP NetAddress) and a real key. */
        uintptr_t address_page = NATIVE_AUTH_SERVER_ADDRESS & ~((uintptr_t)size - 1);
        assert(mmap((void *)address_page, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)address_page);
        int32_t ip_type = 1;
        memcpy((void *)NATIVE_AUTH_SERVER_ADDRESS, &ip_type, 4);
        memcpy((void *)(NATIVE_AUTH_SERVER_ADDRESS + 4), "\x0a\x00\x00\x07", 4);
        bitstream_write_flag = fake_write_flag;
        bitstream_write_int = fake_write_int;
        bitstream_read_int = fake_read_int;
        static struct fake_stream stream;

        /* Without unlocked credentials the stock WON flag goes through. */
        tribesnext_certificate[0] = 0;
        memset(&stream, 0, sizeof(stream));
        assert(native_auth_challenge_flag(&stream, 0x7701) == 1 && stream.length == 1 && stream.bits[0]);

        snprintf(tribesnext_certificate, sizeof(tribesnext_certificate), "%s", certificate);
        snprintf(tribesnext_modulus, sizeof(tribesnext_modulus), "%s", TEST_RSA_MODULUS);
        snprintf(tribesnext_private_exponent, sizeof(tribesnext_private_exponent), "%s", TEST_RSA_PRIVATE);
        memset(&stream, 0, sizeof(stream));
        assert(native_auth_challenge_flag(&stream, 1) == 0 && native_auth.sent);
        assert(fake_read_int(&stream, 1) == 0 && fake_read_int(&stream, 1) == 1);
        assert(fake_read_int(&stream, 8) == NATIVE_AUTH_MARKER);
        for (int i = 0; i < 8; i++) assert(fake_read_int(&stream, 8) == native_auth.nonce[i]);
        assert(fake_read_int(&stream, 8) == 10 && fake_read_int(&stream, 8) == 0 &&
               fake_read_int(&stream, 8) == 0 && fake_read_int(&stream, 8) == 7);
        assert(fake_read_int(&stream, 12) == length);
        for (int i = 0; i < length; i++) assert(fake_read_int(&stream, 8) == blob[i]);
        assert(stream.position == stream.length);

        /* The server's challenge: the nonce and 56 more bytes, encrypted to the account key. */
        char helper[4096];
        snprintf(helper, sizeof(helper), "%s", getenv("T2LAUNCH_TEST_RSA_HELPER") ?
                 getenv("T2LAUNCH_TEST_RSA_HELPER") : "games/tribes2/rsa_helper");
        assert(access(helper, X_OK) == 0);
        setenv("T2LAUNCH_RSA_HELPER", helper, 1);
        memset(plain, 0, sizeof(plain));
        plain[1] = 0x42;  /* nonzero bytes before the nonce are skipped */
        memcpy(plain + 5, native_auth.nonce, 8);
        for (int i = 13; i < 69; i++) plain[i] = (unsigned char)(i * 7);
        char plain_hex[257], cipher_hex[2048];
        bytes_to_hex(plain, sizeof(plain), plain_hex);
        assert(run_rsa_helper(plain_hex, "5", TEST_RSA_MODULUS, cipher_hex, sizeof(cipher_hex)));
        unsigned char cipher[128];
        int cipher_length = native_auth_hex_bytes(cipher_hex, strlen(cipher_hex), cipher, sizeof(cipher));
        assert(cipher_length >= 64 && cipher_length <= 128);
        memset(&stream, 0, sizeof(stream));
        fake_write_flag(&stream, 1);
        fake_write_int(&stream, cipher_length, 9);
        for (int i = 0; i < cipher_length; i++) fake_write_int(&stream, cipher[i], 8);
        assert(native_auth_reply_flag(&stream) == 0 && native_auth.verified && !native_auth.sent);
        assert(memcmp(native_auth.block, plain + 5, 64) == 0);

        memset(&stream, 0, sizeof(stream));
        assert(native_auth_connect_flag(&stream, 0) == 0);
        assert(fake_read_int(&stream, 1) == 1);
        for (int i = 12; i < 52; i++) assert(fake_read_int(&stream, 8) == plain[5 + i]);
        assert(stream.position == stream.length && !native_auth.verified);
        memset(&stream, 0, sizeof(stream));
        assert(native_auth_connect_flag(&stream, 1) == 1 && stream.length == 1);

        /* A stock server's WON flag reaches the stock code unchanged. */
        memset(&stream, 0, sizeof(stream));
        fake_write_flag(&stream, 1);
        assert(native_auth_reply_flag(&stream) == 1);
        memset(&stream, 0, sizeof(stream));
        fake_write_flag(&stream, 0);
        assert(native_auth_reply_flag(&stream) == 0);
        munmap((void *)address_page, size);
        tribesnext_certificate[0] = tribesnext_modulus[0] = tribesnext_private_exponent[0] = 0;

        /* The patches: two calls retargeted and the inline flag read replaced. */
        uintptr_t pages[2] = {0x08236000, 0x08237000};
        for (int i = 0; i < 2; i++)
            assert(mmap((void *)pages[i], size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)pages[i]);
        memcpy((void *)NATIVE_AUTH_CHALLENGE_FLAG_CALL, native_auth_challenge_call_original, 5);
        memcpy((void *)NATIVE_AUTH_CONNECT_FLAG_CALL, native_auth_connect_call_original, 5);
        memcpy((void *)NATIVE_AUTH_REPLY_FLAG_READ, native_auth_reply_read_original,
               sizeof(native_auth_reply_read_original));
        ((unsigned char *)NATIVE_AUTH_REPLY_FLAG_READ)[13] = 0x01;
        for (int i = 0; i < 2; i++) assert(mprotect((void *)pages[i], size, PROT_READ | PROT_EXEC) == 0);
        assert(!install_native_auth());
        assert(memcmp((void *)NATIVE_AUTH_CHALLENGE_FLAG_CALL, native_auth_challenge_call_original, 5) == 0);
        assert(mprotect((void *)pages[1], size, PROT_READ | PROT_WRITE) == 0);
        ((unsigned char *)NATIVE_AUTH_REPLY_FLAG_READ)[13] = 0x00;
        assert(mprotect((void *)pages[1], size, PROT_READ | PROT_EXEC) == 0);
        assert(NATIVE_AUTH_CHALLENGE_FLAG_CALL + 5 + call_target_offset(NATIVE_AUTH_CHALLENGE_FLAG_CALL) ==
               BITSTREAM_WRITE_FLAG);
        assert(NATIVE_AUTH_CONNECT_FLAG_CALL + 5 + call_target_offset(NATIVE_AUTH_CONNECT_FLAG_CALL) ==
               BITSTREAM_WRITE_FLAG);
        assert(install_native_auth());
        assert(NATIVE_AUTH_CHALLENGE_FLAG_CALL + 5 + call_target_offset(NATIVE_AUTH_CHALLENGE_FLAG_CALL) ==
               (uintptr_t)native_auth_challenge_flag);
        assert(NATIVE_AUTH_CONNECT_FLAG_CALL + 5 + call_target_offset(NATIVE_AUTH_CONNECT_FLAG_CALL) ==
               (uintptr_t)native_auth_connect_flag);
        const unsigned char *reply = (const unsigned char *)NATIVE_AUTH_REPLY_FLAG_READ;
        assert(reply[0] == 0x57 && reply[1] == 0xe8 && reply[6] == 0x83 && reply[7] == 0xc4 &&
               reply[8] == 0x04 && reply[9] == 0xe9);
        assert(NATIVE_AUTH_REPLY_FLAG_READ + 6 + call_target_offset(NATIVE_AUTH_REPLY_FLAG_READ + 1) ==
               (uintptr_t)native_auth_reply_flag);
        assert(NATIVE_AUTH_REPLY_FLAG_READ + 14 + call_target_offset(NATIVE_AUTH_REPLY_FLAG_READ + 9) ==
               NATIVE_AUTH_REPLY_FLAG_STORE);
        for (int i = 0; i < 2; i++) munmap((void *)pages[i], size);
    }

    /* Locked arrays: only the exact extension token is removed. */
    {
        char out[256];
        remove_extension("GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_EXT_compiled_vertex_array_x "
                         "GL_EXT_texture_env_add", LOCKED_ARRAYS_EXTENSION, out, sizeof(out));
        assert(strcmp(out, "GL_ARB_multitexture GL_EXT_compiled_vertex_array_x GL_EXT_texture_env_add") == 0);
        remove_extension(LOCKED_ARRAYS_EXTENSION, LOCKED_ARRAYS_EXTENSION, out, sizeof(out));
        assert(strcmp(out, "") == 0);
        remove_extension("  GL_A  " LOCKED_ARRAYS_EXTENSION " GL_B ", LOCKED_ARRAYS_EXTENSION, out, sizeof(out));
        assert(strcmp(out, "GL_A GL_B") == 0);
    }

    /* Frame cap: steady frames sleep to the deadline; a late frame restarts it. */
    {
        struct timespec deadline = {0, 0}, now = {100, 0};
        const long period = 1000000000L / 100;
        assert(frame_cap_delay_ns(&deadline, &now, period) == 0);
        assert(deadline.tv_sec == 100 && deadline.tv_nsec == period);
        now.tv_nsec = 4000000;
        assert(frame_cap_delay_ns(&deadline, &now, period) == period - 4000000);
        assert(deadline.tv_nsec == 2 * period);
        now.tv_sec = 101;
        assert(frame_cap_delay_ns(&deadline, &now, period) == 0);
        assert(deadline.tv_sec == 101 && deadline.tv_nsec == 4000000 + period);
    }

    /* Web links: only the marker with a plain http(s)/ftp URL is opened. */
    assert(strcmp(web_link_url(WEB_LINK_MARKER "https://www.tribesnext.com/forum"),
                  "https://www.tribesnext.com/forum") == 0);
    assert(web_link_url(WEB_LINK_MARKER "ftp://example.net/map.vl2") != NULL);
    assert(web_link_url("netscape https://www.tribesnext.com") == NULL);
    assert(web_link_url(WEB_LINK_MARKER "file:///etc/passwd") == NULL);
    assert(web_link_url(WEB_LINK_MARKER "https://x.net/';touch /tmp/pwned;'") == NULL);
    assert(web_link_url(WEB_LINK_MARKER "https://x.net/a b") == NULL);
    assert(web_link_url(WEB_LINK_MARKER "https://x.net/$(id)`id`") == NULL);
    assert(system(WEB_LINK_MARKER "javascript:alert(1)") == -1);
    assert(system("exit 3") != -1);
    /* The URL reaches xdg-open as one argument, without the game's loader settings. */
    char bin_dir[] = "/tmp/t2_hook_xdg_XXXXXX";
    assert(mkdtemp(bin_dir));
    char fake_open[96], opened[96], saved_path[4096];
    snprintf(fake_open, sizeof(fake_open), "%s/xdg-open", bin_dir);
    snprintf(opened, sizeof(opened), "%s/opened", bin_dir);
    FILE *script = fopen(fake_open, "w");
    assert(script);
    fprintf(script, "#!/bin/sh\nprintf '%%s|%%s|%%s' \"$#\" \"$1\" \"${LD_PRELOAD-unset}\" > '%s'\n", opened);
    fclose(script);
    assert(chmod(fake_open, 0700) == 0);
    snprintf(saved_path, sizeof(saved_path), "%s", getenv("PATH"));
    setenv("PATH", bin_dir, 1);
    setenv("LD_PRELOAD", "/nonexistent/t2_hook.so", 1);
    assert(system(WEB_LINK_MARKER "https://www.tribesnext.com/?a=1&b=$HOME") == 0);
    setenv("PATH", saved_path, 1);
    unsetenv("LD_PRELOAD");
    char result[256] = "";
    for (int tries = 0; tries < 100 && !result[0]; tries++) {
        FILE *seen = fopen(opened, "r");
        if (seen) { if (!fgets(result, sizeof(result), seen)) result[0] = 0; fclose(seen); }
        if (!result[0]) usleep(20000);
    }
    assert(strcmp(result, "1|https://www.tribesnext.com/?a=1&b=$HOME|unset") == 0);
    unlink(opened); unlink(fake_open); rmdir(bin_dir);

    /* Depot downloads: the helper gets the file name after "--", runs without
       the game's loader settings, and its one output line is the status. */
    assert(strcmp(hook_depot_status_cb(NULL, 1, NULL), "error no download is running") == 0);
    char depot_dir[] = "/tmp/t2_hook_depot_XXXXXX";
    assert(mkdtemp(depot_dir));
    char fake_depot[96];
    snprintf(fake_depot, sizeof(fake_depot), "%s/depot.py", depot_dir);
    script = fopen(fake_depot, "w");
    assert(script);
    fprintf(script, "#!/bin/sh\n[ \"$#|$1|$3|${LD_PRELOAD-unset}\" = \"4|--dest|--|unset\" ] || exit 2\n"
                    "[ \"$4\" = 'missing map.ter' ] || { echo \"error not in the depot\"; exit 1; }\n"
                    "echo \"ok $2/terrains/$4\"\n");
    fclose(script);
    assert(chmod(fake_depot, 0700) == 0);
    const char *depot_args[] = {"hook_depot_fetch", "missing map.ter"};
    unsetenv("T2LAUNCH_DEPOT_HELPER");
    assert(strcmp(hook_depot_fetch_cb(NULL, 2, depot_args), "0") == 0);
    setenv("T2LAUNCH_DEPOT_HELPER", fake_depot, 1);
    setenv("HOME", "/home/player", 1);
    setenv("LD_PRELOAD", "/nonexistent/t2_hook.so", 1);
    assert(strcmp(hook_depot_fetch_cb(NULL, 2, depot_args), "1") == 0);
    assert(strcmp(hook_depot_fetch_cb(NULL, 2, depot_args), "0") == 0);  /* one at a time */
    unsetenv("LD_PRELOAD");
    const char *depot_status = "running";
    for (int tries = 0; tries < 250 && strcmp(depot_status, "running") == 0; tries++) {
        usleep(20000);
        depot_status = hook_depot_status_cb(NULL, 1, NULL);
    }
    assert(strcmp(depot_status, "ok /home/player/.loki/tribes2/base/terrains/missing map.ter") == 0);
    depot_args[1] = "other.dif";
    assert(strcmp(hook_depot_fetch_cb(NULL, 2, depot_args), "1") == 0);
    depot_status = "running";
    for (int tries = 0; tries < 250 && strcmp(depot_status, "running") == 0; tries++) {
        usleep(20000);
        depot_status = hook_depot_status_cb(NULL, 1, NULL);
    }
    assert(strcmp(depot_status, "error not in the depot") == 0);
    unlink(fake_depot); rmdir(depot_dir);

    /* Discord presence: the helper reads one line per state change on stdin,
       runs without the game's loader settings, and sees EOF when the game
       closes its end. */
    char discord_dir[] = "/tmp/t2_hook_discord_XXXXXX";
    assert(mkdtemp(discord_dir));
    char fake_discord[96], discord_seen[96];
    snprintf(fake_discord, sizeof(fake_discord), "%s/discord.py", discord_dir);
    snprintf(discord_seen, sizeof(discord_seen), "%s/seen", discord_dir);
    script = fopen(fake_discord, "w");
    assert(script);
    fprintf(script, "#!/bin/sh\necho \"$#|$1|$2|$3|$4|${LD_PRELOAD-unset}\" > '%s'\ncat >> '%s'\n",
            discord_seen, discord_seen);
    fclose(script);
    assert(chmod(fake_discord, 0700) == 0);
    unsetenv("T2LAUNCH_DISCORD_HELPER");
    assert(!start_discord_presence());
    setenv("T2LAUNCH_DISCORD_HELPER", fake_discord, 1);
    setenv("LD_PRELOAD", "/nonexistent/t2_hook.so", 1);
    assert(start_discord_presence());
    unsetenv("LD_PRELOAD");
    send_discord_presence("menu");
    send_discord_presence("menu");  /* unchanged: not sent again */
    send_discord_presence("game\tServer\tKatabatic\tCTF\t12\r\nextra");
    send_discord_presence(NULL);
    close(discord_socket);
    discord_socket = -1;
    assert(waitpid(discord_child, NULL, 0) == discord_child);
    char discord_expected[256], discord_result[256] = "";
    snprintf(discord_expected, sizeof(discord_expected),
             "4|--app-id|1563045749846056970|--pid|%d|unset\nmenu\ngame\tServer\tKatabatic\tCTF\t12\n",
             (int)getpid());
    FILE *discord_file = fopen(discord_seen, "r");
    assert(discord_file);
    size_t discord_length = fread(discord_result, 1, sizeof(discord_result) - 1, discord_file);
    discord_result[discord_length] = 0;
    fclose(discord_file);
    assert(strcmp(discord_result, discord_expected) == 0);
    send_discord_presence("menu");  /* no helper: ignored */
    unlink(fake_discord); unlink(discord_seen); rmdir(discord_dir);

    char path[64];
    setenv("T2LAUNCH_GAME_DIR", "/games/t2", 1);
    assert(strcmp(game_path(path, sizeof(path), "base/public.store"),
                  "/games/t2/base/public.store") == 0);
    char tiny[8];
    assert(game_path(tiny, sizeof(tiny), "rsa_helper")[0] == 0);

    char slots[3][WON_ENDPOINT_SIZE];
    assert(parse_won_endpoints(NULL, NULL, slots) && strcmp(slots[2], "127.0.0.1:15104") == 0);
    assert(parse_won_endpoints("", "16000", slots) && strcmp(slots[0], "127.0.0.1:16000") == 0);
    assert(parse_won_endpoints("won.example.net:15104", NULL, slots));
    assert(strcmp(slots[0], "won.example.net:15104") == 0 && strcmp(slots[2], slots[0]) == 0);
    assert(parse_won_endpoints("10.0.0.2:15104,10.0.0.3:15200", NULL, slots));
    assert(strcmp(slots[1], "10.0.0.3:15200") == 0 && strcmp(slots[2], "10.0.0.3:15200") == 0);
    assert(!parse_won_endpoints("a-very-long-hostname.example:15104", NULL, slots));  /* 34 bytes */
    assert(!parse_won_endpoints("10.0.0.2", NULL, slots));
    assert(!parse_won_endpoints("10.0.0.2:0", NULL, slots));
    assert(!parse_won_endpoints("bad host:1", NULL, slots));
    assert(!parse_won_endpoints("a:1,b:2,c:3,d:4", NULL, slots));
    assert(!parse_won_endpoints("a:1,,b:2", NULL, slots));
    assert(!parse_won_endpoints("", "70000", slots));

    puts("Hook mode enumeration, WON patch, game clock, browser bot limit, build identifier, demo tag, RadarHUD LOS filtering, and Shazbot telemetry passed");
    return 0;
}
