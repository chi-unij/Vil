deferred_lighting.hlsl` 全函式解析（繁體中文）

我會依照檔案中出現的順序，逐一解釋**每個函式**的：
1. **做什麼**（What）
2. **怎麼做**（How，數學原理）
3. **用在哪**（Where，在整個 pipeline 的角色）

---

## 一、PBR BRDF 數學函式群

這 5 個函式構成 **Cook-Torrance BRDF 模型** 的核心。BRDF（Bidirectional Reflectance Distribution Function，雙向反射分佈函數）描述「光從某方向打到表面，從另一方向射出的比例」。

---

### 1. `FresnelSchlick` — 菲涅耳近似

```hlsl
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}
```

#### 做什麼
計算「**在某個視角下，有多少光被鏡面反射**」。

#### 物理原理
**菲涅耳效應**：所有材質在**掠射角**（接近平視表面）時，反射率都會大幅提升。

```
垂直看湖面 → 看到水底（反射弱）
斜眼看湖面 → 看到天空倒影（反射強）
```

#### 數學
原始菲涅耳方程很複雜，**Schlick 近似**用一條 5 次方曲線逼近：

$$ F(\theta) = F_0 + (1 - F_0)(1 - \cos\theta)^5 $$

- `cosTheta` = 視線與半向量的夾角餘弦（接近 1 = 垂直，接近 0 = 掠射）
- `F0` = 垂直入射時的反射率（非金屬 = 0.04，金屬 = albedo）

#### 用在哪
```hlsl
float3 F = FresnelSchlick(VdotH, F0);
```
用於**直接光源**（太陽、點光、聚光）的鏡面反射計算。

---

### 2. `FresnelSchlickRoughness` — 帶粗糙度修正的菲涅耳

```hlsl
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 maxR = max((1.0f - roughness).xxx, F0);
    return F0 + (maxR - F0) * pow(1.0f - cosTheta, 5.0f);
}
```

#### 做什麼
和上一個類似，但**考慮粗糙度的影響**，專門用於 **IBL（環境光照）**。

#### 為什麼需要不同版本？
普通 `FresnelSchlick` 假設「能反射的環境光在每個方向都一樣」，但實際上**粗糙表面會把環境光「模糊化」**，掠射角的反射不會像光滑表面那樣強。`max(1-roughness, F0)` 抑制了這個效應 —— 粗糙度越高，掠射反射越被壓低。

#### 用在哪
```hlsl
float3 kS_ibl = FresnelSchlickRoughness(max(NdotV, 0.0f), F0_ibl, roughness);
```
**只在 IBL 環境光區塊**使用，計算 IBL 的能量分配。

---

### 3. `DistributionGGX` — GGX 法線分佈函數（D 項）

```hlsl
float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = (NdotH * NdotH) * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 1e-6f);
}
```

#### 做什麼
描述「**微表面中，有多少『微觀法線』朝向半向量 H**」。

#### 物理直覺
真實表面不是完美光滑，而是由無數**微小平面**組成。只有當某個微平面**剛好朝向 H 方向**時，它才能把光從 L 反射到 V。

```
        L (光)     V (視線)
         ↘   ↗  
           H ← 半向量
         ↑
   ━╱╲━╱╲━╱╲━╱╲━  微表面
   有些朝H方向（貢獻高光）
   有些不朝H（不貢獻）
```

D 函數的值 = **朝向 H 的微平面的密度**。

#### 數學（GGX / Trowbridge-Reitz）

$$ D(h) = \frac{\alpha^2}{\pi[(N \cdot H)^2(\alpha^2 - 1) + 1]^2} $$

其中 $\alpha = \text{roughness}^2$。

#### 粗糙度的影響
- `roughness ≈ 0` → D 函數變成尖刺（只有 NdotH=1 時值很大）→ **銳利的高光點**
- `roughness ≈ 1` → D 函數變平緩 → **柔和散開的高光**

#### 用在哪
```hlsl
float D = DistributionGGX(NdotH, roughness);
float3 spec = (D * G) * F / ...;
```
組成 Cook-Torrance 鏡面公式的 D 項。

---

### 4. `GeometrySchlickGGX` — 單向幾何遮蔽

```hlsl
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-6f);
}
```

#### 做什麼
計算「**某個方向上，微表面互相遮擋造成的光損失**」。

#### 物理直覺
微表面雖然朝對方向，但可能被旁邊的小山丘擋住：

```
   光線方向 →
        ╱╲ ← 這個微面被擋
   ━╱╲╱──╲╱╲━
       擋住
