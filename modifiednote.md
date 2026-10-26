# LiteRT-LM v0.18.0 之后的修改说明

> 发布状态更新（2026-10-26）：本文下方保留 2026-10-15 的历史审计记录，其中“工作区未提交”描述的是当时状态。相关核心源码、测试及本文现纳入 `fix/npu-nonspeculative-token-scrub` 分支提交；Android 运行时扩展仍位于独立的 annoymous 仓库。构建产物不纳入本次提交。

> 本文记录截至 2026-10-15 已实现的修改、修改原因、验证证据与已知限制。面向代码审查和 memory forensics 实验复现。
>
> 核心目标是减少专用 NPU 执行器中的明文 Token ID 历史残留，并提供可比较的原始行为和清理行为。**这不是整个进程、全部 Token 副本或物理 DRAM 的安全擦除保证。**

## 1. 基线、提交和交付范围

### 1.1 原始版本

本地 `v0.18.0` 标签实际指向：

```text
b2f686e2ed4718fb84ec398a61dd59ca0f0aff27
Add an --output_size flag to embedding_litert_lm_main for truncation of the output embedding.
```

`f8fb0445` 是添加 v0.18.0 release notes 的上游提交，但本仓库的版本标签指向其后的 `b2f686e2`。这里以上述标签为比较基线，不把基线已经包含的上游 embedding 改动算作我们的修改。

当前工作分支为 `fix/npu-nonspeculative-token-scrub`。标签之后的两个本地提交为：

| 提交 | 内容 | 最初解决的问题 |
| --- | --- | --- |
| `309ce4ced5b605aab3eecaf51fb3404668223305` | Reset / RestoreContext 时清理 Token 数组 | 仅缩短 vector 的 size，旧 ID 仍留在 capacity 内 |
| `87203e8fdba7a951660cb57c7c9f67b6bf7dead3` | 会话运行期间覆盖已消费 Token ID | 等到会话结束才清理，无法减少运行中内存取证暴露 |

这两个提交最初无条件清理。后续工作区修改增加了默认关闭的开关，使 reviewer 可以在同一个构建中比较原始与清理行为。

### 1.2 为什么不能只看 HEAD

当前实现分为以下几部分：

| 部分 | 保存位置 | 状态 |
| --- | --- | --- |
| 最初两项 Token 清理 | 上述两个 Git 提交 | 已提交 |
| 漏掉的非推测解码修复、配置开关、C/CLI 接口和测试 | 当前 LiteRT-LM 工作区 | 尚未形成新提交 |
| 当前轮多模态、实时检查、导出、只读地址元数据 | [`../annoymous/runtime-extension/`](../annoymous/runtime-extension/) | 作为应用运行时扩展源码和补丁保存 |
| Pixel 的 Google Tensor dispatch 兼容修改 | [`../annoymous/native-patches/`](../annoymous/native-patches/) | 修改 fetched LiteRT 的构建依赖，不属于 LiteRT-LM 核心算法 |
| Android/JNI、模式和模型选择、旧地址读取、界面 | [`../annoymous/app/src/`](../annoymous/app/src/) | 调用上述运行时的验证应用 |
| 分配追踪、写监视点、分阶段释放和定时实验 | [`../annoymous/diagnostics/`](../annoymous/diagnostics/) | 诊断材料；临时 hook / JNI 不保留在当前 APK |

APK 使用的 `liblitert-lm.so` 是在独立的 Android 构建 checkout 中应用扩展后生成的。**当前主仓库的 HEAD 或工作区本身，不等于 APK 全部运行时扩展都已经合入。** 核心清理补丁与应用扩展应分别审查。

本说明没有新增提交、修改版本标签或改动运行时算法。

## 2. 原始问题与内存对象模型

### 2.1 `size=0` 不代表字节消失

`ProcessedTokens` 为每个 candidate 保存 `std::vector<int> token_ids`。原始逻辑中的：

```cpp
t.token_ids.resize(0);
```

只改变有效元素数量。capacity 通常保留，先前写入的 ID 字节可能仍在那块分配中。`RestoreContext()` 的 vector 赋值也可能复用旧 capacity，使较长旧历史的尾部字节留在备用区。

需要区分以下概念：

| 概念 | 意义 |
| --- | --- |
| size | 当前有效元素数量，也参与 step / position 计算 |
| capacity | 当前数组分配可以容纳的元素数量 |
| spare | `[size, capacity)` 的原始存储，不是有效 Token 历史 |
| frontier | 当前必须保留、下一步仍可能使用的边界 Token |
| pending | 独立 `TokenData` 对象中的下一步输入 ID，不是 token_ids 数组的最后一格 |
| 释放后的旧地址 | 原对象生命周期已结束；该虚拟地址可失效，也可被其他对象复用 |

