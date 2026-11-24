#ifndef MAPPINGS_HPP
#define MAPPINGS_HPP
#include <jni.h>
#include <cstdio>
#include <string>

namespace sdk
{
	namespace mappings
	{
		// Flags
		inline bool is_mojang_layout = false;
		inline bool fov_returns_double = false;
		inline bool fall_distance_is_float = false;
		inline bool is_modern_input = false;

		// Resolved at bind_mappings(): SharedConstants.getCurrentVersion().getProtocolVersion()
		// for the running game, or 0 if we couldn't read it (host shipped a
		// stripped SharedConstants, hostile mappings, etc.). Modules can read
		// this to branch behavior across 1.21.x sub-versions without having
		// to re-probe each individual method/field signature.
		//
		// Reference protocol versions (Mojang):
		//   1.21       = 767     1.21.4 = 769     1.21.7 = 772
		//   1.21.1     = 767     1.21.5 = 770     1.21.8 = 772
		//   1.21.2     = 768     1.21.6 = 771     1.21.9 = 773
		//   1.21.3     = 768                      1.21.10 = 773
		//                                         1.21.11 = 774
		inline int detected_protocol_version = 0;

		// Mappings variables
		inline const char* version = "1.21.10";
		inline const char* minecraftclass_sig = "net/minecraft/class_310";
		inline const char* minecraftclient_name = "field_1700";
		inline const char* minecraftclient_sig = "Lnet/minecraft/class_310;";
		inline const char* player_name = "field_1724";
		inline const char* player_sig = "Lnet/minecraft/class_746;";
		inline const char* world_name = "field_1687";
		inline const char* world_sig = "Lnet/minecraft/class_638;";
		inline const char* crosshair_target_name = "field_1765";
		inline const char* crosshair_target_sig = "Lnet/minecraft/class_239;";
		inline const char* interaction_manager_name = "field_1761";
		inline const char* interaction_manager_sig = "Lnet/minecraft/class_636;";
		inline const char* interaction_manager_class_sig = "net/minecraft/class_636";
		inline const char* attack_entity_name = "method_2918";
		inline const char* attack_entity_sig = "(Lnet/minecraft/class_1657;Lnet/minecraft/class_1297;)V";
		inline const char* network_handler_name = "method_1562";
		inline const char* network_handler_sig = "()Lnet/minecraft/class_634;";
		inline const char* connection_name = "field_1746";
		inline const char* connection_sig = "Lnet/minecraft/class_2535;";
		inline const char* do_attack_name = "method_1536";
		inline const char* do_attack_sig = "()Z";
		inline const char* handle_input_events_name = "method_1508";
		inline const char* handle_input_events_sig = "()V";
		inline const char* attack_cooldown_name = "field_1771";
		inline const char* attack_cooldown_sig = "I";
		// Minecraft.rightClickDelay (Mojang) / itemUseCooldown (Yarn).
		// Decremented every tick; while > 0, handleKeybinds() refuses to
		// fire startUseItem() even with keyUse held. Subiéndolo bloquea
		// el uso automático; bajándolo a 0 lo habilita. Intermediary
		// CORRECTO = field_1752 (mismo que usan fastpot/fastexp, que
		// funcionan). OJO: NO es field_1772.
		inline const char* item_use_cooldown_name = "field_1752";
		inline const char* item_use_cooldown_sig = "I";
		inline const char* gamerenderer_class_sig = "net/minecraft/class_757";
		inline const char* gamerenderer_name = "field_1773";
		inline const char* gamerenderer_sig = "Lnet/minecraft/class_757;";
		inline const char* update_crosshair_target_name = "method_3190";
		inline const char* update_crosshair_target_sig = "(F)V";
		inline const char* get_fov_name = "method_3196";
		inline const char* get_fov_sig = "(Lnet/minecraft/class_4184;FZ)F";
		inline const char* get_camera_name = "method_19418";
		inline const char* get_camera_sig = "()Lnet/minecraft/class_4184;";
		inline const char* camera_class_sig = "net/minecraft/class_4184";
		inline const char* camera_get_pos_name = "method_19326";
		inline const char* camera_get_pos_sig = "()Lnet/minecraft/class_243;";
		inline const char* camera_get_yaw_name = "method_19330";
		inline const char* camera_get_yaw_sig = "()F";
		inline const char* camera_get_pitch_name = "method_19329";
		inline const char* camera_get_pitch_sig = "()F";
		inline const char* vec3d_class_sig = "net/minecraft/class_243";
		inline const char* vec3d_x_name = "field_1352";
		inline const char* vec3d_x_sig = "D";
		inline const char* vec3d_y_name = "field_1351";
		inline const char* vec3d_y_sig = "D";
		inline const char* vec3d_z_name = "field_1350";
		inline const char* vec3d_z_sig = "D";
		inline const char* clientplayerentity_class_sig = "net/minecraft/class_746";
		inline const char* send_movement_packets_name = "method_3136";
		inline const char* send_movement_packets_sig = "()V";
		inline const char* tick_movement_name = "method_6007";
		inline const char* tick_movement_sig = "()V";
		inline const char* render_yaw_name = "field_3932";
		inline const char* render_yaw_sig = "F";
		inline const char* render_pitch_name = "field_3916";
		inline const char* render_pitch_sig = "F";
		inline const char* playermovec2spacket_class_sig = "net/minecraft/class_2828";
		inline const char* playermovec2spacket_yaw_name = "field_12887";
		inline const char* playermovec2spacket_yaw_sig = "F";
		inline const char* playermovec2spacket_pitch_name = "field_12885";
		inline const char* playermovec2spacket_pitch_sig = "F";
		inline const char* abilities_name = "field_7503";
		inline const char* abilities_sig = "Lnet/minecraft/class_1656;";
		inline const char* get_entity_interaction_range_name = "method_55755";
		inline const char* get_entity_interaction_range_sig = "()D";
		inline const char* get_attack_cooldown_progress_name = "method_7261";
		inline const char* get_attack_cooldown_progress_sig = "(F)F";
		// PlayerEntity.resetLastAttackedTicks() — resets the attack-strength
		// cooldown so the first-person weapon plays its 1.9+ recharge dip.
		// Calling swingHand WITHOUT this produces a bare "1.8-style" swing.
		inline const char* reset_last_attacked_ticks_name = "method_7350";
		inline const char* reset_last_attacked_ticks_sig = "()V";
		inline const char* player_entity_class_sig = "net/minecraft/class_1657";
		inline const char* fly_name = "field_7479";
		inline const char* fly_sig = "Z";
		inline const char* entity_get_x_name = "method_23317";
		inline const char* entity_get_x_sig = "()D";
		inline const char* entity_get_y_name = "method_23318";
		inline const char* entity_get_y_sig = "()D";
		inline const char* entity_get_z_name = "method_23321";
		inline const char* entity_get_z_sig = "()D";
		inline const char* entity_get_yaw_name = "method_36454";
		inline const char* entity_get_yaw_sig = "()F";
		inline const char* entity_get_pitch_name = "method_5695";
		inline const char* entity_get_pitch_sig = "(F)F";
		inline const char* entity_set_yaw_name = "method_36456";
		inline const char* entity_set_yaw_sig = "(F)V";
		inline const char* entity_set_pitch_name = "method_36457";
		inline const char* entity_set_pitch_sig = "(F)V";
		inline const char* get_bounding_box_name = "field_6005";
		inline const char* get_bounding_box_sig = "Lnet/minecraft/class_238;";
		inline const char* set_bounding_box_name = "method_5857";
		inline const char* set_bounding_box_sig = "(Lnet/minecraft/class_238;)V"; // Note signature change to V!
		inline const char* entity_class_sig = "net/minecraft/class_1297";
		inline const char* entity_set_flag_name = "method_5729";
		inline const char* entity_set_flag_sig = "(IZ)V";
		// Entity.setGlowing(boolean) — setea el campo local `glowing` (no lo pisa
		// el server como sí puede pisar el flag del DataTracker). Yarn 1.21:
		// method_5834. GLOWING flag index para setFlag = 6.
		inline const char* entity_set_glowing_name = "method_5834";
		inline const char* entity_set_glowing_sig  = "(Z)V";
		inline const char* entity_get_id_name = "method_5628";
		inline const char* entity_get_id_sig = "()I";
		inline const char* box_min_x_name = "field_1323";
		inline const char* box_min_x_sig = "D";
		inline const char* box_max_x_name = "field_1320";
		inline const char* box_max_x_sig = "D";
		inline const char* box_min_y_name = "field_1322";
		inline const char* box_min_y_sig = "D";
		inline const char* box_max_y_name = "field_1325";
		inline const char* box_max_y_sig = "D";
		inline const char* box_min_z_name = "field_1321";
		inline const char* box_min_z_sig = "D";
		inline const char* box_max_z_name = "field_1324";
		inline const char* box_max_z_sig = "D";
		inline const char* players_field_name = "field_18226";
		inline const char* players_field_sig = "Ljava/util/List;";
		inline const char* living_entity_class_sig = "net/minecraft/class_1309";
		inline const char* living_entity_is_blocking_name = "method_6039";
		inline const char* living_entity_is_blocking_sig = "()Z";
		inline const char* living_entity_is_using_item_name = "method_6115";
		inline const char* living_entity_is_using_item_sig = "()Z";
		inline const char* swing_hand_name = "method_6104";
		inline const char* swing_hand_sig = "(Lnet/minecraft/class_1268;)V";
		inline const char* entity_is_sneaking_name = "method_5715";
		inline const char* entity_is_sneaking_sig = "()Z";
		inline const char* entity_is_touching_water_name = "method_5799";
		inline const char* entity_is_touching_water_sig = "()Z";
		inline const char* set_sprinting_name = "method_5728";
		inline const char* set_sprinting_sig = "(Z)V";
		inline const char* is_sprinting_name = "method_5624";
		inline const char* is_sprinting_sig = "()Z";
		inline const char* is_on_ground_name = "method_24828";
		inline const char* is_on_ground_sig = "()Z";
		inline const char* entity_set_velocity_name = "method_18800";
		inline const char* entity_set_velocity_sig = "(DDD)V";
		inline const char* entity_velocity_name = "field_18276";
		inline const char* entity_velocity_sig = "Lnet/minecraft/class_243;";
		inline const char* entity_handle_fall_damage_name = "method_5747";
		inline const char* entity_handle_fall_damage_sig = "(DFLnet/minecraft/class_1282;)Z";
		inline const char* entity_fall_distance_name = "field_6017";
		inline const char* entity_fall_distance_sig = "D";
		inline const char* player_attack_name = "method_7324";
		inline const char* player_attack_sig = "(Lnet/minecraft/class_1297;)V";
		inline const char* player_attack_living_entity_name = "method_5997";
		inline const char* player_attack_living_entity_sig = "(Lnet/minecraft/class_1309;)V";
		inline const char* player_inventory_name = "field_7514";
		inline const char* player_inventory_sig = "Lnet/minecraft/class_1661;";
		inline const char* player_set_sprinting_name = "method_5728";
		inline const char* player_set_sprinting_sig = "(Z)V";
		inline const char* player_input_name = "field_3915";
		inline const char* player_input_sig = "Lnet/minecraft/class_744;";
		inline const char* player_inventory_class_sig = "net/minecraft/class_1661";
		inline const char* inventory_selected_slot_name = "field_7545";
		inline const char* inventory_selected_slot_sig = "I";
		inline const char* inventory_get_stack_name = "method_5438";
		inline const char* inventory_get_stack_sig = "(I)Lnet/minecraft/class_1799;";
		inline const char* inventory_set_stack_name = "method_5447";
		inline const char* inventory_set_stack_sig = "(ILnet/minecraft/class_1799;)V";
		inline const char* itemstack_get_item_name = "method_7909";
		inline const char* itemstack_get_item_sig = "()Lnet/minecraft/class_1792;";
		inline const char* itemstack_is_empty_name = "method_7960";
		inline const char* itemstack_is_empty_sig = "()Z";
		inline const char* item_class_sig = "net/minecraft/class_1792";
		inline const char* item_get_translation_key_name = "method_7876";
		inline const char* item_get_translation_key_sig = "()Ljava/lang/String;";
		inline const char* itemstack_get_name_name = "method_7964";
		inline const char* itemstack_get_name_sig = "()Lnet/minecraft/class_2561;";
		inline const char* interact_item_name = "method_2919";
		inline const char* interact_item_sig = "(Lnet/minecraft/class_1657;Lnet/minecraft/class_1268;)Lnet/minecraft/class_1269;";
		inline const char* hand_class_sig = "net/minecraft/class_1268";
		inline const char* hand_main_hand_name = "field_5808";
		inline const char* hand_main_hand_sig = "Lnet/minecraft/class_1268;";
		inline const char* entity_hit_result_class_sig = "net/minecraft/class_3966";
		inline const char* entity_hit_result_get_entity_name = "method_17782";
		inline const char* entity_hit_result_get_entity_sig = "()Lnet/minecraft/class_1297;";
		inline const char* living_entity_get_health_name = "method_6032";
		inline const char* living_entity_get_health_sig = "()F";
		inline const char* living_entity_get_max_health_name = "method_6063";
		inline const char* living_entity_get_max_health_sig = "()F";
		// Absorption (golden apple). En 1.21.x devuelve el valor crudo en
		// puntos (no corazones). Manzana dorada normal otorga 4 puntos
		// durante 2 minutos. Enchanted gapple otorga hasta 20.
		inline const char* living_entity_get_absorption_name = "method_6067";
		inline const char* living_entity_get_absorption_sig  = "()F";
		inline const char* living_entity_hurt_time_name = "field_6235";
		inline const char* living_entity_hurt_time_sig = "I";
		// LivingEntity.jump() — aplica la velocidad de salto correcta
		// (incluye boost de sprint / efectos). Lo usa el módulo Velocity
		// (jump reset legit) para saltar de forma 100% natural.
		inline const char* living_entity_jump_name = "method_6043";
		inline const char* living_entity_jump_sig = "()V";