```

#### 數學
$$ G_{\text{Schlick}}(v) = \frac{N \cdot v}{(N \cdot v)(1-k) + k} $$

`k` 是粗糙度衍生的常數（**直接光用 `(r+1)²/8`**，IBL 用 `r²/2`，本檔案用前者）。

#### 用在哪
作為 `GeometrySmith` 的子函式，**不會直接被呼叫**。

---

### 5. `GeometrySmith` — Smith 法的雙向幾何遮蔽（G 項）

```hlsl
float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    return GeometrySchlickGGX(NdotV, roughness) *
           GeometrySchlickGGX(NdotL, roughness);
}
```

#### 做什麼
**同時考慮「光線方向」和「視線方向」的遮蔽**，把兩個方向的單向遮蔽函數相乘。

#### 物理直覺
光要先「**進得來**」（不被擋）→ G(L)，
然後反射光要「**出得去**」（不被擋）→ G(V)。
兩者都要成立，所以相乘。

#### 用在哪
```hlsl
float G = GeometrySmith(NdotV, NdotL, roughness);
float3 spec = (D * G) * F / ...;
```
組成 Cook-Torrance 公式的 G 項。

---

### 6. `EvaluateBRDF` — Cook-Torrance BRDF 統合計算

```hlsl
float3 EvaluateBRDF(float3 N, float3 V, float3 L, float3 radiance,
                    float3 albedo, float metallic, float roughness, float3 F0)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3  F = FresnelSchlick(VdotH, F0);
    float   D = DistributionGGX(NdotH, roughness);
    float   G = GeometrySmith(NdotV, NdotL, roughness);
    float3  spec = (D * G) * F / max(4.0f * NdotV * NdotL, 1e-4f);

    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diff = kD * albedo / PI;

    return (diff + spec) * radiance * NdotL;
}
```

#### 做什麼
**核心函式** —— 對任意光源計算「該光源對這個像素的最終貢獻」。

#### 流程
1. **計算所有需要的方向向量**
   - `H` = 半向量（光線方向與視線的「中間」）
   - 各種點積 = 各種角度的餘弦
2. **計算 D、F、G 三項**
3. **組合鏡面反射**：
   $$ \text{spec} = \frac{DFG}{4(N \cdot V)(N \cdot L)} $$
4. **計算漫反射的能量比例 kD**：能量守恆 + 金屬抑制
5. **組合漫反射**：`kD × albedo / π`
6. **乘上入射光 radiance 與餘弦項 NdotL** → 輸出最終貢獻

#### 完整公式
$$ L_o = \left(k_D \cdot \frac{\text{albedo}}{\pi} + \frac{DFG}{4(N\cdot V)(N \cdot L)}\right) \cdot L_i \cdot (N \cdot L) $$

#### 用在哪
**所有三種直接光源**都呼叫它：
```hlsl
color = EvaluateBRDF(N, V, sunL, sunRadiance, ...);     // 太陽光
color += EvaluateBRDF(N, V, L, radiance, ...);          // 點光
color += EvaluateBRDF(N, V, L, radiance, ...);          // 聚光
```
**設計優勢**：把光源類型的差異隔離在「如何計算 L 和 radiance」，BRDF 本身完全通用。

---

## 二、光照衰減函式群

光源不能無限遠都一樣亮 —— 這兩個函式處理「光怎麼隨距離/角度變弱」。

---

### 7. `DistanceAttenuation` — 距離衰減（含範圍切斷）

```hlsl
float DistanceAttenuation(float dist, float range)
{
    float d2 = dist * dist;
    float r2 = range * range;
    float num = saturate(1.0f - (d2 * d2) / (r2 * r2));
    return (num * num) / max(d2, 0.0001f);
}
```

#### 做什麼
計算「**距離光源 `dist` 時，光照強度的衰減倍率**」，並在距離超過 `range` 時平滑歸零。

#### 物理 vs 實用
- **物理公式**：$1/d^2$（平方反比定律）—— 但問題是**永遠不會變零**，效能差。
- **這裡用的是 UE4 / Frostbite 的修改版**：
  $$ \text{atten} = \frac{\text{saturate}\left(1 - \frac{d^4}{r^4}\right)^2}{d^2} $$

  - 分子：當 `d` 接近 `range` 時平滑降到 0
  - 分母：保留物理上的平方反比衰減
  - 結果：**有限範圍**內物理合理，邊界處平滑歸零

#### 為什麼這樣設計
- **效能**：超出範圍的像素 atten=0，可被跳過（早期 out）
- **視覺**：沒有突然的硬邊
- **可控性**：美術調整 `range` 直接控制影響範圍

#### 用在哪
```hlsl
float atten = DistanceAttenuation(dist, pl.range);
float3 radiance = pl.color * pl.intensity * atten;
```
**點光源**和**聚光源**都用它計算距離衰減。

---

### 8. `SpotAngleAttenuation` — 聚光錐角衰減

```hlsl
float SpotAngleAttenuation(float3 L, float3 spotDir, float innerCos, float outerCos)
{
    float cosAngle = dot(-L, spotDir);
    return saturate((cosAngle - outerCos) / max(innerCos - outerCos, 0.001f));
}
```

#### 做什麼
讓**聚光燈**有一個錐形範圍，並在內外角之間產生柔邊。

#### 物理直覺
聚光燈（手電筒、舞台燈）有：
- **內角**（innerCone）：核心明亮區，光照 100%
- **外角**（outerCone）：邊緣淡出區，光照 100% → 0%
- **外角之外**：完全黑暗

```
       光源
        ▼
       ╱│╲              innerCone（亮區，atten=1）
      ╱ │ ╲     ╳     ╳ outerCone（淡出區）
     ╱  │  ╲ ╳     ╳   完全黑（atten=0）
    ╱   │   ╳     ╳