### 2.2 防御目标

原 Token ID 历史可被 tokenizer vocabulary 直接解释，比 KV 更容易还原为文本。我们的修改优先缩短专用 NPU 路径中这份直接历史的保留时间。

KV 继续保留上下文，支持持续会话。**清理 Token ID 不等于清理 KV，也不等于消除模型记忆或阻止从 KV 进行信息恢复。** 明文 prompt、回复字符串、tokenizer 中间结果、sampler 临时值及其他副本也是不同的对象。

## 3. 核心清理实现

### 3.1 Reset 和上下文切换：`ScrubAll()`

涉及：

- [`runtime/executor/llm_executor_processed_tokens.cc`](runtime/executor/llm_executor_processed_tokens.cc)
- [`runtime/executor/llm_executor_processed_tokens.h`](runtime/executor/llm_executor_processed_tokens.h)
- [`runtime/executor/llm_litert_npu_compiled_model_executor.cc`](runtime/executor/llm_litert_npu_compiled_model_executor.cc)

对所有 candidate 的数组执行：

```cpp
const size_t cap = t.token_ids.capacity();
t.token_ids.resize(cap);
std::fill(t.token_ids.begin(), t.token_ids.end(), 0);
t.token_ids.clear();
t.token_ids.shrink_to_fit();
t.pending_input_token = nullptr;
```

原因：覆盖整个 `[0, capacity)`，而不是只覆盖当前 `[0, size)`。Reset 中原逻辑先 `RollBackToStep(0)`；若只按当时 size 清理，就会完全漏掉旧存储。

调用位置：

1. `Reset()`：清理本执行器的 processed-token 数组。
2. `RestoreContext()`：在旧数组被新上下文赋值复用前清理它。

当前两个调用都受 `npu_config_.enable_token_id_scrubbing` 控制。

边界：

- `pending_input_token = nullptr` 是释放引用，不是覆盖 `TokenData::id_`；存在其他引用时，对象还可继续存活。
- `shrink_to_fit()` 在 C++ 标准中是非强制请求，不能对所有标准库保证 capacity 一定归零。本次 Pixel / libc++ 测试观察到了清理路径释放 capacity 的行为。
- 实现使用普通 `resize` / `std::fill`，没有增加 `explicit_bzero` 一类明确防止清零优化消除的原语；不能宣称跨编译器的安全擦除证明。
- “所有 candidate”仅指此 `ProcessedTokens` 实例拥有的数组，不代表所有 Token 副本。

### 3.2 会话内：`ScrubConsumedValues(keep_from)`

对每个 candidate 的 `[0, min(keep_from, size))` 扫描，将非负 ID 写为 0。

保持以下状态不变：

- vector 的 size、step 和位置计数；
- `keep_from` 及之后的位置；
- 独立 pending 输入对象；
- 负数多模态 sentinel，它们参与 mask / 占位逻辑。

调用使用 `keep_from = current_step_ - 1`，保留必要 frontier。原始 Token 值被覆盖后，数组长度仍反映位置，因此不会把会话当作空上下文。

设计理由：专用 NPU 路径中，已经进入 KV 的旧 ID 不再需要作为正常下一步输入；删除 vector 元素却会破坏位置计数。这里修改值而不改变长度。

性能：每次调用扫描历史前缀，开销为 `O(candidate 数 × 前缀长度)`；连续长会话的累计扫描工作可能呈二次增长。本轮没有独立性能基准，不能仅凭推理耗时较长就宣称其开销可忽略。

### 3.3 补齐 `DecodeNonSpeculative()` 漏点

最初 live scrub 提交覆盖了五个推进位置，但遗漏了无约束的 `DecodeNonSpeculative()` 分支。普通聊天可能正好走这条路径，造成回答后的已消费 ID 仍留在数组里。

修复：在该分支推进 `current_step_`、设置 `ran_decode_` 后，同样调用受开关控制的 `ScrubConsumedValues(current_step_ - 1)`。受约束分支通过 `DecodeLogits()` 清理。

当前六个 live 调用点：

| 方法 | 覆盖场景 |
| --- | --- |
| `DecodeLogits()` | 返回 logits 的解码路径 |
| `DecodeNonSpeculative()` | 非推测、无约束解码补漏 |
| `PopPendingAcceptedToken()` | 从已接受 Token 队列推进 |
| `Decode()` | 推测解码的推进路径 |
| `PrefillInternal()` | 新输入 prefill 后 |
| `DecodeSingleToken()` | 单 Token 推进后 |