		// ── LivingEntity.timeUntilRegen / hurtResistantTime (1.8 alias) ──
		// Field reseteado a 20 cuando el jugador recibe daño con KB. Decae
		// cada tick. Es el equivalente exacto al `hurtResistantTime` que
		// usa el velocity 1.8: cuando == 20 - delay, scale-eamos motion.
		// Yarn intermediary 1.21.x: field_6008 (best-guess; el debug
		// dynamic resolver lo confirma vía reflexión al cargar). El sig
		// es `I` — int 0..20.
		inline const char* living_entity_time_until_regen_name = "field_6008";
		inline const char* living_entity_time_until_regen_sig  = "I";

		// ── KeyBinding (class_304) ─────────────────────────────────────
		// Métodos que usa JumpReset para inyectar press de jump/sprint/
		// forward al recibir KB (estilo 1.8 GameSettings.KeyBinding).
		// setPressed(Z)V → method_23481  (best-guess, verificar vía debug)
		// isPressed()Z   → method_1434   (best-guess)
		// El field interno `pressed` (boolean) → field_3675 (best-guess)
		inline const char* key_binding_class_sig         = "net/minecraft/class_304";
		inline const char* key_binding_set_pressed_name  = "method_23481";
		inline const char* key_binding_set_pressed_sig   = "(Z)V";
		inline const char* key_binding_is_pressed_name   = "method_1434";
		inline const char* key_binding_is_pressed_sig    = "()Z";
		inline const char* key_binding_pressed_field_name = "field_3675";
		inline const char* key_binding_pressed_field_sig  = "Z";
		inline const char* key_binding_get_translation_key_name = "method_1431";
		inline const char* key_binding_get_translation_key_sig  = "()Ljava/lang/String;";

		// ── GameOptions key fields (KeyBinding-typed) ──────────────────
		// Los nombres exactos de los fields de class_315 cambian
		// frecuentemente entre versiones; estos son best-guess
		// 1.21.x. El velocity_debug los resuelve dinámicamente leyendo
		// cada KeyBinding y llamando getTranslationKey() para identificar
		// "key.jump" / "key.sprint" / "key.forward" / "key.back".
		//
		// CONFIRMADOS por inspector en log del usuario (1.21.x Yarn):
		//   jump    = field_1903  (key.jump)
		//   forward = field_1894  (key.forward)
		//   back    = field_1881  (key.back)
		//   sprint  = field_1867  (key.sprint)  ← NO field_1832 (ese es key.sneak)
		inline const char* game_options_jump_key_name      = "field_1903";
		inline const char* game_options_jump_key_sig       = "Lnet/minecraft/class_304;";
		inline const char* game_options_sprint_key_name    = "field_1867";
		inline const char* game_options_sprint_key_sig     = "Lnet/minecraft/class_304;";
		inline const char* game_options_forward_key_name   = "field_1894";
		inline const char* game_options_forward_key_sig    = "Lnet/minecraft/class_304;";
		inline const char* game_options_back_key_name      = "field_1881";
		inline const char* game_options_back_key_sig       = "Lnet/minecraft/class_304;";
		// ── Combat keys (LMB / RMB) ──────────────────────────────────
		// AutoTotem usa estos para bloquear input del usuario mientras
		// el inventario está abierto durante un swap (evita que MC
		// dispare swing/use packets que el AC asociaría con el click
		// del totem).
		inline const char* game_options_attack_key_name    = "field_1886";
		inline const char* game_options_attack_key_sig     = "Lnet/minecraft/class_304;";
		inline const char* game_options_use_key_name       = "field_1904";
		inline const char* game_options_use_key_sig        = "Lnet/minecraft/class_304;";

		// ── EntityVelocityUpdateS2CPacket (class_2743) ─────────────────
		//
		// REFACTORIZADO en 1.21.x. Antes (≤1.20) tenía 4 int fields
		// (id + velocityX/Y/Z packed como int*8000). Ahora (1.21+) tiene:
		//   field_12564 (int)        final  ← entityId
		//   field_61887 (class_243)  final  ← velocity (Vec3d)
		//
		// Confirmado via inspector reflexivo en log del usuario.
		inline const char* entity_velocity_packet_class_sig = "net/minecraft/class_2743";
		// 1.21+ format:
		inline const char* entity_velocity_packet_id_name      = "field_12564";
		inline const char* entity_velocity_packet_id_sig       = "I";
		inline const char* entity_velocity_packet_vec3d_name   = "field_61887";
		inline const char* entity_velocity_packet_vec3d_sig    = "Lnet/minecraft/class_243;";
		// Legacy 1.20- format (sigan declarados para compat con resolver
		// que busca ambos formatos):
		inline const char* entity_velocity_packet_vx_name   = "field_12553";
		inline const char* entity_velocity_packet_vx_sig    = "I";
		inline const char* entity_velocity_packet_vy_name   = "field_12552";
		inline const char* entity_velocity_packet_vy_sig    = "I";
		inline const char* entity_velocity_packet_vz_name   = "field_12551";
		inline const char* entity_velocity_packet_vz_sig    = "I";

