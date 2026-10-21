/*******************************************************************************
 * tefkernel - netmanager（联机同步辅助）
 *
 * 与内核 src/internal/terraria/netmanager.h 保持同步。
 *******************************************************************************/
#ifndef TEFKERNEL_LOADER_NETMANAGER_H
#define TEFKERNEL_LOADER_NETMANAGER_H

#include <stdint.h>

#include "../tef_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 发送一次图格操作请求（原版 MessageID 17，客户端 -> 服务端）
 *
 * 服务端权威执行后广播给所有客户端。客户端调用后应立即返回，
 * 不要假设本地已生效。
 *
 * @param action 0=KillTile(掉物品) 1=PlaceTile 2=KillWall 3=PlaceWall
 *               4=KillTile(不掉物品)
 */
DEFINE_FUNCTION(void, terraria_net_send_tile_action, int x, int y, int action)

/** @brief 请求服务端同步一块区域的图格（NetMessage.SendTileSquare） */
DEFINE_FUNCTION(void, terraria_net_send_tile_square, int x, int y, int width, int height)

/** @brief mod 消息回调（只在服务端触发） */
typedef void (*terraria_net_mod_message_cb)(const char *mod_id, const char *channel,
                                            const uint8_t *data, int len, int player);

/** @brief 注册 mod 消息回调（加载器初始化时调用一次） */
DEFINE_FUNCTION(void, terraria_net_set_mod_message_handler, terraria_net_mod_message_cb cb)

/**
 * @brief 客户端向服务端发送一条 Mod 消息（服务端权威架构入口）
 * @param channel 业务频道名（Mod 自定义）
 * @param data    负载字节（可为 NULL）
 * @param len     负载长度
 */
DEFINE_FUNCTION(void, terraria_net_send_mod_message,
                const char *mod_id, const char *channel, const void *data, int len)

#ifdef __cplusplus
}
#endif

#endif /* TEFKERNEL_LOADER_NETMANAGER_H */