应用运行时补丁：[`npu-nonspeculative-scrub.patch`](../annoymous/runtime-extension/npu-nonspeculative-scrub.patch)。该修复也存在于主仓库工作区；它不是单纯的 UI 显示修改。

### 3.4 纠正过强的注释

最初头文件曾写成清理可保证 Token ID 不再存在于整个进程内存。这超出了实现范围。当前注释已改为：仅覆盖该 vector 的存储，不涵盖其他分配和副本。

## 4. 可比较的开关：默认保持原始行为

### 4.1 配置传播

| 接口层 | 新增项 | 默认值 |
| --- | --- | --- |
| executor | `NpuConfig::enable_token_id_scrubbing` | `false` |
| engine helper | `LiteRtLmSettings::enable_npu_token_id_scrubbing` | `false` |
| CLI | `--enable_npu_token_id_scrubbing` | `false` |
| C API | `litert_lm_engine_settings_set_npu_token_id_scrubbing()` | 不调用即关闭 |

原因：让 reviewer 不必切换分支或重编译，就能比较相同模型、相同输入下原始保留行为与清理行为。

| 配置 | 运行中旧 ID | Reset / outgoing context 的 Token 数组 |
| --- | --- | --- |
| 省略或 false | 原始保留行为 | 原始数组处理行为 |
| true | 覆盖已消费的非负 ID | 执行本实例的 `ScrubAll()` |

开关覆盖全部六个 live 调用和两个 `ScrubAll()` 调用；不会改变原来的 KV 清理策略。

新增配置字段放在结构末尾，避免已有位置式 aggregate 初始化参数改变含义；`NpuConfig` 的文本输出也包含新字段。这是源码初始化兼容性措施，**不是跨版本 C++ 二进制 ABI 保证**。

### 4.2 CLI / C / C++

两个 CLI（`litert_lm_main`、`litert_lm_advanced_main`）均支持：

```sh
--backend=npu --enable_npu_token_id_scrubbing=true
```

C 调用者在创建引擎前设置：

```c
int rc = litert_lm_engine_settings_set_npu_token_id_scrubbing(settings, true);
if (rc != 0) {
  /* 处理错误；不要忽略失败后继续声称启用了清理。 */
}
LiteRtLmEngine* engine = litert_lm_engine_create(settings);
```

C setter 为本 fork 扩展，成功返回 0；空 settings 或非 NPU 配置返回 -1，并设置 last error。`LiteRtLmSettings` 经 `CreateEngineSettings()` 将设置传入 `NpuConfig`；直接 C++ 调用者也可设置已有 backend config。

### 4.3 不支持的路径明确报错

只有专用 `TF_LITE_AUX` NPU executor 实现本清理行为。

- CLI / engine helper 对非 NPU 后端启用 true 报错。
- C setter 对非 NPU 配置报错。
- 缺少 `TF_LITE_AUX`、需要 generic compiler-plugin executor 的模型，在启用 true 时返回 Unimplemented。
- 关闭时保留原 generic fallback 行为。

原因：避免用户看到“清理已开启”，实际却走了没有实现清理的执行器。

相关文件包括 `shared_flags.{h,cc}`、`litert_lm_lib.{h,cc}`、两个 main、`llm_executor_settings.{h,cc}`、executor factory 和 `c/engine.{h,cc}`。详见 [`docs/npu_token_id_scrubbing.md`](docs/npu_token_id_scrubbing.md)。

## 5. Android 可部署库与 Pixel 兼容

### 5.1 交付形态

构建 `//c:litert-lm` 得到 Android ARM64 的 `liblitert-lm.so`。它是动态库，不是能单独在 Pixel 上执行的命令行程序。Ghost APK 通过 Java → JNI → C API 加载、创建引擎和调用模型。

应用打包：

- `liblitert-lm.so`；
- `libannoymous.so`（应用 JNI bridge）；
- `libLiteRtDispatch_GoogleTensor.so`；
- `libGemmaModelConstraintProvider.so`。

构建配置使用 Android API 31，并给本地库设置 `-Wl,-z,max-page-size=16384` LOAD 对齐。模型文件独立于 APK，不嵌入安装包。

### 5.2 闪退修复：dispatch 必须匹配运行时

