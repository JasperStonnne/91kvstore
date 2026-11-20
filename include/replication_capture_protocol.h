#ifndef KVS_REPLICATION_CAPTURE_PROTOCOL_H
#define KVS_REPLICATION_CAPTURE_PROTOCOL_H

/* TCP 字节协议；不把 C struct 或本机端序直接写入网络。
 * BEGIN <primary-id> <full-sync-id> <S>\r\n
 * DATA <start> <length>\r\n<length bytes of encoded commands>
 * APPLIED <offset>\r\n
 * ONLINE <T>\r\n
 * ERROR <reason>\r\n
 * BEGIN 成功后直接发送 DATA/ONLINE。APPLIED 必须停在完整命令边界。
 */
#define KVS_CAPTURE_COMMAND_BYTES 4096u
#define KVS_CAPTURE_DATA_BYTES (64u * 1024u)
#define KVS_CAPTURE_LINE_BYTES 256u
#define KVS_CAPTURE_TIMEOUT_MS 5000u
#endif
