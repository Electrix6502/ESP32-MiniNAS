/* MiniNAS additions. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "config.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include "compat.h"
#include "smb2.h"
#include "libsmb2.h"
#include "libsmb2-raw.h"
#include "libsmb2-private.h"
#include "mininas-server.h"
uint32_t mininas_tree_id(struct smb2_context* ctx){return ctx->hdr.sync.tree_id;}
int mininas_queue(struct smb2_context* ctx,struct smb2_pdu* pdu){
    if(!pdu){smb2_close_context(ctx);return 1;}
    smb2_set_pdu_message_id(ctx,pdu,ctx->message_id);smb2_queue_pdu(ctx,pdu);return 1;
}
int mininas_error(struct smb2_context* ctx,uint16_t cmd,uint32_t status){
    struct smb2_error_reply reply;memset(&reply,0,sizeof(reply));
    return mininas_queue(ctx,smb2_cmd_error_reply_async(ctx,&reply,cmd,status,NULL,NULL));
}
void mininas_drop(struct smb2_context* ctx){smb2_close_context(ctx);}
#ifdef ESP_PLATFORM
int gethostname(char* name,size_t len){if(!len)return -1;strncpy(name,"espnas",len);name[len-1]=0;return 0;}
#endif