曾使用旧 v2.2.0 Google Tensor dispatch 预编译库。它的函数表缺少运行时预期的 `get_hooks` 成员，初始化时发生非法访问 / SIGSEGV。

处理：使用与 runtime 相同的 LiteRT revision `26895c9fbcc25c43faa8c1a98cd1fd28951602c3` 构建 dispatch，替换旧库。公开构建去掉不可用的内部 profiling hooks 依赖：

[`native-patches/google-tensor-oss-build.patch`](../annoymous/native-patches/google-tensor-oss-build.patch)

该补丁改变的是 LiteRT dispatch 的 BUILD 依赖，不是 Token 清理算法。不能把修复描述为回退后端或禁用清理。

应用显式设置 dispatch library directory，并声明可选 vendor 库 `libedgetpu_litert.so`、`libedgetpu_util.so`。旧 dispatch 不应混入当前运行时。

## 6. 当前轮多模态 Session 扩展

### 6.1 使用 Session 的原因

用户要求上下文通过 KV 持续累加，直到创建新会话，同时应用不保存对话历史列表、只显示最新回复。

因此采用 Session，而不是应用维护历史 messages 并逐轮重放。应用层不保留文本历史，不代表运行时或 Java/native 临时分配没有残留。

### 6.2 `ghost_session_generate_multimodal()`

新增应用专用 C 入口，保存在：

[`runtime-extension/ghost_multimodal.cc`](../annoymous/runtime-extension/ghost_multimodal.cc)

它从引擎 tokenizer 和 Session 模型类型创建 model data processor，只构造本轮 text / image / audio 输入，调用现有 `Session::GenerateContent()`，不创建 Conversation 或历史快照。

`c/BUILD` 增加该源码及 model data processor 依赖。安装器 [`install.py`](../annoymous/runtime-extension/install.py) 完成复制与 BUILD 修改；[`c-build.patch`](../annoymous/runtime-extension/c-build.patch) 是同一构建接入的静态记录，不需要与安装器重复应用。

E2B 文本解码走 NPU，图像和音频编码器使用 CPU。Session 配置关闭再次套 prompt template，由本轮接口构造模型格式，避免重复模板。

无文本附件的默认 prompt 改为聊天意图：音频作为用户说的话自然回答，图片结合当前上下文理解意图；意图不明时简短询问，不默认翻译、转录或泛泛描述。用户输入的文字优先。

临时图片/音频在发送、移除、新会话或关闭后删除，启动时清理残留附件。附件文件删除本身不承诺底层存储安全擦除。

## 7. 实时 Token / KV 检查与导出

### 7.1 为什么新增运行时检查入口

不能靠应用重新 tokenize 回复来模拟“内存中的 ID”，也不能把应用逐轮记录当作 memory forensic 结果。需要通过 Session 找到当前实际 executor，再检查它拥有的存储。

补丁：[`runtime-debug.patch`](../annoymous/runtime-extension/runtime-debug.patch)。它增加：

- `Engine::Session` 和 `LlmExecutor` 的 `GetGhostDebugJson()` 虚方法；默认返回 Unimplemented。
- `SessionAdvanced` → ExecutionManager → ResourceManager proxy → NPU executor 的检查转发。
- serial / threaded 两种 manager 通过 `AcquireExecutorWithContextHandler()` 获得对应 executor，再读取数据。
- `TokenData::GhostIdAddress()` 和 `ProcessedTokens::GhostStorageViews()`，暴露锁持有期间有效的非 owning 地址、size、capacity 和 pending 地址。
- NPU executor 的 Token、KV inspect 和导出实现，以及 executor BUILD 的 JSON 依赖。
- `ghost_session_debug()` C 入口，将 JSON 查询转入 Session 检查接口。

应用在生成空闲时才允许检查或导出，不在每轮生成后自动捕获数据。Debug 默认关闭，不等于清理开关。

### 7.2 Token 显示修正

Processed 与 Spare 共用数组基址，分别对应 `[0,size)` 和 `[size,capacity)`；Pending 单独显示地址和逻辑位置。每格显示 int32，详情提供 candidate、index、偏移和小端原始字节。

原因：先前把数组、备用容量和 pending 按屏幕顺序排列，容易让人误以为它们物理连续，或者误认为所有非零数都是有效 ID。

**颜色表示逻辑区域，不证明是否安全擦除。** Token 数值 106、37 等，必须结合该模型词表、存储区域和当时状态解释，不能仅凭数值把它归为 pending 或泄漏。

### 7.3 KV 读取

界面按 layer → K/V buffer → physical slot → components 展开。这里的 physical slot 是缓冲区槽位索引，不是 DRAM 物理地址。

