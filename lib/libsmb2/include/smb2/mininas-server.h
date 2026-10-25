#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct smb2_context;
struct smb2_pdu;
uint32_t mininas_tree_id(struct smb2_context* ctx);
int mininas_queue(struct smb2_context* ctx,struct smb2_pdu* pdu);
int mininas_error(struct smb2_context* ctx,uint16_t command,uint32_t status);
void mininas_drop(struct smb2_context* ctx);
#ifdef __cplusplus
}
#endif