		// ── ClientPlayNetworkHandler.onEntityVelocityUpdate(class_2743) ──
		// JNIHook target para PACKET mode + jumpreset trigger. La firma
		// es (Lclass_2743;)V. Best-guess yarn intermediary: method_11108.
		inline const char* on_entity_velocity_update_name = "method_11108";
		inline const char* on_entity_velocity_update_sig  = "(Lnet/minecraft/class_2743;)V";
		inline const char* inventory_offhand_name = "field_30639";
		inline const char* inventory_offhand_sig = "I";
		inline const char* client_world_class_sig = "net/minecraft/class_638";
		// ClientWorld.getEntities() / entitiesForRendering() → Iterable<Entity>.
		// Usado para enumerar todas las entidades (no solo players), p.ej.
		// End Crystals para el Auto Hit Crystal.
		inline const char* world_get_entities_name = "method_18112";
		inline const char* world_get_entities_sig = "()Ljava/lang/Iterable;";
		inline const char* client_world_block_entities_name = "field_60919";
		inline const char* client_world_block_entities_sig = "Ljava/util/Set;";
		inline const char* block_entity_class_sig = "net/minecraft/class_2586";
		inline const char* block_entity_get_pos_name = "method_11016";
		inline const char* block_entity_get_pos_sig = "()Lnet/minecraft/class_2338;";
		inline const char* block_pos_class_sig = "net/minecraft/class_2338";
		inline const char* block_pos_get_x_name = "method_10263";
		inline const char* block_pos_get_x_sig = "()I";
		inline const char* block_pos_get_y_name = "method_10264";
		inline const char* block_pos_get_y_sig = "()I";
		inline const char* block_pos_get_z_name = "method_10260";
		inline const char* block_pos_get_z_sig = "()I";
		inline const char* world_class_sig = "net/minecraft/class_1937";
		inline const char* world_get_block_state_name = "method_66016";
		inline const char* world_get_block_state_sig = "(Lnet/minecraft/class_2338;Lnet/minecraft/class_2680;Lnet/minecraft/class_2680;)V";
		inline const char* block_state_class_sig = "net/minecraft/class_4970$class_4971";
		inline const char* block_state_get_block_name = "method_26204";
		inline const char* block_state_get_block_sig = "()Lnet/minecraft/class_2248;";
		inline const char* chest_block_entity_class_sig = "net/minecraft/class_2595";
		inline const char* ender_chest_block_entity_class_sig = "net/minecraft/class_2611";
		inline const char* shulker_box_block_entity_class_sig = "net/minecraft/class_2627";
		inline const char* block_hit_result_class_sig = "net/minecraft/class_3965";
		inline const char* block_hit_result_get_block_pos_name = "method_17777";
		inline const char* block_hit_result_get_block_pos_sig = "()Lnet/minecraft/class_2338;";
		inline const char* block_hit_result_get_type_name = "method_17783";
		inline const char* block_hit_result_get_type_sig = "()Lnet/minecraft/class_239$class_240;";
		inline const char* obsidian_block_class_sig = "net/minecraft/class_663$class_11926";
		inline const char* network_handler_class_sig = "net/minecraft/class_634";
		inline const char* update_selected_slot_c2s_packet_class_sig = "net/minecraft/class_2868";
		inline const char* player_action_c2s_packet_class_sig = "net/minecraft/class_2846";
		inline const char* player_action_c2s_packet_action_class_sig = "net/minecraft/class_2846$class_2847";
		inline const char* swap_item_with_offhand_action_name = "field_12969";
		inline const char* swap_item_with_offhand_action_sig = "Lnet/minecraft/class_2846$class_2847;";
		inline const char* pick_from_inventory_c2s_packet_class_sig = "net/minecraft/class_2868";
		inline const char* block_pos_origin_name = "field_10980";
		inline const char* block_pos_origin_sig = "Lnet/minecraft/class_2338;";
		inline const char* direction_class_sig = "net/minecraft/class_2350";
		inline const char* direction_down_name = "field_11033";
		inline const char* direction_down_sig = "Lnet/minecraft/class_2350;";
		inline const char* channel_inbound_handler_adapter_class_sig = "io/netty/channel/ChannelInboundHandlerAdapter";
		inline const char* channel_read0_name = "channelRead0";
		inline const char* channel_read0_sig = "(Lio/netty/channel/ChannelHandlerContext;Ljava/lang/Object;)V";
		inline const char* entity_s2c_packet_class_sig = "net/minecraft/class_2684";
		inline const char* entity_position_s2c_packet_class_sig = "net/minecraft/class_2777";
		inline const char* entity_move_s2c_packet_class_sig = "net/minecraft/class_2777";
		inline const char* entity_teleport_s2c_packet_class_sig = "net/minecraft/class_2777";
		inline const char* minecraft_current_screen_name = "field_1755";
		inline const char* minecraft_current_screen_sig = "Lnet/minecraft/class_437;";
		inline const char* minecraft_set_screen_name = "method_1507";
		inline const char* minecraft_set_screen_sig = "(Lnet/minecraft/class_437;)V";
		inline const char* player_network_handler_name = "field_3944";
		inline const char* player_network_handler_sig = "Lnet/minecraft/class_634;";
		inline const char* send_packet_name = "method_2883";
		inline const char* send_packet_sig = "(Lnet/minecraft/class_2596;)V";
		inline const char* client_command_c2s_class_sig = "net/minecraft/class_2848";
		inline const char* client_command_action_enum_sig = "net/minecraft/class_2848$class_2849";
		inline const char* player_input_c2s_class_sig = "net/minecraft/class_2851";

		// ── NoJumpDelay — campo del cooldown de salto ──────────────
		// `LivingEntity.jumpingCooldown` — int, 0..10. Vanilla lo setea
		// a 10 después de saltar y decrementa cada tick. Mientras sea
		// > 0, jump() no hace nada. Setarlo a 0 cada tick = sin delay.
		inline const char* living_entity_jumping_cooldown_name = "field_6228";
		inline const char* living_entity_jumping_cooldown_sig  = "I";

		// Player swing animation — clickaim las lee para saber si un attack
		// ya está swingando (evitar doble hit / doble swing en el mismo tick).
		// field_6251 era handSwingProgress -- un float, no un boolean.
		// GetFieldID casa por nombre Y descriptor, y pedirlo con "Z" fallaba
		// siempre: el Target HUD no podia detectar un solo golpe. Verificado
		// contra tools/genmaps/.cache/*/yarn_merged.tiny -- field_6252/Z en las
		// doce versiones 1.21.x cacheadas.
		inline const char* hand_swinging_name    = "field_6252";
		inline const char* hand_swinging_sig     = "Z";
		inline const char* hand_swing_ticks_name = "field_6279";
		inline const char* hand_swing_ticks_sig  = "I";

		inline const char* input_class_sig = "net/minecraft/class_744";
		inline const char* input_movement_sideways_name = "field_3905";
		inline const char* input_movement_forward_name = "field_3907";
		inline const char* input_pressing_forward_name = "field_3910";
		inline const char* input_pressing_back_name = "field_3909";
		inline const char* input_pressing_left_name = "field_3908";
		inline const char* input_pressing_right_name = "field_3906";
		inline const char* inventory_screen_class_sig = "net/minecraft/class_490";
		inline const char* mob_entity_class_sig = "net/minecraft/class_1308";
		inline const char* animal_entity_class_sig = "net/minecraft/class_1429";
		inline const char* villager_entity_class_sig = "net/minecraft/class_1646";
		inline const char* end_crystal_entity_class_sig = "net/minecraft/class_1511";
		inline const char* armor_stand_entity_class_sig = "net/minecraft/class_1531";
		inline const char* handled_screen_class_sig = "net/minecraft/class_465";
		inline const char* handled_screen_focused_slot_name = "field_2787";
		inline const char* handled_screen_focused_slot_sig = "Lnet/minecraft/class_1735;";
		inline const char* handled_screen_handler_name = "field_2797";
		inline const char* handled_screen_handler_sig = "Lnet/minecraft/class_1703;";
		inline const char* slot_class_sig = "net/minecraft/class_1735";
		inline const char* slot_id_name = "field_7874";
		inline const char* slot_id_sig = "I";
		inline const char* slot_has_stack_name = "method_7681";
		inline const char* slot_has_stack_sig = "()Z";
		inline const char* slot_get_stack_name = "method_7677";
		inline const char* slot_get_stack_sig = "()Lnet/minecraft/class_1799;";
		inline const char* screen_handler_class_sig = "net/minecraft/class_1703";
		inline const char* screen_handler_sync_id_name = "field_7763";
		inline const char* screen_handler_sync_id_sig = "I";
		inline const char* screen_handler_get_slot_name = "method_7611";
		inline const char* screen_handler_get_slot_sig = "(I)Lnet/minecraft/class_1735;";
		inline const char* click_slot_name = "method_2906";
		inline const char* click_slot_sig = "(IIILnet/minecraft/class_1713;Lnet/minecraft/class_1657;)V";
		inline const char* slot_action_type_class_sig = "net/minecraft/class_1713";
		inline const char* slot_action_quick_move_name = "field_7794";
		inline const char* slot_action_quick_move_sig = "Lnet/minecraft/class_1713;";
		inline const char* entity_get_name_method_name = "method_5477";
		inline const char* entity_get_name_method_sig = "()Lnet/minecraft/class_2561;";
		inline const char* text_class_sig = "net/minecraft/class_2561";
		inline const char* text_get_string_name = "method_10851";
		inline const char* text_get_string_sig = "()Ljava/lang/String;";
		inline const char* list_class_sig = "java/util/List";
		inline const char* list_size_name = "size";
		inline const char* list_size_sig = "()I";
		inline const char* list_get_name = "get";
		inline const char* list_get_sig = "(I)Ljava/lang/Object;";
		inline const char* mc_get_window_name = "method_22683";
		inline const char* mc_get_window_sig = "()Lnet/minecraft/class_1041;";
		inline const char* window_class_sig = "net/minecraft/class_1041";
		inline const char* window_get_fb_width_name = "method_4489";
		inline const char* window_get_fb_width_sig = "()I";
		inline const char* window_get_fb_height_name = "method_4506";
		inline const char* window_get_fb_height_sig = "()I";
		inline const char* window_get_scale_factor_name = "method_4495";
		inline const char* window_get_scale_factor_sig = "()D";
		inline const char* sync_selected_slot_name = "method_2911";
		inline const char* sync_selected_slot_sig = "()V";
		inline const char* data_component_types_class_sig = "net/minecraft/class_9334";
		inline const char* potion_contents_type_name = "field_49274";
		inline const char* potion_contents_type_sig = "Lnet/minecraft/class_9331;";
		inline const char* data_component_type_class_sig = "net/minecraft/class_9331";
		inline const char* itemstack_get_component_name = "method_57824";
		inline const char* itemstack_get_component_sig = "(Lnet/minecraft/class_9331;)Ljava/lang/Object;";
		inline const char* gameprofile_class_sig = "com/mojang/authlib/GameProfile";
		inline const char* player_get_gameprofile_name = "method_7334";
		inline const char* player_get_gameprofile_sig = "()Lcom/mojang/authlib/GameProfile;";
		inline const char* gameprofile_get_name_name = "getName";
		inline const char* gameprofile_get_name_sig = "()Ljava/lang/String;";

