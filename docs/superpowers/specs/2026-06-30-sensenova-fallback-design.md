# SenseNova Cloud Vision Fallback Design

## 概述

为 Trash App 增加 SenseNova 云端视觉保底方案：当 TFLite 本地推理置信度 < 60% 时，自动切换为云端推理结果。TFLite + 云端**并发执行**，取较优结果。

## 架构

```
裁剪确认
  │
  ├─ async TFLite.infer(cropped)       ← 本地
  │
  ├─ async SenseNova.infer(cropped)    ← 云端 (aallam/openai-kotlin)
  │
  ▼ awaitAll
  │
  TFLite.confidence ≥ 0.6 ? → 用 TFLite
  TFLite.confidence < 0.6  ? → 用 SenseNova
  │
  ▼
  发 0x32 TrashResult → ESP32
```

## Python 先行验证

先写 `sensenova-trash.py` 测试 prompt 效果和 JSON 格式稳定性。

### 调用方式

```bash
python sensenova-trash.py <image_path>
```

输出严格 JSON：
```json
{
  "label": "苹果核",
  "category": "厨余垃圾",
  "confidence": 0.88,
  "topK": [
    ["苹果核", 0.88],
    ["香蕉皮", 0.06],
    ["茶叶渣", 0.03],
    ["纸巾", 0.02],
    ["剩饭", 0.01]
  ]
}
```

### Prompt 设计

**system prompt**（严格约束格式）：
```
你是一个垃圾分类识别专家。你的任务是识别图片中的物品，判断它属于哪个垃圾类别。

垃圾分类类别（严格使用以下四类）：
- 可回收物 (Recyclable)
- 厨余垃圾 (Kitchen Waste)
- 有害垃圾 (Hazardous)
- 其他垃圾 (Other)

你必须只输出一个JSON对象，不要包含任何其他文字、markdown、代码块标记。

JSON格式：
{
  "label": "物品中文名称（如'塑料瓶'、'苹果核'、'电池'）",
  "category": "四类之一",
  "confidence": 0.XX,
  "topK": [
    ["最可能物品", 0.XX],
    ["第二可能", 0.XX],
    ["第三可能", 0.XX],
    ["第四可能", 0.XX],
    ["第五可能", 0.XX]
  ]
}

约束：
- label 是具体物品名称
- category 必须是"可回收物"/"厨余垃圾"/"有害垃圾"/"其他垃圾"之一
- confidence 是0-1浮点数，最高项的置信度
- topK 包含5个候选项，每项是[物品名, 置信度]
- 输出必须合法JSON，无markdown包裹
- 如果不确定，选择最接近的类别
```

**temperature**: 0.1（最低随机性，确保输出稳定）

## Android 集成

### 新增依赖

**build.gradle.kts** 添加：
```kotlin
// OpenAI Kotlin client (用于 SenseNova 云端视觉保底)
implementation("com.aallam.openai:openai-client:4.1.0")
implementation("io.ktor:ktor-client-okhttp:3.0.3")
```

### 新增文件

```
inference/SenseNovaFallback.kt
```

```kotlin
object SenseNovaFallback {
    private const val API_KEY = "sk-ZSfT6X1tnuqZOEvP2eTzSagelJ5gmzTF"
    private const val BASE_URL = "https://token.sensenova.cn/v1"
    private const val MODEL = "sensenova-6.7-flash-lite"

    private val client = OpenAI(
        token = API_KEY,
        config = {
            baseUrl = BASE_URL
        }
    )

    suspend fun infer(bitmap: Bitmap): DetectionResult? {
        // 1. Bitmap → base64 JPEG
        val baos = ByteArrayOutputStream()
        bitmap.compress(Bitmap.CompressFormat.JPEG, 85, baos)
        val b64 = Base64.encodeToString(baos.toByteArray(), Base64.NO_WRAP)

        // 2. 构造 chat completion request (vision)
        val request = ChatCompletionRequest(
            model = ModelId(MODEL),
            messages = listOf(
                ChatMessage(role = ChatRole.System, content = SYSTEM_PROMPT),
                ChatMessage(
                    role = ChatRole.User,
                    content = listOf(
                        ImagePart(imageUrl = ImagePart.ImageURL("data:image/jpeg;base64,$b64")),
                        TextPart("识别这张图片中的垃圾")
                    )
                )
            ),
            temperature = 0.1,
            maxTokens = 500
        )

        return try {
            val response = client.chatCompletion(request)
            val text = response.choices.first().message.content ?: return null
            parseResponse(text)
        } catch (e: Exception) {
            Log.e(TAG, "SenseNova fallback failed", e)
            null
        }
    }

    private fun parseResponse(json: String): DetectionResult? {
        // 解析 JSON → DetectionResult
        val obj = JSONObject(json)
        val label = obj.getString("label")
        val category = obj.getString("category")
        val confidence = obj.getDouble("confidence").toFloat()
        val topKArr = obj.getJSONArray("topK")
        val topK = (0 until topKArr.length()).map { i ->
            val item = topKArr.getJSONArray(i)
            item.getString(0) to item.getDouble(1).toFloat()
        }
        return DetectionResult(label, category, confidence, topK)
    }
}
```

