#pragma once

#include <stdbool.h>

void gallery_service_sync_uploaded(void);
int  gallery_service_uploaded_count(void);
int  gallery_service_uploaded_slot_at(int idx);
void gallery_service_set_pending_pick(int slot);
int  gallery_service_consume_pending_pick(void);
void gallery_service_request_pick_mode(void);
bool gallery_service_consume_pick_mode_request(void);
void gallery_service_hide_slot(int slot);
bool gallery_service_slot_hidden(int slot);
void gallery_service_clear_hidden(void);
void gallery_service_on_slot_deleted(int slot);
