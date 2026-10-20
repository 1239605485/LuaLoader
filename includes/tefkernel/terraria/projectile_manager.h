/*******************************************************************************
 * tefkernel - projectile_manager
 *
 * 自定义弹幕（Projectile）管理：注册 / ID 分配 / 贴图 / SetDefaults / AI 分发。
 * 结构与 item_manager 同构，便于维护。
 *******************************************************************************/
#ifndef TEFFKERNEL_TERRARIA_PROJECTILE_MANAGER_H
#define TEFFKERNEL_TERRARIA_PROJECTILE_MANAGER_H

#include <stdbool.h>

#include "../tef_api.h"
#include "../patchlib/type.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct terraria_projectile_handle_t terraria_projectile_handle_t;

typedef struct terraria_projectile_ops_t {
    /**
     * @brief 最早初始化（注册回调等前置操作），仅执行一次
     */
    void (*early_init)(terraria_projectile_handle_t *current);

    /**
     * @brief 静态初始化（ID 分配完成后调用）
     */
    void (*init_static)(terraria_projectile_handle_t *current);

    /**
     * @brief 属性设置：每次创建该弹幕实例时调用，给 instance 写字段
     * @warning instance 由 hook 管理器释放，不要持有
     */
    void (*set_defaults)(terraria_projectile_handle_t *current, patch_handle_t instance);

    /**
     * @brief 每帧 AI。
     *
     * 默认（ai_after_vanilla = false）：调用后内核会**跳过原版 AI**，
     * 位移/重力/旋转都要在回调里自己做。
     *
     * ai_after_vanilla = true 时：先让原版 AI（按 aiStyle）完整执行，
     * 内核再调用本回调，用于在 vanilla 行为之上补逻辑。
     * @warning instance 由 hook 管理器释放，不要持有
     */
    void (*ai)(terraria_projectile_handle_t *current, patch_handle_t instance);

    /**
     * @brief 获取纹理实例（Asset<Texture2D>），为 NULL 时用占位贴图
     */
    patch_handle_t (*get_texture)(terraria_projectile_handle_t *current);
} terraria_projectile_ops_t;

typedef struct terraria_projectile_handle_t {
    const char *parent_modloader_id;   //<< 所属 modloader id
    const char *parent_id;            //<< 所属 mod id
    const char *internal_name;        //<< 内部名称
    int runtime_id;                   //<< 由内核分配
    bool no_ai_hook;                  //<< true = 不接管 AI（沿用原版 aiStyle）
    bool ai_after_vanilla;            //<< true = 原版 AI 先执行，再调用扩展 ai 回调
    int frames;                       //<< 帧图帧数（Main.projFrames[type]）；0 = 单帧整图
    terraria_projectile_ops_t projectile_ops;
} terraria_projectile_handle_t;

/**
 * @brief 注册弹幕
 * @warning 需在 Main.Initialize 之前调用（否则分配不到有效 runtime_id）
 */
DEFINE_FUNCTION(bool, terraria_projectile_manager_register_projectile,
                terraria_projectile_handle_t* projectile_handle)

/**
 * @brief 通过运行时 ID 注销
 */
DEFINE_FUNCTION(bool, terraria_projectile_manager_unregister_projectile_by_id, int runtime_id)

/**
 * @brief 取自定义弹幕句柄（越界/未注册返回 NULL）
 */
DEFINE_FUNCTION(terraria_projectile_handle_t*, terraria_projectile_manager_get_projectile, int runtime_id)

/**
 * @brief 取全部自定义弹幕（tefstd_vector_t<terraria_projectile_handle_t*>）
 */
DEFINE_FUNCTION(tefstd_vector_t*, terraria_projectile_manager_get_projectiles)

/**
 * @brief 按 loader/mod/name 查找
 */
DEFINE_FUNCTION(terraria_projectile_handle_t*, terraria_projectile_manager_find_projectile,
                const char* loader, const char* mod, const char* name)

/**
 * @brief 原版 ProjectileID.Count
 */
DEFINE_FUNCTION(int, terraria_projectile_id_get_count)

#ifdef __cplusplus
}
#endif

#endif /* TEFFKERNEL_TERRARIA_PROJECTILE_MANAGER_H */