### TrashScreen.kt 修改

在 `doInference()` 中改为并发执行：

```kotlin
fun doInference(cropRect: Rect) {
    scope.launch {
        state = TrashState.Inferring
        statusText = "识别中..."

        // 裁剪
        val cropped = Bitmap.createBitmap(
            frozenFrame!!, cropRect.left, cropRect.top,
            cropRect.width(), cropRect.height()
        )

        // 并发：本地 TFLite + 云端 SenseNova
        val tfliteDeferred = async(Dispatchers.IO) {
            engine.infer(cropped)
        }
        val cloudDeferred = async(Dispatchers.IO) {
            SenseNovaFallback.infer(cropped)
        }

        val tfliteResult = tfliteDeferred.await()
        val cloudResult = cloudDeferred.await()

        // 选结果：TFLite 置信度 ≥ 60% 优先，否则用云端
        val finalResult = when {
            tfliteResult != null && tfliteResult.confidence >= 0.6f -> {
                Log.i(TAG, "Using TFLite: ${tfliteResult.label} ${tfliteResult.confidence}")
                tfliteResult
            }
            cloudResult != null -> {
                Log.i(TAG, "Using SenseNova: ${cloudResult.label} ${cloudResult.confidence}")
                cloudResult
            }
            tfliteResult != null -> {
                Log.i(TAG, "TFLite fallback (low conf): ${tfliteResult.label}")
                tfliteResult
            }
            else -> null
        }

        detectionResult = finalResult
        // ... 后续发 0x32 TrashResult
    }
}
```

## SenseNova Prompt（完整中文）

```
你是一个垃圾分类识别专家。你的任务是识别图片中的物品，判断它属于哪个垃圾类别。

垃圾分类类别（严格使用以下四类）：
- 可回收物 (Recyclable)
- 厨余垃圾 (Kitchen Waste)
- 有害垃圾 (Hazardous)
- 其他垃圾 (Other)

你必须只输出一个JSON对象，不要包含任何其他文字、markdown、代码块标记。

JSON格式：
{
  "label": "物品中文名称（如'塑料瓶'、'苹果核'、'电池'）",
  "category": "四类之一",
  "confidence": 0.XX,
  "topK": [
    ["最可能物品", 0.XX],
    ["第二可能", 0.XX],
    ["第三可能", 0.XX],
    ["第四可能", 0.XX],
    ["第五可能", 0.XX]
  ]
}

约束：
- label 是具体物品中文名称
- category 必须是"可回收物"/"厨余垃圾"/"有害垃圾"/"其他垃圾"之一
- confidence 是0-1浮点数，最高项的置信度
- topK 包含5个候选项，每项是[物品名, 置信度]
- 输出必须合法JSON，无markdown包裹
- 如果不确定，选择最接近的类别
```

## 错误处理

| 场景 | 处理 |
|------|------|
| 云端超时/网络不可用 | 无视云端结果，只用 TFLite |
| JSON 解析失败 | 云端结果无效，回退 TFLite |
| 双端都失败 | 发 0x32 候选数=0 |
| API key 无效 | Log 警告，云端结果丢弃 |
| 图片太大 | Bitmap.compress JPEG 85% 控制 < 1MB |

## 自检

- ✅ 并发执行：TFLite 和云端 async/await 互不阻塞
- ✅ 回退逻辑：三层兜底（TFLite高置信→云端→TFLite低置信→null）
- ✅ 零依赖冲突：openai-client + ktor-client-okhttp 仅新增两个依赖
- ✅ Prompt 严格约束 JSON 输出 + temperature=0.1
- ✅ 模型名/API key/base_url 全用常量，改 SenseNova 端点和模型只需改常量