读取使用 `TensorBufferScopedLock(kRead)` 得到 CPU 映射，依据 shape、实际声明的 strides、sequence axis、element width 和 allocation size 计算偏移。ringbuffer 的 slot 与逻辑 position 分开显示。

实现对非零 offset view 明确报 Unimplemented，不猜测底层分配边界；计算出的分量偏移超过 allocation size 时报错。详情显示原始 dtype / raw storage，不将量化值伪装成反量化数值。

默认视图只展示每个槽位少量分量；打开槽位详情时读取完整分量。表格增加固定列宽、数值右对齐、交替底色、固定表头及双向滚动。返回按钮保留父层列表和方块视图。

### 7.4 CSV 与完整原始导出

| 格式 | 内容 | 适用目的 |
| --- | --- | --- |
| Token CSV | 每个 capacity 槽位和独立 pending 的地址、region、index、int32、raw hex | 与方块显示核对 |
| KV CSV | 每个槽位的所有分量 raw hex，以及 layer、shape、dtype、axis、地址和偏移 | 结构化分析；不含 allocation padding |
| `.gkv` | JSON manifest + 完整选定 KV allocation，包含 padding | 保留原始布局和字节 |

`.gkv` 布局：8 字节 `GHOSTKV1`、8 字节小端 manifest 长度、UTF-8 JSON、buffer payload。manifest 的 `payload_offset` 相对二进制 payload 起点。

运行时写入用户通过系统文件选择器提供的 fd，避免应用先缓存完整导出。解析脚本为 [`unpack_kv.py`](../annoymous/runtime-extension/unpack_kv.py)。导出是用户明确创建的持久取证材料，与默认不记录会话历史不同。

实时检查会产生临时 JSON、字符串和界面副本，锁定 TensorBuffer 也可能触发同步或映射；不能宣称观察没有副作用。

## 8. 模式、模型、旧地址与 No model

### 8.1 Ghost / Normal

右上角图标作为模式入口：粉色 Ghost 对应清理 true，蓝色头像 Normal 对应 false。切换模式关闭旧引擎并创建新 Session；不是生成过程中动态修改 executor 配置。

Debug 只控制检查功能，两种模式都可以启用。当前 APK 的初始 Ghost 行为是显式 opt-in；native 库默认仍为 false。

### 8.2 当前模型

| 模型 | 路径 | 应用配置 |
| --- | --- | --- |
| Gemma 4 E2B Tensor G5 | NPU，辅助图像/音频 CPU | 4096-token context，多模态，Ghost / Normal |
| Gemma 3 1B Q8 Tensor G5 | NPU | 1280-token context，纯文本，Ghost / Normal |
| No model | 无引擎 | 只关闭当前 Session / 引擎，不加载替代模型 |

Gemma 3 文件 `Gemma3-1B-IT_q8_ekv1280_Google_Tensor_G5.litertlm`，Hugging Face revision `d448835d84411a08f577e25f3afebe528402d73b`，SHA-256 `1ed29548b302764ce32ebf03d7df8fff943218b76d14230e97ee4bd0224cd8d1`。

JNI 为 Gemma 3 禁用图像/音频编码器，否则创建引擎会因缺少 `tf_lite_audio_encoder_hw` 失败；同时使用 Gemma 3 的 `<start_of_turn>` / `<end_of_turn>` 当前轮模板，而非 E2B 的 Gemma 4 模板。

曾接入 E4B 通用 CPU 版本，因没有找到对应 G5 专用构建，未把它标为支持本 NPU 清理。按用户要求，该模型文件、入口和下载脚本随后移除。公开 INT4 Gemma 3 的临时下载也未成为当前部署模型。

### 8.3 只保留旧地址元数据

[`address-metadata.patch`](../annoymous/runtime-extension/address-metadata.patch) 新增 `format=addresses`。仅返回数组地址、candidate、size、capacity 和 pending 地址，提前返回，不读取 Token 值，也不导出 KV 内容。

原因：为了切换后找到旧分配，不应先复制一份旧 Token 快照，再把快照当作实际内存。

应用 `RetiredMemory` 在 Debug 开启时保留上一引擎的一组地址元数据，不持久化到磁盘；关闭 Debug 清除元数据。已查看过的 KV CPU 映射地址可一并保留，但它们的锁已经释放、映射可能失效，不是设备物理地址。

