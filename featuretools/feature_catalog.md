# VILLIEN Feature Catalog

更新日: 2026-08-30

この catalog は、VILLIEN の実装済み runtime、gameplay、development tool を VillienEditor から確認するための project-facing contract です。

Default は通常起動時の利用状態を示します。Auto は hardware capability と scene 条件に応じた起動時選択、Off は未実装ではなく品質判断、負荷、scene 条件、Debug 用途による無効状態を示します。安全に切り替えられない renderer foundation や import pipeline は Observed route で実動作を確認します。

Route は Scene、Environment、Rendering、World、VFX、Camera、Overworld、Tavern、Boss、Observed、Legacy のみに限定します。

## VillienEditor Technology Showcase Registry

| Lab ID | Category | Feature | Default | Route | 説明 |
|---|---|---|---|---|---|
| dx12_renderer | Core | DirectX 12 Renderer | On | Observed | Device、command queue、command list、fence、swap chain を使用する実 renderer。 |
| dx12_frame_submission | Core | Frame Submission and Synchronization | On | Observed | frame resource、allocator、fence を同期しながら command を submit する。 |
| dx12_root_signature_pso | Core | Root Signature and PSO Management | On | Observed | pass ごとの root signature、graphics PSO、compute PSO を管理する。 |
| dx12_descriptor_management | Core | Descriptor Heap Management | On | Observed | RTV、DSV、CBV、SRV、UAV descriptor と shader-visible table を管理する。 |
| dx12_resource_barriers | Core | Explicit Resource State Transitions | On | Observed | Render Target、Depth、SRV、UAV、Present 間の state transition を明示する。 |
| dx12_resize_rebuild | Core | Runtime Resize Resource Rebuild | On | Observed | window resize 時に swap-chain resource と camera aspect を再構築する。 |
| d3d12_debug_layer | Debug | D3D12 Debug Layer Validation | On | Observed | Debug build で error と corruption message を収集する。 |
| startup_tracing | Debug | Startup and Crash Tracing | On | Observed | startup stage、exception、runtime trace を log に残す。 |
| render_pass_pipeline | Rendering | Ordered Render-Pass Pipeline | On | Rendering | Shadow、Sky、G-buffer、Deferred、Reflection、Transparent、SSAO、Post、UI を順序管理する。 |
| deferred_gbuffer_4mrt | Rendering | Four-MRT G-buffer | On | Rendering | Albedo、world normal、material、emissive と depth を分離して格納する。 |
| deferred_lighting | Rendering | Fullscreen Deferred Lighting | On | Rendering | G-buffer を入力として PBR lighting を fullscreen pass で評価する。 |
| transparent_forward | Rendering | Transparent Forward Pass | On | Rendering | water、telegraph、transparent mesh を opaque deferred pass の後で描画する。 |
| particle_render_pass | Rendering | Particle Render Pass | On | VFX | billboard particle を transparent stage で描画する。 |
| reflection_pass_order | Rendering | Reflection Before Transparent VFX | On | Rendering | depth を持たない particle や overlay が reflection に混入しない順序を維持する。 |
| instanced_draw_batching | Optimization | Instanced Draw Batching | On | Observed | 同じ mesh の world matrix をまとめて repeated geometry を描画する。 |
| upload_heap_lifetime | Resource | Texture Upload Lifetime Control | On | Observed | GPU copy 完了後に material texture の staging upload resource を解放する。 |
| hdr_scene_target | HDR | HDR Scene Target | On | Rendering | lighting と bloom を FP16 HDR target 上で処理する。 |
| hdri_sky | Sky | HDRI Sky Renderer | On | Environment | equirectangular HDRI と exposure を使用して sky background を描画する。 |
| directional_light | Lighting | Directional Light | On | Environment | scene の sun または indoor key light を PBR lighting に供給する。 |
| point_lights | Lighting | GPU Point Lights | On | Rendering | StructuredBuffer から複数の dynamic point light を評価する。 |
| spot_lights | Lighting | GPU Spot Lights | Off | Rendering | deferred lighting が cone attenuation を持つ spot light を評価できる。 |
| ibl_irradiance | IBL | Irradiance Map | On | Environment | HDR environment から diffuse irradiance cubemap を生成して使用する。 |
| ibl_prefilter | IBL | GGX Prefiltered Environment Map | On | Environment | roughness mip に対応する specular environment map を生成する。 |
| ibl_brdf_lut | IBL | BRDF Integration LUT | On | Environment | split-sum specular IBL 用の BRDF LUT を生成して参照する。 |
| csm | Shadow | Cascaded Shadow Maps | On | Rendering | camera frustum を複数 cascade に分割して directional shadow を描画する。 |
| shadow_pcf | Shadow | PCF Shadow Filtering | On | Rendering | comparison sampler と周辺 sample で shadow edge を平滑化する。 |
| shadow_controls | Shadow | Shadow and Cascade Controls | On | Rendering | enable、bias、strength、cascade debug を Editor から調整する。 |
| ssao | SSAO | Depth-Normal SSAO | On | Rendering | depth reconstruction と normal を使って screen-space occlusion を評価する。 |
| ssao_bilateral_blur | SSAO | Depth-Aware Bilateral Blur | On | Rendering | depth edge を保ちながら SSAO noise を blur する。 |
| ssr_ray_march | SSR | View-Space Reflection Ray March | On | Rendering | reflected ray を screen space へ再投影して scene color の hit を探索する。 |
| ssr_depth_reconstruction | SSR | Scene Depth Reconstruction | On | Rendering | depth buffer から view-space scene position を再構築する。 |
| ssr_thickness_test | SSR | Thickness Hit Test | On | Rendering | ray depth と scene depth の差を thickness parameter で判定する。 |
| ssr_material_exclusion | SSR | Material Reflection Exclusion | On | Rendering | receiver mask と material factor で不適切な surface を reflection から除外する。 |
| reflection_mode_control | Reflection | Off SSR Hybrid DXR Selection | On | Rendering | Editor と Debug input から reflection mode を明示的に選択する。 |
| hybrid_dxr | DXR | Hybrid DXR Reflection | Auto | Rendering | 対応環境では起動時に Hybrid DXR を要求し、非対応または build／dispatch failure 時は SSR に fallback する。 |
| dxr_capability_gate | DXR | Tier 1.1 and DXIL Capability Gate | On | Rendering | Raytracing Tier 1.1 と SM 6.5 DXIL availability を確認する。 |
| dxr_blas_tlas | DXR | Automatic BLAS and TLAS Collector | Off | Overworld | unique static mesh ごとに BLAS、eligible placement ごとに TLAS instance を構築する。 |
| dxr_inline_rayquery | DXR | Inline RayQuery Reflection | Off | Overworld | compute shader の Inline RayQuery で water と mirror reflection を取得する。 |
| dxr_receiver_mask | DXR | Receiver-Aware Trace Mask and Range | Off | Overworld | Water と Mirror ごとに visibility mask、trace range、self exclusion を変える。 |
| dxr_hit_geometry | DXR | Barycentric Hit Reconstruction | Off | Overworld | primitive index と barycentric coordinate から hit normal と UV を再構築する。 |
| dxr_basecolor_sampling | DXR | Hit-Side BaseColor Sampling | Off | Overworld | scene-unique SRV table から sRGB BaseColor と UV transform を適用する。 |
| dxr_hit_lighting | DXR | Hit-Side Cook-Torrance Lighting | Off | Overworld | hit point で directional、point、spot light の PBR response を再評価する。 |
| dxr_hdri_miss | DXR | HDRI Environment Miss | Off | Overworld | ray miss 時に HDRI sky と directional sun core を返す。 |
| dxr_large_scene | DXR | Large-Scene Acceleration Structure Proof | Off | Overworld | 2,000 超の TLAS instances と mesh-shared BLAS cache を使う Overworld large-scene proof。実数は runtime status に表示する。 |
| dxr_ssr_fallback | DXR | Automatic SSR Fallback | On | Rendering | DXR capability、build、dispatch の失敗時に SSR へ安全に戻る。 |
| pbr_cook_torrance | PBR | Cook-Torrance GGX BRDF | On | Rendering | GGX distribution、Smith geometry、Fresnel-Schlick、energy-aware diffuse を評価する。 |
| material_basecolor | Material | BaseColor Texture and Factor | On | Scene | texture と factor を組み合わせて material albedo を設定する。 |
| material_normal | Material | Tangent-Space Normal Mapping | On | Scene | tangent basis と normal texture を G-buffer normal に反映する。 |
| material_metalrough | Material | Metallic and Roughness Workflow | On | Scene | direct light と IBL の specular response を metallic、roughness で制御する。 |
| material_ao | Material | Material Ambient Occlusion | On | Scene | imported AO と SSAO を lighting に統合する。 |
| material_emissive | Material | HDR Emissive Material | On | Scene | emissive texture と factor を HDR scene と bloom に供給する。 |
| material_uv | Material | UV Tiling and Offset | On | Scene | material ごとの UV scale と offset を shader sampling に適用する。 |
| material_alpha | Material | Alpha Mask and Transparent Routing | On | Scene | opaque、mask、blend を適切な PSO と pass へ route する。 |
| material_pom | Material | Parallax Occlusion Mapping | On | Scene | height scale と min／max layers を使って castle surface depth を表現する。 |
| material_procedural | Material | Procedural Material Parameters | On | Scene | shader-side type ID と parameter で ground、path、VFX pattern を生成する。 |
| material_vertex_deformation | Material | Vertex-Deformed Water | On | World | wave parameter と runtime agitation で water geometry を変形する。 |
| material_wet_puddle | Material | Wet Surface and Puddle Response | On | World | wetness、dry time、clarity、tint、ripple を ground material に反映する。 |
| reflection_receiver_contract | Material | Reflection Receiver Contract | On | Scene | None、Water、Mirror と reflection strength、ray-tracing visibility を分離管理する。 |
| bloom_mip_chain | Post Process | Bloom Mip Chain | On | Rendering | 13-tap downsample と 9-tap tent upsample で bloom を合成する。 |
| aces_tonemap | Post Process | ACES Tonemapping | On | Rendering | HDR scene を display 用 LDR color へ変換する。 |
| fxaa | Post Process | FXAA | On | Rendering | final LDR image の edge aliasing を軽減する。 |
| depth_of_field | Post Process | Depth of Field | Off | Rendering | phone UI 使用中など、focus 対象以外の背景を blur する。 |
| taa | Post Process | Temporal Anti-Aliasing | Off | Rendering | projection jitter、history buffer、temporal resolve を実装する。 |
| velocity_buffer | Post Process | Velocity Buffer | Off | Rendering | motion vector を TAA と motion blur の入力として生成する。 |
| motion_blur | Post Process | Velocity-Based Motion Blur | Off | Rendering | velocity buffer を使用して screen-space motion blur を行う。 |
| ink_edge | Post Process | Ink Edge Stylization | On | Boss | Boss Phase 2 で luminance edge を ink treatment に変換する。 |
| ink_flow | Post Process | Screen-Space Ink Flow | On | Boss | Phase 2 で animated ink flow を合成し、gameplay cue を保持する。 |
| boss_ssr_policy | Reflection | BossArena SSR Quality Policy | Off | Boss | artifact と attack readability の判断により通常は SSR を無効にする。 |
| gltf_loader | Asset | glTF Loader | On | Observed | mesh、node、material、texture、skin、animation を読み込む。 |
| glb_loader | Asset | GLB Loader | On | Observed | embedded buffer と image を含む binary glTF を読み込む。 |
| vrm_loader | Asset | VRM Character Loading | On | Observed | Player 用 VRM mesh、skin、material を読み込む。 |
| node_hierarchy | Asset | Node Hierarchy Transform Propagation | On | Observed | parent-child local transform を global transform へ伝播する。 |
| pbr_import | Asset | glTF PBR Material Import | On | Observed | BaseColor、MetalRough、Normal、AO、Emissive、alpha mode を保持する。 |
| vrm_material_semantics | Asset | VRM Primitive and Material Semantics | On | Observed | material pair、double-sided winding、normal、tangent handedness を保持する。 |
| skeleton_import | Animation | Skeleton and Inverse Bind Import | On | Observed | joint hierarchy と inverse bind matrix を検証して読み込む。 |
| clip_import | Animation | Animation Clip Import | On | Observed | translation、rotation、scale channel と clip timing を読み込む。 |
| mixamo_vrm_retarget | Animation | Mixamo-to-VRM Retargeting | On | Observed | name mapping と ancestor closure で source clip を target skeleton へ適用する。 |
| root_motion_conversion | Animation | In-Place Root Motion Conversion | On | Observed | linear drift を除き、cyclic bob と sway を保持する。 |
| animation_validation | Animation | Animation Input Validation | On | Observed | timestamp、interpolation、quaternion、channel range を検証する。 |
| finite_pose_safety | Animation | Deterministic Finite-Pose Safety | On | Observed | cycle、singular matrix、non-finite value に deterministic fallback を適用する。 |
| gpu_skinning | Animation | GPU Bone Palette Skinning | On | Observed | evaluated bone matrices を frame ごとに GPU へ送る。 |
| bind_pose_fallback | Animation | Bind-Pose Fallback | On | Observed | animation が使用できない場合も valid pose を維持する。 |
| procedural_idle_fallback | Animation | Procedural Idle Fallback | On | Observed | clip がない場合に簡易 idle motion を生成する。 |
| imported_collision | Asset | Imported Collision Extraction | On | World | imported mesh triangle を gameplay collision data へ変換する。 |
| model_cache | Asset | Model Cache | On | Observed | 同じ asset path の mesh と material の重複 load を防ぐ。 |
| procedural_mesh | Asset | Procedural Mesh Generation | On | Scene | Plane、Cube、Cylinder、Cone、Ring、VFX geometry を生成する。 |
| tavern_asset_extract | Asset | Tavern GLB Extraction Pipeline | On | Tavern | embedded GLB から named prop、metre scale、texture cap、attribution manifest を生成する。 |
| multi_skin_remap | Animation | Multi-Skin Canonical Skeleton Remap | On | Observed | garment と body skin の joint union、primitive JOINTS remap、namespace 差を処理する。 |
| player_locomotion | Animation | Player Idle Walk Run States | On | Overworld | input speed に応じて Idle、Walk、Run clip を選択する。 |
| player_crossfade | Animation | Interruptible Local-TRS Crossfade | On | Overworld | translation、scale lerp と normalized quaternion slerp で locomotion を遷移する。 |
| taking_item | Animation | Taking Item One-Shot | On | Boss | movement と locomotion blend を保ちながら pickup one-shot を再生する。 |
| tavern_npc_animation | Animation | Tavern NPC Idle Walk Sitting | On | Tavern | entrance、arrival、seating、leaving state に応じて imported clip を切り替える。 |
| tavern_pose_isolation | Animation | Per-Instance NPC Pose Isolation | On | Tavern | shared mesh と clip を使いながら instance ごとに time、transition、bone palette を分離する。 |
| keyboard_input | Input | Keyboard Movement | On | Camera | WASD、方向キー、action key を gameplay state へ入力する。 |
| xinput | Input | XInput Gamepad | On | Camera | left stick、button、camera input を keyboard と同じ action へ統合する。 |
| third_person_camera | Camera | Smooth Third-Person Follow | On | Camera | Overworld と Boss で Player を interpolation 追従する。 |
| tavern_first_person_camera | Camera | Tavern First-Person Work Camera | On | Tavern | 1.68 m eye height、mouse／gamepad look、camera-relative movement を使用する。 |
| camera_impulse | Camera | Camera Impulse and Shake | On | Camera | hit、counter、pickup ごとに異なる impulse を加える。 |
| capsule_box | Collision | Capsule versus Oriented Box | On | World | rotation-aware box に対して Player capsule を解決する。 |
| capsule_circle | Collision | Capsule versus Circle | On | World | gameplay plane 上の circle collider を解決する。 |
| capsule_triangle | Collision | Capsule versus Triangle | On | World | manual triangle collider に対して movement を解決する。 |
| capsule_mesh | Collision | Capsule versus Imported Mesh | On | World | imported triangle mesh に対して Player movement を解決する。 |
| world_boundary | Collision | World Boundary Clamp | On | World | Player を scene の有効範囲内へ制限する。 |
| player_scaled_collision | Collision | Player-Scaled Collision Footprint | On | World | visual height に合わせて Player の XZ gameplay footprint radius を比例調整する。現在の 1.20 m 設定では radius 0.3375 m；vertical capsule 判定は未実装。 |
| overworld_route | Game Flow | Overworld Game View | On | Overworld | Editor から実 Overworld を起動して F1 で戻る。 |
| overworld_heightfield | Terrain | CPU Heightfield Terrain | On | Overworld | 96×96 grid（9,409 vertices／18,432 triangles）を CPU で生成し、約 -0.23～+0.23 m の緩やかな地形起伏を raster、CSM、SSR depth、DXR BLAS に共通使用する。 |
| terrain_shared_grounding | Terrain | Shared Visual and Player Grounding | On | Overworld | render mesh と同じ cell diagonal／triangle interpolation を `GroundHeightAt` で再現し、XZ obstacle resolve 後の Player Y、camera follow、placement、collision debug を同じ地表へ同期する。 |
| terrain_flatten_zones | Terrain | Protected Terrain Flatten Zones | On | Overworld | 道路、建築 footprint、spawn、Tavern entrance／return、Boss warp、gate、reflection monolith、moat／puddle 周辺を smooth mask で平滑化し、浮遊・埋没・水面貫通を防ぐ。 |
| placement_json | Overworld | JSON Model Placements | On | Overworld | model path、position、scale、yaw を external JSON で管理する。 |
| overworld_environment | Overworld | Castle Shrine Houses and Forest | On | Overworld | procedural geometry と imported asset を組み合わせて world を構築する。 |
| overworld_instancing | Overworld | Repeated World Geometry Instancing | On | Overworld | forest と repeated props を batch／instance 描画する。 |
| time_of_day | Environment | Dynamic Time of Day | On | Environment | shared clock から sun、sky、exposure を更新する。 |
| world_rain | Environment | World-Scale Rain | On | Environment | Overworld と Boss scene に world-scale rain emitter を配置する。 |
| overworld_water | Overworld | Moat Water and Reflection | On | Overworld | transparent water、vertex waves、SSR または Hybrid DXR を組み合わせる。 |
| wet_surface | Overworld | Dynamic Wet Surface | On | Overworld | rain impact、dry seconds、cycle に応じて ground wetness を変化させる。 |
| procedural_puddles | Overworld | Procedural Puddles | On | Overworld | radius、build time、clarity、tint、ripple を持つ puddle を生成する。 |
| reflection_monolith | Overworld | DXR Reflection Monolith | On | Overworld | floor、water、forest を長距離反射する mirror landmark。 |
| tavern_route | Game Flow | Tavern Game View | On | Tavern | Editor から実 Tavern を起動し、Overworld return flow も確認する。 |
| tavern_workstations | Tavern | Proximity Work Stations | On | Tavern | customer、rack、counter、tap、kitchen、wash basin、entrance を world interaction 化する。 |
| tavern_ale_loop | Tavern | Complete Ale Service Loop | On | Tavern | order、empty mug、pour、carry、serve、collect、wash を state machine で接続する。 |
| tavern_food_loop | Tavern | Complete Food Service Loop | On | Tavern | Food order、clean bowl、cook、collect、serve、dirty bowl、wash を接続する。 |
| tavern_kitchen_state | Tavern | Kitchen Cooking State Machine | On | Tavern | Idle、Cooking、Ready と pause、collect、cancel、reset を管理する。 |
| tavern_table_system | Tavern | Purchasable Two-Seat and Four-Seat Tables | On | Tavern | table ごとに spawn、party、order、dirty turnover、HUD を独立管理する。 |
| tavern_party_orders | Tavern | Per-Guest Mixed Orders | On | Tavern | 1～4 人 party の各 guest に Ale または Food order を割り当てる。 |
| tavern_tableware_pool | Tavern | Reusable Mug and Bowl Pools | On | Tavern | Held、Table、Dirty、Wash、Counter 間で有限 tableware の conservation を保つ。 |
| tavern_pour_quality | Tavern | Ale Stream Foam Overflow and Quality | On | Tavern | hold-to-pour、3D stream、foam、overflow、ideal band と reward を連動する。 |
| tavern_economy | Tavern | Gold Reward Breakdown | On | Tavern | base、pour quality、patience を分けて reward を計算する。 |
| tavern_stock | Tavern | Finite Ale Stock | On | Tavern | capacity、purchase、consumption、empty-state guard を管理する。 |
| tavern_management | Tavern | Physical Management Table UI | On | Tavern | Supplies、Upgrades、Table Layout を in-world modal から操作する。 |
| tavern_upgrades | Tavern | Tavern Upgrade Dependency Graph | On | Tavern | mug、capacity、table、expansion、party size の cost と dependency を管理する。 |
| tavern_table_layout | Tavern | In-World 3D Table Placement | On | Tavern | preview、grid、rotation、validity、save／cancel、collider update を行う。 |
| tavern_recovery | Tavern | Economy Soft-Lock Recovery | On | Tavern | new-day stock floor と emergency purchase reserve で進行不能を防ぐ。 |
| tavern_persistence | Tavern | Cross-Day and Re-Entry Persistence | On | Tavern | Gold、stock、upgrade、layout、statistics を shift reset から分離する。 |
| tavern_day_loop | Tavern | Shared 24-Hour Day Loop | On | Tavern | Overworld と Tavern が同じ clock を共有し、sleep で次の営業時間へ進む。 |
| tavern_satisfaction | Tavern | Satisfaction and Walkout Pressure | On | Tavern | waiting time、walkout、loss、pause protection を管理する。 |
| tavern_waiting | Tavern | Dirty-Table Entrance Waiting | On | Tavern | dirty table ごとの waiting guest、timeout、seat transition を管理する。 |
| tavern_order_bubbles | Tavern | Per-Guest 3D Order Bubbles | On | Tavern | Mug／Bowl model bubble を guest ごとに表示し、個別 delivery 後に消す。 |
| tavern_npc_roster | Tavern | Imported NPC Model Roster | On | Tavern | NPC1 と Remy を table、party、entrance state で再利用する。 |
| tavern_tutorial | Tavern | First-Cycle Guided Tutorial | On | Tavern | six-step card と pulsing world marker で最初の service cycle を案内する。 |
| tavern_wash_water | Tavern | Dynamic Wash-Basin Water | On | Tavern | irregular high-density surface、edge agitation、wash splash、SSR receiver を使用する。 |
| boss_route | Game Flow | BossArena Game View | On | Boss | Editor から実 Boss battle を起動して F1 で戻る。 |
| boss_state_machine | Boss | Telegraph Resolve Recovery State Machine | On | Boss | attack phase、timer、damage window、recovery を deterministic に管理する。 |
| boss_meteor | Boss | Meteor Area Attack | On | Boss | circular telegraph、scan、impact、distance hit test を使用する。 |
| boss_laser | Boss | Laser Line Attack | On | Boss | vertical／horizontal line telegraph と width hit test を使用する。 |
| boss_sanctuary | Boss | Sanctuary Inverse Safe Zone | On | Boss | Phase 2 で安全領域へ入る inverse attack を使用する。 |
| boss_scheduler | Boss | Phase-Aware Attack Scheduler | On | Boss | phase ごとに attack family と timing escalation を制御する。 |
| boss_charges | Boss | Three Mirror Charges | On | Boss | deterministic variation、pickup、HUD、VFX、animation feedback を管理する。 |
| boss_phone | Boss | Mizukagami Phone Overlay | On | Boss | custom phone UI、slide animation、gameplay pause を統合する。 |
| puzzle_symbol | Boss | Symbol Memory Puzzle | On | Boss | three-symbol sequence を記憶して順番に入力する。 |
| puzzle_number | Boss | Number Position Puzzle | On | Boss | number と screen position rule に従って入力する。 |
| puzzle_trace | Boss | Reflection Trace Puzzle | On | Boss | mouse trace、ordered node、timeout、wrong path、Phase 2 variant を実装する。 |
| puzzle_recovery | Boss | Puzzle Failure Recovery | On | Boss | failure 後に charge、spawn、attack flow を安全に再構築する。 |
| boss_counter | Boss | Successful Counter Damage Flow | On | Boss | puzzle success、Boss damage、phone close、VFX、result ordering を同期する。 |
| boss_phase2 | Boss | Phase 2 Presentation and Rules | On | Boss | HP gate、red moon、seal、rift、ink、timing change を一括遷移する。 |
| boss_result | Boss | Clear Failed Rank Result Flow | On | Boss | timer、counter、damage count から result と S／A／B rank を生成する。 |
| vfx_particle_system | VFX | Continuous and Burst Particle System | On | VFX | billboard、lifetime、velocity、color、continuous／burst emitter を管理する。 |
| vfx_meteor | VFX | Meteor Flame and Impact Burst | On | VFX | attack telegraph と resolve に flame、spark、light を同期する。 |
| vfx_laser | VFX | Laser Rift VFX | On | VFX | line rift particle と transparent geometry を laser state に同期する。 |
| vfx_sanctuary | VFX | Sanctuary Dome VFX | On | VFX | Fresnel outline、ink band、rise、resolve pulse を表示する。 |
| vfx_mirror | VFX | Mirror Pickup and Spark Bursts | On | VFX | charge pickup と counter success に burst emitter を使用する。 |
| vfx_counter | VFX | Counter Rings Rays Ribbons and Sakura | On | VFX | successful counter の climax feedback を複数 emitter で構築する。 |
| vfx_damage | VFX | Damage Droplets and Boss Shards | On | VFX | Player hit と Boss damage を particle／transparent mesh で表示する。 |
| vfx_dynamic_lights | VFX | Dynamic VFX Point Lights | On | VFX | pickup、impact、counter、phase shift、failure に timed light を追加する。 |
| editor_workspace | Editor | Integrated Editor Workspace | On | Scene | Scene Objects、Scene View、Inspector、Project、Systems、Console を統合する。 |
| editor_entity_ops | Editor | Entity Create Duplicate Delete | On | Scene | entity の作成、選択、複製、削除を scene data に反映する。 |
| editor_picking | Editor | Viewport Mouse Picking | On | Scene | viewport-local ray から entity と grid element を選択する。 |
| editor_gizmo | Editor | Transform Gizmo | On | Scene | translate、rotate、scale と local／world mode を提供する。 |
| editor_undo_redo | Editor | Command-Based Undo and Redo | On | Scene | transform、material、lighting、grid edit を command history に記録する。 |
| editor_scene_serialization | Editor | JSON Scene Save and Load | On | Scene | entity、material、light、reflection property を serialize する。 |
| editor_play_snapshot | Editor | Play Snapshot and Restore | On | Scene | preview 前の scene snapshot を停止時に復元する。 |
| editor_material | Editor | PBR Material Inspector | On | Scene | factor、UV、texture、POM、procedural、reflection property を編集する。 |
| editor_reflection_inspector | Editor | Reflection Material Inspector | On | Scene | Reflection Receiver、strength、ray-tracing visibility を material ごとに編集する。 |
| editor_texture_slots | Editor | Texture Slot Assignment | On | Scene | BaseColor、Normal、MetalRough、AO、Emissive、Height を割り当てる。 |
| editor_lighting | Editor | Lighting Inspector | On | Rendering | directional、point、spot light を編集する。 |
| editor_shadow | Editor | Shadow and CSM Panel | On | Rendering | shadow enable、bias、strength、cascade debug を編集する。 |
| editor_ssao | Editor | SSAO Panel | On | Rendering | radius、bias、power、kernel、strength を編集する。 |
| editor_post | Editor | Post-Process Panel | On | Rendering | Bloom、TAA、FXAA、Motion Blur、DOF parameter を編集する。 |
| editor_reflection | Editor | Reflection and DXR Status Panel | On | Rendering | requested／active mode、capability、fallback、BLAS／TLAS status を表示する。 |
| editor_asset_browser | Editor | Asset Browser | On | Scene | scene、mesh、texture file を列挙して Inspector operation へ接続する。 |
| editor_grid | Editor | Grid and Stage Editor | On | Scene | tile paint、tower selection、stage save／load を提供する。 |
| editor_shader_reload | Editor | Raster Shader Hot Reload | On | Rendering | mesh、post、SSAO、sky、deferred、SSR、particle shader を再 compile する。 |
| editor_placement_reload | Editor | Overworld Placement Hot Reload | On | Overworld | placement JSON を runtime 中に再読込する。 |
| editor_collision_debug | Editor | Collision Visualization | On | World | collider shape と imported mesh triangle を debug line で表示する。 |
| editor_forest | Editor | Forest Debugger | On | World | density、scale、distance、spacing、single cluster を調整する。 |
| editor_water | Editor | Water Wetness and Puddle Debugger | On | World | wave、transparency、wet surface、puddle parameter を調整する。 |
| editor_time | Editor | Time-of-Day Debugger | On | Environment | hour、automatic time、speed、sun、exposure を調整する。 |
| editor_boss | Editor | Boss Debug Panel | On | Boss | attack、phase、timer、render toggle、No Clip を操作する。 |
| editor_camera_lock | Editor | Camera Navigation Lock | On | Camera | Scene View mouse、keyboard、wheel navigation を lock する。 |
| editor_feature_coverage | Editor | Feature Coverage Matrix | On | Observed | catalog の ID、route、status と source inventory を audit 表示する。 |
| debug_smoke_launchers | Debug | Scene and Feature Smoke Modes | On | Observed | animation、pickup、Tavern、DXR、scene startup の deterministic smoke mode を提供する。 |
| runtime_settings | Editor | Runtime Settings Persistence | On | Observed | Editor runtime parameter を local JSON へ save／load する。 |

