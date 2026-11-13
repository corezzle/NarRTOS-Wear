#include "gallery_service.h"

#include "face_upload_service.h"
#include <nvs.h>
#include <nvs_flash.h>

static int s_uploaded_slots[FACE_UPLOAD_MAX_SLOTS] = {};
static int s_uploaded_count = 0;
static int s_pending_pick = -1;
static bool s_pick_mode_request = false;
static bool s_hidden[FACE_UPLOAD_MAX_SLOTS] = {};
static bool s_hidden_loaded = false;

static void load_hidden_flags(void)
{
    if (s_hidden_loaded) return;
    s_hidden_loaded = true;

    nvs_handle_t nvs;
    if (nvs_open("gallery", NVS_READONLY, &nvs) != ESP_OK) return;
    uint32_t mask = 0;
    if (nvs_get_u32(nvs, "hidden_mask", &mask) == ESP_OK) {
        for (int i = 0; i < FACE_UPLOAD_MAX_SLOTS; i++) {
            s_hidden[i] = ((mask >> i) & 1U) != 0;
        }
    }
    nvs_close(nvs);
}

static void save_hidden_flags(void)
{
    nvs_handle_t nvs;
    if (nvs_open("gallery", NVS_READWRITE, &nvs) != ESP_OK) return;
    uint32_t mask = 0;
    for (int i = 0; i < FACE_UPLOAD_MAX_SLOTS; i++) {
        if (s_hidden[i]) mask |= (1U << i);
    }
    nvs_set_u32(nvs, "hidden_mask", mask);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void gallery_service_sync_uploaded(void)
{
    load_hidden_flags();
    s_uploaded_count = 0;
    int n = face_upload_slot_count();
    if (n > FACE_UPLOAD_MAX_SLOTS) n = FACE_UPLOAD_MAX_SLOTS;
    for (int i = 0; i < n; i++) {
        const face_upload_slot_t* s = face_upload_get_slot(i);
        if (!s || !s->valid || !s->buf) continue;
        s_uploaded_slots[s_uploaded_count++] = i;
    }
}

int gallery_service_uploaded_count(void)
{
    return s_uploaded_count;
}

int gallery_service_uploaded_slot_at(int idx)
{
    if (idx < 0 || idx >= s_uploaded_count) return -1;
    return s_uploaded_slots[idx];
}

void gallery_service_set_pending_pick(int slot)
{
    load_hidden_flags();
    if (slot >= 0 && slot < FACE_UPLOAD_MAX_SLOTS && s_hidden[slot]) {
        s_hidden[slot] = false;
        save_hidden_flags();
    }
    s_pending_pick = slot;
}

int gallery_service_consume_pending_pick(void)
{
    int v = s_pending_pick;
    s_pending_pick = -1;
    return v;
}

void gallery_service_request_pick_mode(void)
{
    s_pick_mode_request = true;
}

bool gallery_service_consume_pick_mode_request(void)
{
    bool v = s_pick_mode_request;
    s_pick_mode_request = false;
    return v;
}

void gallery_service_hide_slot(int slot)
{
    load_hidden_flags();
    if (slot < 0 || slot >= FACE_UPLOAD_MAX_SLOTS) return;
    s_hidden[slot] = true;
    save_hidden_flags();
}

bool gallery_service_slot_hidden(int slot)
{
    load_hidden_flags();
    if (slot < 0 || slot >= FACE_UPLOAD_MAX_SLOTS) return false;
    return s_hidden[slot];
}

void gallery_service_clear_hidden(void)
{
    load_hidden_flags();
    for (int i = 0; i < FACE_UPLOAD_MAX_SLOTS; i++) s_hidden[i] = false;
    save_hidden_flags();
}

void gallery_service_on_slot_deleted(int slot)
{
    load_hidden_flags();
    if (slot < 0 || slot >= FACE_UPLOAD_MAX_SLOTS) return;

    for (int i = slot; i < FACE_UPLOAD_MAX_SLOTS - 1; i++) {
        s_hidden[i] = s_hidden[i + 1];
    }
    s_hidden[FACE_UPLOAD_MAX_SLOTS - 1] = false;
    save_hidden_flags();
}