按需读取旧地址通过 Linux `process_vm_readv` 访问本进程，去掉 Android 指针 top-byte tag，不直接解引用已释放指针。单次 native 读取限制 4096 字节，界面每页读取最多 256 字节；不可读或部分读取都应按实际结果解释。

旧 Token 方块沿用原网格样式，显示序号、当前 int32、地址、字节偏移和 raw bytes。粉色 / 灰色表示该位置原来属于 processed/pending 或 spare，**不代表当前内存仍属于原 Token 对象**。

### 8.4 No model

新增 No model 是为区分“卸载行为”与“加载新模型造成的复用”：先按 Debug 设置捕获旧地址，再删除 Session 和引擎，将 handle 置 0，不调用新模型初始化。

输入、发送和附件禁用；模型选择、模式菜单及 Previous model addresses 仍可用。重复选择 No model 不覆盖已经保留的旧地址；重新选实际模型才创建新引擎。

No model 不冻结堆。界面和其他运行时分配仍可能复用旧空间，读取也可能影响页面驻留。

### 8.5 源码核实：Session 析构会间接 Reset

后来检查得到的完整路径：

```text
NativeModel.close()
  -> litert_lm_session_delete()
  -> SessionAdvanced::~SessionAdvanced()
  -> ExecutionManager::ReleaseSession()
  -> 最后一个 Session 释放：ResourceManager::ResetCurrentHandler()
  -> llm_executor_->Reset()
  -> 删除 engine，释放执行器和其成员
```

Ghost 的 Reset 调用 `ScrubAll()`；Normal 的 Reset 只执行原 Token 数组处理。两种模式的 Reset 都执行原有 `ClearKVCache()`，这不是本轮新加的 KV 清理策略。

之前仅检查 NPU executor 析构函数，未发现显式 `ScrubAll()`，曾据此怀疑关闭路径缺少清理；完整 Session 释放链纠正了这个判断。不能再把该初步判断作为现状结论。

## 9. 取证实验和实测结论

### 9.1 spare 数值的来源追踪

临时分配追踪与硬件写监视点，在合成复现中捕获到 Pixel 的 `mapper.pixel.so` 写入 PlaneLayout 元数据。64 位写入值为 8、1048576、1，因相对 int32 方块边界偏移 1 字节，显示成 2048、268435456、256。

这证明了该模式的一条实际写入来源，不证明所有 spare 非零数都来自图形元数据，也未恢复先前用户截图的同一分配历史。

证据和限制：[`write-watch/findings.md`](../annoymous/diagnostics/provenance/write-watch/findings.md)。分配包装、LLDB、硬件监视点及 trace hook 已清除，不存在于当前正式 native 库。

### 9.2 分阶段释放实验

Gemma 3 G5 Q8 → Gemma 4 E2B；Normal / Ghost 各 3 次。旧数组 size=21、capacity=36，共 144 字节。

Normal 三次均观察到：删除 Session 后和删除引擎后，144 字节与释放前完全一致，21 个原始正 ID 全部保留。新模型加载后字节才发生变化，原位置匹配 ID 分别剩 5、5、0；加载后的相同数值可能是巧合，不证明旧归属。

页面在各检查点仍可读，`mincore` 在读取前报告驻留。说明本次小数组并非销毁后立即失去可恢复内容；不证明中间没有任何内核还页/重映射，也不证明物理 PFN 恒定。

记录：[`retirement-experiment/findings.txt`](../annoymous/diagnostics/retirement-experiment/findings.txt)。

### 9.3 按秒实验

两种环境：native 内等待后统一返回结果；实际 MainActivity → No model。每种环境各测 Normal / Ghost 一次，不加载替代模型，采样 0、1、2、5、10、20 秒。

| 对象 | 观察结果 |
| --- | --- |
| Normal，两种环境 | 所有时间点 144 字节完全未变，21/21 原 ID 保留 |
| Ghost，两种环境 | 卸载后首个检查点已与卸载前不同；后续到 20 秒与该首个检查点完全一致 |
| 页面 | 24 个检查点均完整读到 144 字节，读取前页面驻留 |

Ghost 的已消费位置在卸载前已为零；释放后出现其他内容，无法用最终字节证明清零瞬间。定时读取本身可能保持页面活跃，这不是无观察干扰的物理内存实验。

记录：[`timed-unload/findings.txt`](../annoymous/diagnostics/timed-unload/findings.txt)、[`summary.txt`](../annoymous/diagnostics/timed-unload/summary.txt)。

### 9.4 完全通过应用界面的复核

在实际 APK 中选 Normal + Debug，输入 `Reply with only hello.`，打开 Token IDs，随后选 No model，再打开 Previous model addresses。