```

#### 數學
1. 計算光線方向與聚光方向的夾角餘弦：`cosAngle = dot(-L, spotDir)`
2. 在內外角之間做**線性插值**：
   - `cosAngle ≥ innerCos` → 1（在內角內，全亮）
   - `cosAngle ≤ outerCos` → 0（外角外，全黑）
   - 之間：線性過渡

#### 為什麼用 cos 而不是角度？
比較 cos 值不需要呼叫 `acos`（昂貴），且 dot 結果本來就是 cos —— **效能優化**。

#### 用在哪
```hlsl
float spot = SpotAngleAttenuation(L, normalize(sl.direction),
                                  sl.innerConeAngleCos, sl.outerConeAngleCos);
float3 radiance = sl.color * sl.intensity * atten * spot;
```
**只有聚光源**用它。

---

## 三、頂點著色器

---

### 9. `VSFullscreen` — 全螢幕三角形頂點著色器

```hlsl
VSOut VSFullscreen(uint vid : SV_VertexID) {
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}
```

#### 做什麼
**不需要 vertex buffer**，直接從頂點 ID 推算出一個覆蓋整個螢幕的大三角形。

#### 為什麼用三角形，不用兩個三角形的 quad？
- **效能**：GPU 一次處理 2×2 像素 block，quad 對角線處的像素會被執行兩次
- **簡單**：只需要 3 個頂點，DX12 端 `DrawInstanced(3, 1, 0, 0)` 即可

#### 三個頂點推算過程

| `vid` | `vid << 1` | `(vid<<1)&2` | `vid & 2` | `uv` | `pos` |
|-------|-----------|--------------|-----------|------|-------|
| 0 | 0 | 0 | 0 | (0, 0) | (-1, 1) |
| 1 | 2 | 2 | 0 | (2, 0) | (3, 1) |
| 2 | 4 | 0 | 2 | (0, 2) | (-1, -3) |

這個三角形**比螢幕大很多**，超出 [-1,1] 的部分被自動裁掉，剩下的剛好覆蓋整個畫面。

#### UV 翻轉的 `2.0f, -2.0f`
DirectX 的 UV 原點在**左上**，但 NDC 的 Y 軸**朝上**，所以 Y 要乘 -2 來翻轉。

#### 用在哪
**Lighting Pass 唯一的頂點著色器**，每個像素著色器執行一次都從這裡來。

---

## 四、世界座標重建

---

### 10. `ReconstructWorldPos` — 從深度反推世界座標

```hlsl
float3 ReconstructWorldPos(float2 uv, float depth) {
    float4 ndc = float4(uv * 2.0f - 1.0f, depth, 1.0f);
    ndc.y = -ndc.y; // DX UV convention
    float4 worldPos = mul(ndc, gInvViewProj);
    return worldPos.xyz / worldPos.w;
}
```

#### 做什麼
**G-Buffer 不存 world position**（會浪費頻寬），而是從**深度紋理 + UV** 反推回來。

#### 為什麼可以反推？
任何螢幕像素都對應一條從相機出發的射線。`depth` 告訴你這條射線上「**多遠**」是表面 → 加在一起就確定了世界座標。

#### 步驟分解

**Step 1**：UV → NDC（裁剪空間）
```hlsl
float4 ndc = float4(uv * 2.0f - 1.0f, depth, 1.0f);
```
- UV 的範圍是 `[0, 1]`，NDC 是 `[-1, 1]` → 線性映射
- 深度本來就在 `[0, 1]` 範圍（DX 慣例），直接用

**Step 2**：翻轉 Y（DX 慣例調整）
```hlsl
ndc.y = -ndc.y;
```
DX 的 UV 原點在左上，NDC 的 Y 朝上 → 必須翻轉。

**Step 3**：乘上反 ViewProjection 矩陣
```hlsl
float4 worldPos = mul(ndc, gInvViewProj);
```
正向流程是：`world → view → projection → NDC`
反向流程是：`NDC → world`，所以乘 `(ViewProj)⁻¹`

**Step 4**：透視除法
```hlsl
return worldPos.xyz / worldPos.w;
```
反矩陣會把齊次座標還原成 `(x×w, y×w, z×w, w)`，必須除以 w 才能拿到實際世界座標。

#### 用在哪
```hlsl
float3 posW = ReconstructWorldPos(pin.uv, depth);
```
PSMain 開頭呼叫一次，後續的光照計算（距離、方向、陰影矩陣）全部都要用 `posW`。

#### 為什麼這個技巧很重要？
- **省記憶體**：不用在 G-Buffer 再加一張 `float3` 的 position 紋理（每個 1080p 像素省 12 byte = 約 24MB）
- **省頻寬**：寫入和讀取 G-Buffer 是延遲渲染的主要瓶頸

---

## 五、主像素著色器

---

### 11. `PSMain` — 主光照計算函式

```hlsl
float4 PSMain(VSOut pin) : SV_TARGET { ... }
```

這是**整個 shader 的主入口**，每個螢幕像素執行一次。它本身不算「函式」，而是把上面所有函式整合起來。我把它分成 9 個區塊講：

#### 區塊 1：跳過天空像素
```hlsl
float depth = gDepthTex.Sample(gSampPoint, pin.uv);
if (depth >= 1.0f) discard;
```
深度 = 1.0 表示遠平面，沒物件 → `discard` 直接放棄這個像素（讓天空盒處理）。

#### 區塊 2：取樣 G-Buffer
```hlsl
float3 albedo   = gAlbedoTex.Sample(...).rgb;
float3 N        = normalize(gNormalTex.Sample(...).xyz);
float4 matData  = gMaterialTex.Sample(...);
float metallic  = matData.r;
float roughness = matData.g;
float ao        = matData.b;
float3 emissive = gEmissiveTex.Sample(...).rgb;
```
從 5 張 G-Buffer 紋理把這個像素的「材質資料」全部讀出來。

#### 區塊 3：重建世界座標 + 計算共用向量
```hlsl
float3 posW = ReconstructWorldPos(pin.uv, depth);
float3 V = normalize(gCameraPos.xyz - posW);
float3 F0 = lerp(float3(0.04f), albedo, metallic);
```
- `V` = 視線方向（從表面指向相機）
- `F0` = 基準反射率（非金屬 0.04，金屬用 albedo）

#### 區塊 4：太陽光直接光照
```hlsl
float3 sunL = normalize(-raysDir);
float3 sunRadiance = gLightColor.rgb * gLightDirIntensity.w;
float3 color = EvaluateBRDF(N, V, sunL, sunRadiance, ...);
```
呼叫 `EvaluateBRDF` 算太陽光的貢獻。

#### 區塊 5：CSM 層級陰影貼圖
- 用 `viewZ` 選擇陰影層級
- 把世界座標轉到光源裁剪空間
- 做 **3×3 PCF**（9-tap 軟陰影）
- 結果 `shadowFactor ∈ [0, 1]` 乘到 `color`

#### 區塊 6：Debug 彩色層級
若啟用 `gCascadeDebug`，每個 CSM 層級染上不同顏色，方便美術/工程師檢查切割是否合理。

#### 區塊 7：點光源迴圈
```hlsl
for (uint i = 0; i < numPointLights; ++i) {
    PointLight pl = gPointLights[i];
    // 計算方向、距離、衰減
    color += EvaluateBRDF(...);
}
```

#### 區塊 8：聚光源迴圈
和點光類似，但多乘了 `SpotAngleAttenuation`。

#### 區塊 9：IBL 環境光（Split-Sum 近似）
- **漫反射 IBL**：從 `gIrradianceMap` 用法線方向取樣
- **鏡面 IBL**：從 `gPrefilteredMap` 用反射方向取樣（mip = roughness×4）
- **BRDF LUT**：用 `(NdotV, roughness)` 查表
- 最後乘上 AO（**只衰減環境光**，不影響直接光）

#### 區塊 10：加上自發光
```hlsl
color += emissive;
```
自發光不受任何光照影響，直接疊加。

---

## 六、整體呼叫關係圖

```
PSMain (主入口)
  │
  ├── ReconstructWorldPos          ← 反推世界座標
  │
  ├── EvaluateBRDF (太陽光)         ← 直接光 BRDF
  │     ├── FresnelSchlick         ← F 項
  │     ├── DistributionGGX        ← D 項
  │     └── GeometrySmith          ← G 項
  │           └── GeometrySchlickGGX
  │
  ├── CSM 陰影採樣（直接寫在主函式內，沒抽函式）
  │
  ├── 點光源迴圈
  │     ├── DistanceAttenuation     ← 距離衰減
  │     └── EvaluateBRDF
  │
  ├── 聚光源迴圈
  │     ├── DistanceAttenuation
  │     ├── SpotAngleAttenuation    ← 錐角衰減
  │     └── EvaluateBRDF
  │
  └── IBL 環境光區塊
        └── FresnelSchlickRoughness ← IBL 專用 F 項