## Standalone 3D Feature Lab Registry

> Legacy FeatureTools.exe の互換 schema。Default On は standalone lab の起動時設定を示します。

| Lab ID | Category | Feature | Default | 說明 |
|---|---|---|---|---|
| ground | World | Ground Plane | On | indexed plane geometry を standalone world から切り替える。 |
| environment | World | Environment Geometry | On | procedural pillar、lantern、shrine geometry。 |
| material_gallery | World | PBR Material Gallery | On | metallic と roughness の比較 sphere。 |
| water | Water | Transparent Water | On | standalone forward transparent water pass。 |
| water_waves | Water | Vertex-Deformed Waves | On | water vertex shader の簡易 wave deformation。 |
| ssr | Water | Screen-Space Reflection | On | standalone scene position buffer を使う簡易 reflection。 |
| rain | Weather | Rain Particles | On | instanced rain quad。 |
| fog | Weather | Distance Fog | On | standalone lighting shader の distance fog。 |
| time_of_day | Lighting | Dynamic Time of Day | On | sun direction、color、intensity の簡易更新。 |
| deferred | Rendering | Deferred G-buffer Pipeline | On | three-MRT G-buffer と forward fallback。 |
| directional_light | Lighting | Directional Light | On | standalone sun diffuse／specular lighting。 |
| point_lights | Lighting | GPU Point Lights | On | frame constants の dynamic point lights。 |
| shadows | Rendering | Shadow Map | On | single light-space depth map と PCF。 |
| ssao | Rendering | Screen-Space Ambient Occlusion | On | position／normal buffer を使う簡易 AO。 |
| hdr | Post Process | HDR Rendering | On | FP16 scene と composite target。 |
| bloom | Post Process | Bloom | On | emissive extract と separable blur。 |
| tonemap | Post Process | ACES Tone Mapping | On | standalone HDR から LDR への変換。 |
| fxaa | Post Process | FXAA | On | final resolve の簡易 edge smoothing。 |
| wireframe | Rendering | Wireframe Override | Off | standalone wireframe PSO。 |
| pbr | Material | Metallic Roughness PBR | On | standalone GGX-style direct lighting。 |
| normal_mapping | Material | Procedural Normal Detail | On | texture normal map ではない procedural perturbation。 |
| emissive | Material | Emissive Materials | On | HDR と bloom に入る emissive。 |
| procedural_material | Material | Procedural Material Patterns | On | grid、stripe、stone pattern。 |
| uv_animation | Material | UV Animation | On | water、hologram、emissive pattern の UV motion。 |
| entity_system | System | Entity Scene System | On | transform、mesh、material、feature tag の draw list。 |
| animation | System | Runtime Transform Animation | On | prop、sphere、Boss の transform animation。 |
| gpu_instancing | System | GPU Instancing | On | SV_InstanceID を使う repeated geometry。 |
| collision | System | Collision Simulation | On | moving sphere と AABB の CPU resolution。 |
| collision_debug | Tool | Collision Visualization | On | standalone collider proxy geometry。 |
| grid | Tool | World Grid | On | ground reference grid。 |
| camera_orbit | System | Orbit Camera | On | mouse drag と wheel zoom。 |
| camera_auto_move | System | Automatic Camera Movement | Off | automatic orbit の独立 toggle。 |
| boss | Gameplay | Boss Battle Simulation | On | standalone Boss timer、HP、phase simulation。 |
| meteor | Gameplay | Meteor AoE | On | danger disc と timed pulse。 |
| laser | Gameplay | Laser Line | On | line telegraph と resolve beam。 |
| sanctuary | Gameplay | Sanctuary Seal | On | inverse safe-zone ring。 |
| mirror_charges | Gameplay | Mirror Charges | On | three emissive collectible orbs。 |
| phone_hologram | Gameplay | Phone Puzzle Hologram | On | phone panel と ordered node animation。 |
| counter_vfx | Gameplay | Counter VFX | On | procedural ring、ray、burst particle。 |
| phase2 | Gameplay | Phase 2 Presentation | On | standalone red lighting と VFX state。 |
| particles | VFX | Spark and Mist Particles | On | instanced transparent particle quad。 |
| auto_demo | Tool | Automatic Feature Demo | On | Boss attack と showcase animation の自動循環。 |