同一基址 `0xb4000076a3764020` 的 processed 槽位 #0～#14，15/15 值与卸载前一致。#15～#25 是原 spare，不是新增 Token。该次未比较独立 pending。

记录：[`app-no-model/findings.txt`](../annoymous/diagnostics/app-no-model/findings.txt)、[`comparison.json`](../annoymous/diagnostics/app-no-model/comparison.json)。

以上实验只使用合成对话。实验用 `stageProbe` / `timedUnload` JNI 和诊断测试已从应用源码移除；补丁、测试源码和结果仅保存在 diagnostics，之后恢复正常 APK。实验材料中的历史 APK hash 不代表当前发布文件。

## 10. 测试覆盖与不能推导的结论

以下是各阶段已保存的验证记录，不表示写本文时重新运行了全部历史测试。

| 验证 | 已覆盖内容 |
| --- | --- |
| 34 项 native 测试 | 29 项 ProcessedTokens；NpuConfig 默认及 aggregate 兼容；2 项 C setter；engine 设置传播；generic NPU 拒绝 |
| `TokenScrubbingFlagDeviceTest` | E2B false/true 两轮 greedy 回复一致；已消费 ID 保留/清除；所测 active KV slot 字节一致；reset capacity 行为 |
| `MemoryInspectionDeviceTest` | int32 与 raw bytes；CSV 行数；KV manifest / 分量 / payload；检查前后 step 不变 |
| `MultimodalDeviceTest` | E2B 图像、音频、后续上下文及损坏图片错误 |
| `Gemma3DeviceTest` | G5 NPU Normal / Ghost 两轮记忆与已消费 ID 检查 |
| `RetiredMemoryDeviceTest` | 内核读取与活数组一致；已释放地址安全读取/报错；无效地址不直接解引用 |
| `NoModelDeviceTest` | 卸载后 handle=0、保留地址、禁用输入、重复卸载和重新加载 |
| Android 构建 / lint | `assembleDebug`、`assembleDebugAndroidTest`、`lintDebug` 多阶段通过 |

native C settings 测试为 Android 交叉构建时，仅在临时 checkout 移除测试 target 的平台限制，测试后恢复；主仓库 `c/BUILD` 的原测试平台限制没有因此改变。

A/B 证据只覆盖实测模型、解码方式和选定 KV slot，不能推广为全部模型、推测解码、多 candidate、长上下文、回滚和重复惩罚配置都已验证无回归。需要历史 ID 的调用方应保持关闭，或另行验证兼容性。

还没有实现或证明：

- 全部 Token 副本、pending `TokenData::id_`、临时张量、日志或 allocator 历史的完整清理；
- CPU / GPU 或 generic NPU executor 的同等清理；
- 已释放 KV 的完整 layer/shape 解析与物理设备内存扫描；
- 跨进程、内核页表/PFN 或任意物理 DRAM 读取；
- KV 序列化后恢复会话的通用功能；当前 `.gkv` 是取证导出，不是可恢复 Session checkpoint；
- 所有编译优化和标准库实现下的安全擦除证明；
- 运行中 spare capacity 持续清零，或每个非零方块的唯一写入归属。

NPU debug logging、tokenizer、返回 vectors 等未纳入本清理保证；开启其他日志功能时也不能因 Ghost 开启就假定其中没有 Token 信息。

## 11. 重建与 reviewer 阅读顺序

### 11.1 只审查核心变更

```sh
# 在 LiteRT-LM 根目录；不会切换当前分支。
git diff v0.18.0..HEAD -- runtime/executor/
git diff -- c/ runtime/engine/ runtime/executor/
```

先读 `ProcessedTokens` 的两个清理方法，再读六个 live 点及 Reset / RestoreContext 的开关，最后读配置传播、拒绝不支持路径的逻辑和测试。

主工作区新增但尚未提交的说明/测试也必须纳入审查：`docs/npu_token_id_scrubbing.md`、`runtime/engine/npu_token_id_scrubbing_test.cc`。

### 11.2 重建 APK 使用的扩展运行时

在独立且干净的 `87203e8f` checkout 中，按以下顺序应用：

1. `runtime-extension/runtime-debug.patch`。
2. 运行 `python3 /path/to/annoymous/runtime-extension/install.py /path/to/checkout`。
3. `runtime-extension/npu-nonspeculative-scrub.patch`。
4. `runtime-extension/token-id-scrubbing-flag.patch`。
5. `runtime-extension/address-metadata.patch`。