		// ── Watermark — PlayerListEntry para leer el ping ─────────────
		// AbstractClientPlayerEntity (class_742).getPlayerListEntry() →
		// PlayerListEntry (class_640). Si null, no hay tablist (offline).
		// PlayerListEntry.getLatency() → int (ms). El nombre yarn varía
		// entre versiones; el watermark intenta varios candidates.
		inline const char* abstract_client_player_class_sig = "net/minecraft/class_742";
		inline const char* abstract_client_get_player_list_entry_name = "method_3123";
		inline const char* abstract_client_get_player_list_entry_sig = "()Lnet/minecraft/class_640;";
		inline const char* player_list_entry_class_sig = "net/minecraft/class_640";
		inline const char* player_list_entry_get_latency_name = "method_2959";
		inline const char* player_list_entry_get_latency_sig = "()I";
		inline const char* game_options_name = "field_1690";
		inline const char* game_options_sig = "Lnet/minecraft/class_315;";
		inline const char* game_options_class_sig = "net/minecraft/class_315";
		inline const char* game_options_gamma_name = "field_1840";
		inline const char* game_options_gamma_sig = "Lnet/minecraft/class_7172;";
		inline const char* simple_option_class_sig = "net/minecraft/class_7172";
		inline const char* simple_option_set_value_name = "method_41753";
		inline const char* simple_option_set_value_sig = "(Ljava/lang/Object;)V";
		inline const char* simple_option_get_value_name = "method_41754";
		inline const char* simple_option_get_value_sig = "()Ljava/lang/Object;";
		inline const char* status_effect_instance_class_sig = "net/minecraft/class_1293";
		inline const char* status_effect_instance_init_sig = "(Lnet/minecraft/class_6880;IIZZ)V";
		inline const char* status_effects_class_sig = "net/minecraft/class_1294";
		inline const char* night_vision_effect_name = "field_5925";
		inline const char* night_vision_effect_sig = "Lnet/minecraft/class_6880;";
		inline const char* add_status_effect_name = "method_6092";
		inline const char* add_status_effect_sig = "(Lnet/minecraft/class_1293;)Z";
		inline const char* remove_status_effect_name = "method_6016";
		inline const char* remove_status_effect_sig = "(Lnet/minecraft/class_6880;)Z";

		// Newly added variables for dynamic mapping:
		inline const char* itemstack_class_sig = "net/minecraft/class_1799";
		inline const char* slot_action_pickup_name = "field_7790";
		inline const char* slot_action_pickup_sig = "Lnet/minecraft/class_1713;";
		inline const char* slot_action_swap_name = "field_7791";
		inline const char* slot_action_swap_sig = "Lnet/minecraft/class_1713;";
		inline const char* living_entity_get_main_hand_name = "method_6047";
		inline const char* living_entity_get_main_hand_sig = "()Lnet/minecraft/class_1799;";

		// ── Render textures de items reales del juego ─────────────────────────
		// MinecraftClient.getTextureManager -> TextureManager (class_1060)
		inline const char* mc_get_texture_manager_name = "method_1531";
		inline const char* mc_get_texture_manager_sig = "()Lnet/minecraft/class_1060;";
		inline const char* texture_manager_class_sig = "net/minecraft/class_1060";
		// TextureManager.getTexture(Identifier) -> AbstractTexture (class_1044)
		inline const char* texture_manager_get_texture_name = "method_4619";
		inline const char* texture_manager_get_texture_sig = "(Lnet/minecraft/class_2960;)Lnet/minecraft/class_1044;";
		// AbstractTexture.getGlId() -> int   (id de la textura GL ya cargada)
		inline const char* abstract_texture_class_sig = "net/minecraft/class_1044";
		inline const char* abstract_texture_get_glid_name = "method_4624";
		inline const char* abstract_texture_get_glid_sig = "()I";
		// Identifier (class_2960) constructor
		inline const char* identifier_class_full_sig = "net/minecraft/class_2960";
		inline const char* identifier_init_sig = "(Ljava/lang/String;Ljava/lang/String;)V";
		// Identifier.of(String, String) static factory (1.21+)
		inline const char* identifier_of_name = "method_60655";
		inline const char* identifier_of_sig = "(Ljava/lang/String;Ljava/lang/String;)Lnet/minecraft/class_2960;";

		// Para mostrar info de items en ESP nametag
		// LivingEntity.getOffHandStack -> ItemStack
		inline const char* living_entity_get_off_hand_name = "method_6079";
		inline const char* living_entity_get_off_hand_sig = "()Lnet/minecraft/class_1799;";
		// LivingEntity.getArmorItems -> Iterable<ItemStack>  (returns List in fact)
		inline const char* living_entity_get_armor_items_name = "method_5661";
		inline const char* living_entity_get_armor_items_sig = "()Ljava/lang/Iterable;";
		// ItemStack.getCount -> int
		inline const char* itemstack_get_count_name = "method_7947";
		inline const char* itemstack_get_count_sig = "()I";
		// ItemStack.getDamage -> int (damage taken so far)
		inline const char* itemstack_get_damage_name = "method_7919";
		inline const char* itemstack_get_damage_sig = "()I";
		// ItemStack.getMaxDamage -> int
		inline const char* itemstack_get_max_damage_name = "method_7936";
		inline const char* itemstack_get_max_damage_sig = "()I";
		// ItemStack.isDamageable -> boolean
		inline const char* itemstack_is_damageable_name = "method_7963";
		inline const char* itemstack_is_damageable_sig = "()Z";
		// ItemStack.getEnchantments -> ItemEnchantmentsComponent (1.20.5+) o NbtList (older)
		// 1.21.x usa el nuevo sistema de componentes (DataComponentType)
		inline const char* itemstack_get_enchantments_name = "method_58657";
		inline const char* itemstack_get_enchantments_sig = "()Lnet/minecraft/class_9304;";
		inline const char* item_enchantments_component_class_sig = "net/minecraft/class_9304";
		// ItemEnchantmentsComponent.getEnchantments -> Set<RegistryEntry<Enchantment>>
		inline const char* enchants_get_enchantments_name = "method_57534";
		inline const char* enchants_get_enchantments_sig = "()Ljava/util/Set;";
		// ItemEnchantmentsComponent.getLevel(RegistryEntry<Enchantment>) -> int
		inline const char* enchants_get_level_name = "method_57536";
		inline const char* enchants_get_level_sig = "(Lnet/minecraft/class_6880;)I";
		// RegistryEntry.getIdAsString or getKey for fetching enchant name (uses Optional<RegistryKey>)
		// Más simple: usar RegistryEntry.value().getTranslationKey() pero requiere Enchantment class
		// Usaremos toString() del entry y parseamos.

		inline const char* inventory_screen_init_sig = "(Lnet/minecraft/class_1657;)V";
		inline const char* client_command_init_sig = "(Lnet/minecraft/class_1297;Lnet/minecraft/class_2848$class_2849;)V";
		inline const char* shared_constants_class_sig = "net/minecraft/class_3797";
		inline const char* shared_constants_version_field_name = "field_25319";
		inline const char* shared_constants_version_field_sig = "Lnet/minecraft/class_6489;";
		inline const char* box_class_sig = "net/minecraft/class_238";
		inline const char* camera_pos_field_name = "field_18712";
		inline const char* camera_pos_field_sig = "Lnet/minecraft/class_243;";

		// Modern input mapping variables
		inline const char* player_input_record_class_sig = "net/minecraft/class_10185";
		inline const char* vec2_class_sig = "net/minecraft/class_241";
		inline const char* input_player_input_field_name = "field_54155";
		inline const char* input_player_input_field_sig = "Lnet/minecraft/class_10185;";
		inline const char* input_movement_vec2_field_name = "field_55868";
		inline const char* input_movement_vec2_field_sig = "Lnet/minecraft/class_241;";

		// MinecraftClient.getCurrentServerEntry() — null when in singleplayer.
		// 1.21.x Yarn: ServerInfo class is class_642, address field is field_3761.
		inline const char* mc_get_current_server_entry_name = "method_1558";
		inline const char* mc_get_current_server_entry_sig  = "()Lnet/minecraft/class_642;";
		inline const char* server_info_class_sig            = "net/minecraft/class_642";
		inline const char* server_info_address_field_name   = "field_3761";
		inline const char* server_info_address_field_sig    = "Ljava/lang/String;";