```

---

## 七、函式速查表

| 函式 | 一句話用途 | 何時呼叫 |
|------|----------|---------|
| `FresnelSchlick` | 算鏡面反射比例（F 項）| 直接光的 BRDF |
| `FresnelSchlickRoughness` | 帶粗糙度修正的 F | IBL 環境光 |
| `DistributionGGX` | 算微表面朝向密度（D 項）| 直接光的 BRDF |
| `GeometrySchlickGGX` | 單方向遮蔽 | `GeometrySmith` 內部 |
| `GeometrySmith` | 雙方向遮蔽（G 項）| 直接光的 BRDF |
| `EvaluateBRDF` | **核心**：算單一光源的最終貢獻 | 太陽 + 每盞點光 + 每盞聚光 |
| `DistanceAttenuation` | 距離衰減（帶範圍切斷）| 點光 + 聚光 |
| `SpotAngleAttenuation` | 聚光錐角衰減 | 只有聚光 |
| `VSFullscreen` | 全螢幕三角形頂點 | 每幀渲染前執行 3 次 |
| `ReconstructWorldPos` | 從深度反推世界座標 | `PSMain` 開頭 |
| `PSMain` | 主入口，整合所有計算 | 每個非天空像素一次 |

---

如果你想深入了解某一個函式的**數學推導**（例如 GGX 怎麼推來的、菲涅耳完整方程怎麼推出 Schlick 近似），或者想看**對應的 C++ root signature / descriptor heap 設定**，告訴我下一步。