随后在该 checkout 根目录构建：

```sh
ANDROID_NDK_HOME=/path/to/ndk bazel build \
  --config=android_arm64 \
  --macos_sdk_version=15.5 \
  --linkopt=-Wl,-z,max-page-size=16384 \
  //c:litert-lm
```

这些补丁是针对上述基线的累积应用流程。主工作区已包含部分相同修改，不能无判断地再次叠加，避免重复应用。

Google Tensor dispatch 使用同一 LiteRT revision，应用 OSS BUILD 补丁后构建 `@litert//litert/vendors/google_tensor/dispatch:dispatch_api_so`。将匹配的库复制到应用 `jniLibs/arm64-v8a/`；扩展新增 C++ 虚接口，相关运行时组件须一起重建。

在应用目录执行：

```sh
./gradlew assembleDebug assembleDebugAndroidTest lintDebug
```

当前可安装产物为 [`../annoymous/ghost.apk`](../annoymous/ghost.apk)，另有兼容名称 `annoymous.apk`；hash 以 [`APK-SHA256.txt`](../annoymous/APK-SHA256.txt) 为准。它是本地 debug 签名 APK，不是应用商店发布包。

### 11.3 应用侧辅助变化

为支持测试和正确解释结果，应用还进行了英文界面、粉色 Ghost 图标、侧栏/附件底部面板、Markdown 回复、KV 对齐表格、详情返回按钮和 Token / KV 导出入口等调整。应用显示名为 `ghost`，包名仍为 `com.example.annoymous`。

这些属于 UI/JNI 和实验入口，不是 LiteRT-LM 推理算法修改。Manifest 不申请 INTERNET 权限，关闭应用备份；窗口使用 `FLAG_SECURE`。它们也不能替代 native 内存清理。

完整应用使用方法与构建环境见 [`../annoymous/README.md`](../annoymous/README.md)。该 README 保留部分早期阶段描述；涉及当前模型切换、No model 和清理开关时，以实际源码及本说明的当前状态为准。

### 11.4 核心工作区文件索引

下表对应本文核对时主仓库相对 HEAD 的源码/测试差异；前述两个提交中的 `.cc` 清理算法另外由 `git diff v0.18.0..HEAD` 查看。

| 文件 | 本轮修改及原因 |
| --- | --- |
| `runtime/executor/llm_litert_npu_compiled_model_executor.cc` | 补非推测解码漏点；给六处 live 清理和两处全数组清理加统一开关 |
| `runtime/executor/llm_executor_processed_tokens.h` | 修正“全进程保证”注释，限制为本数组存储 |
| `runtime/executor/llm_executor_settings.h` | 末尾追加默认 false 的 NpuConfig 字段，保留旧位置初始化含义 |
| `runtime/executor/llm_executor_settings.cc` | 配置输出包含新开关，便于核对实际设置 |
| `runtime/executor/llm_litert_compiled_model_executor_factory.cc` | 对启用清理的 generic NPU 路径明确拒绝 |
| `runtime/engine/shared_flags.h` / `.cc` | 声明/定义默认 false 的 CLI flag |
| `runtime/engine/litert_lm_lib.h` / `.cc` | engine helper 增加字段、向 NpuConfig 传播、拒绝非 NPU 启用 |
| `runtime/engine/litert_lm_main.cc` | 基础 CLI 在创建引擎前应用开关 |
| `runtime/engine/litert_lm_advanced_main.cc` | advanced CLI 将 flag 传入 helper 设置 |
| `c/engine.h` / `.cc` | 新增 fork 的 C setter、输入检查和 last-error 返回 |
| `runtime/executor/llm_executor_processed_tokens_test.cc` | 检查清理后位置、frontier、sentinel、pending 和 reset 状态 |
| `runtime/executor/llm_executor_settings_test.cc` | 检查默认关闭及旧 aggregate 初始化兼容 |
| `runtime/executor/llm_litert_compiled_model_executor_factory_test.cc` | 检查不支持路径不能假装启用 |
| `c/engine_test.cc` | 检查 C 默认值、开启/关闭、空 settings 和非 NPU 错误 |
| `runtime/engine/npu_token_id_scrubbing_test.cc` | 新增专门测试，覆盖设置传播和 CPU 拒绝 |
| `runtime/engine/BUILD` | 注册上述新测试 target，使其实际参与构建 |
| `docs/npu_token_id_scrubbing.md` | 面向调用者说明开关用法、默认值和范围 |
| `modifiednote.md` | 本文，汇总核心修改、应用扩展、证据与限制 |