		// ── Apply Mojang (deobfuscated) mappings for 1.21.x ─────────────────
		// Lunar Client and any other launcher that runs a Mojang-mapped jar
		// (parchment / official mojmap) sees readable class & field names
		// instead of yarn intermediary. Most field names are still single
		// letters in mojmap because Mojang only published class/method names
		// in 1.14.4+ — the field names are recovered via parchment or
		// deobfuscated by each launcher independently. Lunar uses the
		// Mojang-published method names + their own "Genesis" field names
		// which match parchment. Where a name is uncertain we leave probing
		// to the runtime probe section below or to per-module re-probe paths.
		inline void apply_mojang_mappings_1_21()
		{
			version = "1.21.x (mojang)";

			// MinecraftClient → Minecraft
			minecraftclass_sig                     = "net/minecraft/client/Minecraft";
			minecraftclient_name                   = "instance";
			minecraftclient_sig                    = "Lnet/minecraft/client/Minecraft;";
			player_name                            = "player";
			player_sig                             = "Lnet/minecraft/client/player/LocalPlayer;";
			world_name                             = "level";
			world_sig                              = "Lnet/minecraft/client/multiplayer/ClientLevel;";
			crosshair_target_name                  = "hitResult";
			crosshair_target_sig                   = "Lnet/minecraft/world/phys/HitResult;";
			interaction_manager_name               = "gameMode";
			interaction_manager_sig                = "Lnet/minecraft/client/multiplayer/MultiPlayerGameMode;";
			interaction_manager_class_sig          = "net/minecraft/client/multiplayer/MultiPlayerGameMode";
			attack_entity_name                     = "attack";
			attack_entity_sig                      = "(Lnet/minecraft/world/entity/player/Player;Lnet/minecraft/world/entity/Entity;)V";
			network_handler_name                   = "getConnection";
			network_handler_sig                    = "()Lnet/minecraft/client/multiplayer/ClientPacketListener;";
			connection_name                        = "connection";
			connection_sig                         = "Lnet/minecraft/network/Connection;";
			do_attack_name                         = "startAttack";
			do_attack_sig                          = "()Z";
			handle_input_events_name               = "handleKeybinds";
			handle_input_events_sig                = "()V";
			attack_cooldown_name                   = "missTime";
			attack_cooldown_sig                    = "I";
			item_use_cooldown_name                 = "rightClickDelay";
			item_use_cooldown_sig                  = "I";
			gamerenderer_class_sig                 = "net/minecraft/client/renderer/GameRenderer";
			gamerenderer_name                      = "gameRenderer";
			gamerenderer_sig                       = "Lnet/minecraft/client/renderer/GameRenderer;";
			update_crosshair_target_name           = "pick";
			update_crosshair_target_sig            = "(F)V";
			get_fov_name                           = "getFov";
			get_fov_sig                            = "(Lnet/minecraft/client/Camera;FZ)F";
			get_camera_name                        = "getMainCamera";
			get_camera_sig                         = "()Lnet/minecraft/client/Camera;";
			camera_class_sig                       = "net/minecraft/client/Camera";
			camera_get_pos_name                    = "getPosition";
			camera_get_pos_sig                     = "()Lnet/minecraft/world/phys/Vec3;";
			camera_get_yaw_name                    = "getYRot";
			camera_get_yaw_sig                     = "()F";
			camera_get_pitch_name                  = "getXRot";
			camera_get_pitch_sig                   = "()F";
			vec3d_class_sig                        = "net/minecraft/world/phys/Vec3";
			vec3d_x_name                           = "x";
			vec3d_x_sig                            = "D";
			vec3d_y_name                           = "y";
			vec3d_y_sig                            = "D";
			vec3d_z_name                           = "z";
			vec3d_z_sig                            = "D";
			clientplayerentity_class_sig           = "net/minecraft/client/player/LocalPlayer";
			send_movement_packets_name             = "sendPosition";
			send_movement_packets_sig              = "()V";
			tick_movement_name                     = "aiStep";
			tick_movement_sig                      = "()V";
			render_yaw_name                        = "yRot";
			render_yaw_sig                         = "F";
			render_pitch_name                      = "xRot";
			render_pitch_sig                       = "F";
			playermovec2spacket_class_sig          = "net/minecraft/network/protocol/game/ServerboundMovePlayerPacket";
			playermovec2spacket_yaw_name           = "yRot";
			playermovec2spacket_yaw_sig            = "F";
			playermovec2spacket_pitch_name         = "xRot";
			playermovec2spacket_pitch_sig          = "F";
			abilities_name                         = "abilities";
			abilities_sig                          = "Lnet/minecraft/world/entity/player/Abilities;";
			get_entity_interaction_range_name      = "entityInteractionRange";
			get_entity_interaction_range_sig       = "()D";
			get_attack_cooldown_progress_name      = "getAttackStrengthScale";
			get_attack_cooldown_progress_sig       = "(F)F";
			reset_last_attacked_ticks_name         = "resetAttackStrengthTicker";
			reset_last_attacked_ticks_sig          = "()V";
			player_entity_class_sig                = "net/minecraft/world/entity/player/Player";
			fly_name                               = "flying";
			fly_sig                                = "Z";
			entity_get_x_name                      = "getX";
			entity_get_x_sig                       = "()D";
			entity_get_y_name                      = "getY";
			entity_get_y_sig                       = "()D";
			entity_get_z_name                      = "getZ";
			entity_get_z_sig                       = "()D";
			entity_get_yaw_name                    = "getYRot";
			entity_get_yaw_sig                     = "()F";
			entity_get_pitch_name                  = "getViewXRot";
			entity_get_pitch_sig                   = "(F)F";
			entity_set_yaw_name                    = "setYRot";
			entity_set_yaw_sig                     = "(F)V";
			entity_set_pitch_name                  = "setXRot";
			entity_set_pitch_sig                   = "(F)V";
			get_bounding_box_name                  = "bb";                                  // TODO mojang verify (parchment: "bb")
			get_bounding_box_sig                   = "Lnet/minecraft/world/phys/AABB;";
			set_bounding_box_name                  = "setBoundingBox";
			set_bounding_box_sig                   = "(Lnet/minecraft/world/phys/AABB;)V";
			entity_class_sig                       = "net/minecraft/world/entity/Entity";
			entity_set_flag_name                   = "setSharedFlag";
			entity_set_flag_sig                    = "(IZ)V";
			entity_get_id_name                     = "getId";
			entity_get_id_sig                      = "()I";
			box_class_sig                          = "net/minecraft/world/phys/AABB";
			box_min_x_name                         = "minX";
			box_min_x_sig                          = "D";
			box_max_x_name                         = "maxX";
			box_max_x_sig                          = "D";
			box_min_y_name                         = "minY";
			box_min_y_sig                          = "D";
			box_max_y_name                         = "maxY";
			box_max_y_sig                          = "D";
			box_min_z_name                         = "minZ";
			box_min_z_sig                          = "D";
			box_max_z_name                         = "maxZ";
			box_max_z_sig                          = "D";
			players_field_name                     = "players";
			players_field_sig                      = "Ljava/util/List;";
			living_entity_class_sig                = "net/minecraft/world/entity/LivingEntity";
			living_entity_is_blocking_name         = "isBlocking";
			living_entity_is_blocking_sig          = "()Z";
			living_entity_is_using_item_name       = "isUsingItem";
			living_entity_is_using_item_sig        = "()Z";
			swing_hand_name                        = "swing";
			swing_hand_sig                         = "(Lnet/minecraft/world/InteractionHand;)V";
			entity_is_sneaking_name                = "isShiftKeyDown";
			entity_is_sneaking_sig                 = "()Z";
			entity_is_touching_water_name          = "isInWater";
			entity_is_touching_water_sig           = "()Z";
			set_sprinting_name                     = "setSprinting";
			set_sprinting_sig                      = "(Z)V";
			is_sprinting_name                      = "isSprinting";
			is_sprinting_sig                       = "()Z";
			is_on_ground_name                      = "onGround";
			is_on_ground_sig                       = "()Z";
			entity_set_velocity_name               = "setDeltaMovement";
			entity_set_velocity_sig                = "(DDD)V";
			entity_velocity_name                   = "deltaMovement";
			entity_velocity_sig                    = "Lnet/minecraft/world/phys/Vec3;";
			entity_handle_fall_damage_name         = "causeFallDamage";
			entity_handle_fall_damage_sig          = "(DFLnet/minecraft/world/damagesource/DamageSource;)Z";
			entity_fall_distance_name              = "fallDistance";
			entity_fall_distance_sig               = "D"; // 1.21.5+ is double; runtime probe will switch to F if needed
			player_attack_name                     = "attack";
			player_attack_sig                      = "(Lnet/minecraft/world/entity/Entity;)V";
			player_attack_living_entity_name       = "doHurtTarget";
			player_attack_living_entity_sig        = "(Lnet/minecraft/world/entity/LivingEntity;)V";
			player_inventory_name                  = "inventory";
			player_inventory_sig                   = "Lnet/minecraft/world/entity/player/Inventory;";
			player_set_sprinting_name              = "setSprinting";
			player_set_sprinting_sig               = "(Z)V";
			player_input_name                      = "input";
			player_input_sig                       = "Lnet/minecraft/client/player/Input;";
			player_inventory_class_sig             = "net/minecraft/world/entity/player/Inventory";
			inventory_selected_slot_name           = "selected";
			inventory_selected_slot_sig            = "I";
			inventory_get_stack_name               = "getItem";
			inventory_get_stack_sig                = "(I)Lnet/minecraft/world/item/ItemStack;";
			inventory_set_stack_name               = "setItem";
			inventory_set_stack_sig                = "(ILnet/minecraft/world/item/ItemStack;)V";
			itemstack_class_sig                    = "net/minecraft/world/item/ItemStack";
			itemstack_get_item_name                = "getItem";
			itemstack_get_item_sig                 = "()Lnet/minecraft/world/item/Item;";
			itemstack_is_empty_name                = "isEmpty";
			itemstack_is_empty_sig                 = "()Z";
			item_class_sig                         = "net/minecraft/world/item/Item";
			item_get_translation_key_name          = "getDescriptionId";
			item_get_translation_key_sig           = "()Ljava/lang/String;";
			itemstack_get_name_name                = "getHoverName";
			itemstack_get_name_sig                 = "()Lnet/minecraft/network/chat/Component;";
			interact_item_name                     = "useItemOn";
			interact_item_sig                      = "(Lnet/minecraft/world/entity/player/Player;Lnet/minecraft/world/InteractionHand;)Lnet/minecraft/world/InteractionResult;";
			hand_class_sig                         = "net/minecraft/world/InteractionHand";
			hand_main_hand_name                    = "MAIN_HAND";
			hand_main_hand_sig                     = "Lnet/minecraft/world/InteractionHand;";
			entity_hit_result_class_sig            = "net/minecraft/world/phys/EntityHitResult";
			entity_hit_result_get_entity_name      = "getEntity";
			entity_hit_result_get_entity_sig       = "()Lnet/minecraft/world/entity/Entity;";
			living_entity_get_health_name          = "getHealth";
			living_entity_get_health_sig           = "()F";
			living_entity_get_max_health_name      = "getMaxHealth";
			living_entity_get_max_health_sig       = "()F";
			living_entity_get_absorption_name      = "getAbsorptionAmount";
			living_entity_get_absorption_sig       = "()F";
			living_entity_hurt_time_name           = "hurtTime";
			living_entity_hurt_time_sig            = "I";
			living_entity_jump_name                = "jumpFromGround";
			living_entity_jump_sig                 = "()V";
			inventory_offhand_name                 = "SLOT_OFFHAND";
			inventory_offhand_sig                  = "I";
			client_world_class_sig                 = "net/minecraft/client/multiplayer/ClientLevel";
			world_get_entities_name                = "entitiesForRendering";
			world_get_entities_sig                 = "()Ljava/lang/Iterable;";
			client_world_block_entities_name       = "blockEntityTickers"; // TODO mojang verify
			client_world_block_entities_sig        = "Ljava/util/Set;";
			block_entity_class_sig                 = "net/minecraft/world/level/block/entity/BlockEntity";
			block_entity_get_pos_name              = "getBlockPos";
			block_entity_get_pos_sig               = "()Lnet/minecraft/core/BlockPos;";
			block_pos_class_sig                    = "net/minecraft/core/BlockPos";
			block_pos_get_x_name                   = "getX";
			block_pos_get_x_sig                    = "()I";
			block_pos_get_y_name                   = "getY";
			block_pos_get_y_sig                    = "()I";
			block_pos_get_z_name                   = "getZ";
			block_pos_get_z_sig                    = "()I";
			world_class_sig                        = "net/minecraft/world/level/Level";
			world_get_block_state_name             = "setBlock";
			world_get_block_state_sig              = "(Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/block/state/BlockState;)V";
			block_state_class_sig                  = "net/minecraft/world/level/block/state/BlockBehaviour$BlockStateBase";
			block_state_get_block_name             = "getBlock";
			block_state_get_block_sig              = "()Lnet/minecraft/world/level/block/Block;";
			chest_block_entity_class_sig           = "net/minecraft/world/level/block/entity/ChestBlockEntity";
			ender_chest_block_entity_class_sig     = "net/minecraft/world/level/block/entity/EnderChestBlockEntity";
			shulker_box_block_entity_class_sig     = "net/minecraft/world/level/block/entity/ShulkerBoxBlockEntity";
			block_hit_result_class_sig             = "net/minecraft/world/phys/BlockHitResult";
			block_hit_result_get_block_pos_name    = "getBlockPos";
			block_hit_result_get_block_pos_sig     = "()Lnet/minecraft/core/BlockPos;";
			block_hit_result_get_type_name         = "getType";
			block_hit_result_get_type_sig          = "()Lnet/minecraft/world/phys/HitResult$Type;";
			obsidian_block_class_sig               = "net/minecraft/world/level/block/ObsidianBlock";
			network_handler_class_sig              = "net/minecraft/client/multiplayer/ClientPacketListener";
			update_selected_slot_c2s_packet_class_sig = "net/minecraft/network/protocol/game/ServerboundSetCarriedItemPacket";
			player_action_c2s_packet_class_sig     = "net/minecraft/network/protocol/game/ServerboundPlayerActionPacket";
			player_action_c2s_packet_action_class_sig = "net/minecraft/network/protocol/game/ServerboundPlayerActionPacket$Action";
			swap_item_with_offhand_action_name     = "SWAP_ITEM_WITH_OFFHAND";
			swap_item_with_offhand_action_sig      = "Lnet/minecraft/network/protocol/game/ServerboundPlayerActionPacket$Action;";
			pick_from_inventory_c2s_packet_class_sig = "net/minecraft/network/protocol/game/ServerboundSetCarriedItemPacket";
			block_pos_origin_name                  = "ZERO";
			block_pos_origin_sig                   = "Lnet/minecraft/core/BlockPos;";
			direction_class_sig                    = "net/minecraft/core/Direction";
			direction_down_name                    = "DOWN";
			direction_down_sig                     = "Lnet/minecraft/core/Direction;";
			channel_inbound_handler_adapter_class_sig = "io/netty/channel/ChannelInboundHandlerAdapter";
			channel_read0_name                     = "channelRead0";
			channel_read0_sig                      = "(Lio/netty/channel/ChannelHandlerContext;Ljava/lang/Object;)V";
			entity_s2c_packet_class_sig            = "net/minecraft/network/protocol/game/ClientboundMoveEntityPacket";
			entity_position_s2c_packet_class_sig   = "net/minecraft/network/protocol/game/ClientboundTeleportEntityPacket";
			entity_move_s2c_packet_class_sig       = "net/minecraft/network/protocol/game/ClientboundMoveEntityPacket";
			entity_teleport_s2c_packet_class_sig   = "net/minecraft/network/protocol/game/ClientboundTeleportEntityPacket";
			minecraft_current_screen_name          = "screen";
			minecraft_current_screen_sig           = "Lnet/minecraft/client/gui/screens/Screen;";
			minecraft_set_screen_name              = "setScreen";
			minecraft_set_screen_sig               = "(Lnet/minecraft/client/gui/screens/Screen;)V";
			player_network_handler_name            = "connection";
			player_network_handler_sig             = "Lnet/minecraft/client/multiplayer/ClientPacketListener;";
			send_packet_name                       = "send";
			send_packet_sig                        = "(Lnet/minecraft/network/protocol/Packet;)V";
			client_command_c2s_class_sig           = "net/minecraft/network/protocol/game/ServerboundPlayerCommandPacket";
			client_command_action_enum_sig         = "net/minecraft/network/protocol/game/ServerboundPlayerCommandPacket$Action";
			player_input_c2s_class_sig             = "net/minecraft/network/protocol/game/ServerboundPlayerInputPacket";
			input_class_sig                        = "net/minecraft/client/player/Input";
			input_movement_sideways_name           = "leftImpulse";
			input_movement_forward_name            = "forwardImpulse";
			input_pressing_forward_name            = "up";
			input_pressing_back_name               = "down";
			input_pressing_left_name               = "left";
			input_pressing_right_name              = "right";
			inventory_screen_class_sig             = "net/minecraft/client/gui/screens/inventory/InventoryScreen";
			mob_entity_class_sig                   = "net/minecraft/world/entity/Mob";
			animal_entity_class_sig                = "net/minecraft/world/entity/animal/Animal";
			villager_entity_class_sig              = "net/minecraft/world/entity/npc/Villager";
			end_crystal_entity_class_sig           = "net/minecraft/world/entity/boss/enderdragon/EndCrystal";
			armor_stand_entity_class_sig           = "net/minecraft/world/entity/decoration/ArmorStand";
			handled_screen_class_sig               = "net/minecraft/client/gui/screens/inventory/AbstractContainerScreen";
			handled_screen_focused_slot_name       = "hoveredSlot";
			handled_screen_focused_slot_sig        = "Lnet/minecraft/world/inventory/Slot;";
			handled_screen_handler_name            = "menu";
			handled_screen_handler_sig             = "Lnet/minecraft/world/inventory/AbstractContainerMenu;";
			slot_class_sig                         = "net/minecraft/world/inventory/Slot";
			slot_id_name                           = "index";
			slot_id_sig                            = "I";
			slot_has_stack_name                    = "hasItem";
			slot_has_stack_sig                     = "()Z";
			slot_get_stack_name                    = "getItem";
			slot_get_stack_sig                     = "()Lnet/minecraft/world/item/ItemStack;";
			screen_handler_class_sig               = "net/minecraft/world/inventory/AbstractContainerMenu";
			screen_handler_sync_id_name            = "containerId";
			screen_handler_sync_id_sig             = "I";
			screen_handler_get_slot_name           = "getSlot";
			screen_handler_get_slot_sig            = "(I)Lnet/minecraft/world/inventory/Slot;";
			click_slot_name                        = "handleInventoryMouseClick";
			click_slot_sig                         = "(IIILnet/minecraft/world/inventory/ClickType;Lnet/minecraft/world/entity/player/Player;)V";
			slot_action_type_class_sig             = "net/minecraft/world/inventory/ClickType";
			slot_action_quick_move_name            = "QUICK_MOVE";
			slot_action_quick_move_sig             = "Lnet/minecraft/world/inventory/ClickType;";
			slot_action_pickup_name                = "PICKUP";
			slot_action_pickup_sig                 = "Lnet/minecraft/world/inventory/ClickType;";
			slot_action_swap_name                  = "SWAP";
			slot_action_swap_sig                   = "Lnet/minecraft/world/inventory/ClickType;";
			entity_get_name_method_name            = "getName";
			entity_get_name_method_sig             = "()Lnet/minecraft/network/chat/Component;";
			text_class_sig                         = "net/minecraft/network/chat/Component";
			text_get_string_name                   = "getString";
			text_get_string_sig                    = "()Ljava/lang/String;";
			list_class_sig                         = "java/util/List";
			list_size_name                         = "size";
			list_size_sig                          = "()I";
			list_get_name                          = "get";
			list_get_sig                           = "(I)Ljava/lang/Object;";
			mc_get_window_name                     = "getWindow";
			mc_get_window_sig                      = "()Lcom/mojang/blaze3d/platform/Window;";
			window_class_sig                       = "com/mojang/blaze3d/platform/Window";
			window_get_fb_width_name               = "getWidth";
			window_get_fb_width_sig                = "()I";
			window_get_fb_height_name              = "getHeight";
			window_get_fb_height_sig               = "()I";
			window_get_scale_factor_name           = "getGuiScale";
			window_get_scale_factor_sig            = "()D";
			sync_selected_slot_name                = "broadcastCarriedItem"; // TODO mojang verify
			sync_selected_slot_sig                 = "()V";
			data_component_types_class_sig         = "net/minecraft/core/component/DataComponents";
			potion_contents_type_name              = "POTION_CONTENTS";
			potion_contents_type_sig               = "Lnet/minecraft/core/component/DataComponentType;";
			data_component_type_class_sig          = "net/minecraft/core/component/DataComponentType";
			itemstack_get_component_name           = "get";
			itemstack_get_component_sig            = "(Lnet/minecraft/core/component/DataComponentType;)Ljava/lang/Object;";
			gameprofile_class_sig                  = "com/mojang/authlib/GameProfile";
			player_get_gameprofile_name            = "getGameProfile";
			player_get_gameprofile_sig             = "()Lcom/mojang/authlib/GameProfile;";
			gameprofile_get_name_name              = "getName";
			gameprofile_get_name_sig               = "()Ljava/lang/String;";
			game_options_name                      = "options";
			game_options_sig                       = "Lnet/minecraft/client/Options;";
			game_options_class_sig                 = "net/minecraft/client/Options";
			game_options_gamma_name                = "gamma";
			game_options_gamma_sig                 = "Lnet/minecraft/client/OptionInstance;";
			simple_option_class_sig                = "net/minecraft/client/OptionInstance";
			simple_option_set_value_name           = "set";
			simple_option_set_value_sig            = "(Ljava/lang/Object;)V";
			simple_option_get_value_name           = "get";
			simple_option_get_value_sig            = "()Ljava/lang/Object;";
			status_effect_instance_class_sig       = "net/minecraft/world/effect/MobEffectInstance";
			status_effect_instance_init_sig        = "(Lnet/minecraft/core/Holder;IIZZ)V";
			status_effects_class_sig               = "net/minecraft/world/effect/MobEffects";
			night_vision_effect_name               = "NIGHT_VISION";
			night_vision_effect_sig                = "Lnet/minecraft/core/Holder;";
			add_status_effect_name                 = "addEffect";
			add_status_effect_sig                  = "(Lnet/minecraft/world/effect/MobEffectInstance;)Z";
			remove_status_effect_name              = "removeEffect";
			remove_status_effect_sig               = "(Lnet/minecraft/core/Holder;)Z";
			living_entity_get_main_hand_name       = "getMainHandItem";
			living_entity_get_main_hand_sig        = "()Lnet/minecraft/world/item/ItemStack;";

			// Texture / identifier mappings
			mc_get_texture_manager_name            = "getTextureManager";
			mc_get_texture_manager_sig             = "()Lnet/minecraft/client/renderer/texture/TextureManager;";
			texture_manager_class_sig              = "net/minecraft/client/renderer/texture/TextureManager";
			texture_manager_get_texture_name       = "getTexture";
			texture_manager_get_texture_sig        = "(Lnet/minecraft/resources/ResourceLocation;)Lnet/minecraft/client/renderer/texture/AbstractTexture;";
			abstract_texture_class_sig             = "net/minecraft/client/renderer/texture/AbstractTexture";
			abstract_texture_get_glid_name         = "getId";
			abstract_texture_get_glid_sig          = "()I";
			identifier_class_full_sig              = "net/minecraft/resources/ResourceLocation";
			identifier_init_sig                    = "(Ljava/lang/String;Ljava/lang/String;)V";
			identifier_of_name                     = "fromNamespaceAndPath";
			identifier_of_sig                      = "(Ljava/lang/String;Ljava/lang/String;)Lnet/minecraft/resources/ResourceLocation;";

			// Living entity item info (ESP nametags)
			living_entity_get_off_hand_name        = "getOffhandItem";
			living_entity_get_off_hand_sig         = "()Lnet/minecraft/world/item/ItemStack;";
			living_entity_get_armor_items_name     = "getArmorSlots";
			living_entity_get_armor_items_sig      = "()Ljava/lang/Iterable;";
			itemstack_get_count_name               = "getCount";
			itemstack_get_count_sig                = "()I";
			itemstack_get_damage_name              = "getDamageValue";
			itemstack_get_damage_sig               = "()I";
			itemstack_get_max_damage_name          = "getMaxDamage";
			itemstack_get_max_damage_sig           = "()I";
			itemstack_is_damageable_name           = "isDamageableItem";
			itemstack_is_damageable_sig            = "()Z";
			itemstack_get_enchantments_name        = "getEnchantments";
			itemstack_get_enchantments_sig         = "()Lnet/minecraft/world/item/enchantment/ItemEnchantments;";
			item_enchantments_component_class_sig  = "net/minecraft/world/item/enchantment/ItemEnchantments";
			enchants_get_enchantments_name         = "keySet";
			enchants_get_enchantments_sig          = "()Ljava/util/Set;";
			enchants_get_level_name                = "getLevel";
			enchants_get_level_sig                 = "(Lnet/minecraft/core/Holder;)I";

			inventory_screen_init_sig              = "(Lnet/minecraft/world/entity/player/Player;)V";
			client_command_init_sig                = "(Lnet/minecraft/world/entity/Entity;Lnet/minecraft/network/protocol/game/ServerboundPlayerCommandPacket$Action;)V";
			shared_constants_class_sig             = "net/minecraft/SharedConstants";
			shared_constants_version_field_name    = "WORLD_VERSION";   // TODO mojang verify
			shared_constants_version_field_sig     = "I";
			camera_pos_field_name                  = "position";
			camera_pos_field_sig                   = "Lnet/minecraft/world/phys/Vec3;";

			// Modern input mapping (1.21.5+) — Mojang
			player_input_record_class_sig          = "net/minecraft/world/entity/player/Input";
			vec2_class_sig                         = "net/minecraft/world/phys/Vec2";
			input_player_input_field_name          = "playerInput";    // TODO mojang verify
			input_player_input_field_sig           = "Lnet/minecraft/world/entity/player/Input;";
			input_movement_vec2_field_name         = "movementVector"; // TODO mojang verify
			input_movement_vec2_field_sig          = "Lnet/minecraft/world/phys/Vec2;";

			// MinecraftClient.getCurrentServerEntry — Mojang: getCurrentServer
			mc_get_current_server_entry_name       = "getCurrentServer";
			mc_get_current_server_entry_sig        = "()Lnet/minecraft/client/multiplayer/ServerData;";
			server_info_class_sig                  = "net/minecraft/client/multiplayer/ServerData";
			server_info_address_field_name         = "ip";
			server_info_address_field_sig          = "Ljava/lang/String;";

			// Watermark ping — AbstractClientPlayer.getPlayerInfo() → PlayerInfo
			// .getLatency() (Mojang). Sin esto, en Lunar el ping no resuelve.
			abstract_client_player_class_sig            = "net/minecraft/client/player/AbstractClientPlayer";
			abstract_client_get_player_list_entry_name  = "getPlayerInfo";
			abstract_client_get_player_list_entry_sig   = "()Lnet/minecraft/client/multiplayer/PlayerInfo;";
			player_list_entry_class_sig                 = "net/minecraft/client/multiplayer/PlayerInfo";
			player_list_entry_get_latency_name          = "getLatency";
			player_list_entry_get_latency_sig           = "()I";

			// Player swing animation (clickaim). LivingEntity.swinging / swingTime.
			hand_swinging_name                          = "swinging";
			hand_swinging_sig                           = "Z";
			hand_swing_ticks_name                       = "swingTime";
			hand_swing_ticks_sig                        = "I";
		}

		inline void bind_mappings(JNIEnv* env, bool is_mojang)
		{
			is_mojang_layout = is_mojang;

			// Lunar Client and other Mojang-mapped launchers don't have the
			// yarn-intermediary names embedded in their jars — every JNI
			// lookup needs the deobfuscated Mojang name instead. Swap the
			// whole table on detection. Defaults remain Yarn so Feather
			// (Fabric) and any other Fabric-based loader still works.
			if (is_mojang)
			{
				apply_mojang_mappings_1_21();
			}

			// For Fabric/Yarn 1.21.1 - 1.21.11 (Feather Client, vanilla
			// Fabric, Quilt, Modrinth Feather) the default values declared
			// at the top of this file are correct.

			// â”€â”€ Speculative probing logic â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
			// These probes detect runtime differences between 1.21.x sub-versions.

			// Helper lambda: find a class using the game's classloader
			// env->FindClass may fail on the cheat thread if it doesn't have the game's classloader
			auto probe_find_class = [&](const char* name) -> jclass {
				// First try FindClass
				jclass cls = env->FindClass(name);
				if (env->ExceptionCheck()) { env->ExceptionClear(); cls = nullptr; }
				if (cls) return cls;

				// Fallback: Thread.currentThread().getContextClassLoader().loadClass()
				jclass thread_cls = env->FindClass("java/lang/Thread");
				if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
				if (!thread_cls) return nullptr;
				jmethodID ct_mid = env->GetStaticMethodID(thread_cls, "currentThread", "()Ljava/lang/Thread;");
				if (!ct_mid) { env->DeleteLocalRef(thread_cls); return nullptr; }
				jobject ct = env->CallStaticObjectMethod(thread_cls, ct_mid);
				if (!ct) { env->DeleteLocalRef(thread_cls); return nullptr; }
				jmethodID gcl_mid = env->GetMethodID(thread_cls, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
				env->DeleteLocalRef(thread_cls);
				if (!gcl_mid) { env->DeleteLocalRef(ct); return nullptr; }
				jobject cl = env->CallObjectMethod(ct, gcl_mid);
				env->DeleteLocalRef(ct);
				if (!cl) return nullptr;
				jclass cl_cls = env->GetObjectClass(cl);
				jmethodID lc_mid = env->GetMethodID(cl_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
				if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cl_cls); env->DeleteLocalRef(cl); return nullptr; }
				env->DeleteLocalRef(cl_cls);
				if (!lc_mid) { env->DeleteLocalRef(cl); return nullptr; }
				std::string dotted(name);
				for (auto& c : dotted) if (c == '/') c = '.';
				jstring jn = env->NewStringUTF(dotted.c_str());
				if (!jn) { env->DeleteLocalRef(cl); return nullptr; }
				cls = (jclass)env->CallObjectMethod(cl, lc_mid, jn);
				if (env->ExceptionCheck()) { env->ExceptionClear(); cls = nullptr; }
				env->DeleteLocalRef(jn);
				env->DeleteLocalRef(cl);
				return cls;
			};

			// 1. Probe getFov return type (double in 1.21.1+, was float in older)
			jclass gr_cls = probe_find_class("net/minecraft/class_757");
			if (gr_cls)
			{
				jmethodID mid = env->GetMethodID(gr_cls, "method_3196", "(Lnet/minecraft/class_4184;FZ)D");
				if (env->ExceptionCheck())
				{
					env->ExceptionClear();
					fov_returns_double = false;
				}
				else if (mid)
				{
					fov_returns_double = true;
					get_fov_sig = "(Lnet/minecraft/class_4184;FZ)D";
				}
				env->DeleteLocalRef(gr_cls);
			}

			// 2. Probe fallDistance field type (float in 1.21.1-1.21.4, double in 1.21.5+).
			//
			// We try BOTH F and D against whichever field name is current in
			// this mapping flavour. Picking the wrong type silently reads
			// garbage (or always 0) — the triggerbot's canCrit then never
			// fires crits because fallDistance never appears > 0. This is the
			// root cause of the "no crits in 1.21.4" report: the original
			// probe was hardcoded to Yarn name + assumed D as default, so a
			// Mojang-mapped launcher (Lunar) on 1.21.4 silently kept D and
			// the float field was read as a garbage double.
			{
				const char* ent_class = is_mojang
					? "net/minecraft/world/entity/Entity"
					: "net/minecraft/class_1297";
				const char* fall_name = entity_fall_distance_name; // already swapped per layout

				jclass ent_cls = probe_find_class(ent_class);
				if (ent_cls)
				{
					// Try float first; if that misses, try double.
					jfieldID fid_f = env->GetFieldID(ent_cls, fall_name, "F");
					if (env->ExceptionCheck()) env->ExceptionClear();

					if (fid_f) {
						fall_distance_is_float   = true;
						entity_fall_distance_sig = "F";
					} else {
						jfieldID fid_d = env->GetFieldID(ent_cls, fall_name, "D");
						if (env->ExceptionCheck()) env->ExceptionClear();
						if (fid_d) {
							fall_distance_is_float   = false;
							entity_fall_distance_sig = "D";
						}
						// If both miss, leave defaults — the cache lookup will
						// then fail and the consumer (can_crit) treats fall=0.
						// That's better than reading garbage.
					}
					env->DeleteLocalRef(ent_cls);
				}
			}

			// 3. Probe modern input fields (1.21.5+)
			jclass input_cls = probe_find_class("net/minecraft/class_744");
			if (input_cls)
			{
				jfieldID fid = env->GetFieldID(input_cls, "field_54155", "Lnet/minecraft/class_10185;");
				if (env->ExceptionCheck())
				{
					env->ExceptionClear();
					is_modern_input = false;
				}
				else if (fid)
				{
					is_modern_input = true;
				}
				env->DeleteLocalRef(input_cls);
			}

			// 4. Probe SharedConstants for the protocol version. Both Yarn and
			//    Mojang keep SharedConstants at the same path, but the field
			//    layout shifts between sub-versions. The path that always
			//    works is getCurrentVersion().getProtocolVersion().
			//    On failure we leave detected_protocol_version=0; modules
			//    that branch on it must treat 0 as "unknown, use defaults".
			detected_protocol_version = 0;
			{
				const char* sc_sig = is_mojang
					? "net/minecraft/SharedConstants"
					: "net/minecraft/class_155";
				jclass sc_cls = probe_find_class(sc_sig);
				if (sc_cls)
				{
					// Yarn: method_16673 → WorldVersion
					// Mojang: getCurrentVersion → WorldVersion
					const char* gcv_name = is_mojang ? "getCurrentVersion" : "method_16673";
					jmethodID gcv = nullptr;
					// The return type's class name varies; try a few common ones.
					const char* wv_sigs[] = {
						"()Lnet/minecraft/WorldVersion;",
						"()Lnet/minecraft/class_6489;",
						"()Lnet/minecraft/class_6603;",
						nullptr
					};
					const char* wv_class_sigs[] = {
						"net/minecraft/WorldVersion",
						"net/minecraft/class_6489",
						"net/minecraft/class_6603",
						nullptr
					};
					const char* hit_class = nullptr;
					for (int i = 0; wv_sigs[i] && !gcv; ++i) {
						gcv = env->GetStaticMethodID(sc_cls, gcv_name, wv_sigs[i]);
						if (env->ExceptionCheck()) { env->ExceptionClear(); gcv = nullptr; }
						if (gcv) hit_class = wv_class_sigs[i];
					}
					if (gcv && hit_class)
					{
						jobject wv = env->CallStaticObjectMethod(sc_cls, gcv);
						if (env->ExceptionCheck()) { env->ExceptionClear(); wv = nullptr; }
						if (wv)
						{
							jclass wv_cls = probe_find_class(hit_class);
							if (wv_cls)
							{
								// Yarn: method_37912 / Mojang: getProtocolVersion → ()I
								const char* gpv_names[] = {
									is_mojang ? "getProtocolVersion" : "method_37912",
									"getProtocolVersion",
									nullptr
								};
								jmethodID gpv = nullptr;
								for (int i = 0; gpv_names[i] && !gpv; ++i) {
									gpv = env->GetMethodID(wv_cls, gpv_names[i], "()I");
									if (env->ExceptionCheck()) { env->ExceptionClear(); gpv = nullptr; }
								}
								if (gpv) {
									jint pv = env->CallIntMethod(wv, gpv);
									if (env->ExceptionCheck()) env->ExceptionClear();
									else detected_protocol_version = (int)pv;
								}
								env->DeleteLocalRef(wv_cls);
							}
							env->DeleteLocalRef(wv);
						}
					}
					env->DeleteLocalRef(sc_cls);
				}
			}

			// 5. Probe ClientPacketListener.send(Packet)V — the intermediary
			//    name shifts across 1.21.x (method_45729 vs method_52787 etc.).
			//    Try our default; if it isn't there, scan declared methods for
			//    the unique signature (Packet)V and pick the first match. This
			//    keeps the cheat alive across yarn renumbers without having
			//    to update every single sub-version mapping by hand.
			if (!is_mojang)
			{
				jclass cpl = probe_find_class("net/minecraft/class_634");
				if (cpl)
				{
					// Build the full signature once (it includes the packet class).
					std::string send_sig = std::string("(Lnet/minecraft/class_2596;)V");

					jmethodID mid = env->GetMethodID(cpl, send_packet_name, send_sig.c_str());
					if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }

					if (!mid)
					{
						// Scan candidates we've seen across versions.
						static const char* kCandidates[] = {
							"method_45729", "method_52787", "method_2883",
							"method_36117", nullptr
						};
						for (int i = 0; kCandidates[i] && !mid; ++i) {
							mid = env->GetMethodID(cpl, kCandidates[i], send_sig.c_str());
							if (env->ExceptionCheck()) { env->ExceptionClear(); mid = nullptr; }
							if (mid) {
								send_packet_name = kCandidates[i];
								break;
							}
						}
					}
					env->DeleteLocalRef(cpl);
				}
			}
		}
	}
}

#endif
